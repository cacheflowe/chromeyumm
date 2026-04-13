# Chromeyumm — Development Guide

Deep C++ internals, GPU findings, project history, and known issues. For commands see [COMMANDS.md](COMMANDS.md); for FFI patterns see [BACKEND.md](BACKEND.md); for build environment setup see [references/cpp-tooling.md](references/cpp-tooling.md).

---

## Project Origin

Started as an Electrobun fork (a Bun-based CEF wrapper) to create a specialized browser for live visual installations. The key insight was that CEF's Offscreen Rendering (OSR) with `shared_texture_enabled=1` delivers DXGI shared texture handles via `OnAcceleratedPaint` — exactly what Spout needs for GPU→GPU texture sharing, and what a D3D11 multi-window output system needs for zero-overhead canvas splitting.

After proving the architecture through several GPU performance phases (see history below), the project was detached from Electrobun upstream and simplified into a standalone CEF wrapper. The core C++ work lives in `native/cef-wrapper.cpp` (~9k lines) and the frame-output module (`native/frame-output/`), both fully owned — no upstream changes to track.

**Why not Electron?** CEF + Bun is lighter (~350MB distributable vs Electron's ~500MB+), faster cold start (~50ms), and the FFI story is cleaner (Bun FFI → DLL vs Node Native Addons). The tradeoff is that you own more of the plumbing — which for this project is an advantage, since the plumbing is the product.

---

## C++ Internals

### cef-wrapper.cpp

**Multi-window output**
- `g_windowIdToHwnd` map populated in `createWindowWithFrameAndStyleFromWorker`
- `DisplayWindowProc`, `g_displayWindows` — bare Win32 HWNDs with custom WndProc (blocks cursor, blocks close)
- Exports: `createNativeDisplayWindow`, `destroyNativeDisplayWindow`, `setNativeDisplayWindowAlwaysOnTop`, `setNativeDisplayWindowFullScreen`, `setNativeDisplayWindowVisible`

**D3D output (Phase 5)**
- `D3DOutputSlot` struct: HWND, `IDXGISwapChain1*`, srcX/Y/W/H
- `D3DOutputState` struct: vector of `D3DOutputSlot`
- `g_d3dOutputStates` map (webviewId → D3DOutputState)
- `OnAcceleratedPaint`: batch-copies all D3D output slots → `Flush()` → `Present(0, ALLOW_TEARING)` (non-blocking, avoids DWM vsync stalls)
- Source box clamping: queries `D3D11_TEXTURE2D_DESC` first frame, clamps x0/y0/x1/y1

**Frame output host (shared D3D device)**
- `FrameOutputHostState` struct: D3D11 device/context, DXGI swap chain, active flag — shared by all frame-output transports (Spout sender, DDP) and D3D display output
- `g_frameOutputHosts` map (webviewId → FrameOutputHostState)
- `ensureFrameTransportHostRuntime(webviewId)` — single D3D init path; called by both D3D output and the frame-output module. `OpenSharedResource1` in `OnAcceleratedPaint` requires a device created through this path — independently created devices fail with `DXGI_ERROR_DEVICE_REMOVED`.
- `notifyFrameTransportOutputsStopped` — releases the device only when no transport outputs remain active
- `getFrameOutputHostDevice` — callback registered with the frame-output module to return the device pointer

**Spout input**
- `SpoutInputState` struct: Win32 named file mapping, NVIDIA D3D11 device, staging texture, receiver thread
- `spoutReceiverThreadFn`: `ReceiveTexture` → `CopyResource` → `Map(DO_NOT_WAIT)` inner retry loop → single-block `memcpy` → `_InterlockedIncrement`
- NVIDIA adapter: explicit DXGI enumeration (VendorId `0x10DE`) — avoids Intel/NVIDIA cross-adapter PCIe penalty on Optimus
- **`DO_NOT_WAIT` inner loop is critical**: outer loop calling `ReceiveTexture()` again resets seq → texture gone. The retry must be inside the Map loop.
- Exports: `startSpoutReceiver`, `stopSpoutReceiver`, `getSpoutReceiverSeq`, `getSpoutReceiverMappingName`

**Event loop bootstrap**
- `initEventLoop(identifier, name, channel)` — spins `startEventLoop` on a background Win32 thread via `CreateThread` and blocks until `MainThreadDispatcher` is ready
- `MainThreadDispatcher::dispatch_sync` posts `WM_APP` and blocks on `std::future` — all window/webview creation must use this path

**ANGLE backend**
- Always `use-angle=d3d11` — required for OSR `shared_texture_enabled=1`. VIZ OOM crash with `gl` or `vulkan` in OSR mode.
- `disable-features=VizDisplayCompositor` was **removed** — crashes GPU process in CEF 145+. Do not add it back.

**WGPU guard** (`#ifdef CHROMEYUMM_HAS_WGPU`)
- `#include "dawn/webgpu.h"` and WGPU shims are wrapped in `#ifdef CHROMEYUMM_HAS_WGPU`
- `build.ts` sets `/DCHROMEYUMM_HAS_WGPU` only if `native/vendor/wgpu/win-x64/include` exists
- Prevents build failure when wgpu vendor is absent

### cef-helper.cpp

CEF renderer process helper — runs in `chromeyumm Helper (Renderer).exe`.

- `SpoutFrameHandler` (`CefV8Handler`): owns `hMap` + `ptr`; destructor cleans up; `Execute` copies into persistent ArrayBuffer
- `MappedMemoryReleaseCallback`: tier-1 cleanup on V8 GC
- `GetQueryParam`: parses `?key=value` from URL
- `OnContextCreated`: parses `?spoutReceiverId`, opens named mapping, injects `window.__spoutFrameBuffer` (tier 1, fails when V8 sandbox is on) or registers `window.__spoutGetFrame` handler (tier 2)
- `OnBeforeBrowse` fix: skips Ctrl+click detection when `url == frame->GetURL()` — prevents Ctrl+R from triggering navigation interception

### frame-output module (`native/frame-output/`)

Transport protocols (Spout sender, DDP) that share a single D3D staging readback per webview. Communicates back to cef-wrapper.cpp via the `HostServices` callback struct registered at `initEventLoop` time.

- `FrameTransportRuntime` — singleton; maps webviewId → `TransportSession`
- `TransportSession` — per-webview: `FrameOutputManager`, staging texture, DDP raw pointer cache
- `FrameOutputManager` — owns `vector<unique_ptr<IOutputProtocol>>`; dispatches GPU/CPU frames
- `DdpOutput` — CPU-path: full-frame comparison (skip unchanged), UDP packetization with keepalive
- `SpoutOutput` — GPU-path: `SpoutDX::SendTexture`, no CPU round-trip
- Exports are in `frame_transport_exports.cpp`: `startSpoutSender`, `stopSpoutSender`, `startDdpOutput`, `stopDdpOutput`, `getDdpOutputStats`

### Spout input shared memory layout

```
[0-3]   seq      — Int32LE; incremented by C++ after pixels written
[4-7]   width    — Uint32LE
[8-11]  height   — Uint32LE
[12-15] reserved
[16+]   BGRA pixels (~31.6 MB at 3840×2160)
```

---

## GPU / Windows Notes

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

### GPU preference registry

At startup, `setGpuPreference()` writes `GpuPreference=2` to `HKCU\Software\Microsoft\DirectX\UserGpuPreferences` for the host and helper executables. Takes effect on the **next launch** — restart once after first run. `helperNames` in `src/app/index.ts` derives these from `process.execPath` so renames are handled automatically.

### `in-process-gpu` requirement

Required on Optimus hardware. Without it the GPU subprocess crashes with exit 143 at ~30 seconds. The GPU thread runs in a subprocess where `GpuPreference=2` doesn't apply — it falls back to Intel iGPU, which lacks the D3D11 features CEF needs for OSR. With `in-process-gpu`, the GPU thread runs inside the main process where the registry preference applies.

---

## Known Issues

See [RELIABILITY.md](RELIABILITY.md) for full failure mode documentation. Additional notes:

- `loadURL()` ignores its url argument — use `executeJavascript("location.reload()")` for content reload
- Ctrl+D / debug panel only works when the loaded page includes `debug-panel.js`. Remote HTTPS pages: shortcut fires but `window.__ebPanelToggle` is undefined — silent no-op in browser, logged in console
- **GPU-direct Spout input** (no CPU readback): `EGL_ANGLE_d3d_texture_client_buffer` doesn't work cross-device on Optimus. Viable paths would require `EGL_ANGLE_device_d3d` or a custom WebGPU import extension. Not currently implemented.

---

## Planned Work

- **Strip `cef-wrapper.cpp` further** — remove ASAR reading (~200 lines), remaining WGPU shims (~400 lines, already `#ifdef`'d). Establish clean baseline first, strip section by section with a build test after each.
- **Rename helper processes** — once Bun adds support for custom subprocess names, add full helper name set to `helperNames` in `src/app/index.ts` for GPU-preference registry writes. Currently only `chromeyumm.exe` and `chromeyumm Helper.exe` are registered.

---

## Related

- [ARCHITECTURE.md](ARCHITECTURE.md) — System context, data flow, domain boundaries
- [BACKEND.md](BACKEND.md) — C++ DLL internals, FFI patterns
- [COMMANDS.md](COMMANDS.md) — All build/run/dev commands
- [references/cpp-tooling.md](references/cpp-tooling.md) — C++ build environment setup
- [references/cef-upgrade.md](references/cef-upgrade.md) — CEF version upgrade procedure
