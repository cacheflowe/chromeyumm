# Backend — C++ DLL, FFI, and Native Layer

## Overview

The "backend" is `libNativeWrapper.dll` — a single C++ DLL compiled from `native/cef-wrapper.cpp` (~9k lines) plus the renderer helper `native/cef-helper.cpp`. This DLL is loaded by Bun via `dlopen` in `src/chromeyumm/ffi.ts`.

## C++ Source Files

| File | Lines | Role |
|---|---|---|
| `native/cef-wrapper.cpp` | ~9k | Main DLL: CEF initialization, window management, D3D11 output, Spout I/O, event loop |
| `native/cef-helper.cpp` | ~300 | CEF renderer process: V8 bindings for Spout input, navigation fixes |
| `native/shared/*.h` | ~16 files | Shared headers: callbacks, config, parsers, accelerator handling, etc. |
| `native/frame-output/` | — | Frame transport module: protocol outputs (DDP, Spout sender), staging readback, per-webview session management |

## Key C++ Structures

### D3D Output

| Struct | Purpose |
|---|---|
| `D3DOutputSlot` | HWND, `IDXGISwapChain1*`, source x/y/w/h for one display window |
| `D3DOutputState` | `vector<D3DOutputSlot>` — the slot list for one webview |
| `g_d3dOutputStates` | Map of webviewId → `D3DOutputState` |

### Frame Output Host

| Struct | Purpose |
|---|---|
| `FrameOutputHostState` | D3D11 device/context, DXGI swap chain, active flag — shared by all frame-output transports (Spout sender, DDP) and D3D display output |
| `g_frameOutputHosts` | Map of webviewId → `FrameOutputHostState` |
| `g_nextWebviewSharedTexture` | Flag set by `setNextWebviewSharedTexture` before webview creation |

**D3D device ownership**: All frame-output transports (Spout sender, DDP) and D3D multi-window output share the single `FrameOutputHostState.d3dDevice`, initialized once by `ensureFrameTransportHostRuntime`. This is required because `OpenSharedResource1` in `OnAcceleratedPaint` needs a device created through that specific path — independently created devices fail with `DXGI_ERROR_DEVICE_REMOVED` on the first call. `notifyFrameTransportOutputsStopped` releases the device only when no transport outputs remain active.

### Frame Transport Module

The `native/frame-output/` module owns all transport protocol outputs. It is separate from `cef-wrapper.cpp` and communicates back via the `HostServices` callback struct (registered at `initEventLoop` time).

| Component | Role |
|---|---|
| `FrameTransportRuntime` | Singleton; maps webviewId → `TransportSession` |
| `TransportSession` | Per-webview: `FrameOutputManager`, staging texture, DDP raw pointer cache |
| `FrameOutputManager` | Owns `vector<unique_ptr<IOutputProtocol>>`; dispatches GPU frames and CPU (staging readback) frames |
| `DdpOutput` | CPU-path protocol: dirty-row detection, UDP packetization |
| `SpoutOutput` | GPU-path protocol: `SpoutDX::SendTexture`, no CPU round-trip |

### Spout Input

| Struct | Purpose |
|---|---|
| `SpoutInputState` | Win32 named file mapping, NVIDIA D3D11 device, staging texture, receiver thread |
| Shared memory | `SpoutFrame_<id>`: 16-byte header + BGRA pixels |

### NativeDisplayWindow

| Item | Purpose |
|---|---|
| `g_windowIdToHwnd` | Map populated in `createWindowWithFrameAndStyleFromWorker` |
| `DisplayWindowProc` | Custom WndProc: blocks cursor, blocks close events, forwards mouse input |
| `g_displayWindows` | Active display window tracking |
| `DisplayWindowInputState` | Per-HWND struct: webviewId, source rect, cached `CefRefPtr<CefBrowser>`, showCursor flag |
| `g_displayWindowInputMap` | Map of HWND → `DisplayWindowInputState` (empty when input forwarding is off) |

### Mouse Input — Two Modes

There are two ways mouse input reaches CEF:

**1. Interactive mode (Ctrl+M)** — the master window is shown, display windows are hidden. The master window's `ContainerView` WndProc handles `WM_LBUTTON*` etc. directly and routes them to `CEFView::HandleWindowMessage` → `OSRWindow::HandleMouseEvent` → `CefBrowserHost::SendMouseClickEvent`. Mouse coords are 1:1 (master window = virtual canvas size). `SetCapture`/`ReleaseCapture` ensure button-up events aren't lost. `CefBrowserHost::SetFocus(true)` is called on button-down to keep CEF's internal focus state correct.

**2. Display window input (`interactiveWindows: true`)** — display windows stay visible and borderless (visitor-safe). `DisplayWindowProc` handles `WM_MOUSEMOVE`, `WM_LBUTTON*`, `WM_RBUTTON*`, `WM_MBUTTON*`, `WM_MOUSEWHEEL`. It translates NDW-local pixel coords → virtual canvas coords using the source rect stored in `DisplayWindowInputState`, then calls `CefBrowserHost::SendMouseClickEvent`/`SendMouseMoveEvent`/`SendMouseWheelEvent`. The browser pointer is cached at enable time (via `findBrowserByWebviewId`) to avoid a per-event map scan. Cursor visibility is controlled per-window via `WM_SETCURSOR`.

**Enabling display window input**: call `enableDisplayWindowInput(displayWindowId, webviewId, srcX, srcY, srcW, srcH, enable, showCursor)`. The TS layer calls this automatically for each display window when `interactiveWindows: true` is in `display-config.json`.

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

### DDP Output
- `startDdpOutput(webviewId, controllerAddress, port, destinationId, pixelStart, srcX, srcY, srcW, srcH, zigZagRows, flipH, flipV, rotate, clearExisting)` → bool
- `stopDdpOutput(webviewId)`
- `getDdpOutputStats(webviewId)` → JSON cstring

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
- `enableDisplayWindowInput(displayId, webviewId, srcX, srcY, srcW, srcH, enable, showCursor)`

### Global Shortcuts
- `setGlobalShortcutCallback(callback)`
- `registerGlobalShortcut(accelerator)` → bool
- `unregisterGlobalShortcut(accelerator)` → bool
- `unregisterAllGlobalShortcuts()`

### Screen
- `getAllDisplays()` → JSON cstring

## Event Routing (C++ → TypeScript)

Two callback channels deliver events from C++ to the TS layer:

```
1. webviewEventCallback (3-arg: webviewId, eventType, eventData)
   └── C++ WebviewEventHandler fires: did-navigate, will-navigate, new-window-open
   └── dispatches to webviewListeners.get(webviewId)?.get(eventName)

2. eventBridgeCallback (2-arg: webviewId, JSON message)
   └── CEF renderer → browser process IPC (EventBridgeMessage)
   └── payload: { id: "webviewEvent", payload: { id, eventName, detail } }
```

Main event: `did-navigate` (used by master webview to inject scripts after page load).

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

- **App icon**: `getAppIcon()` loads `app.ico` from the exe directory once (cached), `applyAppIcon(hwnd)` sends `WM_SETICON` (big + small) to any window. Applied to both `BasicWindowClass` and `NativeDisplayWindowClass` windows at creation time.
- **`DO_NOT_WAIT` inner loop** in Spout receiver: the Map retry must be inside the inner loop. Calling `ReceiveTexture()` again in the outer loop resets the sequence — texture data is lost.
- **NVIDIA adapter selection**: Spout receiver uses explicit DXGI enumeration (VendorId `0x10DE`) to avoid Intel/NVIDIA cross-adapter PCIe penalty on Optimus.
- **Source box clamping**: D3D output queries `D3D11_TEXTURE2D_DESC` on first frame and clamps coordinates to texture dims to avoid invalid `CopySubresourceRegion` calls.
- **ANGLE backend**: Always `use-angle=d3d11`. Other backends crash in OSR mode.
- **`disable-features=VizDisplayCompositor`**: Removed — crashes GPU process in CEF 145+. Do not add back.

## Related

- [DEVELOPMENT.md](DEVELOPMENT.md) — C++ internals, GPU findings, project history
- [ARCHITECTURE.md](ARCHITECTURE.md) — System context and data flow
- [FRONTEND.md](FRONTEND.md) — Browser-side architecture
- [references/cef-upgrade.md](references/cef-upgrade.md) — CEF upgrade procedure
- [references/ffi-patterns.md](references/ffi-patterns.md) — FFI coding patterns
- [references/global-shortcuts.md](references/global-shortcuts.md) — Global shortcut design, WH_KEYBOARD_LL rationale, adding new shortcuts
