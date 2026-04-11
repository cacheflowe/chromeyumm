#!/usr/bin/env bun
/**
 * smoke-test.ts — Verify DLL loads and all FFI symbols resolve.
 *
 * Usage:
 *   bun scripts/smoke-test.ts          (run from project root)
 *   bun scripts/smoke-test.ts --dll path/to/libNativeWrapper.dll
 *
 * Exit code 0 = all checks pass, 1 = failures detected.
 */

import { dlopen, FFIType } from "bun:ffi";
import { join } from "path";
import { existsSync } from "fs";
import { $ } from "bun";

// ── Expected FFI symbols ────────────────────────────────────────────────────
// Mirrors src/chromeyumm/ffi.ts — kept as a flat list for easy diffing.
const EXPECTED_SYMBOLS: Record<string, { args: number[]; returns: number }> = {
  // Window
  getWindowStyle: { args: new Array(12).fill(FFIType.bool), returns: FFIType.u32 },
  createWindowWithFrameAndStyleFromWorker: {
    args: [
      FFIType.u32,
      FFIType.f64,
      FFIType.f64,
      FFIType.f64,
      FFIType.f64,
      FFIType.u32,
      FFIType.cstring,
      FFIType.bool,
      FFIType.function,
      FFIType.function,
      FFIType.function,
      FFIType.function,
      FFIType.function,
      FFIType.function,
    ],
    returns: FFIType.ptr,
  },
  setWindowTitle: { args: [FFIType.ptr, FFIType.cstring], returns: FFIType.void },
  showWindow: { args: [FFIType.ptr], returns: FFIType.void },
  hideWindow: { args: [FFIType.ptr], returns: FFIType.void },
  closeWindow: { args: [FFIType.ptr], returns: FFIType.void },
  setWindowAlwaysOnTop: { args: [FFIType.ptr, FFIType.bool], returns: FFIType.void },
  setWindowFullScreen: { args: [FFIType.ptr, FFIType.bool], returns: FFIType.void },

  // Webview
  initWebview: {
    args: [
      FFIType.u32,
      FFIType.ptr,
      FFIType.cstring,
      FFIType.cstring,
      FFIType.f64,
      FFIType.f64,
      FFIType.f64,
      FFIType.f64,
      FFIType.bool,
      FFIType.cstring,
      FFIType.function,
      FFIType.function,
      FFIType.function,
      FFIType.function,
      FFIType.function,
      FFIType.cstring,
      FFIType.cstring,
      FFIType.cstring,
      FFIType.bool,
      FFIType.bool,
    ],
    returns: FFIType.ptr,
  },
  setNextWebviewFlags: { args: [FFIType.bool, FFIType.bool], returns: FFIType.void },
  setNextWebviewSharedTexture: { args: [FFIType.bool], returns: FFIType.void },
  evaluateJavaScriptWithNoCompletion: { args: [FFIType.ptr, FFIType.cstring], returns: FFIType.void },

  // D3D output
  startD3DOutput: { args: [FFIType.u32], returns: FFIType.bool },
  addD3DOutputSlot: {
    args: [FFIType.u32, FFIType.u32, FFIType.i32, FFIType.i32, FFIType.i32, FFIType.i32],
    returns: FFIType.bool,
  },
  stopD3DOutput: { args: [FFIType.u32], returns: FFIType.void },

  // Spout sender
  startSpoutSender: { args: [FFIType.u32, FFIType.cstring], returns: FFIType.bool },
  stopSpoutSender: { args: [FFIType.u32], returns: FFIType.void },

  // Spout receiver
  startSpoutReceiver: { args: [FFIType.cstring], returns: FFIType.u32 },
  stopSpoutReceiver: { args: [FFIType.u32], returns: FFIType.void },
  getSpoutReceiverMappingName: { args: [FFIType.u32], returns: FFIType.cstring },
  getSpoutReceiverSeq: { args: [FFIType.u32], returns: FFIType.i32 },

  // NativeDisplayWindow
  createNativeDisplayWindow: {
    args: [FFIType.u32, FFIType.i32, FFIType.i32, FFIType.i32, FFIType.i32],
    returns: FFIType.ptr,
  },
  destroyNativeDisplayWindow: { args: [FFIType.u32], returns: FFIType.void },
  setNativeDisplayWindowVisible: { args: [FFIType.u32, FFIType.bool], returns: FFIType.void },
  setNativeDisplayWindowAlwaysOnTop: { args: [FFIType.u32, FFIType.bool], returns: FFIType.void },
  setNativeDisplayWindowFullScreen: { args: [FFIType.u32, FFIType.bool], returns: FFIType.void },
  enableDisplayWindowInput: {
    args: [FFIType.u32, FFIType.u32, FFIType.i32, FFIType.i32, FFIType.i32, FFIType.i32, FFIType.bool, FFIType.bool],
    returns: FFIType.void,
  },

  // Global shortcuts
  setGlobalShortcutCallback: { args: [FFIType.function], returns: FFIType.void },
  registerGlobalShortcut: { args: [FFIType.cstring], returns: FFIType.bool },
  unregisterGlobalShortcut: { args: [FFIType.cstring], returns: FFIType.bool },
  unregisterAllGlobalShortcuts: { args: [], returns: FFIType.void },

  // Event loop
  initEventLoop: { args: [FFIType.cstring, FFIType.cstring, FFIType.cstring], returns: FFIType.void },

  // Screen
  getAllDisplays: { args: [], returns: FFIType.cstring },
};

// ── Locate DLL ──────────────────────────────────────────────────────────────
const dllArg =
  process.argv.find((a) => a.startsWith("--dll="))?.split("=")[1] ??
  (process.argv.indexOf("--dll") >= 0 ? process.argv[process.argv.indexOf("--dll") + 1] : undefined);

const candidates = dllArg
  ? [dllArg]
  : [
      join(process.cwd(), "dist", "libNativeWrapper.dll"),
      join(process.cwd(), "native", "build", "libNativeWrapper.dll"),
    ];

const dllPath = candidates.find((p) => existsSync(p));
if (!dllPath) {
  console.error("FAIL: DLL not found. Searched:", candidates.join(", "));
  process.exit(1);
}

// ── Check 1: DLL loads ──────────────────────────────────────────────────────
let pass = 0;
let fail = 0;

function ok(msg: string) {
  pass++;
  console.log(`  ✓ ${msg}`);
}
function bad(msg: string) {
  fail++;
  console.error(`  ✗ ${msg}`);
}

console.log(`\nSmoke test — ${dllPath}\n`);
console.log("── DLL load ──");

let lib: ReturnType<typeof dlopen> | null = null;
try {
  lib = dlopen(dllPath, EXPECTED_SYMBOLS);
  ok("dlopen succeeded");
} catch (err) {
  bad(`dlopen failed: ${(err as Error).message}`);
  process.exit(1);
}

// ── Check 2: All symbols resolve ────────────────────────────────────────────
console.log("\n── FFI symbols ──");
const symbolNames = Object.keys(EXPECTED_SYMBOLS);
for (const name of symbolNames) {
  if (typeof (lib.symbols as any)[name] === "function" || (lib.symbols as any)[name] !== undefined) {
    ok(name);
  } else {
    bad(`${name} — not resolved`);
  }
}

// ── Check 3: dumpbin exports vs expected (optional — only if dumpbin available) ──
console.log("\n── DLL exports vs FFI bindings ──");
try {
  const result = await $`dumpbin /exports ${dllPath}`.quiet();
  const output = result.stdout.toString();
  // Parse exported function names (dumpbin format: ordinal hint RVA name)
  const exportedNames = new Set(
    output
      .split("\n")
      .map((line) => line.trim().split(/\s+/).pop()!)
      .filter((name) => name && /^[a-zA-Z_]/.test(name) && !name.includes(".")),
  );

  // Check for symbols we expect but DLL doesn't export
  for (const name of symbolNames) {
    if (!exportedNames.has(name)) {
      bad(`FFI expects "${name}" but DLL does not export it`);
    }
  }

  // Informational: DLL exports we don't bind (not a failure)
  const unbound = [...exportedNames].filter((e) => !symbolNames.includes(e));
  if (unbound.length > 0) {
    console.log(`\n  ℹ ${unbound.length} DLL exports not in FFI bindings (not a problem):`);
    for (const name of unbound.sort()) {
      console.log(`    · ${name}`);
    }
  }
} catch {
  console.log("  ℹ dumpbin not available — skipping export cross-check");
}

// ── Summary ─────────────────────────────────────────────────────────────────
console.log(`\n── Summary: ${pass} passed, ${fail} failed ──\n`);
process.exit(fail > 0 ? 1 : 0);
