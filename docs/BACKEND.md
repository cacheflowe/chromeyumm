# Backend — C++ DLL, FFI, and Native Layer

## Overview

The "backend" is `libNativeWrapper.dll` — a single C++ DLL compiled from `native/cef-wrapper.cpp` (~12k lines) plus the renderer helper `native/cef-helper.cpp`. This DLL is loaded by Bun via `dlopen` in `src/chromeyumm/ffi.ts`.

## C++ Source Files

| File | Lines | Role |
|---|---|---|
| `native/cef-wrapper.cpp` | ~12k | Main DLL: CEF initialization, window management, D3D11 output, Spout I/O, event loop |
| `native/cef-helper.cpp` | ~300 | CEF renderer process: V8 bindings for Spout input, navigation fixes |
| `native/shared/*.h` | ~20 files | Shared headers: callbacks, config, parsers, accelerator handling, etc. |

## Key C++ Structures

### D3D Output

| Struct | Purpose |
|---|---|
| `D3DOutputSlot` | HWND, `IDXGISwapChain1*`, source x/y/w/h for one display window |
| `D3DOutputState` | D3D11 device/context, active flag, `vector<D3DOutputSlot>` |
| `g_d3dOutputStates` | Map of webviewId → `D3DOutputState` |

### Spout Output

| Struct | Purpose |
|---|---|
| `SpoutWindowState` | D3D11 device/context, SpoutDX sender, DXGI swap chain, active flag |
| `g_nextWebviewSharedTexture` | Flag set by `setNextWebviewSharedTexture` before webview creation |

### Spout Input

| Struct | Purpose |
|---|---|
| `SpoutInputState` | Win32 named file mapping, NVIDIA D3D11 device, staging texture, receiver thread |
| Shared memory | `SpoutFrame_<id>`: 16-byte header + BGRA pixels |

### NativeDisplayWindow

| Item | Purpose |
|---|---|
| `g_windowIdToHwnd` | Map populated in `createWindowWithFrameAndStyleFromWorker` |
| `DisplayWindowProc` | Custom WndProc: blocks cursor, blocks close events |
| `g_displayWindows` | Active display window tracking |

## FFI Symbol Map

All FFI symbols are declared in `src/chromeyumm/ffi.ts`. The full export list:

### Event Loop
- `initEventLoop(identifier, name, channel)` — starts Win32 message loop + CEF on background thread

### Window Management
- `getWindowStyle(...)` → `u32` style mask
- `createWindowWithFrameAndStyleFromWorker(...)` → window pointer
- `setWindowTitle`, `showWindow`, `hideWindow`, `closeWindow`
- `setWindowAlwaysOnTop`, `setWindowFullScreen`

### Webview
- `initWebview(...)` → webview pointer
- `setNextWebviewFlags(transparent, passthrough)`
- `setNextWebviewSharedTexture(enabled)`
- `evaluateJavaScriptWithNoCompletion(ptr, script)`

### D3D Output
- `startD3DOutput(webviewId)` → bool
- `addD3DOutputSlot(webviewId, displayId, srcX, srcY, srcW, srcH)` → bool
- `stopD3DOutput(webviewId)`

### Spout Sender
- `startSpoutSender(webviewId, senderName)` → bool
- `stopSpoutSender(webviewId)`

### Spout Receiver
- `startSpoutReceiver(senderName)` → receiverId
- `stopSpoutReceiver(receiverId)`
- `getSpoutReceiverMappingName(receiverId)` → cstring
- `getSpoutReceiverSeq(receiverId)` → i32

### NativeDisplayWindow
- `createNativeDisplayWindow(id, x, y, w, h)` → ptr
- `destroyNativeDisplayWindow(id)`
- `setNativeDisplayWindowVisible(id, visible)`
- `setNativeDisplayWindowAlwaysOnTop(id, enabled)`
- `setNativeDisplayWindowFullScreen(id, enabled)`

### Global Shortcuts
- `setGlobalShortcutCallback(callback)`
- `registerGlobalShortcut(accelerator)` → bool
- `unregisterGlobalShortcut(accelerator)` → bool
- `unregisterAllGlobalShortcuts()`

### Screen
- `getAllDisplays()` → JSON cstring

## Event Routing (C++ → TypeScript)

```
C++ eventBridgeCallback (JSCallback)
  └── payload: { id: "webviewEvent", payload: { id, eventName, detail } }
  └── dispatches to webviewListeners.get(webviewId)?.get(eventName)
```

Main event: `dom-ready` (used by master webview to inject scripts after page load).

## Threading Model

- `initEventLoop` starts the Win32 message loop on a background thread via `CreateThread`
- Blocks until `MainThreadDispatcher` is ready (signalled via `g_eventLoopReadyEvent`)
- All window/webview creation uses `dispatch_sync` — posts `WM_APP` to message window, blocks on `std::future`
- Spout receiver runs on its own thread (`spoutReceiverThreadFn`)

## Adding New FFI Symbols

1. Add the C++ export in `cef-wrapper.cpp` (use `extern "C" __declspec(dllexport)`)
2. Declare in `src/chromeyumm/ffi.ts` under the `dlopen(...)` call
3. Use via `native.symbols.yourFunction(...)`
4. Verify: `dumpbin /exports dist/libNativeWrapper.dll`

## Critical Implementation Notes

- **`DO_NOT_WAIT` inner loop** in Spout receiver: the Map retry must be inside the inner loop. Calling `ReceiveTexture()` again in the outer loop resets the sequence — texture data is lost.
- **NVIDIA adapter selection**: Spout receiver uses explicit DXGI enumeration (VendorId `0x10DE`) to avoid Intel/NVIDIA cross-adapter PCIe penalty on Optimus.
- **Source box clamping**: D3D output queries `D3D11_TEXTURE2D_DESC` on first frame and clamps coordinates to texture dims to avoid invalid `CopySubresourceRegion` calls.
- **ANGLE backend**: Always `use-angle=d3d11`. Other backends crash in OSR mode.
- **`disable-features=VizDisplayCompositor`**: Removed — crashes GPU process in CEF 145+. Do not add back.

## WGPU Guard

`#ifdef ELECTROBUN_HAS_WGPU` wraps Dawn/WGPU includes and shims. `build.ts` sets `/DELECTROBUN_HAS_WGPU` only if `native/vendor/wgpu/win-x64/include` exists. Safe to ignore unless adding WebGPU support.

## Related

- [DEVELOPMENT.md](../DEVELOPMENT.md) — Full C++ internals reference
- [ARCHITECTURE.md](../ARCHITECTURE.md) — System context and data flow
- [FRONTEND.md](FRONTEND.md) — Browser-side architecture
- [references/cef-upgrade.md](references/cef-upgrade.md) — CEF upgrade procedure
- [references/ffi-patterns.md](references/ffi-patterns.md) — FFI coding patterns
