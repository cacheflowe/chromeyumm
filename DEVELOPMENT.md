# Chromeyumm — Development Guide

Architecture decisions, C++ internals, GPU findings, build workflows, and known issues.

---

## Project Origin

Started as an Electrobun fork (a Bun-based CEF wrapper) to create a specialized browser for live visual installations. The key insight was that CEF's Offscreen Rendering (OSR) with `shared_texture_enabled=1` delivers DXGI shared texture handles via `OnAcceleratedPaint` — exactly what Spout needs for GPU→GPU texture sharing, and what a D3D11 multi-window output system needs for zero-overhead canvas splitting.

After proving the architecture through several GPU performance phases (see history below), the project was detached from Electrobun upstream and simplified into a standalone CEF wrapper. The core C++ work lives in `native/cef-wrapper.cpp` (~12k lines) and is now fully owned — no upstream changes to track.

**Why not Electron?** CEF + Bun is lighter (~350MB distributable vs Electron's ~500MB+), faster cold start (~50ms), and the FFI story is cleaner (Bun FFI → DLL vs Node Native Addons). The tradeoff is that you own more of the plumbing — which for this project is an advantage, since the plumbing is the product.

---

## Architecture

### Two operating modes

Mode is set by presence/absence of `"spoutOutput"` in `display-config.json`.

#### Multi-window D3D output (no `"spoutOutput"`) — current default

```
Master BrowserWindow (OSR, use-angle=d3d11, shared_texture_enabled=1)
  └── renders full virtual canvas (any size, no monitor boundary limit)
  └── OnAcceleratedPaint → DXGI NT shared texture handle every frame
  └── CopySubresourceRegion(srcBox) → each NDW's D3D11 swap chain → Present
  └── No DWM thumbnails; no monitor intersection requirement

NativeDisplayWindow × N (lightweight Win32 HWNDs)
  └── D3D11 swap chain per window; content driven by GPU blit from master OSR texture
  └── alwaysOnTop, suppresses cursor and close events
  └── zero CPU per frame — pure GPU CopySubresourceRegion
```

The D3D11 device is initialised by `startD3DOutput()` via the same code path as `startSpoutSender()` — `OpenSharedResource1` in `OnAcceleratedPaint` requires a device initialised through that path to work reliably.

Master is hidden at startup in output mode. Ctrl+M toggles: output (NDWs visible, master hidden) ↔ interactive (NDWs hidden, master shown). Ctrl+Shift+M destroys and recreates all NDWs + D3D output + reloads content — recovers from monitor topology changes.

#### Spout output (`"spoutOutput"` present)

```
Master BrowserWindow (OSR, use-angle=d3d11, shared_texture_enabled=1)
  └── OnAcceleratedPaint → DXGI NT shared texture handle
  └── OpenSharedResource1 → ID3D11Texture2D on SpoutDX NVIDIA device
  └── SpoutDX::SendTexture → GPU→GPU zero-copy to any Spout receiver
  └── No NativeDisplayWindows; no DWM involvement
```

GPU result: **Intel 0%, NVIDIA 15% at 60fps** on an Optimus laptop (RTX 4090 + Iris Xe). Best achievable — no CPU involvement.

#### Spout input (available in both modes)

```
C++ receiver thread (nativeWrapper / cef-wrapper.cpp)
  └── SpoutDX::ReceiveTexture + D3D11 staging readback
  └── memcpy pixels → Win32 named file mapping ("SpoutFrame_<id>")
  └── _InterlockedIncrement(seqPtr) — release barrier on x86

CEF renderer process (cef-helper.cpp, OnContextCreated)
  └── parse ?spoutReceiverId from URL
  └── OpenFileMappingA → tier selection:
      Tier 1: CefV8Value::CreateArrayBuffer → window.__spoutFrameBuffer
              (zero-copy, MapViewOfFile-backed; requires V8 sandbox off)
      Tier 2: SpoutFrameHandler::Execute → window.__spoutGetFrame
              (persistent-buf memcpy per call; V8 sandbox safe)

Browser (spout-input-receiver.ts)
  └── createSpoutInputReceiver() tries tier 1 then tier 2
  └── poll() called each requestAnimationFrame
```

Shared memory layout:
```
[0-3]   seq      — Int32LE; incremented by C++ after pixels written
[4-7]   width    — Uint32LE
[8-11]  height   — Uint32LE
[12-15] reserved
[16+]   BGRA pixels (~31.6 MB at 3840×2160)
```

---

## C++ Internals

### cef-wrapper.cpp (native/cef-wrapper.cpp)

This is `nativeWrapper.cpp` from the Electrobun fork, all additions are preserved.

**Multi-window output**
- `g_windowIdToHwnd` map populated in `createWindowWithFrameAndStyleFromWorker`
- `DisplayWindowProc`, `g_displayWindows` — bare Win32 HWNDs with custom WndProc (blocks cursor, blocks close)
- Exports: `createNativeDisplayWindow`, `destroyNativeDisplayWindow`, `setNativeDisplayWindowAlwaysOnTop`, `setNativeDisplayWindowFullScreen`, `setNativeDisplayWindowVisible`

**D3D output mode (Phase 5)**
- `D3DOutputSlot` struct: HWND, `IDXGISwapChain1*`, srcX/Y/W/H
- `D3DOutputState` struct: D3D11 device/context, active flag, `vector<D3DOutputSlot>`
- `g_d3dOutputStates` map (webviewId → D3DOutputState)
- Extended `OnAcceleratedPaint`: after Spout block, iterates D3D output slots → `GetBuffer(0)` → clamp source box to texture dims → `CopySubresourceRegion` → `Present`
- Source box clamping: queries `D3D11_TEXTURE2D_DESC` first frame, clamps x0/y0/x1/y1, logs first-frame diagnostics and out-of-bounds warnings
- Exports: `startD3DOutput`, `addD3DOutputSlot`, `stopD3DOutput`, `hideWindow`

**Spout output (Phase 2d)**
- `SpoutWindowState` struct: D3D11 device/context, SpoutDX sender, DXGI swap chain, active flag
- `g_nextWebviewSharedTexture` flag + `setNextWebviewSharedTexture` export
- `createCEFView` takes the OSR path (`SetAsWindowless`, `shared_texture_enabled=1`, `windowless_frame_rate=60`) when flag is set
- `OnAcceleratedPaint`: `OpenSharedResource1` → `SpoutDX::SendTexture`
- Exports: `startSpoutSender`, `stopSpoutSender`

**Spout input (Phase 3)**
- `SpoutInputState` struct: Win32 named file mapping, NVIDIA D3D11 device, staging texture, receiver thread
- `spoutReceiverThreadFn`: `ReceiveTexture` → `CopyResource` → `Map(DO_NOT_WAIT)` inner retry loop → single-block `memcpy` → `_InterlockedIncrement`
- NVIDIA adapter: explicit DXGI enumeration (VendorId `0x10DE`) — avoids Intel/NVIDIA cross-adapter PCIe penalty on Optimus
- `DO_NOT_WAIT` inner loop is **critical**: outer loop calling `ReceiveTexture()` again resets seq → texture gone. The retry must be inside the Map loop.
- Exports: `startSpoutReceiver`, `stopSpoutReceiver`, `getSpoutReceiverSeq`, `spoutReadFrame`

**ANGLE backend**
- Always `use-angle=d3d11` — required for OSR `shared_texture_enabled=1`. VIZ OOM crash occurs with `gl` or `vulkan` in OSR mode.
- `disable-features=VizDisplayCompositor` was **removed** — crashes GPU process in CEF 145+.

**WGPU guard** (`#ifdef ELECTROBUN_HAS_WGPU`)
- `#include "dawn/webgpu.h"` and the WGPU shims section are wrapped in `#ifdef ELECTROBUN_HAS_WGPU`
- `build.ts` sets `/DELECTROBUN_HAS_WGPU` only if `native/vendor/wgpu/win-x64/include` exists
- This prevents a build failure when the wgpu vendor is absent (upstream added it unconditionally)

### cef-helper.cpp (native/cef-helper.cpp)

CEF renderer process helper — runs in `bun Helper (Renderer).exe`.

- `SpoutFrameHandler` (`CefV8Handler`): owns `hMap` + `ptr`; destructor cleans up; `Execute` copies into persistent ArrayBuffer, returns `CefV8Value::CreateBool(true)`.
- `MappedMemoryReleaseCallback`: tier-1 cleanup on V8 GC.
- `GetQueryParam`: parses `?key=value` from URL.
- `OnContextCreated`: parses `?spoutReceiverId`, opens named mapping, injects `window.__spoutFrameBuffer` (tier 1, tries `CefV8Value::CreateArrayBuffer` — returns nullptr when V8 sandbox is on) or registers `window.__spoutGetFrame` handler (tier 2).
- `OnBeforeBrowse` fix: skips Ctrl+click detection when `url == frame->GetURL()` (same-URL = reload, not a link click — prevents Ctrl+R from triggering Ctrl+click navigation interception).

---

## GPU / Windows Notes

### GPU preference registry

At startup, `setGpuPreference()` writes `GpuPreference=2` to `HKCU\Software\Microsoft\DirectX\UserGpuPreferences` for all 6 CEF helper executables. Takes effect on the **next launch** — restart once after first run.

Helper exe names (based on the Bun runtime host):
- `bun.exe`, `bun Helper.exe`, `bun Helper (GPU).exe`, `bun Helper (Renderer).exe`, `bun Helper (Alerts).exe`, `bun Helper (Plugin).exe`

If the host binary is renamed, update `helperNames` in `src/app/index.ts`.

### `in-process-gpu` requirement

`in-process-gpu: true` is required on Optimus hardware. Without it the GPU subprocess (`bun Helper (GPU).exe`) crashes with exit 143 at ~30 seconds. With in-process-gpu, the GPU thread runs inside `bun.exe` where `GpuPreference=2` applies.

### GPU performance history

| Phase | Approach | Intel | NVIDIA | fps |
|---|---|---|---|---|
| 1 | PrintWindow (multi-window) | 75% | 60% | 60 |
| 2c | Windows Graphics Capture (Spout output) | 65% | 30% | 45–50 |
| **2d** | **OSR OnAcceleratedPaint (Spout output)** | **0%** | **15%** | **60** |
| **5** | **D3D output (OSR + CopySubresourceRegion)** | **~25%** | **~15%** | **60** |

Multi-window ~25% Intel = DWM compositing overhead for N NDW swap chains on Optimus. Inherent floor. Use Spout output for 0% Intel.

### Spout input GPU cost

| | Intel | NVIDIA |
|---|---|---|
| Multi-window alone | ~25% | ~35% |
| + Spout input | ~35% | ~65% |
| Delta | +10% | +30% |

NVIDIA +30%: `CopyResource` + staging `Map/memcpy` readback. Intel +10%: `texSubImage2D` upload. This is the inherent cost of any CPU-mediated path — two PCIe crossings (NVIDIA readback + WebGL upload) are unavoidable without GPU-direct texture import.

For GPU-direct research findings (why `EGL_ANGLE_d3d_texture_client_buffer` doesn't work cross-device, viable paths via `EGL_ANGLE_device_d3d`, etc.) — see the archived DEVELOPMENT.md in the Electrobun fork or the session memory files.

---

## Build Workflow

### Commands (all from project root)

| Command | What happens |
|---|---|
| `bun build.ts` | Compile C++ DLL + bundle TS + copy CEF runtime to `dist/` |
| `bun build.ts --skip-native` | TS bundle only (no MSVC) — fast, for TS-only changes |
| `bun build.ts --dev` | Same as above but no minification, inline sourcemaps |
| `cd dist && bun app.js` | Run the app |

### What requires a full rebuild

| Changed | Rebuild needed? |
|---|---|
| `native/cef-wrapper.cpp` or `cef-helper.cpp` | Full (`bun build.ts`) |
| Anything in `src/chromeyumm/` | `--skip-native` is fine |
| Anything in `src/app/` | `--skip-native` is fine |
| `display-config.json` only | No rebuild — file is copied to dist/ on each build, or just copy it manually |
| External `contentUrl` (`http://localhost:5173`) | No rebuild |

### Vendor setup (first time / new machine)

See `native/README.md` for full instructions. The short version:

```powershell
# Create junctions to existing vendor dirs (no large file copies)
New-Item -ItemType Junction -Path native/vendor/cef     -Target path/to/cef/vendor
New-Item -ItemType Junction -Path native/vendor/spout   -Target path/to/spout/vendor
New-Item -ItemType Junction -Path native/vendor/webview2 -Target path/to/webview2/vendor

# shared/ junction so #include "../shared/foo.h" resolves from native/
New-Item -ItemType Junction -Path shared -Target native/shared
```

---

## chromeyumm Framework (src/chromeyumm/)

This is the TypeScript layer that replaces `electrobun/bun`. It's intentionally minimal — only the FFI symbols and class wrappers actually used by this app.

**Key design choices vs Electrobun:**
- No `ffi.request` abstraction — everything calls `native.symbols.*` directly
- No cross-platform guards (`process.platform !== "win32"` checks gone)
- No RPC system — `nullCallback` passed for `bunBridgePostmessageHandler` and `internalBridgeHandler`
- No `BuildConfig` — renderer is always `"cef"`, no dynamic config loading
- No event emitter class — per-webview `Map<event, handler[]>` populated by `addWebviewListener()`
- `cs()` helper instead of `toCString()` for FFI string conversion

**Event routing (C++ → TypeScript):**
C++ calls `eventBridgeCallback` (a `JSCallback`) with `{id: "webviewEvent", payload: {id, eventName, detail}}`. The callback dispatches to `webviewListeners.get(webviewId)?.get(eventName)`. `dom-ready` is the main event used (`master.webview.on("dom-ready", ...)`).

**Adding new FFI symbols:**
1. Declare in `src/chromeyumm/ffi.ts` under `native = dlopen(...)`
2. Use via `native.symbols.yourFunction(...)`
3. Check `dumpbin /exports dist/libNativeWrapper.dll` to confirm the export name

---

## Known Issues

- `loadURL()` ignores its url argument — use `location.reload()` for content reload (Ctrl+R uses `executeJavascript("location.reload()")`)
- `views://` URL query strings required the `?` stripping fix in `ElectrobunSchemeHandler::Open` — already applied in `cef-wrapper.cpp`
- GPU process crash at ~30s without `in-process-gpu: true` — already set in `display-config.json`'s chromium flags (via `electrobun.config.ts` originally; now hardcoded in the build config / chromium_flags.h)
- Ctrl+D / debug panel only works when the loaded page includes `debug-panel.js` and registers `window.__ebPanelToggle`. Remote HTTPS pages don't have it — the shortcut fires but the JS function is undefined (silent no-op in browser, logged in console)
- `disable-features=VizDisplayCompositor` removed — caused GPU crash in CEF 145+. Do not add it back.

---

## CEF Version Upgrade Procedure

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
5. Test — watch for:
   - `windowless_frame_rate` setting location changes (check `CefBrowserSettings`)
   - Helper exe naming convention changes
   - New V8 sandbox restrictions affecting Spout input tier 1
   - `OnAcceleratedPaint` / `CefRenderHandler` API changes (rare)

The CEF C++ API surface we use is very stable. Breaking changes are documented in the [CEF changelog](https://bitbucket.org/chromiumembedded/cef/wiki/Home).

---

## Planned Work

- **Strip `cef-wrapper.cpp`** — remove WebView2 fallback path (~800 lines), ASAR reading (~200 lines), WGPU shims (~400 lines, already `#ifdef`'d), update/packaging machinery (~300 lines). Establish clean baseline first, strip section by section with a build test after each.
- **GitHub releases** — write `scripts/release.ts` to tag, package, and push versioned releases (replaces `installation-browser/scripts/release.ts`)
- **Consolidated debug panel** — single Ctrl+D panel with fps, draw calls, canvas size, mouse coords, Spout status, mode info, key reference. Replaces scattered `debugEl` + stats.js + slot overlay.
- **Rename helper processes** — once Bun adds support for custom subprocess names, update from `bun Helper (GPU).exe` to `chromeyumm Helper (GPU).exe` and update `helperNames` in `src/app/index.ts`.
