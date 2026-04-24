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
| `debug-panel.js` | `<debug-panel>` | Consolidated debug overlay: hotkey reference, display/output status, Spout input, stats.js perf graphs, slot boundary overlay, extensible view sections |
| `spout-receiver.js` | `<spout-receiver>` | Spout input with WebGL BGRA→RGBA rendering + disconnect detection |
| `spout-video.js` | `<spout-video>` | Extends spout-receiver — wraps frames in a native `<video>` via captureStream |
| `layout-params.js` | (utility) | Parses standard Chromeyumm URL parameters |
| `inject.js` | (auto-injector) | Bundled into `dist/debug-inject.js`; auto-injects `<debug-panel>` on did-navigate |

## Video Codec Support

The CEF standard distribution from Spotify CDN does **not** include proprietary codec decoders (H.264/AVC, H.265/HEVC, AAC). These are excluded due to patent licensing. Supported codecs out of the box:

| Codec | Container | Supported |
|---|---|---|
| VP8 | WebM | ✅ |
| VP9 | WebM | ✅ |
| AV1 | WebM / MP4 | ✅ |
| Opus | WebM | ✅ |
| Vorbis | WebM | ✅ |
| H.264 (AVC) | MP4 | ❌ |
| H.265 (HEVC) | MP4 | ❌ |
| AAC | MP4 | ❌ |

### Converting H.264 → VP9 with ffmpeg

```bash
# Single file — high quality, good compression
ffmpeg -i input.mp4 -c:v libvpx-vp9 -crf 30 -b:v 0 -c:a libopus output.webm

# Same but 2-pass for better quality at target bitrate (e.g. 4 Mbps)
ffmpeg -i input.mp4 -c:v libvpx-vp9 -b:v 4M -pass 1 -an -f null NUL
ffmpeg -i input.mp4 -c:v libvpx-vp9 -b:v 4M -pass 2 -c:a libopus output.webm

# Lossless (large files, useful for installation source material)
ffmpeg -i input.mp4 -c:v libvpx-vp9 -lossless 1 -c:a libopus output.webm

# Batch convert all .mp4 in a folder (PowerShell)
Get-ChildItem *.mp4 | ForEach-Object { ffmpeg -i $_.Name -c:v libvpx-vp9 -crf 30 -b:v 0 -c:a libopus "$($_.BaseName).webm" }
```

**Tips:**
- `-crf 30` with `-b:v 0` = constant quality mode (lower CRF = higher quality, 15–35 is typical)
- VP9 encoding is slow; add `-row-mt 1 -threads 4` to speed it up
- For 4K installation content, `-crf 24 -row-mt 1` is a good starting point
- Use `<video>` with `.webm` source in your views — it will Just Work™

All components are framework-agnostic, use Shadow DOM, and require zero build tools. See [product-specs/web-components.md](product-specs/web-components.md) for full API docs.

## Injected Scripts

The app injects scripts via `executeJavascript()`:
- **State object**: `window.__chromeyumm = { alwaysOnTop, interactiveMode, display, output, input, hotkeys }` — full runtime state, polled by debug panel
- **Debug panel auto-injection**: `dist/debug-inject.js` — injects `<debug-panel>` into any page (skips if already present)
- **Debug panel toggle**: `window.__chromeyummToggle()` (Ctrl+D)
- **Hide/show cursor**: `document.documentElement.style.setProperty('cursor','none','important')` in output mode

## Hotkeys

| Key | Action |
|---|---|
| Ctrl+M | Toggle interactive ↔ output mode |
| Ctrl+Shift+M | Reset display windows |
| Ctrl+R | Reload content (`location.reload()`) |
| Ctrl+F | Toggle alwaysOnTop |
| Ctrl+D | Toggle debug panel / overlay |
| Escape | Quit cleanly |

## Interactive Mode and OSR Input Constraints

Chromeyumm uses CEF's **Off-Screen Rendering (OSR)** mode. This is the architectural choice that enables the GPU texture pipeline — but it has a direct cost for interactivity.

In normal CEF windowed mode, CEF owns an `HWND` and the OS delivers mouse/keyboard events to it directly. Everything works as in a normal browser with no extra wiring.

In OSR mode, there is no visible CEF window. The OS has nowhere to send input, so Chromeyumm must intercept events on its own windows and manually forward them into CEF's API. This forwarding is incomplete by design — it covers the common cases (mouse move/click/scroll, keyboard, double-click, focus, mouse-leave) but it is not a full browser input stack. Things that are missing or degraded:

- Native `<select>` dropdowns (OS popup, not composited into OSR surface)
- Drag-and-drop (requires OLE `IDropTarget` — not implemented)
- IME / CJK text input (requires `WM_IME_*` — not implemented)
- Context menus (popup, same problem as `<select>`)
- Any browser UI that relies on OS-native widget rendering

**Bottom line:** Chromeyumm is built for generative, time-driven visual content — not for interactive UI surfaces. If your content needs rich browser UI (forms, menus, drag-and-drop), it will feel broken compared to a real browser. Use interactive mode for development convenience (reloading, debug panel, parameter tweaking), not as a product surface.

For truly interactive installations (touch walls, visitor-facing screens), the right architecture is to run a separate normal browser or Electron app for the UI layer and route its output into Chromeyumm as a Spout texture.

## Known Limitations

- `loadURL()` ignores its url argument — use `location.reload()` for content reload
- "Black window" in OSR: master HWND is always black — content only exists in the `OnAcceleratedPaint` shared texture. Verify pipeline via staging-texture pixel readback: alpha=0 → unrendered, alpha=FF + black RGB → page content is black.

## Related

- [ARCHITECTURE.md](../ARCHITECTURE.md) — System context diagram
- [BACKEND.md](BACKEND.md) — C++ internals, FFI layer
- [product-specs/spout-input.md](product-specs/spout-input.md) — Spout input feature spec
- [product-specs/multi-window-d3d-output.md](product-specs/multi-window-d3d-output.md) — D3D output feature spec
