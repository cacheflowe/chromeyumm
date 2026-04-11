# Feature: Hotkeys

## User Story
As a live visual artist, I want keyboard shortcuts to control the app during performances without a UI, so I can toggle modes, reload content, and quit without touching the mouse.

## Implementation
Global shortcuts are registered via `GlobalShortcut.register()` in `src/app/index.ts`, which calls `native.symbols.registerGlobalShortcut()`. The C++ side uses `accelerator_parser.h` to parse accelerator strings and registers Win32 hotkeys via `RegisterHotKey`.

To avoid hijacking key combos from other applications, hotkeys are **suspended when the app loses focus** and **resumed when it regains focus**. This is implemented via a `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` callback on the hotkey message loop thread that calls `UnregisterHotKey` / `RegisterHotKey` on focus transitions.

## Hotkey Map

| Key | Action | Code | Condition |
|---|---|---|---|
| **Ctrl+M** | Toggle interactive ↔ output mode | Shows/hides master window and display windows | D3D output mode only |
| **Ctrl+Shift+M** | Reset display windows | Destroys and recreates all NDWs + D3D output + reloads content | D3D output mode only |
| **Ctrl+R** | Reload content | `executeJavascript("location.reload()")` | Always |
| **Ctrl+F** | Toggle alwaysOnTop | Applies to display windows (D3D) or master (Spout) | Always |
| **Ctrl+D** | Toggle debug panel + overlay | Calls `window.__chromeyummToggle()` | Auto-injected; works on any page |
| **Escape** | Quit cleanly | Stops Spout, destroys display windows, `process.exit(0)` | Always |

## Interactive vs Output Mode (Ctrl+M)

- **Output mode** (default): Display windows visible, master hidden, cursor hidden
- **Interactive mode**: Display windows hidden, master visible, cursor visible
- State injected to page: `window.__chromeyumm = { alwaysOnTop, interactiveMode, display, output, input, hotkeys }`

## Edge Cases
- Ctrl+D works on any page via auto-injection (`dist/debug-inject.js` evaluated on did-navigate)
- Ctrl+M and Ctrl+Shift+M are only registered when D3D output is active (`useD3DOutput` flag) — not needed when Spout-only (master window stays visible)
- `loadURL()` is broken — Ctrl+R uses `location.reload()` instead
- Hotkeys are suspended/resumed on app focus changes via `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`. Resume unconditionally unregisters+re-registers to clear stale OS state.

## Out of Scope
- Custom/configurable hotkeys
- Hotkeys that depend on focus state

## Related
- [../FRONTEND.md](../FRONTEND.md) — Injected scripts and debug panel
- [multi-window-d3d-output.md](multi-window-d3d-output.md) — Display window management
