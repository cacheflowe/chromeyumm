/** Global keyboard shortcuts via the CEF accelerator API. */

import { JSCallback, FFIType, CString, type Pointer } from "bun:ffi";
import { native, cs } from "./ffi.ts";

const handlers = new Map<string, () => void>();

const shortcutCallback = new JSCallback(
  (acceleratorPtr: number) => {
    const accelerator = new CString(acceleratorPtr as unknown as Pointer).toString();
    handlers.get(accelerator)?.();
  },
  { args: [FFIType.cstring], returns: FFIType.void, threadsafe: true },
);

native.symbols.setGlobalShortcutCallback(shortcutCallback);

export const GlobalShortcut = {
  register(accelerator: string, callback: () => void): boolean {
    if (handlers.has(accelerator)) return false;
    const ok = !!native.symbols.registerGlobalShortcut(cs(accelerator));
    if (ok) handlers.set(accelerator, callback);
    return ok;
  },

  unregister(accelerator: string): boolean {
    const ok = !!native.symbols.unregisterGlobalShortcut(cs(accelerator));
    if (ok) handlers.delete(accelerator);
    return ok;
  },

  unregisterAll() {
    native.symbols.unregisterAllGlobalShortcuts();
    handlers.clear();
  },
};
