/**
 * Chromeyumm build script
 *
 * Usage:
 *   bun build.ts          — full rebuild (compile DLL + bundle TS + copy runtime)
 *   bun build.ts --dev    — same, but skips release optimisations
 *   bun build.ts --skip-native  — only rebundle TS (no C++ compile, for fast TS-only changes)
 *
 * Outputs to dist/:
 *   dist/libNativeWrapper.dll   — compiled C++ DLL
 *   dist/app.js                 — bundled TypeScript entry point
 *   dist/*.dll / *.bin / ...    — CEF runtime files (copied from native/vendor/cef/Release/)
 */

import { existsSync, mkdirSync, copyFileSync, readdirSync, statSync } from "fs";
import { writeFileSync } from "fs";
import { join, basename } from "path";
import { $ } from "bun";

const ROOT = import.meta.dir;
const NATIVE_DIR = join(ROOT, "native");
const DIST_DIR = join(ROOT, "dist");

const skipNative = process.argv.includes("--skip-native");
const isDev = process.argv.includes("--dev");

// ---------------------------------------------------------------------------
// MSVC helpers
// ---------------------------------------------------------------------------

let vcvarsallPath = "";
let _batCounter = 0;
let _useSccache = false;

async function findMsvc() {
  const vswhere = join(
    process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)",
    "Microsoft Visual Studio",
    "Installer",
    "vswhere.exe",
  );
  if (!existsSync(vswhere)) {
    console.log("vswhere not found — using PATH cl/link");
    return;
  }

  const result =
    await $`powershell -command "& '${vswhere}' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`.quiet();
  if (result.exitCode !== 0) {
    console.log("MSVC not found via vswhere — using PATH cl/link");
    return;
  }

  const vsPath = result.stdout.toString().trim();
  const candidate = join(vsPath, "VC", "Auxiliary", "Build", "vcvarsall.bat");
  if (existsSync(candidate)) {
    vcvarsallPath = candidate;
    console.log(`✓ MSVC: ${candidate}`);
  }

  try {
    await $`sccache --version`.quiet();
    _useSccache = true;
    console.log("✓ sccache: found — C++ compile steps will be cached");
  } catch {
    // sccache not installed; compile normally
  }
}

async function runMsvc(command: string) {
  // Prefix compile-only steps with sccache when available. Link steps (starting
  // with "link ") are not cached — sccache only supports compilation.
  const isCompile = command.trimStart().startsWith("cl ");
  const cmd = _useSccache && isCompile ? command.replace(/^(\s*)cl /, "$1sccache cl ") : command;

  if (!vcvarsallPath) {
    return await $`${cmd}`;
  }
  // Each invocation gets its own bat file so parallel calls don't overwrite each other.
  const bat = join(ROOT, `_build_tmp_${_batCounter++}.bat`);
  writeFileSync(bat, `@echo off\ncall "${vcvarsallPath}" x64 >nul\n${cmd}`);
  try {
    return await $`cmd /c "${bat}"`;
  } finally {
    await $`rm -f "${bat}"`.catch(() => {});
  }
}

// ---------------------------------------------------------------------------
// Build steps
// ---------------------------------------------------------------------------

async function buildNative() {
  if (skipNative) {
    console.log("Skipping native build (--skip-native)");
    return;
  }

  console.log("\n── Native DLL ──────────────────────────────────────────");
  await findMsvc();

  mkdirSync(join(NATIVE_DIR, "build"), { recursive: true });

  const cefInclude = join(NATIVE_DIR, "vendor", "cef");
  const cefLib = join(NATIVE_DIR, "vendor", "cef", "Release", "libcef.lib");
  const cefWrapper = join(
    NATIVE_DIR,
    "vendor",
    "cef",
    "build",
    "libcef_dll_wrapper",
    "Release",
    "libcef_dll_wrapper.lib",
  );
  const spoutLib = join(NATIVE_DIR, "vendor", "spout", "MT", "lib", "SpoutDX_static.lib");

  if (!existsSync(cefLib)) throw new Error(`CEF lib not found: ${cefLib}\nSee native/README.md for vendor setup.`);
  if (!existsSync(spoutLib)) throw new Error(`Spout lib not found: ${spoutLib}\nRun: bun scripts/setup-vendors.ts`);

  const obj = join(NATIVE_DIR, "build", "cef-wrapper.obj");
  const frameTransportRuntimeObj = join(NATIVE_DIR, "build", "frame-transport-runtime.obj");
  const frameTransportExportsObj = join(NATIVE_DIR, "build", "frame-transport-exports.obj");
  const frameOutputManagerObj = join(NATIVE_DIR, "build", "frame-output-manager.obj");
  const ddpOutputObj = join(NATIVE_DIR, "build", "frame-output-ddp.obj");
  const spoutOutputObj = join(NATIVE_DIR, "build", "frame-output-spout.obj");
  const dll = join(NATIVE_DIR, "build", "libNativeWrapper.dll");
  const helperObj = join(NATIVE_DIR, "build", "cef-helper.obj");
  const helperExe = join(NATIVE_DIR, "build", "chromeyumm Helper.exe");

  // Compile DLL — all translation units are independent, build in parallel.
  await Promise.all([
    runMsvc(
      `cl /c /EHsc /std:c++20 /DNOMINMAX /MT` +
        ` /I"${cefInclude}"` +
        ` /D_USRDLL /D_WINDLL` +
        ` /Fo"${obj}" "${join(NATIVE_DIR, "cef-wrapper.cpp")}"`,
    ),
    runMsvc(
      `cl /c /EHsc /std:c++20 /DNOMINMAX /MT` +
        ` /I"${cefInclude}"` +
        ` /Fo"${frameTransportRuntimeObj}" "${join(NATIVE_DIR, "frame-output", "frame_transport_runtime.cpp")}"`,
    ),
    runMsvc(
      `cl /c /EHsc /std:c++20 /DNOMINMAX /MT` +
        ` /I"${cefInclude}"` +
        ` /Fo"${frameTransportExportsObj}" "${join(NATIVE_DIR, "frame-output", "frame_transport_exports.cpp")}"`,
    ),
    runMsvc(
      `cl /c /EHsc /std:c++20 /DNOMINMAX /MT` +
        ` /I"${cefInclude}"` +
        ` /Fo"${frameOutputManagerObj}" "${join(NATIVE_DIR, "frame-output", "core", "frame_output_manager.cpp")}"`,
    ),
    runMsvc(
      `cl /c /EHsc /std:c++20 /DNOMINMAX /MT` +
        ` /I"${cefInclude}"` +
        ` /Fo"${ddpOutputObj}" "${join(NATIVE_DIR, "frame-output", "protocols", "ddp", "ddp_output.cpp")}"`,
    ),
    runMsvc(
      `cl /c /EHsc /std:c++20 /DNOMINMAX /MT` +
        ` /I"${cefInclude}"` +
        ` /I"${NATIVE_DIR}"` +
        ` /I"${join(NATIVE_DIR, "vendor", "spout", "include", "SpoutDX")}"` +
        ` /I"${join(NATIVE_DIR, "vendor", "spout", "include", "SpoutGL")}"` +
        ` /Fo"${spoutOutputObj}" "${join(NATIVE_DIR, "frame-output", "protocols", "spout", "spout_output.cpp")}"`,
    ),
  ]);

  await runMsvc(
    `link /DLL /OUT:"${dll}"` +
      ` user32.lib ole32.lib shell32.lib shlwapi.lib advapi32.lib dcomp.lib d2d1.lib` +
      ` dwmapi.lib d3d11.lib dxgi.lib kernel32.lib comctl32.lib delayimp.lib libcmt.lib` +
      ` "${cefLib}" "${cefWrapper}" /DELAYLOAD:libcef.dll` +
      ` "${spoutLib}" "${obj}" "${frameTransportRuntimeObj}" "${frameTransportExportsObj}" "${frameOutputManagerObj}" "${ddpOutputObj}" "${spoutOutputObj}"`,
  );
  console.log(`✓ DLL: ${dll}`);

  // Compile CEF subprocess helper exe
  // /O1 works around MSVC 19.43 C1001 ICE in string_view with /Od + C++20.
  console.log("\n── CEF helper exe ──────────────────────────────────────");
  await runMsvc(
    `cl /c /EHsc /std:c++20 /O1 /DNOMINMAX /MT` +
      ` /I"${cefInclude}"` +
      ` /Fo"${helperObj}" "${join(NATIVE_DIR, "cef-helper.cpp")}"`,
  );
  await runMsvc(
    `link /SUBSYSTEM:WINDOWS /OUT:"${helperExe}"` + ` user32.lib "${cefLib}" "${cefWrapper}" "${helperObj}"`,
  );
  console.log(`✓ Helper: ${helperExe}`);
}

async function compileExe() {
  console.log("\n── Compile chromeyumm.exe ──────────────────────────────");
  mkdirSync(DIST_DIR, { recursive: true });
  const outExe = join(DIST_DIR, "chromeyumm.exe");
  const result = await Bun.build({
    entrypoints: [join(ROOT, "src", "app", "index.ts")],
    outdir: join(ROOT, "_exe_tmp"),
    target: "bun",
    naming: "app.js",
    minify: !isDev,
    alias: { chromeyumm: join(ROOT, "src", "chromeyumm", "index.ts") },
  });
  if (!result.success) {
    for (const msg of result.logs) console.error(msg);
    throw new Error("Exe bundle step failed");
  }
  const bundled = join(ROOT, "_exe_tmp", "app.js");
  const compile = await $`bun build --compile --outfile="${outExe}" "${bundled}"`.quiet();
  if (compile.exitCode !== 0) {
    console.error(compile.stderr.toString());
    throw new Error("bun build --compile failed");
  }
  // Clean up temp bundle
  await $`rm -rf "${join(ROOT, "_exe_tmp")}"`.catch(() => {});
  console.log(`✓ Exe: ${outExe}`);
}

async function bundleTs() {
  console.log("\n── TypeScript bundle ───────────────────────────────────");
  mkdirSync(DIST_DIR, { recursive: true });

  const result = await Bun.build({
    entrypoints: [join(ROOT, "src", "app", "index.ts")],
    outdir: DIST_DIR,
    target: "bun",
    naming: "app.js",
    minify: !isDev,
    sourcemap: isDev ? "inline" : "none",
    // Resolve the "chromeyumm" bare import to our local framework
    alias: { chromeyumm: join(ROOT, "src", "chromeyumm", "index.ts") },
  });

  if (!result.success) {
    for (const msg of result.logs) console.error(msg);
    throw new Error("TypeScript bundle failed");
  }
  console.log(`✓ Bundle: ${join(DIST_DIR, "app.js")}`);

  // Bundle debug-inject.js — auto-injects <debug-panel> into any page.
  // This is a browser-target bundle (debug-panel.js inlined)
  // that the app evaluates via executeJavascript on dom-ready.
  const injectResult = await Bun.build({
    entrypoints: [join(ROOT, "src", "components", "inject.js")],
    outdir: DIST_DIR,
    target: "browser",
    naming: "debug-inject.js",
    minify: !isDev,
  });
  if (!injectResult.success) {
    for (const msg of injectResult.logs) console.error(msg);
    throw new Error("Debug inject bundle failed");
  }
  console.log(`✓ Debug inject: ${join(DIST_DIR, "debug-inject.js")}`);
}

function copyDir(src: string, dest: string, filter?: (name: string) => boolean) {
  mkdirSync(dest, { recursive: true });
  for (const entry of readdirSync(src)) {
    const srcPath = join(src, entry);
    const destPath = join(dest, entry);
    if (statSync(srcPath).isDirectory()) copyDir(srcPath, destPath, filter);
    else if (!filter || filter(entry)) copyFileSync(srcPath, destPath);
  }
}

async function copyRuntime() {
  console.log("\n── Copy CEF runtime ────────────────────────────────────");
  mkdirSync(DIST_DIR, { recursive: true });

  // Copy built DLL + helper exe
  const builtDll = join(NATIVE_DIR, "build", "libNativeWrapper.dll");
  const builtHelper = join(NATIVE_DIR, "build", "chromeyumm Helper.exe");
  if (!skipNative) {
    if (existsSync(builtDll)) copyFileSync(builtDll, join(DIST_DIR, "libNativeWrapper.dll"));
    if (existsSync(builtHelper)) copyFileSync(builtHelper, join(DIST_DIR, "chromeyumm Helper.exe"));
  }

  // Copy CEF Release binaries
  const cefRelease = join(NATIVE_DIR, "vendor", "cef", "Release");
  if (existsSync(cefRelease)) {
    for (const f of readdirSync(cefRelease)) {
      const src = join(cefRelease, f);
      if (statSync(src).isFile()) copyFileSync(src, join(DIST_DIR, f));
    }
    console.log(`✓ CEF runtime files copied from ${cefRelease}`);
  }

  // Copy CEF Resources (only en-US locale — no multilanguage support)
  const cefResources = join(NATIVE_DIR, "vendor", "cef", "Resources");
  if (existsSync(cefResources)) {
    copyDir(cefResources, DIST_DIR, (name) => {
      // Keep only en-US locale paks, skip all others
      if (name.endsWith(".pak") && !name.startsWith("en-US")) {
        const isLocale = !name.startsWith("chrome_") && !name.startsWith("resources");
        if (isLocale) return false;
      }
      return true;
    });
    console.log(`✓ CEF resources copied (en-US locale only)`);
  }
  // Copy app icon if present
  const appIcon = join(NATIVE_DIR, "app.ico");
  if (existsSync(appIcon)) {
    copyFileSync(appIcon, join(DIST_DIR, "app.ico"));
    console.log("✓ App icon copied");
  }
}

console.log(`\nChromeyumm build  [${isDev ? "dev" : "release"}]`);
console.log("─".repeat(50));

await buildNative();
await bundleTs();
await compileExe();
await copyRuntime();

console.log("\n✓ Build complete →", DIST_DIR);
