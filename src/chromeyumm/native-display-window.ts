/**
 * NativeDisplayWindow — a bare Win32 HWND driven by D3D GPU blit.
 * No DWM thumbnails — content is set by BrowserWindow.addD3DOutputSlot().
 */

import { native } from "./ffi.ts";

let nextId = 1;

export interface NativeDisplayWindowOptions {
  frame: { x: number; y: number; width: number; height: number };
  alwaysOnTop?: boolean;
  fullscreen?: boolean;
}

export class NativeDisplayWindow {
  readonly id: number;

  constructor(opts: NativeDisplayWindowOptions) {
    this.id = nextId++;

    native.symbols.createNativeDisplayWindow(
      this.id,
      Math.round(opts.frame.x),
      Math.round(opts.frame.y),
      Math.round(opts.frame.width),
      Math.round(opts.frame.height),
    );

    if (opts.alwaysOnTop) this.setAlwaysOnTop(true);
    if (opts.fullscreen)  this.setFullScreen(true);
  }

  setVisible(value: boolean) {
    native.symbols.setNativeDisplayWindowVisible(this.id, value);
  }

  setAlwaysOnTop(value: boolean) {
    native.symbols.setNativeDisplayWindowAlwaysOnTop(this.id, value);
  }

  setFullScreen(value: boolean) {
    native.symbols.setNativeDisplayWindowFullScreen(this.id, value);
  }

  destroy() {
    native.symbols.destroyNativeDisplayWindow(this.id);
  }
}
