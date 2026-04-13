# Global Shortcuts — Design and Implementation

How keyboard shortcuts work in chromeyumm, why the current approach was chosen,
and what to know when adding new shortcuts.

## Architecture: WH_KEYBOARD_LL

Global shortcuts use a Win32 **low-level keyboard hook** (`WH_KEYBOARD_LL`) installed
via `SetWindowsHookEx`. The hook runs on a dedicated background thread (`hotkeyMessageLoop`)
started when `setGlobalShortcutCallback` is first called.

```
hotkeyMessageLoop thread
  └── SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc)
        └── called for every keydown in the system
              ├── foreground PID check — skip if not our process
              ├── skip bare modifier keys
              ├── check g_shortcuts for vk + modifier match
              └── match → call g_globalShortcutCallback, return 1 (swallow)
                  no match → CallNextHookEx (pass through)
```

Registered shortcuts are stored in `g_shortcuts` (`std::vector<ShortcutEntry>`),
protected by `g_shortcutMutex`. No OS registration step — shortcuts are purely
in-process; the hook is the single interception point.

## Why Not RegisterHotKey?

`RegisterHotKey` was the original implementation. It has a fundamental flaw for
this app: **it requires the app to have a focusable window to receive `WM_HOTKEY`
messages, and it consumes the keypress system-wide regardless of which app is
focused**.

We tried a suspend/resume approach: unregister hotkeys on focus loss, re-register
on focus gain. This broke in output mode (D3D multi-window output) because:

- The master window is hidden (`SW_HIDE`) in output mode.
- Native Display Windows (NDWs) had `WS_EX_NOACTIVATE` to prevent them from
  stealing foreground focus.
- With no focusable chromeyumm window, `resumeHotkeys()` could never fire after
  the first `suspendHotkeys()`. Result: shortcuts permanently dead after first
  focus loss. Some shortcuts (`Ctrl+R`) appeared to work via CEF's built-in
  reload, masking the real breakage.

`WH_KEYBOARD_LL` avoids all of this: focus awareness is **per-keypress**, not
a state machine. There is nothing to get stuck.

## Focus Check

The hook only fires the callback when a chromeyumm window owns the foreground:

```cpp
HWND fgHwnd = GetForegroundWindow();
DWORD fgPid = 0;
if (fgHwnd) GetWindowThreadProcessId(fgHwnd, &fgPid);
if (fgPid != GetCurrentProcessId())
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
```

This means:
- **Interactive mode** (master window visible): shortcuts fire only when
  chromeyumm is focused. Switching to VS Code or any other app silences them.
- **Output mode** (NDWs showing, master hidden): clicking an NDW makes it the
  foreground window (NDWs do **not** have `WS_EX_NOACTIVATE` — that flag was
  removed when we switched to `WH_KEYBOARD_LL`). Once an NDW is foreground,
  shortcuts work.

> **Do not add `WS_EX_NOACTIVATE` back to NDW creation.** It was only ever
> needed to avoid triggering the old suspend/resume focus hook.  With
> `WH_KEYBOARD_LL` it has no benefit and breaks Alt+Tab visibility.

## Adding a New Shortcut

Shortcuts are registered from TypeScript via `registerGlobalShortcut`:

```typescript
native.symbols.registerGlobalShortcut(cs("Ctrl+D"));
```

Accelerator string format: `[Ctrl+][Shift+][Alt+][Win+]<Key>`  
Key names are resolved in `getVirtualKeyCode()` in `cef-wrapper.cpp`.

To add a new key name (e.g. a function key or symbol not yet handled):

1. Find `getVirtualKeyCode` in `cef-wrapper.cpp`.
2. Add a new `if (key == "F13") return VK_F13;` entry.
3. Add the modifier if needed in `parseModifiers`.

The shortcut fires `g_globalShortcutCallback(accelerator)` with the original
accelerator string. The TS side receives it in the callback passed to
`setGlobalShortcutCallback` and dispatches from there.

## CString Pointer Handling

The shortcut callback receives a `const char*` from C++ via a `threadsafe: true`
JSCallback. Bun delivers this as a **raw pointer** (number), not a JS string.
The callback in `shortcut.ts` must use explicit `CString` wrapping:

```typescript
import { CString, type Pointer } from "bun:ffi";

const shortcutCallback = new JSCallback(
  (acceleratorPtr: number) => {
    const accelerator = new CString(acceleratorPtr as unknown as Pointer).toString();
    handlers.get(accelerator)?.();
  },
  { args: [FFIType.cstring], returns: FFIType.void, threadsafe: true },
);
```

> **Do not** change the parameter type to `string` or use `typeof x === "string"`
> guards. This silently breaks — the pointer becomes a number string like
> `"140234567890"` that never matches any accelerator. See
> [ffi-patterns.md](ffi-patterns.md#critical-cstring-handling-in-threadsafe-jscallbacks)
> for the full explanation.

## Shutdown

`hotkeyMessageLoop` exits its `GetMessage` loop when `g_hotkeyThreadRunning`
is set to false and a message is posted to unblock it. `UnhookWindowsHookEx`
is called before the thread exits. There is no explicit shutdown call in the
current app lifecycle — the process exit cleans up the hook automatically.
If a clean shutdown path is ever needed, post `WM_QUIT` to the hook thread.

## Related

- `native/cef-wrapper.cpp` — `LowLevelKeyboardProc`, `hotkeyMessageLoop`,
  `registerGlobalShortcut`, `unregisterGlobalShortcut`
- `src/chromeyumm/ffi.ts` — FFI declarations for all shortcut symbols
- `src/app/index.ts` — where shortcuts are registered at startup
- [ffi-patterns.md](ffi-patterns.md) — general FFI conventions
