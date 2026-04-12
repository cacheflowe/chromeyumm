# Feature: display-config.json

## User Story
As an installation designer, I want to define my display layout, canvas size, content URL, and output transport settings in a single JSON file, so I can reconfigure installations without code changes.

## File Location
- Project root: `display-config.json` (source of truth)
- Copied to `dist/` on each build
- Loader (`src/app/config.ts`) searches upward from `process.cwd()` to find it

## Schema

```json
{
  "virtualCanvas": { "width": 3840, "height": 1080 },

  "windows": [
    {
      "label": "Left display",
      "slot": 0,
      "window":  { "x": 0,    "y": 0, "width": 1920, "height": 1080 },
      "source":  { "x": 0,    "y": 0, "width": 1920, "height": 1080 }
    }
  ],

  "alwaysOnTop": true,
  "fullscreen": false,
  "startInteractive": false,

  "contentUrl": "http://localhost:5173",

  "spoutOutput": { "senderName": "Chromeyumm" },
  "spoutInput":  { "senderName": "TD_Spout_Sender" },

  "ddpOutputs": [
    {
      "label": "FPP strip A",
      "controllerAddress": "192.168.1.50",
      "port": 4048,
      "destinationId": 1,
      "pixelStart": 0,
      "source": { "x": 0, "y": 0, "width": 960, "height": 1080 },
      "zigZagRows": false
    }
  ]
}
```

## Fields

| Field | Type | Default | Description |
|---|---|---|---|
| `virtualCanvas` | `{width, height}` | Auto-computed from windows | Total virtual canvas size in virtual pixels |
| `windows` | array | **Required** | Display window definitions |
| `windows[].slot` | number | — | Ordering index |
| `windows[].window` | `{x, y, width, height}` | — | Screen position (OS logical pixels) |
| `windows[].source` | `{x, y, width, height}` | Derived from window | Virtual canvas region to sample |
| `windows[].label` | string | — | Human-readable label (logged only) |
| `alwaysOnTop` | boolean | `false` | Set display windows alwaysOnTop |
| `fullscreen` | boolean | `false` | Set display windows fullscreen |
| `startInteractive` | boolean | `false` | Start in interactive mode (master visible) |
| `interactiveWindows` | boolean | `false` | Forward mouse input from display windows to CEF (visitor-safe touch/click) |
| `contentUrl` | string \| null | `null` | Content URL to load. Relative paths (e.g. `src/views/feature-check/index.html`) are resolved to `file:///` URLs relative to cwd. |
| `spoutOutput` | `{senderName}` | — | Enable Spout output sender |
| `spoutInput` | `{senderName}` | — | Enable Spout input receiver |
| `ddpOutput` | object | — | Single native DDP output (legacy shorthand) |
| `ddpOutputs` | array | — | One or more native DDP outputs (preferred) |
| `ddpOutputs[].controllerAddress` | string | — | DDP controller host/IP |
| `ddpOutputs[].port` | number | `4048` | DDP UDP port |
| `ddpOutputs[].destinationId` | number | `1` | DDP destination ID byte |
| `ddpOutputs[].pixelStart` | number | `0` | Pixel offset for this output |
| `ddpOutputs[].source` | `{x, y, width, height}` | — | Virtual canvas region sampled for this output |
| `ddpOutputs[].zigZagRows` | boolean | `false` | Reverse every other row for snake-wired LED strips |
| `ddpOutputs[].flipH` | boolean | `false` | Mirror output horizontally |
| `ddpOutputs[].flipV` | boolean | `false` | Mirror output vertically |
| `ddpOutputs[].rotate` | `0\|90\|180\|270` | `0` | Clockwise rotation in degrees (90/270 swap output dimensions) |
| `ddpOutputs[].enabled` | boolean | `true` | Toggle output without deleting config |
| `ddpOutputs[].label` | string | — | Human-readable label (logged/debug panel only) |

## Transport Outputs

All three output transports read from the same rendered CEF texture and can run simultaneously. Each is enabled by the presence of its key in `display-config.json`; removing or suffixing the key with `_DISABLED` turns it off.

| Transport | Config key | Enabled when | Can combine with |
|---|---|---|---|
| **Spout output** | `spoutOutput` | Key present with `senderName` | DDP, D3D multi-window |
| **DDP output** | `ddpOutputs` (or `ddpOutput`) | Array has ≥ 1 enabled entry | Spout, D3D multi-window |
| **D3D multi-window** | `windows` | Array has ≥ 1 entry (slot mapping) | Spout, DDP |

### How they coexist

Each frame, `OnAcceleratedPaint` fires once with a shared GPU texture. The frame-output manager routes it in two paths:

- **GPU path** (Spout): `SendTexture` directly on the shared D3D11 texture — no copy, no CPU involvement
- **CPU path** (DDP, ArtNet, …): one staging-texture readback shared across all CPU-path protocols, then each protocol crops and packetizes its region independently

D3D multi-window blits are performed separately, also directly from the shared texture.

### Adding a new protocol

Implement `IOutputProtocol` in `native/frame-output/protocols/<name>/`, register it in `frame_transport_runtime.cpp`, add a config key to `DisplayConfig` in `src/app/config.ts`, and start it in `src/app/index.ts` following the same `active<Name>` / `startFn` / `stopFn` pattern used by Spout and DDP.

## Auto-Detect Fallback
When no `display-config.json` is found:
- Auto-detects connected displays via `Screen.getAllDisplays()`
- Single display → simulation mode: two half-size windows side by side
- Multiple displays → one display window per physical display, canvas spans all

## Conventions
- JSON with `_comment` fields for documentation (not JSONC — parsed with `JSON.parse`)
- Disabled features use renamed keys (e.g., `"spoutOutput_DISABLED"` instead of removing the key)
- Multiple `contentUrl` options stored as `contentUrl_`, `contentUrl__`, etc. — only `contentUrl` is active

## Validation
- `windows` must be a non-empty array (returns null from loader otherwise)
- `JSON.parse` failure → console error, returns null → app falls back to auto-detect

## Out of Scope
- Hot-reloading config at runtime
- GUI config editor
- Per-display content URLs

## Related
- [multi-window-d3d-output.md](multi-window-d3d-output.md) — How windows are created from config
- [../FRONTEND.md](../FRONTEND.md) — URL parameters passed to content
