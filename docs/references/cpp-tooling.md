# C++ Tooling Setup

How to set up the C++ build environment for Chromeyumm on a Windows machine. After this, `bun build.ts` handles everything.

## Prerequisites

### 1. Visual Studio 2022 (required)

Install via the [Visual Studio installer](https://visualstudio.microsoft.com/downloads/) or `winget`:

```powershell
winget install --id Microsoft.VisualStudio.2022.Community
```

Required workload: **Desktop development with C++**

The installer provides:
- MSVC compiler (`cl.exe`) and linker (`link.exe`)
- Windows SDK headers and libs
- `vswhere.exe` — used by `build.ts` to locate `vcvarsall.bat` automatically

You do **not** need to open Visual Studio or configure the environment manually. `build.ts` locates `vcvarsall.bat` at startup and sets up the environment per-compile invocation.

### 2. CMake (required — for building CEF wrapper lib)

```powershell
winget install --id Kitware.CMake
```

CMake is only needed once (via `bun scripts/setup-vendors.ts`) to build `libcef_dll_wrapper.lib`. It is not invoked during normal `bun build.ts` runs.

### 3. Bun (required)

```powershell
winget install --id Oven-sh.Bun
```

Bun runs `build.ts`, the setup scripts, the app itself, and the smoke test.

---

## Optional: sccache (recommended for iterative C++ work)

[sccache](https://github.com/mozilla/sccache) is a transparent compiler cache for MSVC. It can cut a full C++ rebuild from ~30s to ~3s after the first build. `build.ts` detects and uses it automatically — no flags or config needed.

### Install via Chocolatey

```powershell
choco install sccache
```

### Install via winget

```powershell
winget install --id Mozilla.sccache
```

### Install via cargo

```powershell
cargo install sccache
```

### Verify

```powershell
sccache --version
sccache --show-stats   # after a build, shows cache hit rate
```

`build.ts` prints `✓ sccache: found — C++ compile steps will be cached` at startup when sccache is on the PATH.

**Note**: sccache is applied only to compile steps (`cl`), not link steps. Link steps are fast and not cacheable.

---

## First-Time Setup

### 1. Install vendors

```bash
bun scripts/setup-vendors.ts
```

Downloads CEF (prebuilt) and Spout2, extracts them to `native/vendor/`, then runs CMake to build `libcef_dll_wrapper.lib`. This is the slow step (~5 min on first run, dominated by CMake + MSVC building the wrapper).

Spout is optional. If not present, the build proceeds without Spout support.

### 2. Build

```bash
bun build.ts
```

Produces `dist/chromeyumm.exe` and all runtime files. Subsequent builds are fast if sccache is installed.

---

## How `build.ts` Works

`build.ts` is a plain Bun script — no Makefile, no CMake, no Ninja. MSVC is invoked directly.

### MSVC detection

At startup, `findMsvc()` runs `vswhere.exe` to find the Visual Studio installation path, then constructs the path to `vcvarsall.bat`. Each MSVC invocation writes a temporary `.bat` file that calls `vcvarsall.bat x64` then runs the compile/link command, so the environment is self-contained per invocation.

If Visual Studio is not found, `cl` and `link` are called directly (assumes they're already on PATH — useful in CI with pre-activated environments).

### Parallel compilation

All `.cpp` → `.obj` translation units are compiled in parallel via `Promise.all`. Each invocation gets its own uniquely named batch file (`_build_tmp_0.bat`, `_build_tmp_1.bat`, …) to avoid race conditions on the temporary file.

Link is a single sequential step after all objects are ready.

### sccache integration

When sccache is found, `runMsvc()` prefixes `cl` compile commands with `sccache`:

```
sccache cl /c /EHsc ... /Fo"obj" "source.cpp"
```

sccache intercepts the compile, hashes the inputs (source + flags + includes), and serves a cached `.obj` if available. The cache is stored at `%LOCALAPPDATA%\Mozilla\sccache\` by default.

### Skipping native

```bash
bun build.ts --skip-native
# or
bun run build:ts
```

Skips the entire MSVC compile/link phase — only re-bundles TypeScript. Use this for any change that doesn't touch `native/`.

---

## What Each Step Produces

| Step | Output | When needed |
|---|---|---|
| `buildNative()` | `native/build/libNativeWrapper.dll` | `native/*.cpp` or `native/frame-output/**` changed |
| `buildNative()` | `native/build/chromeyumm Helper.exe` | Same |
| `bundleTs()` | `dist/app.js`, `dist/debug-inject.js` | Any `src/` change |
| `compileExe()` | `dist/chromeyumm.exe` | Any `src/` change |
| `copyRuntime()` | `dist/libNativeWrapper.dll`, `dist/libcef.dll`, … | Always (idempotent copy) |

See [COMMANDS.md](../COMMANDS.md#what-requires-which-build) for a per-file rebuild guide.

---

## Incremental C++ Development

The native DLL is fully rebuilt each time `buildNative()` runs. There is no incremental linking or header dependency tracking — the build script is intentionally simple. In practice this means:

- **sccache** is the main tool for fast iteration. After the first build, unchanged `.cpp` files are served from cache. A change to one file recompiles only that translation unit; the others are cache hits.
- **`--skip-native`** is the fastest path for changes that don't touch C++.
- For tight C++ iteration loops (e.g. debugging DDP output), build once to warm sccache, then subsequent rebuilds of the changed file are typically under 5s.

There is currently no hot-reload for the native DLL — a full build and restart is needed.

---

## Troubleshooting

### `vswhere not found` / `MSVC not found`

Visual Studio is not installed, or was installed without the C++ workload. Re-run the VS installer and enable **Desktop development with C++**.

### `CEF lib not found`

Vendors are not set up. Run `bun scripts/setup-vendors.ts`.

### `libcef_dll_wrapper.lib` missing

CMake step failed or was skipped. Run:

```powershell
cd native/vendor/cef
mkdir build; cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release --target libcef_dll_wrapper
```

### Permission denied on `.obj` file (parallel build)

A stale `_build_tmp_N.bat` is being held by another process. Kill any orphaned `cmd.exe` processes and retry.

### sccache not caching

Run `sccache --show-stats` after a build. If `cache_hits` stays at 0:
- Confirm sccache is on PATH (`sccache --version`)
- Check that you're not running as a different user between builds (cache is per-user)
- Compiler flags must be identical between builds — changing `/O2` vs `/Od` creates separate cache entries

---

## Related

- [native/README.md](../../native/README.md) — C++ source layout and vendor setup
- [COMMANDS.md](../COMMANDS.md) — All build commands and what each rebuilds
- [references/vendor-management.md](vendor-management.md) — Long-term vendor upgrade strategy
- [references/cef-upgrade.md](cef-upgrade.md) — CEF version upgrade procedure
