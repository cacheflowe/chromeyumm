/**
 * Release script for chromeyumm.
 *
 * Two modes:
 *
 *   LOCAL PACKAGING (for testing):
 *     bun scripts/release.ts                  — build + package zip
 *     bun scripts/release.ts --skip-build     — package existing dist/ without rebuilding
 *
 *   PUBLISH (CI does the actual release):
 *     bun scripts/release.ts --publish              — bump patch, commit, tag, push → CI builds & releases
 *     bun scripts/release.ts --publish --bump minor — bump minor instead of patch
 *     bun scripts/release.ts --publish --bump major — bump major
 *
 * --publish does NOT build or create a GitHub release locally.
 * It only bumps the version, commits, tags, and pushes — the GitHub Actions
 * workflow (.github/workflows/release.yml) handles the build + release.
 */

import { existsSync, mkdirSync, readdirSync, statSync, readFileSync, writeFileSync, unlinkSync } from "fs";
import { join } from "path";
import { $ } from "bun";

const ROOT = join(import.meta.dir, "..");
const DIST_DIR = join(ROOT, "dist");
const RELEASE_DIR = join(ROOT, "release");
const PKG_PATH = join(ROOT, "package.json");

// Files/dirs in dist/ to exclude from the release zip
const EXCLUDE = new Set([
  "views", // leftover dev views (served via dev server, not bundled)
  "app.js", // redundant — baked into chromeyumm.exe
  "app.log", // runtime log
  "debug-inject.js", // dev artifact
  "libcef.lib", // link-time only, not needed at runtime
  "libNativeWrapper.dll.manifest",
  "libNativeWrapper.exp",
]);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function readVersion(): string {
  const pkg = JSON.parse(readFileSync(PKG_PATH, "utf-8"));
  return pkg.version;
}

function bumpVersion(type: "patch" | "minor" | "major"): string {
  const pkg = JSON.parse(readFileSync(PKG_PATH, "utf-8"));
  const parts = pkg.version.split(".").map(Number);
  if (type === "major") {
    parts[0]++;
    parts[1] = 0;
    parts[2] = 0;
  } else if (type === "minor") {
    parts[1]++;
    parts[2] = 0;
  } else {
    parts[2]++;
  }
  pkg.version = parts.join(".");
  writeFileSync(PKG_PATH, JSON.stringify(pkg, null, 2) + "\n");
  console.log(`✓ Version bumped to ${pkg.version}`);
  return pkg.version;
}

/** Recursively collect all files in a directory, returning paths relative to base. */
function collectFiles(dir: string, base: string = dir): string[] {
  const results: string[] = [];
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    const rel = full.slice(base.length + 1);
    const topLevel = rel.split(/[\\/]/)[0];
    if (EXCLUDE.has(topLevel)) continue;
    if (statSync(full).isDirectory()) {
      results.push(...collectFiles(full, base));
    } else {
      results.push(rel);
    }
  }
  return results;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

const args = process.argv.slice(2);
const bumpType = args.includes("--bump") ? (args[args.indexOf("--bump") + 1] as "patch" | "minor" | "major") : null;
const skipBuild = args.includes("--skip-build");
const publish = args.includes("--publish");

if (bumpType && !["patch", "minor", "major"].includes(bumpType)) {
  console.error("Usage: --bump patch|minor|major");
  process.exit(1);
}

// ---------------------------------------------------------------------------
// PUBLISH MODE: bump → commit → tag → push (CI handles build + release)
// ---------------------------------------------------------------------------
if (publish) {
  const effectiveBump = bumpType ?? "patch";
  bumpVersion(effectiveBump);
  const version = readVersion();
  const tag = `v${version}`;

  console.log(`\nChromeyumm publish  ${tag}`);
  console.log("─".repeat(50));

  // Check git is clean (warn but don't block)
  const dirty = await $`git status --porcelain`.cwd(ROOT).quiet();
  if (dirty.stdout.toString().trim()) {
    console.warn("⚠ Working tree has uncommitted changes (only package.json bump will be committed)");
  }

  // Commit the version bump
  console.log(`  Committing version bump...`);
  await $`git add package.json`.cwd(ROOT);
  await $`git commit -m "release: ${tag}"`.cwd(ROOT).catch(() => {
    // No changes to commit
  });

  // Create and push tag — this triggers CI
  console.log(`  Creating tag ${tag}...`);
  await $`git tag -a ${tag} -m "Release ${tag}"`.cwd(ROOT);
  console.log(`  Pushing to origin...`);
  await $`git push origin HEAD ${tag}`.cwd(ROOT);

  console.log(`\n✓ Tag ${tag} pushed — GitHub Actions will build and create the release`);
  process.exit(0);
}

// ---------------------------------------------------------------------------
// LOCAL PACKAGING MODE: build + zip (no publish, no version bump)
// ---------------------------------------------------------------------------
if (bumpType) bumpVersion(bumpType);
const version = readVersion();
const tag = `v${version}`;
const zipName = `chromeyumm-${tag}-win-x64.zip`;

console.log(`\nChromeyumm package  ${tag}`);
console.log("─".repeat(50));

// Build (unless --skip-build)
if (!skipBuild) {
  console.log("\n── Building ────────────────────────────────────────────");
  const build = await $`bun build.ts`.cwd(ROOT);
  if (build.exitCode !== 0) {
    console.error("Build failed");
    process.exit(1);
  }
}

// Verify dist/
if (!existsSync(join(DIST_DIR, "chromeyumm.exe"))) {
  console.error("dist/chromeyumm.exe not found — run build first");
  process.exit(1);
}

// Package into zip
console.log("\n── Packaging ───────────────────────────────────────────");
mkdirSync(RELEASE_DIR, { recursive: true });
const zipPath = join(RELEASE_DIR, zipName);

const files = collectFiles(DIST_DIR);
console.log(`  ${files.length} files to package`);

// Use PowerShell Compress-Archive for zip creation (available on all Windows 10+)
// First, create a temp manifest of files to include
const manifestPath = join(ROOT, "_release_manifest.txt");
writeFileSync(manifestPath, files.map((f) => join(DIST_DIR, f)).join("\n"));

// Build zip using tar (built into Windows 10+, faster than Compress-Archive for large files)
// Remove old zip first
if (existsSync(zipPath)) {
  unlinkSync(zipPath);
}

// Use 7z if available (better compression), fall back to PowerShell
const sevenZip = await $`where 7z`.quiet().catch(() => null);
if (sevenZip && sevenZip.exitCode === 0) {
  const excludeArgs = [...EXCLUDE].map((e) => `-xr!${e}`).join(" ");
  await $`cmd /c "cd /d "${DIST_DIR}" && 7z a -tzip -mx=7 "${zipPath}" . ${excludeArgs}"`;
} else {
  // PowerShell approach: copy files to temp dir, then compress
  const tmpDir = join(ROOT, "_release_tmp");
  await $`Remove-Item "${tmpDir}" -Recurse -Force -ErrorAction SilentlyContinue`.quiet().catch(() => {});
  mkdirSync(tmpDir, { recursive: true });

  // Copy files preserving directory structure
  for (const f of files) {
    const src = join(DIST_DIR, f);
    const dest = join(tmpDir, f);
    mkdirSync(join(dest, ".."), { recursive: true });
    await $`Copy-Item "${src}" "${dest}"`.quiet();
  }

  await $`Compress-Archive -Path "${join(tmpDir, "*")}" -DestinationPath "${zipPath}" -Force`;
  await $`Remove-Item "${tmpDir}" -Recurse -Force`.quiet().catch(() => {});
}

await $`Remove-Item "${manifestPath}" -Force`.quiet().catch(() => {});

const zipSize = statSync(zipPath).size;
console.log(`✓ ${zipName}  (${(zipSize / 1024 / 1024).toFixed(1)} MB)`);
console.log(`  → ${zipPath}`);

console.log("\n✓ Packaging complete");
