# Decision: DDP Frame Output via Unified Transport

## Status
Accepted

## Context
Chromeyumm renders web content to a virtual canvas via CEF OSR. The existing output paths (D3D11 blit to display windows, Spout GPU texture sharing) serve screen and VJ tool use cases. A new requirement emerged: driving LED panels and pixel-mapped fixtures via FPP (Falcon Player), WLED, and other DDP-compatible controllers directly from the browser canvas — without external pixel-mapping software.

DDP (Distributed Display Protocol) is a simple UDP protocol used in the LED lighting community. Each packet carries up to 480 RGB pixels (1440 bytes) with a 10-byte header specifying offset, length, and sequence. Controllers like FPP listen on port 4048.

## Options Considered

### External pixel-mapping tool (TouchDesigner / MadMapper)
- Spout output → external tool → DDP/ArtNet to controllers
- Adds latency, another process to manage, and a $$ license
- Works but defeats the "minimal installation" goal

### ArtNet / sACN from browser via WebSocket bridge
- JavaScript generates ArtNet packets → WebSocket → relay process → UDP
- High latency, complex, requires a relay daemon
- Not viable for 60fps pixel data

### Native DDP in the C++ layer
- OnAcceleratedPaint delivers shared texture → staging readback → BGRA→RGB conversion → DDP packetization → UDP send
- No external tools, no extra processes, no license costs
- Sub-millisecond from render to wire

## Decision
Native DDP output in the C++ layer, driven by the same `OnAcceleratedPaint` frame that feeds D3D and Spout outputs. Implemented as an `IOutputProtocol` within the unified frame transport module.

## Architecture

### Frame transport module (`native/frame-output/`)
Rather than adding DDP-specific code to cef-wrapper, a standalone transport module provides:

- **`IOutputProtocol`** interface — `Start()`, `Stop()`, `OnFrame()` (CPU path), `OnGpuFrame()` (GPU path)
- **`FrameOutputManager`** — registers and fans frames to all active outputs
- **`TransportSession`** — per-webview state: owns the manager, staging texture, and frame counter
- **`FrameTransportRuntime`** — singleton managing sessions by webviewId

GPU-native outputs (Spout) receive the `ID3D11Texture2D*` directly. CPU-path outputs (DDP) trigger a staging texture readback — but only when at least one CPU output is registered. This means Spout-only mode has zero CPU readback overhead.

### DDP protocol (`native/frame-output/protocols/ddp/`)

**Packetization:**
- Source rect from virtual canvas → BGRA→RGB conversion with optional zig-zag row reordering
- RGB payload split into 480-pixel (1440-byte) packets with DDP v1 headers
- 4-bit sequence counter, configurable destination ID, absolute byte offset addressing

**Partial-update optimization:**
- Per-row dirty detection: compares current RGB payload against previous frame row-by-row
- Only changed row ranges are sent, with correct absolute DDP offsets
- Unchanged frames are skipped entirely (no packets sent)
- 1-second keepalive: if no change for 1s, resends full frame to prevent controller timeout

**Multi-controller pixel mapping:**
- Each DDP output maps an independent `source` rect from the virtual canvas
- Multiple outputs fan out in parallel — different canvas regions to different controller IPs
- Config-driven via `ddpOutputs[]` in `display-config.json`

### Host-services adapter
The transport module is decoupled from cef-wrapper via a `HostServices` struct with function pointers:
- `ensureHostRuntime(webviewId)` — lazily creates D3D device if not already active
- `notifyOutputsStopped(webviewId)` — releases D3D resources when last output stops
- `wakeBrowser(webviewId)` — un-hides CEF so OnAcceleratedPaint keeps firing

This allows the transport module to compile and test independently of CEF.

## Config Example

```json
{
  "ddpOutputs": [
    {
      "label": "LED panel A",
      "controllerAddress": "192.168.1.100",
      "port": 4048,
      "source": { "x": 0, "y": 0, "width": 32, "height": 16 }
    },
    {
      "label": "LED strip B",
      "controllerAddress": "192.168.1.101",
      "source": { "x": 32, "y": 0, "width": 1, "height": 300 },
      "zigZagRows": true
    }
  ]
}
```

## Consequences

- **Staging readback cost**: ~0.1ms per frame for a 32×384 canvas (small LED matrices). For full-HD readback, ~1-2ms — acceptable at 60fps but worth monitoring on GPU-constrained systems.
- **UDP is fire-and-forget**: no delivery guarantee. Keepalive resend mitigates controller timeout but doesn't detect controller failure.
- **Protocol extensibility**: the `IOutputProtocol` interface makes adding ArtNet, sACN, or DMX a matter of implementing one class with the same interface — no changes to cef-wrapper or the frame fan-out logic.
- **No browser-side awareness**: DDP output is invisible to JavaScript. Stats are available via `window.__chromeyumm.output.ddp.stats` (injected periodically from the host).

## Stats & Monitoring
Each DDP output tracks: `framesReceived`, `framesSent`, `partialFramesSent`, `keepaliveFramesSent`, `packetsSent`, `bytesSent`, `sendErrors`, `lastSendTimeMs`. Stats are exported as JSON via FFI and injected into the browser's `window.__chromeyumm` state object every second.

## Related
- [osr-shared-texture.md](osr-shared-texture.md) — How OnAcceleratedPaint delivers the shared texture
- [single-master-gpu-blit.md](single-master-gpu-blit.md) — The D3D11 blit path that shares the same device
- [direct-ffi.md](direct-ffi.md) — Why FFI over RPC for native calls
