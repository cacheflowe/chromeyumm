/**
 * Release packaging script for chromeyumm.
 *
 * Usage:
 *   bun scripts/release.ts                    — package current version
 *   bun scripts/release.ts --bump patch       — bump patch version, build, package
 *   bun scripts/release.ts --bump minor       — bump minor version
 *   bun scripts/release.ts --bump major       — bump major version
 *   bun scripts/release.ts --skip-build       — package existing dist/ without rebuilding
 *   bun scripts/release.ts --publish          — bump patch, build, package, tag, push, create GitHub release
 *   bun scripts/release.ts --publish --bump minor  — same but bump minor instead of patch
 *
 * Produces:  release/chromeyumm-v{VERSION}-win-x64.zip
 *
 * --publish auto-generates release notes from git commits since the last tag.
 */

import { existsSync, mkdirSync, readdirSync, statSync, readFileSync, writeFileSync, unlinkSync } from "fs";
import { join, basename } from "path";
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

/** Get the most recent git tag, or null if none exist. */
async function getLastTag(): Promise<string | null> {
  const result = await $`git describe --tags --abbrev=0`
    .cwd(ROOT)
    .quiet()
    .catch(() => null);
  if (!result || result.exitCode !== 0) return null;
  return result.stdout.toString().trim();
}

/** Generate release notes from git log since the given tag (or all commits if null). */
async function generateReleaseNotes(sinceTag: string | null): Promise<string> {
  const range = sinceTag ? `${sinceTag}..HEAD` : "HEAD";
  const result = await $`git log ${range} --pretty=format:%s --no-merges`
    .cwd(ROOT)
    .quiet()
    .catch(() => null);
  if (!result || result.exitCode !== 0 || !result.stdout.toString().trim()) {
    return "(No changes since last release)";
  }

  const commits = result.stdout
    .toString()
    .trim()
    .split("\n")
    .filter((line: string) => line.trim().length > 0);

  if (commits.length === 0) return "(No changes since last release)";

  const lines = [`## What's Changed\n`];
  for (const msg of commits) {
    lines.push(`- ${msg}`);
  }

  // Add contributor info
  const authors = await $`git log ${range} --pretty=format:%an --no-merges`
    .cwd(ROOT)
    .quiet()
    .catch(() => null);
  if (authors && authors.exitCode === 0) {
    const unique = [...new Set(authors.stdout.toString().trim().split("\n").filter(Boolean))];
    if (unique.length > 0) {
      lines.push(`\n**Contributors:** ${unique.join(", ")}`);
    }
  }

  if (sinceTag) {
    lines.push(`\n**Full Changelog:** \`${sinceTag}...v{VERSION}\``);
  }

  return lines.join("\n");
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

// 1. Bump version: --publish implies --bump patch unless --bump is explicitly specified
const effectiveBump = bumpType ?? (publish ? "patch" : null);
if (effectiveBump) bumpVersion(effectiveBump);
const version = readVersion();
const tag = `v${version}`;
const zipName = `chromeyumm-${tag}-win-x64.zip`;

console.log(`\nChromeyumm release  ${tag}`);
console.log("─".repeat(50));

// 2. Build (unless --skip-build)
if (!skipBuild) {
  console.log("\n── Building ────────────────────────────────────────────");
  const build = await $`bun build.ts`.cwd(ROOT);
  if (build.exitCode !== 0) {
    console.error("Build failed");
    process.exit(1);
  }
}

// 3. Verify dist/
if (!existsSync(join(DIST_DIR, "chromeyumm.exe"))) {
  console.error("dist/chromeyumm.exe not found — run build first");
  process.exit(1);
}

// 4. Package into zip
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

// 5. Publish to GitHub (if --publish)
if (publish) {
  console.log("\n── Publishing ──────────────────────────────────────────");

  // Check for gh CLI
  const ghCheck = await $`where gh`.quiet().catch(() => null);
  if (!ghCheck || ghCheck.exitCode !== 0) {
    console.error("GitHub CLI (gh) not found. Install from https://cli.github.com/");
    console.error("Or manually create a release and upload:", zipPath);
    process.exit(1);
  }

  // Check git is clean (warn but don't block)
  const dirty = await $`git status --porcelain`.cwd(ROOT).quiet();
  if (dirty.stdout.toString().trim()) {
    console.warn("⚠ Working tree has uncommitted changes");
  }

  // Generate release notes from commits since last tag
  console.log(`  Generating release notes...`);
  const lastTag = await getLastTag();
  let notes = await generateReleaseNotes(lastTag);
  notes = notes.replace("{VERSION}", version);

  console.log(`\n── Release Notes ───────────────────────────────────────`);
  console.log(notes);
  console.log("─".repeat(50));

  // Commit the version bump
  console.log(`\n  Committing version bump...`);
  await $`git add package.json`.cwd(ROOT);
  await $`git commit -m "release: ${tag}"`.cwd(ROOT).catch(() => {
    // No changes to commit (e.g. --skip-build without --bump)
  });

  // Create and push tag
  console.log(`  Creating tag ${tag}...`);
  await $`git tag -a ${tag} -m "Release ${tag}"`.cwd(ROOT);
  await $`git push origin HEAD ${tag}`.cwd(ROOT);

  // Write notes to temp file to avoid shell escaping issues
  const notesPath = join(ROOT, "_release_notes.md");
  writeFileSync(notesPath, notes);

  // Create GitHub release with the zip as asset
  console.log(`  Creating GitHub release...`);
  await $`gh release create ${tag} "${zipPath}" --title "Chromeyumm ${tag}" --notes-file "${notesPath}" --latest`.cwd(
    ROOT,
  );

  // Clean up temp file
  unlinkSync(notesPath);

  console.log(`\n✓ Published ${tag} to GitHub`);
}

console.log("\n✓ Release complete");
