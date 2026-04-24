# Decision: OSR with Shared Texture Enabled

## Status
Accepted

## Context
Need to capture the rendered web content as a GPU texture for distribution to multiple displays (via D3D11 blit) and/or sharing with external tools (via Spout).

## Options Considered

### Window capture (PrintWindow / BitBlt)
- CPU readback of window pixels
- Phase 1 result: Intel 75%, NVIDIA 60% at 60fps — unacceptable

### Windows Graphics Capture (WGC)
- Desktop Duplication API / WinRT capture
- Phase 2c result: Intel 65%, NVIDIA 30% at 45-50fps — better but still CPU-bound

### CEF OSR with `shared_texture_enabled=1`
- CEF's offscreen rendering with `OnAcceleratedPaint` callback
- Delivers a DXGI NT shared texture handle directly — entire pipeline stays on GPU
- Phase 2d result: Intel 0%, NVIDIA 15% at 60fps

## Decision
OSR with `shared_texture_enabled=1`. This is the only path that delivers a GPU-native texture handle without CPU readback.

## Implementation Requirements
- ANGLE backend must be d3d11 (`use-angle=d3d11`). Other backends (gl, vulkan) crash in OSR mode.
- `SetAsWindowless` + `shared_texture_enabled=1` in `CefBrowserSettings`
- `windowless_frame_rate=60` for 60fps target
- `in-process-gpu: true` required on Optimus hardware (GPU subprocess crashes without it)

## Consequences
- Master HWND is always black (CEF doesn't paint to it in OSR mode) — content only in shared texture
- Requires careful D3D11 device initialization order — device must be created before `OnAcceleratedPaint` first fires
- `loadURL()` doesn't work — must use `executeJavascript("location.reload()")`
- **Input forwarding is manual and incomplete.** In windowed CEF, the OS delivers input directly to CEF's HWND — everything works. In OSR there is no HWND, so the host application must intercept and re-inject every input event via `CefBrowserHost::Send*Event`. Common events are covered, but native popups (`<select>`, context menus), drag-and-drop, and IME are absent. Interactive mode is a development convenience, not a production UI surface.

## Related
- [cef-over-electron.md](cef-over-electron.md) — Why CEF
- [single-master-gpu-blit.md](single-master-gpu-blit.md) — How the texture is distributed
