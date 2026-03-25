/**
 * Chromeyumm FFI layer — Bun dlopen bindings to libNativeWrapper.dll.
 *
 * Only the symbols actually used by this app are declared here.
 * When upgrading CEF: check for removed/renamed exports in libNativeWrapper.dll
 * using `dumpbin /exports libNativeWrapper.dll` and update accordingly.
 */

import { dlopen, FFIType, JSCallback, CString, type Pointer } from "bun:ffi";
import { join, dirname } from "path";
import { existsSync } from "fs";

// ── DLL load ────────────────────────────────────────────────────────────────

export const native = (() => {
  // When running as a compiled exe, look next to the exe first; fall back to cwd
  // (cwd works for `bun app.js` run from dist/).
  const nextToExe = join(dirname(process.execPath), "libNativeWrapper.dll");
  const dllPath = existsSync(nextToExe) ? nextToExe : join(process.cwd(), "libNativeWrapper.dll");
  try {
    return dlopen(dllPath, {
      // ── Window ──────────────────────────────────────────────────────────
      getWindowStyle: {
        args: [
          FFIType.bool, FFIType.bool, FFIType.bool, FFIType.bool, FFIType.bool,
          FFIType.bool, FFIType.bool, FFIType.bool, FFIType.bool, FFIType.bool,
          FFIType.bool, FFIType.bool,
        ],
        returns: FFIType.u32,
      },
      createWindowWithFrameAndStyleFromWorker: {
        args: [
          FFIType.u32,      // windowId
          FFIType.f64, FFIType.f64,  // x, y
          FFIType.f64, FFIType.f64,  // width, height
          FFIType.u32,      // styleMask
          FFIType.cstring,  // titleBarStyle
          FFIType.bool,     // transparent
          FFIType.function, // closeHandler
          FFIType.function, // moveHandler
          FFIType.function, // resizeHandler
          FFIType.function, // focusHandler
          FFIType.function, // blurHandler
          FFIType.function, // keyHandler
        ],
        returns: FFIType.ptr,
      },
      setWindowTitle:    { args: [FFIType.ptr, FFIType.cstring], returns: FFIType.void },
      showWindow:        { args: [FFIType.ptr], returns: FFIType.void },
      hideWindow:        { args: [FFIType.ptr], returns: FFIType.void },
      closeWindow:       { args: [FFIType.ptr], returns: FFIType.void },
      setWindowAlwaysOnTop: { args: [FFIType.ptr, FFIType.bool], returns: FFIType.void },
      setWindowFullScreen:  { args: [FFIType.ptr, FFIType.bool], returns: FFIType.void },

      // ── Webview ─────────────────────────────────────────────────────────
      initWebview: {
        args: [
          FFIType.u32,      // webviewId
          FFIType.ptr,      // windowPtr
          FFIType.cstring,  // renderer ("cef")
          FFIType.cstring,  // url
          FFIType.f64, FFIType.f64,  // x, y
          FFIType.f64, FFIType.f64,  // width, height
          FFIType.bool,     // autoResize
          FFIType.cstring,  // partition
          FFIType.function, // decideNavigation
          FFIType.function, // webviewEventHandler (unused — pass eventBridgeHandler)
          FFIType.function, // eventBridgeHandler (dom-ready, navigation, etc.)
          FFIType.function, // bunBridgePostmessageHandler (null — no RPC)
          FFIType.function, // internalBridgeHandler (null — no RPC)
          FFIType.cstring,  // electrobunPreloadScript
          FFIType.cstring,  // customPreloadScript
          FFIType.cstring,  // viewsRoot
          FFIType.bool,     // transparent
          FFIType.bool,     // sandbox
        ],
        returns: FFIType.ptr,
      },
      setNextWebviewFlags: {
        args: [FFIType.bool, FFIType.bool], // transparent, passthrough
        returns: FFIType.void,
      },
      setNextWebviewSharedTexture: {
        args: [FFIType.bool],
        returns: FFIType.void,
      },
      evaluateJavaScriptWithNoCompletion: {
        args: [FFIType.ptr, FFIType.cstring],
        returns: FFIType.void,
      },

      // ── D3D output ──────────────────────────────────────────────────────
      startD3DOutput:  { args: [FFIType.u32], returns: FFIType.bool },
      addD3DOutputSlot: {
        args: [FFIType.u32, FFIType.u32, FFIType.i32, FFIType.i32, FFIType.i32, FFIType.i32],
        returns: FFIType.bool,
      },
      stopD3DOutput: { args: [FFIType.u32], returns: FFIType.void },

      // ── Spout sender ────────────────────────────────────────────────────
      startSpoutSender: { args: [FFIType.u32, FFIType.cstring], returns: FFIType.bool },
      stopSpoutSender:  { args: [FFIType.u32], returns: FFIType.void },

      // ── Spout receiver ──────────────────────────────────────────────────
      startSpoutReceiver:       { args: [FFIType.cstring], returns: FFIType.u32 },
      stopSpoutReceiver:        { args: [FFIType.u32], returns: FFIType.void },
      getSpoutReceiverMappingName: { args: [FFIType.u32], returns: FFIType.cstring },
      getSpoutReceiverSeq:      { args: [FFIType.u32], returns: FFIType.i32 },

      // ── NativeDisplayWindow ─────────────────────────────────────────────
      createNativeDisplayWindow: {
        args: [FFIType.u32, FFIType.i32, FFIType.i32, FFIType.i32, FFIType.i32],
        returns: FFIType.ptr,
      },
      destroyNativeDisplayWindow:          { args: [FFIType.u32], returns: FFIType.void },
      setNativeDisplayWindowVisible:       { args: [FFIType.u32, FFIType.bool], returns: FFIType.void },
      setNativeDisplayWindowAlwaysOnTop:   { args: [FFIType.u32, FFIType.bool], returns: FFIType.void },
      setNativeDisplayWindowFullScreen:    { args: [FFIType.u32, FFIType.bool], returns: FFIType.void },

      // ── Global shortcuts ─────────────────────────────────────────────────
      setGlobalShortcutCallback: { args: [FFIType.function], returns: FFIType.void },
      registerGlobalShortcut:    { args: [FFIType.cstring], returns: FFIType.bool },
      unregisterGlobalShortcut:  { args: [FFIType.cstring], returns: FFIType.bool },
      unregisterAllGlobalShortcuts: { args: [], returns: FFIType.void },

      // ── Event loop ──────────────────────────────────────────────────────
      // Starts the Windows message loop + CEF on a background thread.
      // Must be called before any window/webview creation.
      initEventLoop: { args: [FFIType.cstring, FFIType.cstring, FFIType.cstring], returns: FFIType.void },

      // ── Screen ───────────────────────────────────────────────────────────
      getAllDisplays: { args: [], returns: FFIType.cstring },
    });
  } catch (err) {
    console.error("[chromeyumm] FATAL: failed to load", dllPath);
    console.error((err as Error).message);
    console.error("Ensure the DLL is built (bun build.ts) and CEF vendor files are present.");
    process.exit(1);
  }
})();

// ── Helpers ──────────────────────────────────────────────────────────────────

/** Convert a JS string to a NUL-terminated C string pointer (Bun FFI). */
export function cs(s: string): Uint8Array {
  return Buffer.from(s + "\0");
}

// ── Event routing ────────────────────────────────────────────────────────────

type EventHandler = (detail: string) => void;
const webviewListeners = new Map<number, Map<string, EventHandler[]>>();

/** Register a per-webview event listener. Called by Webview.on(). */
export function addWebviewListener(webviewId: number, event: string, handler: EventHandler) {
  if (!webviewListeners.has(webviewId)) webviewListeners.set(webviewId, new Map());
  const events = webviewListeners.get(webviewId)!;
  if (!events.has(event)) events.set(event, []);
  events.get(event)!.push(handler);
}

function dispatchWebviewEvent(webviewId: number, eventName: string, detail: string) {
  const handlers = webviewListeners.get(webviewId)?.get(eventName);
  if (handlers) for (const h of handlers) h(detail);
}

/** CEF → TS event bridge (dom-ready, navigation, etc.) */
export const eventBridgeCallback = new JSCallback(
  (_id: number, msg: number) => {
    try {
      const raw = new CString(msg as unknown as Pointer).toString().trim();
      if (!raw || raw[0] !== "{") return;
      const json = JSON.parse(raw);
      if (json.id === "webviewEvent") {
        const { id, eventName, detail } = json.payload as { id: number; eventName: string; detail: string };
        dispatchWebviewEvent(id, eventName, detail ?? "");
      }
    } catch (e) {
      console.error("[chromeyumm] eventBridgeCallback error:", e);
    }
  },
  { args: [FFIType.u32, FFIType.cstring], returns: FFIType.void, threadsafe: true },
);

/** Null-stub callback for unused bridges (bunBridge, internalBridge). */
export const nullCallback = new JSCallback(
  (_id: number, _msg: number) => {},
  { args: [FFIType.u32, FFIType.cstring], returns: FFIType.void, threadsafe: true },
);

/** Navigation decision — always allow. Override if you need URL filtering. */
export const decideNavCallback = new JSCallback(
  (_id: number, _url: number) => true,
  { args: [FFIType.u32, FFIType.cstring], returns: FFIType.bool, threadsafe: true },
);

// Unused window event stubs (required by createWindowWithFrameAndStyleFromWorker signature)
export const windowNoopCallback = new JSCallback(
  (_id: number) => {},
  { args: [FFIType.u32], returns: FFIType.void, threadsafe: true },
);
export const windowKeyCallback = new JSCallback(
  (_id: number, _key: number) => {},
  { args: [FFIType.u32, FFIType.cstring], returns: FFIType.void, threadsafe: true },
);
