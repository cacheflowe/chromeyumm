# CEF Upgrade Guide

## Procedure

1. Download new prebuilt from [cef-builds.spotifycdn.com](https://cef-builds.spotifycdn.com/index.html) (Windows 64-bit, Standard Distribution)
2. Extract to `native/vendor/cef/` (or update the junction target)
3. Build `libcef_dll_wrapper.lib`:
   ```
   cd native/vendor/cef
   mkdir build && cd build
   cmake -G "Visual Studio 17 2022" -A x64 ..
   cmake --build . --config Release --target libcef_dll_wrapper
   ```
4. `bun build.ts`
5. Test — watch for the items below

## What to Check After Upgrade

| Area | What to verify |
|---|---|
| `windowless_frame_rate` | Setting location in `CefBrowserSettings` — has moved between CEF versions |
| Helper exe naming | Convention changes (see `helperNames` in `src/app/index.ts`) |
| V8 sandbox restrictions | May break Spout input tier 1 (`CefV8Value::CreateArrayBuffer` with external backing) |
| `OnAcceleratedPaint` | `CefRenderHandler` API changes (rare but possible) |
| `CefBrowserSettings` | New/removed fields |
| Feature flags | `disable-features=VizDisplayCompositor` was removed for CEF 145+ — don't add it back |

## CEF API Surface Used

The project uses a small, stable subset of CEF's C++ API:

- `CefApp`, `CefClient`, `CefBrowserHost`
- `CefRenderHandler::OnAcceleratedPaint` (shared texture path)
- `CefBrowserSettings` (windowless_frame_rate, etc.)
- `CefV8Handler`, `CefV8Value` (renderer process — Spout input bindings)
- `CefCommandLine` (chromium flags)

Breaking changes are documented in the [CEF changelog](https://bitbucket.org/chromiumembedded/cef/wiki/Home).

## Related

- [DEVELOPMENT.md](../../DEVELOPMENT.md) — CEF version upgrade section
- [docs/BACKEND.md](../BACKEND.md) — C++ internals
