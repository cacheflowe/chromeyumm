/**
 * chromeyumm — minimal CEF wrapper framework.
 *
 * Drop-in replacement for the subset of electrobun/bun used by this app.
 * Import from here in app code:
 *   import { BrowserWindow, NativeDisplayWindow, Screen, GlobalShortcut, SpoutReceiver } from "chromeyumm";
 */

export { BrowserWindow }            from "./browser-window.ts";
export type { BrowserWindowOptions } from "./browser-window.ts";

export { NativeDisplayWindow }            from "./native-display-window.ts";
export type { NativeDisplayWindowOptions } from "./native-display-window.ts";

export { GlobalShortcut }  from "./shortcut.ts";
export { Screen }          from "./screen.ts";
export type { Display, Rectangle } from "./screen.ts";

export { SpoutReceiver }   from "./spout.ts";
