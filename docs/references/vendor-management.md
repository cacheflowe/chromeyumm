# Vendor Management

Long-term strategy for managing CEF and Spout2 vendor dependencies.

## Overview

| Vendor | Source | Size | Update cadence |
|---|---|---|---|
| **CEF** | [Spotify CDN](https://cef-builds.spotifycdn.com/index.html) | ~1.5 GB (prebuilt) | Every 4-6 weeks (follows Chrome stable) |
| **Spout2** | [GitHub releases](https://github.com/leadedge/Spout2/releases) | ~50 MB | Infrequent (months) |

Both are too large for git. They live in `native/vendor/` which is gitignored.

## Setup Script

```bash
bun scripts/setup-vendors.ts           # download + build all vendors
bun scripts/setup-vendors.ts --cef-only
bun scripts/setup-vendors.ts --spout-only
bun scripts/setup-vendors.ts --cef-version "VERSION"
bun scripts/setup-vendors.ts --spout-tag "TAG"
bun scripts/setup-vendors.ts --cef-archive path/to/local.tar.bz2
```

The script:
1. Downloads from official sources (or uses a local archive)
2. Extracts only the files needed for building
3. Builds `libcef_dll_wrapper.lib` via CMake (CEF only)
4. Writes `.cef-version` / `.spout-version` markers
5. Creates the `shared/` junction (→ `native/shared/`)

Cached archives are kept in `_vendor_tmp/` for re-runs (gitignored by the `_vendor_tmp/` pattern or by being in the root).

## Version Pinning

Versions are pinned as defaults in `scripts/setup-vendors.ts`:

```ts
const DEFAULT_CEF_VERSION = "145.0.23+g3e7fe1c+chromium-145.0.7632.68";
const DEFAULT_SPOUT_TAG   = "2.007.014";
```

When upgrading, update these defaults so future clones get the correct version.
The `--cef-version` and `--spout-tag` CLI flags allow one-off overrides for testing.

After setup, each vendor dir contains a version marker file that records what was installed:
- `native/vendor/cef/.cef-version`
- `native/vendor/spout/.spout-version`

## CEF Upgrade Procedure

1. **Check for new releases**: https://cef-builds.spotifycdn.com/index.html
   - Look at `Windows 64-bit` → `Standard Distribution`
   - CEF tracks Chrome stable — a new build appears every few weeks

2. **Run the setup script with the new version**:
   ```bash
   bun scripts/setup-vendors.ts --cef-only --cef-version "NEW_VERSION_STRING"
   ```

3. **Build and test**:
   ```bash
   bun build.ts
   dist/chromeyumm.exe
   ```

4. **Check for API breakage**:
   - The CEF C++ API is very stable. Breaking changes are rare.
   - Key API surface we use: `OnAcceleratedPaint`, `CefBrowserHost::CreateBrowser`,
     `CefBrowserSettings`, `CefCommandLine`, `CefV8Handler`
   - Changelog: https://bitbucket.org/chromiumembedded/cef/wiki/Home
   - If the wrapper DLL fails to compile, check `cef-wrapper.cpp` for deprecated APIs

5. **Update the default in `scripts/setup-vendors.ts`** and commit

6. **What to test after upgrade**:
   - Multi-window rendering (D3D11 blit pipeline)
   - Offscreen rendering with shared textures
   - Spout output (if enabled)
   - DevTools (Ctrl+Shift+I)
   - JavaScript execution / V8 bindings
   - GPU process stability (check for black windows)

See also: [docs/references/cef-upgrade.md](cef-upgrade.md) for CEF API surface details.

## Spout2 Upgrade Procedure

1. **Check for new releases**: https://github.com/leadedge/Spout2/releases

2. **Run the setup script with the new tag**:
   ```bash
   bun scripts/setup-vendors.ts --spout-only --spout-tag "NEW_TAG"
   ```

3. **Build and test**:
   ```bash
   bun build.ts
   dist/chromeyumm.exe
   ```

4. **Check for API breakage**:
   - We use: `spoutDX.OpenDirectX11()`, `SetSenderName()`, `SendTexture()`,
     `spoutDX.ReceiveTexture()`, `IsUpdated()`, `GetSenderWidth/Height()`
   - Spout2 maintains backward compatibility very well
   - Ensure the release still ships `SpoutDX_static.lib` in `BUILD/Binaries/x64/MT/lib/`

5. **Update the default in `scripts/setup-vendors.ts`** and commit

## New Machine / Fresh Clone Workflow

```bash
git clone <repo>
cd chromeyumm
bun install
bun scripts/setup-vendors.ts    # downloads CEF + Spout2, builds wrapper
bun build.ts                    # compiles DLL + bundles TS + copies runtime
dist/chromeyumm.exe             # run (from project root)
```

## CI Considerations

- The setup script downloads ~1.5 GB for CEF — cache `_vendor_tmp/` between CI runs
- CEF CMake build takes 2-5 minutes — cache `native/vendor/cef/build/` if versions haven't changed
- Spout download is fast (~50 MB) but can be cached the same way
- The `.cef-version` and `.spout-version` files make good cache keys

## Architecture Decisions

- **Script in TypeScript (Bun)**: Consistent with `build.ts`, no extra tooling required
- **Download from official sources**: No vendored copies in git, no dependency on Electrobun or any other repo
- **Version pinned in script**: Easy to bump, clear in diffs, no separate config file needed
- **Archives cached locally**: Re-runs don't re-download (~1.5 GB CEF archive)
- **Only needed files extracted**: Keeps vendor dir manageable (skip CEF tests, samples, etc.)
