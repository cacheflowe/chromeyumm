# Feature: display-config.json

## User Story
As an installation designer, I want to define my display layout, canvas size, content URL, and Spout settings in a single JSON file, so I can reconfigure installations without code changes.

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
  "spoutInput":  { "senderName": "TD_Spout_Sender" }
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
| `spoutOutput` | `{senderName}` | — | Enable Spout output mode (disables D3D multi-window) |
| `spoutInput` | `{senderName}` | — | Enable Spout input receiver |

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
