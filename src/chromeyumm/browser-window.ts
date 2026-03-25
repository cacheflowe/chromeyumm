/**
 * BrowserWindow — a top-level OS window with an embedded CEF webview.
 */

import { type Pointer } from "bun:ffi";
import { native, cs, windowNoopCallback, windowKeyCallback } from "./ffi.ts";
import { Webview } from "./webview.ts";

let nextWindowId = 1;
const windowRegistry = new Map<number, BrowserWindow>();

export interface BrowserWindowOptions {
  title?: string;
  url: string;
  frame: { x: number; y: number; width: number; height: number };
  titleBarStyle?: "hidden" | "default";
  /** true = CEF OSR (offscreen rendering) mode — required for Spout/D3D output */
  spout?: boolean;
  viewsRoot?: string;
}

export class BrowserWindow {
  readonly id: number;
  ptr!: Pointer;
  readonly webview: Webview;
  /** webviewId (alias for webview.id) — used by Spout/D3D FFI calls */
  get webviewId(): number { return this.webview.id; }

  constructor(opts: BrowserWindowOptions) {
    this.id = nextWindowId++;
    windowRegistry.set(this.id, this);

    const {
      title = "Chromeyumm",
      url,
      frame: { x, y, width, height },
      titleBarStyle = "hidden",
      spout = false,
      viewsRoot = process.cwd(),
    } = opts;

    // Build a borderless, hidden-titlebar style mask via getWindowStyle
    const styleMask = native.symbols.getWindowStyle(
      false, // Borderless
      true,  // Titled
      true,  // Closable
      false, // Miniaturizable
      false, // Resizable
      false, // UnifiedTitleAndToolbar
      false, // FullScreen
      titleBarStyle === "hidden", // FullSizeContentView
      false, // UtilityWindow
      false, // DocModalWindow
      false, // NonactivatingPanel
      false, // HUDWindow
    ) as number;

    this.ptr = native.symbols.createWindowWithFrameAndStyleFromWorker(
      this.id,
      x, y, width, height,
      styleMask,
      cs(titleBarStyle),
      false, // transparent
      windowNoopCallback, // close
      windowNoopCallback, // move
      windowNoopCallback, // resize
      windowNoopCallback, // focus
      windowNoopCallback, // blur
      windowKeyCallback,  // key
    ) as Pointer;

    if (!this.ptr) throw new Error(`[chromeyumm] createWindow failed (id=${this.id})`);
    native.symbols.setWindowTitle(this.ptr, cs(title));

    // Arm shared-texture (OSR) mode before webview creation when spout=true.
    if (spout) native.symbols.setNextWebviewSharedTexture(true);

    this.webview = new Webview(this.ptr, this.id, url, width, height, viewsRoot);
  }

  show() { native.symbols.showWindow(this.ptr); }
  hide() { native.symbols.hideWindow(this.ptr); }

  setAlwaysOnTop(value: boolean) {
    native.symbols.setWindowAlwaysOnTop(this.ptr, value);
  }

  setFullScreen(value: boolean) {
    native.symbols.setWindowFullScreen(this.ptr, value);
  }

  // ── Spout output ──────────────────────────────────────────────────────────

  startSpout(senderName: string): boolean {
    return !!native.symbols.startSpoutSender(this.webviewId, cs(senderName));
  }

  stopSpout() {
    native.symbols.stopSpoutSender(this.webviewId);
  }

  // ── D3D output ────────────────────────────────────────────────────────────

  startD3DOutput(): boolean {
    return !!native.symbols.startD3DOutput(this.webviewId);
  }

  addD3DOutputSlot(displayWindowId: number, srcX: number, srcY: number, srcW: number, srcH: number): boolean {
    return !!native.symbols.addD3DOutputSlot(this.webviewId, displayWindowId, srcX, srcY, srcW, srcH);
  }

  stopD3DOutput() {
    native.symbols.stopD3DOutput(this.webviewId);
  }

  static getById(id: number): BrowserWindow | undefined {
    return windowRegistry.get(id);
  }
}
