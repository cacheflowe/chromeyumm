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
 *   dist/views/                 — bundled HTML/JS views
 *   dist/*.dll / *.bin / ...    — CEF runtime files (copied from native/vendor/cef/Release/)
 */

import { existsSync, mkdirSync, copyFileSync, readdirSync, statSync } from "fs";
import { writeFileSync } from "fs";
import { join, basename } from "path";
import { $ } from "bun";

const ROOT = import.meta.dir;
const NATIVE_DIR = join(ROOT, "native");
const DIST_DIR = join(ROOT, "dist");
const VIEWS_SRC = join(ROOT, "src", "views");
const VIEWS_DIST = join(DIST_DIR, "views");

const skipNative = process.argv.includes("--skip-native");
const isDev = process.argv.includes("--dev");

// ---------------------------------------------------------------------------
// MSVC helpers
// ---------------------------------------------------------------------------

let vcvarsallPath = "";

async function findMsvc() {
  const vswhere = join(
    process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)",
    "Microsoft Visual Studio", "Installer", "vswhere.exe",
  );
  if (!existsSync(vswhere)) { console.log("vswhere not found — using PATH cl/link"); return; }

  const result = await $`powershell -command "& '${vswhere}' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`.quiet();
  if (result.exitCode !== 0) { console.log("MSVC not found via vswhere — using PATH cl/link"); return; }

  const vsPath = result.stdout.toString().trim();
  const candidate = join(vsPath, "VC", "Auxiliary", "Build", "vcvarsall.bat");
  if (existsSync(candidate)) {
    vcvarsallPath = candidate;
    console.log(`✓ MSVC: ${candidate}`);
  }
}

async function runMsvc(command: string) {
  if (!vcvarsallPath) {
    return await $`${command}`;
  }
  const bat = join(ROOT, "_build_tmp.bat");
  writeFileSync(bat, `@echo off\ncall "${vcvarsallPath}" x64 >nul\n${command}`);
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
  if (skipNative) { console.log("Skipping native build (--skip-native)"); return; }

  console.log("\n── Native DLL ──────────────────────────────────────────");
  await findMsvc();

  mkdirSync(join(NATIVE_DIR, "build"), { recursive: true });

  const cefInclude    = join(NATIVE_DIR, "vendor", "cef");
  const cefLib        = join(NATIVE_DIR, "vendor", "cef", "Release", "libcef.lib");
  const cefWrapper    = join(NATIVE_DIR, "vendor", "cef", "build", "libcef_dll_wrapper", "Release", "libcef_dll_wrapper.lib");
  const webview2Inc   = join(NATIVE_DIR, "vendor", "webview2", "Microsoft.Web.WebView2", "build", "native", "include");
  const webview2Lib   = join(NATIVE_DIR, "vendor", "webview2", "Microsoft.Web.WebView2", "build", "native", "x64", "WebView2LoaderStatic.lib");
  const spoutLib      = join(NATIVE_DIR, "vendor", "spout", "MT", "lib", "SpoutDX_static.lib");
  const spoutArg      = existsSync(spoutLib) ? `"${spoutLib}"` : "";

  if (!existsSync(cefLib)) throw new Error(`CEF lib not found: ${cefLib}\nSee native/README.md for vendor setup.`);
  if (spoutArg) console.log("SpoutDX found — Spout output enabled.");
  else          console.log("SpoutDX not found — building without Spout output.");

  // WGPU is optional — gate on vendor presence (matches electrobun pattern)
  const wgpuIncDir = join(NATIVE_DIR, "vendor", "wgpu", "win-x64", "include");
  const wgpuFlag   = existsSync(wgpuIncDir) ? `/I"${wgpuIncDir}" /DELECTROBUN_HAS_WGPU` : "";

  const obj = join(NATIVE_DIR, "build", "cef-wrapper.obj");
  const dll = join(NATIVE_DIR, "build", "libNativeWrapper.dll");

  await runMsvc(
    `cl /c /EHsc /std:c++20 /DNOMINMAX /MT` +
    ` /I"${webview2Inc}" /I"${cefInclude}" ${wgpuFlag}` +
    ` /D_USRDLL /D_WINDLL` +
    ` /Fo"${obj}" "${join(NATIVE_DIR, "cef-wrapper.cpp")}"`,
  );

  await runMsvc(
    `link /DLL /OUT:"${dll}"` +
    ` user32.lib ole32.lib shell32.lib shlwapi.lib advapi32.lib dcomp.lib d2d1.lib` +
    ` dwmapi.lib d3d11.lib dxgi.lib kernel32.lib comctl32.lib delayimp.lib libcmt.lib` +
    ` "${webview2Lib}" "${cefLib}" "${cefWrapper}" /DELAYLOAD:libcef.dll` +
    ` ${spoutArg} "${obj}"`,
  );

  console.log(`✓ DLL: ${dll}`);
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
    alias: { "chromeyumm": join(ROOT, "src", "chromeyumm", "index.ts") },
  });

  if (!result.success) {
    for (const msg of result.logs) console.error(msg);
    throw new Error("TypeScript bundle failed");
  }
  console.log(`✓ Bundle: ${join(DIST_DIR, "app.js")}`);
}

function copyDir(src: string, dest: string) {
  mkdirSync(dest, { recursive: true });
  for (const entry of readdirSync(src)) {
    const srcPath  = join(src,  entry);
    const destPath = join(dest, entry);
    if (statSync(srcPath).isDirectory()) copyDir(srcPath, destPath);
    else copyFileSync(srcPath, destPath);
  }
}

async function copyRuntime() {
  console.log("\n── Copy CEF runtime ────────────────────────────────────");
  mkdirSync(DIST_DIR, { recursive: true });

  // Copy built DLL
  const builtDll = join(NATIVE_DIR, "build", "libNativeWrapper.dll");
  if (!skipNative && existsSync(builtDll)) {
    copyFileSync(builtDll, join(DIST_DIR, "libNativeWrapper.dll"));
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

  // Copy CEF Resources
  const cefResources = join(NATIVE_DIR, "vendor", "cef", "Resources");
  if (existsSync(cefResources)) {
    copyDir(cefResources, DIST_DIR);
    console.log(`✓ CEF resources copied`);
  }

  // Copy views
  if (existsSync(VIEWS_SRC)) {
    copyDir(VIEWS_SRC, VIEWS_DIST);
    console.log(`✓ Views copied`);
  }

  // Copy display-config.json if not already in dist
  const configSrc = join(ROOT, "display-config.json");
  if (existsSync(configSrc)) {
    copyFileSync(configSrc, join(DIST_DIR, "display-config.json"));
  }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

console.log(`\nChromeyumm build  [${isDev ? "dev" : "release"}]`);
console.log("─".repeat(50));

await buildNative();
await bundleTs();
await copyRuntime();

console.log("\n✓ Build complete →", DIST_DIR);
