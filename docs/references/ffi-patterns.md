# FFI Patterns

Patterns and conventions for the Bun FFI layer in `src/chromeyumm/ffi.ts`.

## String Conversion

Use `cs()` (not `toCString()`):
```typescript
import { cs } from "../chromeyumm/ffi.ts";
native.symbols.setWindowTitle(ptr, cs("My Title"));
```

`cs()` returns a `Uint8Array` with a NUL terminator — Bun FFI reads it as a C string.

## Adding a New FFI Symbol

1. **C++ side**: Add `extern "C" __declspec(dllexport)` function in `cef-wrapper.cpp`
2. **TS side**: Add declaration in `src/chromeyumm/ffi.ts` under the `dlopen(...)` call:
   ```typescript
   myNewFunction: {
     args: [FFIType.u32, FFIType.cstring],
     returns: FFIType.bool,
   },
   ```
3. **Use**: `native.symbols.myNewFunction(42, cs("hello"))`
4. **Verify**: `dumpbin /exports dist/libNativeWrapper.dll`

## Type Mapping

| C++ Type | FFI Type | Notes |
|---|---|---|
| `int`, `uint32_t` | `FFIType.u32` or `FFIType.i32` | |
| `double` | `FFIType.f64` | Used for window coordinates |
| `bool` | `FFIType.bool` | |
| `const char*` | `FFIType.cstring` | Use `cs()` to create |
| `void*`, `HWND` | `FFIType.ptr` | Returns `Pointer` type |
| `void` | `FFIType.void` | |
| callback function | `FFIType.function` | Pass `JSCallback` or `nullCallback` |

## Callbacks

For event routing, pass a `JSCallback`:
```typescript
// 3-arg native event callback (C++ WebviewEventHandler: webviewId, eventType, eventData)
const webviewEventCallback = new JSCallback(
  (webviewId: number, type: number, detail: number) => { /* handle event */ },
  { args: [FFIType.u32, FFIType.cstring, FFIType.cstring], returns: FFIType.void, threadsafe: true }
);

// 2-arg renderer process IPC callback (JSON messages from CEF renderer)
const eventBridgeCallback = new JSCallback(
  (id: number, msg: number) => { /* parse JSON payload */ },
  { args: [FFIType.u32, FFIType.cstring], returns: FFIType.void, threadsafe: true }
);
```

For unused bridge handlers, pass a no-op JSCallback (`nullCallback`).

## Patterns

- **No RPC**: All calls are direct `native.symbols.*` — no request/response serialization
- **No async FFI**: All calls are synchronous (acceptable for this use case)
- **Event routing**: Two channels — `webviewEventCallback` for C++ native events (did-navigate, will-navigate), `eventBridgeCallback` for renderer process IPC (JSON messages)
- **Guard on load failure**: `ffi.ts` catches `dlopen` errors and exits with a clear message

## Common Mistakes

- Forgetting to NUL-terminate strings (use `cs()`, not `Buffer.from(s)`)
- Passing wrong pointer type (window ptr vs webview ptr — they're different)
- Calling window/webview functions outside the event loop thread (must use `dispatch_sync` in C++)

## Related

- [docs/BACKEND.md](../BACKEND.md) — Full FFI symbol map
- [docs/design-docs/direct-ffi.md](../design-docs/direct-ffi.md) — Why direct FFI
