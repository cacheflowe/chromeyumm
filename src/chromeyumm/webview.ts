/**
 * Webview — wraps a single CEF browser instance.
 * Handles JS evaluation and C++ event routing (did-navigate, will-navigate, etc.).
 */

import { type Pointer } from "bun:ffi";
import {
  native,
  cs,
  addWebviewListener,
  addBunBridgeListener,
  webviewEventCallback,
  eventBridgeCallback,
  bunBridgeCallback,
  nullCallback,
  decideNavCallback,
} from "./ffi.ts";

let nextId = 1;
export const webviewRegistry = new Map<number, Webview>();

export class Webview {
  readonly id: number;
  ptr!: Pointer;

  constructor(windowPtr: Pointer, windowId: number, url: string, width: number, height: number) {
    this.id = nextId++;
    webviewRegistry.set(this.id, this);

    // Minimal preload: sets the webview/window IDs and the event bridge reference.
    // The event bridge itself is injected by CEF's OnContextCreated; we just alias it.
    const preload =
      `window.__chromeyummWebviewId=${this.id};` +
      `window.__chromeyummWindowId=${windowId};` +
      `window.__chromeyummEventBridge=window.__chromeyummEventBridge||window.eventBridge;`;

    native.symbols.setNextWebviewFlags(false, false);

    this.ptr = native.symbols.initWebview(
      this.id,
      windowPtr,
      cs("cef"),
      cs(url),
      0,
      0, // x, y (fills window)
      width,
      height,
      true, // autoResize
      cs("persist:default"),
      decideNavCallback,
      webviewEventCallback, // webviewEventHandler (did-navigate, will-navigate, etc.)
      eventBridgeCallback, // eventBridgeHandler
      bunBridgeCallback, // bunBridgePostmessageHandler
      nullCallback, // internalBridgeHandler (no RPC)
      cs(preload),
      cs(""), // customPreloadScript
      cs(""), // viewsRoot (unused)
      false, // transparent
      false, // sandbox
    ) as Pointer;
  }

  executeJavascript(js: string) {
    if (this.ptr) {
      native.symbols.evaluateJavaScriptWithNoCompletion(this.ptr, cs(js));
    }
  }

  toggleDevTools() {
    if (this.ptr) {
      native.symbols.webviewToggleDevTools(this.ptr);
    }
  }

  on(event: string, handler: (detail: string) => void) {
    addWebviewListener(this.id, event, handler);
  }

  onBunBridgeMessage(handler: (message: string) => void) {
    addBunBridgeListener(this.id, handler);
  }
}
