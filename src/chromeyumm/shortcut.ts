/** Global keyboard shortcuts via the CEF accelerator API. */

import { JSCallback, FFIType } from "bun:ffi";
import { native, cs } from "./ffi.ts";

const handlers = new Map<string, () => void>();

const shortcutCallback = new JSCallback(
  (accelerator: string) => {
    const key = typeof accelerator === "string" ? accelerator : String(accelerator);
    handlers.get(key)?.();
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
