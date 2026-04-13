/**
 * scripts/setup-vendors.ts — download, extract, and build CEF + Spout vendors.
 *
 * Usage:
 *   bun scripts/setup-vendors.ts                # download + build all vendors
 *   bun scripts/setup-vendors.ts --cef-only     # CEF only
 *   bun scripts/setup-vendors.ts --spout-only   # Spout only
 *   bun scripts/setup-vendors.ts --cef-version 131.3.6+g7e8cdab+chromium-131.0.6778.109
 *   bun scripts/setup-vendors.ts --spout-tag 2.007.014
 *   bun scripts/setup-vendors.ts --latest       # auto-detect + install newest stable CEF
 *   bun scripts/setup-vendors.ts --check-latest # report whether a newer CEF is available
 *
 * What it does:
 *   1. Downloads CEF prebuilt from Spotify CDN (or uses a local archive)
 *   2. Extracts to native/vendor/cef/
 *   3. Builds libcef_dll_wrapper.lib via CMake
 *   4. Downloads Spout2 release from GitHub
 *   5. Extracts to native/vendor/spout/
 *   6. Creates the root shared/ junction
 *
 * On subsequent runs, existing vendor dirs are removed and replaced.
 * Run after cloning the repo or when upgrading CEF/Spout versions.
 */

import { existsSync, mkdirSync, rmSync, writeFileSync, readFileSync, readdirSync, copyFileSync, statSync } from "fs";
import { join, basename } from "path";
import { $ } from "bun";

const ROOT = join(import.meta.dir, "..");
const NATIVE_DIR = join(ROOT, "native");
const VENDOR_DIR = join(NATIVE_DIR, "vendor");

// ── Defaults ────────────────────────────────────────────────────────────────

// CEF: find latest at https://cef-builds.spotifycdn.com/index.html
// Format: "VERSION+HASH+chromium-CHROMIUM_VERSION"
const DEFAULT_CEF_VERSION = "146.0.10+g8219561+chromium-146.0.7680.179";

// Spout2: find releases at https://github.com/leadedge/Spout2/releases
const DEFAULT_SPOUT_TAG = "2.007.014";

// ── CLI parsing ─────────────────────────────────────────────────────────────

const args = process.argv.slice(2);
const cefOnly = args.includes("--cef-only");
const spoutOnly = args.includes("--spout-only");
const useLatest = args.includes("--latest");
const checkLatest = args.includes("--check-latest");

function getArg(flag: string): string | null {
  const idx = args.indexOf(flag);
  return idx !== -1 && idx + 1 < args.length ? args[idx + 1] : null;
}

const localCefArchive = getArg("--cef-archive"); // optional: use a local .tar.bz2

// ── Latest version detection ─────────────────────────────────────────────────

/**
 * Fetch the newest stable Windows 64-bit CEF build from the Spotify CDN index.
 * Versions are listed newest-first; we take the first one that has a
 * "standard" distribution (not minimal-only).
 */
async function fetchLatestCefVersion(): Promise<string> {
  process.stdout.write("  Fetching CEF version index from cef-builds.spotifycdn.com...");
  const resp = await fetch("https://cef-builds.spotifycdn.com/index.json");
  if (!resp.ok) throw new Error(`Failed to fetch CEF index: ${resp.status} ${resp.statusText}`);
  const index = (await resp.json()) as any;
  console.log(" done");

  const platform = index?.windows64;
  if (!platform?.versions?.length) {
    throw new Error("Unexpected CEF index format — check https://cef-builds.spotifycdn.com/index.html manually");
  }

  // Prefer a version that ships a standard distribution
  for (const v of platform.versions) {
    if (v.files?.some((f: any) => f.type === "standard")) return v.cef_version as string;
  }
  return platform.versions[0].cef_version as string;
}

// Handle --check-latest: compare current default with latest and exit
if (checkLatest) {
  console.log("Chromeyumm Vendor Update Check");
  console.log("═".repeat(50));
  const latest = await fetchLatestCefVersion();
  if (latest === DEFAULT_CEF_VERSION) {
    console.log(`  CEF is up to date: ${DEFAULT_CEF_VERSION}`);
  } else {
    console.log(`  Current: ${DEFAULT_CEF_VERSION}`);
    console.log(`  Latest:  ${latest}`);
    console.log(`\n  To upgrade:`);
    console.log(`    bun run upgrade:cef   # auto-fetch, download, build wrapper, rebuild app`);
    console.log(`    bun run build         # then: bun run test && dist/chromeyumm.exe`);
  }
  process.exit(0);
}

// Resolve final CEF version (--latest fetches it; --cef-version or default otherwise)
let resolvedCefVersion: string;
if (useLatest) {
  resolvedCefVersion = await fetchLatestCefVersion();
} else {
  resolvedCefVersion = getArg("--cef-version") ?? DEFAULT_CEF_VERSION;
}

const cefVersion = resolvedCefVersion;
const spoutTag = getArg("--spout-tag") ?? DEFAULT_SPOUT_TAG;

// When --latest is used, persist the fetched version back into this script so
// the default stays current and the change shows up cleanly in git diff.
if (useLatest && cefVersion !== DEFAULT_CEF_VERSION) {
  const scriptPath = import.meta.path;
  const scriptContent = readFileSync(scriptPath, "utf-8");
  const updated = scriptContent.replace(
    /^const DEFAULT_CEF_VERSION = "[^"]+";/m,
    `const DEFAULT_CEF_VERSION = "${cefVersion}";`,
  );
  if (updated !== scriptContent) {
    writeFileSync(scriptPath, updated);
    console.log(`  ✓ Updated DEFAULT_CEF_VERSION → ${cefVersion}`);
  }
}

// ── Helpers ─────────────────────────────────────────────────────────────────

async function downloadFile(url: string, dest: string): Promise<void> {
  console.log(`  Downloading: ${url}`);
  console.log(`  Destination: ${dest}`);

  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`Download failed: ${resp.status} ${resp.statusText}\n  URL: ${url}`);

  const total = parseInt(resp.headers.get("content-length") ?? "0");
  const reader = resp.body!.getReader();
  const chunks: Uint8Array[] = [];
  let received = 0;

  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    received += value.length;
    if (total > 0) {
      const pct = ((received / total) * 100).toFixed(1);
      const mb = (received / 1024 / 1024).toFixed(1);
      process.stdout.write(`\r  ${mb} MB (${pct}%)`);
    }
  }
  console.log();

  const blob = new Blob(chunks);
  await Bun.write(dest, blob);
}

function removeDir(dir: string) {
  if (existsSync(dir)) {
    // Check if it's a junction — remove the junction, not the target
    try {
      const stat = Bun.file(dir);
      // Use rmSync with force for junctions and real dirs alike
      rmSync(dir, { recursive: true, force: true });
    } catch {
      rmSync(dir, { recursive: true, force: true });
    }
  }
}

// ── MSVC helpers (reused from build.ts) ─────────────────────────────────────

let vcvarsallPath = "";

async function findMsvc() {
  const vswhere = join(
    process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)",
    "Microsoft Visual Studio",
    "Installer",
    "vswhere.exe",
  );
  if (!existsSync(vswhere)) {
    console.log("  vswhere not found — using PATH cmake");
    return;
  }

  const result =
    await $`powershell -command "& '${vswhere}' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`.quiet();
  if (result.exitCode !== 0) {
    console.log("  MSVC not found via vswhere — using PATH cmake");
    return;
  }

  const vsPath = result.stdout.toString().trim();
  const candidate = join(vsPath, "VC", "Auxiliary", "Build", "vcvarsall.bat");
  if (existsSync(candidate)) {
    vcvarsallPath = candidate;
  }
}

async function runWithMsvc(command: string) {
  if (!vcvarsallPath) {
    return await $`cmd /c ${command}`;
  }
  const bat = join(ROOT, "_vendor_setup_tmp.bat");
  writeFileSync(bat, `@echo off\ncall "${vcvarsallPath}" x64 >nul\n${command}`);
  try {
    return await $`cmd /c "${bat}"`;
  } finally {
    await $`rm -f "${bat}"`.catch(() => {});
  }
}

// ── CEF setup ───────────────────────────────────────────────────────────────

async function setupCef() {
  console.log("\n══ CEF Setup ══════════════════════════════════════════");
  console.log(`  Version: ${cefVersion}`);

  const cefDir = join(VENDOR_DIR, "cef");
  const tmpDir = join(ROOT, "_vendor_tmp");
  mkdirSync(tmpDir, { recursive: true });

  // 1. Download or use local archive
  let archivePath: string;

  if (localCefArchive) {
    archivePath = localCefArchive;
    console.log(`  Using local archive: ${archivePath}`);
  } else {
    // CEF CDN URL format: cef_binary_VERSION_windows64.tar.bz2
    const cefFilename = `cef_binary_${cefVersion}_windows64.tar.bz2`;
    const cefUrl = `https://cef-builds.spotifycdn.com/${cefFilename}`;
    archivePath = join(tmpDir, cefFilename);

    if (existsSync(archivePath)) {
      console.log(`  Archive already downloaded: ${archivePath}`);
    } else {
      await downloadFile(cefUrl, archivePath);
    }
  }

  // 2. Extract
  console.log("  Extracting (this may take a minute)...");
  const extractDir = join(tmpDir, "cef_extract");
  removeDir(extractDir);
  mkdirSync(extractDir, { recursive: true });

  // tar can handle .tar.bz2 on Windows 10+
  const tarResult = await $`tar -xf "${archivePath}" -C "${extractDir}"`.quiet();
  if (tarResult.exitCode !== 0) {
    throw new Error(
      `Failed to extract CEF archive. Ensure tar supports bz2 (Windows 10+ built-in tar does).\n${tarResult.stderr.toString()}`,
    );
  }

  // Find the extracted directory (name varies with version)
  const extracted = readdirSync(extractDir).find((d) => d.startsWith("cef_binary_"));
  if (!extracted) throw new Error("CEF extraction produced no cef_binary_* directory");
  const cefExtracted = join(extractDir, extracted);

  // 3. Replace vendor dir
  removeDir(cefDir);
  mkdirSync(cefDir, { recursive: true });

  // Copy only what we need (not the full ~1.5GB source tree)
  const toCopy = ["include", "Release", "Resources", "libcef_dll", "cmake", "CMakeLists.txt", "LICENSE.txt"];
  for (const item of toCopy) {
    const src = join(cefExtracted, item);
    if (!existsSync(src)) continue;
    const dest = join(cefDir, item);
    if (statSync(src).isDirectory()) {
      copyDirRecursive(src, dest);
    } else {
      copyFileSync(src, dest);
    }
  }

  // Write version marker
  writeFileSync(join(cefDir, ".cef-version"), cefVersion + "\n");

  // Write generated TS constant so app/index.ts can embed the version at build time
  const cefVersionTs = join(ROOT, "src", "app", "_cef-version.ts");
  writeFileSync(
    cefVersionTs,
    `// Auto-generated by scripts/setup-vendors.ts — do not edit manually.\n` +
      `// Update by running: bun scripts/setup-vendors.ts --cef-only\n` +
      `export const CEF_VERSION = "${cefVersion}";\n`,
  );

  console.log(`  ✓ CEF extracted to ${cefDir}`);

  // 4. Build libcef_dll_wrapper.lib
  console.log("  Building libcef_dll_wrapper.lib...");
  await findMsvc();

  const buildDir = join(cefDir, "build");
  mkdirSync(buildDir, { recursive: true });

  const cmakeGen = await $`cmake -G "Visual Studio 17 2022" -A x64 -S "${cefDir}" -B "${buildDir}"`.quiet();
  if (cmakeGen.exitCode !== 0) {
    console.error(cmakeGen.stderr.toString());
    throw new Error("CMake generation failed. Ensure CMake and VS2022 are installed.");
  }

  const cmakeBuild = await $`cmake --build "${buildDir}" --config Release --target libcef_dll_wrapper`.quiet();
  if (cmakeBuild.exitCode !== 0) {
    console.error(cmakeBuild.stderr.toString());
    throw new Error("CMake build failed for libcef_dll_wrapper");
  }

  const wrapperLib = join(buildDir, "libcef_dll_wrapper", "Release", "libcef_dll_wrapper.lib");
  if (!existsSync(wrapperLib)) throw new Error(`Expected ${wrapperLib} not found after build`);

  console.log(`  ✓ libcef_dll_wrapper.lib built`);

  // 5. Cleanup temp
  removeDir(extractDir);
  // Keep the archive for re-use (it's a big download)
  console.log(`  ✓ CEF ${cefVersion} ready`);
}

// ── Spout setup ─────────────────────────────────────────────────────────────

async function setupSpout() {
  console.log("\n══ Spout Setup ════════════════════════════════════════");
  console.log(`  Tag: ${spoutTag}`);

  const spoutDir = join(VENDOR_DIR, "spout");
  const tmpDir = join(ROOT, "_vendor_tmp");
  mkdirSync(tmpDir, { recursive: true });

  // Spout2 publishes prebuilt SDK binaries as a release asset.
  // URL format: https://github.com/leadedge/Spout2/releases/download/TAG/Spout-SDK-binaries_TAG_DASHED.zip
  // The archive contains: Spout-SDK-binaries_TAG_DASHED/TAG_DASHED/Libs/{include,x64}/{...}
  const tagDashed = spoutTag.replace(/\./g, "-");
  const zipFilename = `Spout-SDK-binaries_${tagDashed}.zip`;
  const zipUrl = `https://github.com/leadedge/Spout2/releases/download/${spoutTag}/${zipFilename}`;
  const zipPath = join(tmpDir, zipFilename);

  if (existsSync(zipPath)) {
    console.log(`  Archive already downloaded: ${zipPath}`);
  } else {
    await downloadFile(zipUrl, zipPath);
  }

  // Extract
  console.log("  Extracting...");
  const extractDir = join(tmpDir, "spout_extract");
  removeDir(extractDir);
  mkdirSync(extractDir, { recursive: true });

  const tarResult = await $`tar -xf "${zipPath}" -C "${extractDir}"`.quiet();
  if (tarResult.exitCode !== 0) {
    throw new Error(`Failed to extract Spout archive.\n${tarResult.stderr.toString()}`);
  }

  // Find extracted dir — binary release uses dashed tag: Spout-SDK-binaries_X-XXX-XXX/X-XXX-XXX/Libs/
  const extracted = readdirSync(extractDir).find((d) => d.startsWith("Spout-SDK-binaries"));
  if (!extracted) throw new Error("Spout extraction produced no Spout-SDK-binaries* directory");
  const spoutExtracted = join(extractDir, extracted);

  // Inside is a version-named folder with the Libs directory
  const innerDirs = readdirSync(spoutExtracted);
  const versionDir = innerDirs.find((d) => statSync(join(spoutExtracted, d)).isDirectory());
  if (!versionDir) throw new Error("Spout SDK binaries archive has unexpected structure");
  const libsDir = join(spoutExtracted, versionDir, "Libs");

  // Replace vendor dir
  removeDir(spoutDir);
  mkdirSync(spoutDir, { recursive: true });

  // Copy headers from Libs/include/
  const includesSrc = join(libsDir, "include");
  if (existsSync(includesSrc)) {
    copyDirRecursive(includesSrc, join(spoutDir, "include"));
  }

  // Copy MT and MD prebuilt binaries from Libs/x64/{MT,MD}/
  for (const variant of ["MT", "MD"]) {
    const src = join(libsDir, "x64", variant);
    if (existsSync(src)) {
      copyDirRecursive(src, join(spoutDir, variant));
    }
  }

  // Write version marker
  writeFileSync(join(spoutDir, ".spout-version"), spoutTag + "\n");
  console.log(`  ✓ Spout ${spoutTag} ready at ${spoutDir}`);

  // Verify the lib we actually need exists
  const neededLib = join(spoutDir, "MT", "lib", "SpoutDX_static.lib");
  if (existsSync(neededLib)) {
    console.log(`  ✓ SpoutDX_static.lib (MT) found`);
  } else {
    console.warn(`  ⚠ SpoutDX_static.lib (MT) NOT found — Spout output will be disabled in builds`);
    console.warn(`    Expected: ${neededLib}`);
    console.warn(`    The Spout2 release layout may have changed. Check the extracted contents.`);
  }

  // Cleanup
  removeDir(extractDir);
}

// ── Shared junction ─────────────────────────────────────────────────────────

function setupSharedJunction() {
  const link = join(ROOT, "shared");
  const target = join(NATIVE_DIR, "shared");

  if (existsSync(link)) {
    console.log(`  shared/ junction already exists → ${target}`);
    return;
  }

  console.log(`  Creating shared/ junction → ${target}`);
  // Use Bun shell for mklink
  Bun.spawnSync(["cmd", "/c", `mklink /J "${link}" "${target}"`], { stdio: ["ignore", "ignore", "ignore"] });

  if (existsSync(link)) {
    console.log("  ✓ shared/ junction created");
  } else {
    console.warn("  ⚠ Failed to create shared/ junction. Create manually:");
    console.warn(`    New-Item -ItemType Junction -Path "${link}" -Target "${target}"`);
  }
}

// ── Recursive copy helper ───────────────────────────────────────────────────

function copyDirRecursive(src: string, dest: string) {
  mkdirSync(dest, { recursive: true });
  for (const entry of readdirSync(src)) {
    const srcPath = join(src, entry);
    const destPath = join(dest, entry);
    if (statSync(srcPath).isDirectory()) {
      copyDirRecursive(srcPath, destPath);
    } else {
      copyFileSync(srcPath, destPath);
    }
  }
}

// ── Main ────────────────────────────────────────────────────────────────────

console.log("Chromeyumm Vendor Setup");
console.log("═".repeat(50));

mkdirSync(VENDOR_DIR, { recursive: true });

if (!cefOnly && !spoutOnly) {
  await setupCef();
  await setupSpout();
  setupSharedJunction();
} else if (cefOnly) {
  await setupCef();
  setupSharedJunction();
} else if (spoutOnly) {
  await setupSpout();
}

console.log("\n══ Summary ════════════════════════════════════════════");

const cefVersionFile = join(VENDOR_DIR, "cef", ".cef-version");
const spoutVersionFile = join(VENDOR_DIR, "spout", ".spout-version");

if (existsSync(cefVersionFile)) {
  console.log(`  CEF:   ${readFileSync(cefVersionFile, "utf-8").trim()}`);
} else {
  console.log("  CEF:   not installed");
}

if (existsSync(spoutVersionFile)) {
  console.log(`  Spout: ${readFileSync(spoutVersionFile, "utf-8").trim()}`);
} else {
  console.log("  Spout: not installed");
}

console.log("\nNext steps:");
console.log("  bun build.ts          # full build");
console.log("  cd dist && bun app.js # run");
