# Frontend — Views & Browser-Side Architecture

## Overview

The "frontend" in Chromeyumm is web content rendered by CEF's Offscreen Rendering. The browser runs a single page (the "master") at the configured `contentUrl`. The page has full access to standard Web APIs (WebGL, WebGPU, Canvas 2D, etc.) and any injected Spout input bindings.

## Content URL Resolution

Priority order:
1. `contentUrl` in `display-config.json` (required)
2. App exits with error if not set

URL types supported:
- `http://` / `https://` — external URLs (e.g., `http://localhost:5173` for Vite dev server)

## Example Views

Views are served via dev servers (e.g., Vite), not bundled into dist/.

| View | Path | Description |
|---|---|---|
| Three.js | `src/views/threejs/` | Full-canvas Three.js renderer with FBO scene |
| React Three Fiber | `src/views/r3f/` | Vite-built R3F app; run `bun run dev` in dir |
| Spout Demo | `src/views/spout-demo/` | Spout input demo — displays received texture |
| Spout Video | `src/views/spout-video/` | Video playback + Spout input overlay |

### R3F View

The R3F view is a separate Vite project at `src/views/r3f/`. It has its own `package.json`, `tsconfig.json`, and `vite.config.ts`. Run `cd src/views/r3f && bun run dev` then set `contentUrl` to `http://localhost:5173`.

## URL Parameters

The master BrowserWindow appends query parameters to the content URL:

| Parameter | Value | Purpose |
|---|---|---|
| `totalWidth` | number | Virtual canvas width in pixels |
| `totalHeight` | number | Virtual canvas height in pixels |
| `spoutReceiverId` | number | Spout receiver ID (if Spout input enabled) |
| `spoutInputName` | string | Spout sender name being received |
| `spoutOutputName` | string | Spout sender name being sent |
| `slots` | JSON | Array of display slot geometries (D3D output mode only) |

## Spout Input — Browser-Side API

When `?spoutReceiverId=<id>` is in the URL, `cef-helper.cpp` injects bindings in `OnContextCreated`:

**Tier 1** (zero-copy, V8 sandbox off):
- `window.__spoutFrameBuffer` — `ArrayBuffer` backed by `MapViewOfFile`; updates in-place

**Tier 2** (persistent-buffer memcpy, V8 sandbox safe):
- `window.__spoutGetFrame()` — copies current frame into a persistent ArrayBuffer; returns `true` on success

The TypeScript helper `src/app/spout-input.ts` wraps `createSpoutInputReceiver()` which tries tier 1 then tier 2. Browser code calls `poll()` each `requestAnimationFrame`.

### Shared Memory Layout

```
[0-3]   seq      — Int32LE; incremented by C++ after pixels written
[4-7]   width    — Uint32LE
[8-11]  height   — Uint32LE
[12-15] reserved
[16+]   BGRA pixels (~31.6 MB at 3840×2160)
```

## Web Components

Reusable custom elements in `src/components/` — drop into any page loaded in Chromeyumm:

| Component | Element | Purpose |
|---|---|---|
| `debug-panel.js` | `<debug-panel>` | Debug overlay: hotkey reference, Spout status, stats.js perf graphs, extensible view sections |
| `slot-overlay.js` | `<slot-overlay>` | Coordinate grid + per-slot boundary boxes over the virtual canvas |
| `spout-receiver.js` | `<spout-receiver>` | Spout input with WebGL BGRA→RGBA rendering + disconnect detection |
| `spout-video.js` | `<spout-video>` | Extends spout-receiver — wraps frames in a native `<video>` via captureStream |
| `layout-params.js` | (utility) | Parses standard Chromeyumm URL parameters |

All components are framework-agnostic, use Shadow DOM, and require zero build tools. See [product-specs/web-components.md](product-specs/web-components.md) for full API docs.

## Injected Scripts

The app injects scripts via `executeJavascript()`:
- **Hide/show cursor**: `document.documentElement.style.setProperty('cursor','none','important')` in output mode
- **Debug panel toggle**: `window.__ebPanelToggle()` (Ctrl+D)
- **Debug overlay toggle**: `window.__ebDebugToggle()` (Ctrl+D)
- **State injection**: `window.__ebState = { alwaysOnTop, interactiveMode }`

## Hotkeys

| Key | Action |
|---|---|
| Ctrl+M | Toggle interactive ↔ output mode |
| Ctrl+Shift+M | Reset display windows |
| Ctrl+R | Reload content (`location.reload()`) |
| Ctrl+F | Toggle alwaysOnTop |
| Ctrl+D | Toggle debug panel / overlay |
| Escape | Quit cleanly |

## Known Limitations

- `loadURL()` ignores its url argument — use `location.reload()` for content reload
- Ctrl+D only works when the page includes `debug-panel.js` and registers `window.__ebPanelToggle`
- "Black window" in OSR: master HWND is always black — content only exists in the `OnAcceleratedPaint` shared texture. Verify pipeline via staging-texture pixel readback: alpha=0 → unrendered, alpha=FF + black RGB → page content is black.

## Related

- [ARCHITECTURE.md](../ARCHITECTURE.md) — System context diagram
- [BACKEND.md](BACKEND.md) — C++ internals, FFI layer
- [product-specs/spout-input.md](product-specs/spout-input.md) — Spout input feature spec
- [product-specs/multi-window-d3d-output.md](product-specs/multi-window-d3d-output.md) — D3D output feature spec
