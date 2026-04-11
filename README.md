
![Chromeyumm logo](native/app.png)

# Chromeyumm

A minimal Windows CEF browser built for site-specific installations. Renders web content across multiple displays via direct D3D11 GPU blitting, with Spout texture I/O for integration with TouchDesigner, Resolume, and other real-time environments.

---

## Features

- **Multi-window D3D output** — one OSR CEF browser renders the full virtual canvas; sub-regions are blitted via `CopySubresourceRegion` to bare Win32 windows on each physical display. Zero DWM thumbnail overhead, no monitor boundary limits on canvas size.
- **Spout output** — OSR `OnAcceleratedPaint` delivers a DXGI shared texture directly to `SpoutDX::SendTexture`. GPU→GPU, zero CPU, zero PCIe traffic. Intel 0% / NVIDIA 15% at 60fps on an Optimus laptop.
- **Spout input** — Win32 named shared memory bridge. Two-tier: tier 1 zero-copy `MapViewOfFile`-backed `ArrayBuffer` (V8 sandbox off), tier 2 persistent-buffer `memcpy` (sandbox safe). Available at `window.__spoutFrameBuffer` / `window.__spoutGetFrame` in the browser.
- **`display-config.json`** — human-readable config file for window layout, canvas size, content URL, Spout settings, interactive/output mode defaults.
- **Hotkeys** — Ctrl+M (toggle interactive/output mode), Ctrl+Shift+M (reset displays), Ctrl+R (reload), Ctrl+F (toggle alwaysOnTop), Ctrl+D (debug panel), Escape (quit).
- **Owned codebase** — detached from Electrobun upstream. CEF version upgrades are a vendor drop-in.

---

## Architecture

```
bun.exe (Bun runtime)
├── dist/app.js            TypeScript app (src/app/ bundled)
│     └── chromeyumm       local framework (src/chromeyumm/)
│           └── ffi.ts     Bun dlopen → libNativeWrapper.dll
│
└── libNativeWrapper.dll   C++ CEF/D3D11/Spout wrapper (native/cef-wrapper.cpp)
      ├── CEF OSR browser  (OnAcceleratedPaint → shared DXGI texture)
      ├── D3D11 output     (CopySubresourceRegion → NativeDisplayWindow swap chains)
      ├── Spout sender     (SpoutDX → TouchDesigner / Resolume)
      └── Spout receiver   (staging readback → Win32 shared memory → browser)
```

See `DEVELOPMENT.md` for full architecture details, GPU findings, and performance analysis.

---

## Quick Start

**Prerequisites:** Windows 10/11 x64 · [Bun](https://bun.sh) · Visual Studio 2022 (MSVC C++ Desktop workload) · CMake

```bash
git clone <repo> && cd chromeyumm
bun install
bun scripts/setup-vendors.ts   # download CEF + Spout, build libcef_dll_wrapper (~5 min, ~1.5 GB)
bun build.ts                   # compile DLL + bundle TS + copy CEF runtime → dist/
dist/chromeyumm.exe            # run (configure display-config.json first)
```

---

## Commands

### Build

| Command | Description |
|---|---|
| `bun run build` | Full rebuild: compile C++ DLL + bundle TS + copy CEF runtime to `dist/` |
| `bun run build:dev` | Same, unminified with inline sourcemaps |
| `bun run build:ts` | TS bundle only — no C++ compile. Fast for TS-only changes |

**What requires which build:**

| Changed | Command needed |
|---|---|
| `native/cef-wrapper.cpp` or `cef-helper.cpp` | `bun run build` |
| Anything in `src/` | `bun run build:ts` |
| `display-config.json` only | None — found at runtime via cwd walk-up |
| `src/views/r3f/` source | `cd src/views/r3f && bun run build` (served via dev server) |

### Run

| Command | Description |
|---|---|
| `bun run start` | Run `dist/chromeyumm.exe` |
| `bun run feature-check` | Feature detection page — WebGL, codecs, hardware APIs, CEF version |
| `bun run demo` | Start R3F Vite dev server + chromeyumm browser together |

### Test & Diagnostics

| Command | Description |
|---|---|
| `bun run test` | FFI smoke test — verify DLL loads and all exported symbols resolve |
| `dumpbin /exports dist/libNativeWrapper.dll` | Inspect DLL export names directly |

### Vendor Setup (first time or new machine)

```bash
bun scripts/setup-vendors.ts                          # download + build CEF + Spout
bun scripts/setup-vendors.ts --cef-only               # CEF only
bun scripts/setup-vendors.ts --spout-only             # Spout only
bun scripts/setup-vendors.ts --cef-version "VERSION"  # specific CEF version
bun scripts/setup-vendors.ts --spout-tag "TAG"        # specific Spout tag
bun scripts/setup-vendors.ts --cef-archive file.tar.bz2  # use a local archive
```

### CEF / Chromium Upgrades

```bash
bun run check-updates    # report whether a newer stable CEF build is available
bun run upgrade:cef      # auto-fetch latest CEF, download, build wrapper, rebuild app
```

`upgrade:cef` auto-updates `DEFAULT_CEF_VERSION` in `scripts/setup-vendors.ts` so the version bump appears cleanly in the next commit. After upgrading: run `bun run test`, do a manual smoke test, then release.

See `docs/references/vendor-management.md` for the full procedure and API compatibility notes.

### Release

| Command | Description |
|---|---|
| `bun run release` | Build + package `release/chromeyumm-v{VERSION}-win-x64.zip` |
| `bun run release:publish` | **One-command release**: auto-bump patch, build, package, generate release notes, git tag + push, create GitHub release |
| `bun scripts/release.ts --bump patch` | Bump patch, build, package (no publish) |
| `bun scripts/release.ts --bump minor` | Bump minor, build, package (no publish) |
| `bun scripts/release.ts --bump major` | Bump major, build, package (no publish) |
| `bun scripts/release.ts --publish --bump minor` | Publish with minor bump instead of patch |
| `bun scripts/release.ts --skip-build` | Package existing `dist/` without rebuilding |

`--publish` requires the [GitHub CLI](https://cli.github.com/) (`gh`). Release notes are auto-generated from git commits since the last tag.

Pushing a `v*` tag also triggers `.github/workflows/release.yml` (CI build + release) automatically.

---

## Project Structure

```
native/
  cef-wrapper.cpp        Main C++ DLL (CEF, D3D11, Spout, NativeDisplayWindow)
  cef-helper.cpp         CEF renderer process helper (Spout input V8 bindings)
  shared/                Shared C++ headers
  vendor/                CEF + Spout vendor dirs (gitignored — see native/README.md)

src/
  chromeyumm/            Framework layer
    ffi.ts               Bun FFI bindings to libNativeWrapper.dll
    browser-window.ts    BrowserWindow class
    native-display-window.ts  NativeDisplayWindow class
    shortcut.ts          GlobalShortcut
    screen.ts            Screen.getAllDisplays()
    spout.ts             SpoutReceiver
    webview.ts           Webview (executeJavascript, event routing)
  app/
    index.ts             Main entry point
    config.ts            display-config.json loader
    spout-input.ts       SpoutInput lifecycle wrapper
  views/
    threejs/             Three.js renderer view
    r3f/                 React Three Fiber view
    spout-demo/          Spout input demo
    spout-video/         Video + Spout demo

build.ts                 Build script
display-config.json      Display layout config
```

---

## display-config.json

```jsonc
{
  "virtualCanvas": { "width": 3840, "height": 1080 },

  "windows": [
    {
      "label": "Left display",
      "slot": 0,
      "window":  { "x": 0,    "y": 0, "width": 1920, "height": 1080 },
      "source":  { "x": 0,    "y": 0, "width": 1920, "height": 1080 }
    },
    {
      "label": "Right display",
      "slot": 1,
      "window":  { "x": 1920, "y": 0, "width": 1920, "height": 1080 },
      "source":  { "x": 1920, "y": 0, "width": 1920, "height": 1080 }
    }
  ],

  "alwaysOnTop": true,
  "fullscreen": false,
  "startInteractive": false,

  "contentUrl": "http://localhost:5173",

  // Spout output (can run alongside multi-window D3D output)
  "spoutOutput": { "senderName": "Chromeyumm" },

  // Spout input (optional — works in any mode)
  "spoutInput":  { "senderName": "TD_Spout_Sender" }
}
```

If `display-config.json` is absent, the app auto-detects connected displays and splits the canvas across them.

---

## Hotkeys

| Key | Action |
|---|---|
| **Ctrl+M** | Toggle interactive ↔ output mode |
| **Ctrl+Shift+M** | Reset display windows (recover from monitor topology changes) |
| **Ctrl+R** | Reload content |
| **Ctrl+F** | Toggle alwaysOnTop |
| **Ctrl+D** | Toggle debug panel (pages must include debug-panel.js) |
| **Escape** | Quit cleanly |

---

