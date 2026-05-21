# screen-ddp: Standalone Screen-Capture-to-DDP Sender

## Status
Accepted

## Context
Chromeyumm drives LED controllers by rendering web content via CEF and shipping pixels over DDP. A complementary use case emerged: send pixels from any visible region of the Windows desktop (a video player, a game, a media wall, an existing fullscreen app) to DDP controllers — without loading a browser or requiring a web-based content source.

Rather than building this into chromeyumm itself, a dedicated lightweight executable handles the use case independently. The two apps share the `DdpOutput` class and protocol implementation but are otherwise separate processes.

## What It Is

`screen-ddp.exe` is a standalone Windows executable that:

1. Captures a configurable region of any monitor using DXGI Desktop Duplication (same API Windows itself uses for screen recording)
2. Optionally downscales the captured region to a target canvas size
3. Ships the resulting pixel data to one or more DDP controllers over UDP using the same `DdpOutput` class as chromeyumm

No browser, no CEF, no JavaScript — just a config file, a DXGI device, and UDP sockets.

## How It Relates to Chromeyumm

| | chromeyumm | screen-ddp |
|---|---|---|
| Pixel source | CEF browser canvas (web content) | Any region of any monitor |
| DDP implementation | `DdpOutput` via `IOutputProtocol` in `frame-output/` | Same `DdpOutput` class, linked directly |
| Config file | `display-config.json` | Separate config (e.g. `screen-capture-config.json`) |
| Process | Main app process | Standalone `screen-ddp.exe` |
| GPU path | D3D11 device shared with CEF OSR | Dedicated D3D11 device per run |
| Use case | Web-driven LED content | Anything on screen |

Both share `native/frame-output/protocols/ddp/ddp_output.cpp/.h`. Changes to DDP protocol behavior apply to both.

## Build

```powershell
bun run build:screen-ddp      # compile screen-ddp.exe only (fast, uses sccache)
bun run build                 # full build, also compiles screen-ddp.exe
```

`build:screen-ddp` only compiles `native/screen-ddp/screen_ddp_main.cpp` and `native/frame-output/protocols/ddp/ddp_output.cpp`, then links. With sccache, unchanged files are served from cache — iteration is fast.

Output: `dist/screen-ddp.exe`

## Run

```powershell
dist/screen-ddp.exe <config.json> [flags]
```

If no config path is given, defaults to `display-config.json` in the current directory.

### Flags

| Flag | Description |
|---|---|
| `--show-region` | Show a draggable magenta-border overlay window over the capture region |
| `--save-frame [path]` | Save the first non-black captured frame as a BMP (default: `screen-ddp-capture.bmp`) |
| `--verbose` | Print per-second stats and DDP packet diagnostics |

### Example

```powershell
dist/screen-ddp.exe screen-capture-config.json --show-region
```

## Config Format

```json
{
  "screenCapture": {
    "monitor": 0,
    "captureRect": { "x": 0, "y": 0, "width": 384, "height": 32 },
    "canvasWidth": 384,
    "canvasHeight": 32,
    "targetFps": 60
  },
  "ddpOutputs": [
    {
      "label": "LED strip A",
      "controllerAddress": "192.168.1.245",
      "port": 4048,
      "destinationId": 0,
      "pixelStart": 0,
      "source": { "x": 0, "y": 0, "width": 384, "height": 32 },
      "zigZagRows": false,
      "rotate": 90,
      "flipH": true
    }
  ]
}
```

### `screenCapture` fields

| Field | Default | Description |
|---|---|---|
| `monitor` | `0` | Monitor index across all GPU adapters (0 = primary) |
| `captureRect` | required | Region to capture in physical pixels, relative to the monitor's top-left |
| `canvasWidth` / `canvasHeight` | required | Output canvas size. If different from captureRect, pixels are bilinearly downscaled |
| `targetFps` | `60` | Target capture frame rate |

### `ddpOutputs` fields

Same options as chromeyumm's `ddpOutputs` block in `display-config.json`:

| Field | Default | Description |
|---|---|---|
| `controllerAddress` | required | IP address of DDP controller |
| `port` | `4048` | UDP port |
| `destinationId` | `0` | DDP destination ID byte. `0` is treated as `1` (FPP/WLED default) |
| `pixelStart` | `0` | Absolute pixel offset into controller universe |
| `source` | full canvas | Sub-rect of canvas to send |
| `zigZagRows` | `false` | Reverse even rows for serpentine pixel layouts |
| `flipH` / `flipV` | `false` | Mirror output horizontally / vertically |
| `rotate` | `0` | Clockwise rotation: `0`, `90`, `180`, or `270` |
| `enabled` | `true` | Set `false` to skip an output without removing it |
| `label` | address | Display name in startup log |

## Capture Region Overlay (`--show-region`)

When `--show-region` is passed, a frameless topmost window appears over the capture region with a 6-pixel magenta border. The interior is click-through (transparent to input).

- **Drag** by grabbing the magenta border — cursor changes to the four-arrow move icon
- On drag end, the new position is clamped to the monitor bounds, the capture position updates live, and `captureRect.x/y` is written back to the config file
- The overlay is excluded from DXGI capture (`WDA_EXCLUDEFROMCAPTURE`) so it does not appear in the DDP output
- Closing the overlay window stops the process (Ctrl+C also works)

## Architecture Notes

### Multi-monitor / multi-adapter
DXGI monitors are enumerated across all GPU adapters. On hybrid-GPU laptops (Intel + NVIDIA Optimus), each adapter exposes only its own outputs — `D3D_DRIVER_TYPE_HARDWARE` would silently use the default adapter and fail to find monitors on the other adapter. screen-ddp walks all adapters with `IDXGIFactory1::EnumAdapters` and selects the target monitor by global index.

### Frame loop
- `AcquireNextFrame` with 100ms timeout
- Frames where `LastPresentTime == 0` (cursor-only updates, no pixel change) are skipped — `DdpOutput`'s keepalive thread handles retransmission for static content
- Timer resolution is set to 1ms via `timeBeginPeriod(1)` for precise frame-rate pacing; capture thread runs at `THREAD_PRIORITY_ABOVE_NORMAL`

### Scaling
If `canvasWidth/Height` differs from `captureRect` dimensions, bilinear downscaling is performed on the GPU via a D3D11 render pass with a linear-filtering sampler. The captured sub-region is first copied into a `capW×capH` intermediate texture, then rendered as a full-screen triangle into a `cvW×cvH` render target. The result is copied to a CPU-readable staging texture for DDP transmission. This keeps the PCIe readback size minimal (canvas size, not capture size) and eliminates CPU scaling overhead entirely. When dimensions match, the intermediate render pass is still used but performs a 1:1 copy through the same pipeline.

### DDP keepalive
`DdpOutput` runs a background keepalive thread that retransmits the last frame if no new frame has been sent in 100ms. This prevents LED controllers from blanking when the captured content is static.

## Related

- [ddp-frame-output.md](ddp-frame-output.md) — DDP protocol design and `DdpOutput` implementation
- [COMMANDS.md](../COMMANDS.md) — Build and run commands
