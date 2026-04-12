
![Chromeyumm logo](native/app.png)

# Chromeyumm

A minimal Windows CEF browser built for site-specific installations. Renders web content across multiple displays via direct D3D11 GPU blitting, with Spout texture I/O for integration with TouchDesigner, Resolume, and other real-time environments — plus native DDP output for driving LED panels and pixel-mapped fixtures via FPP and compatible controllers.

---

## Features

- **Multi-window D3D output** — one OSR CEF browser renders the full virtual canvas; sub-regions are blitted via `CopySubresourceRegion` to bare Win32 windows on each physical display. Zero DWM thumbnail overhead, no monitor boundary limits on canvas size.
- **Spout output** — OSR `OnAcceleratedPaint` delivers a DXGI shared texture directly to `SpoutDX::SendTexture`. GPU→GPU, zero CPU, zero PCIe traffic. Intel 0% / NVIDIA 15% at 60fps on an Optimus laptop.
- **Spout input** — Win32 named shared memory bridge. Two-tier: tier 1 zero-copy `MapViewOfFile`-backed `ArrayBuffer` (V8 sandbox off), tier 2 persistent-buffer `memcpy` (sandbox safe). Available at `window.__spoutFrameBuffer` / `window.__spoutGetFrame` in the browser.
- **DDP output** — native DDP (Distributed Display Protocol) over UDP for driving LED panels, pixel strips, and matrix displays via [Falcon Player (FPP)](https://falconchristmas.com/), WLED, and other DDP-compatible controllers. Map any sub-region of the virtual canvas to a controller — multiple DDP outputs fan out independently for pixel-mapping across unique fixtures at different IP addresses. Features dirty-row detection (only changed rows are sent), keepalive resend, and per-output stats.
- **Unified frame transport** — all outputs (D3D, Spout, DDP, and future protocols like ArtNet, sACN, DMX) share a common `IOutputProtocol` architecture. GPU-native outputs (Spout) receive the shared texture directly; pixel-based outputs (DDP) use staging readback only when needed. Adding a new protocol is a single class implementation.
- **`display-config.json`** — human-readable config file for window layout, canvas size, content URL, Spout settings, DDP outputs, interactive/output mode defaults.
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
└── libNativeWrapper.dll   C++ CEF/D3D11/Spout/DDP wrapper
      ├── CEF OSR browser  (OnAcceleratedPaint → shared DXGI texture)
      ├── D3D11 output     (CopySubresourceRegion → NativeDisplayWindow swap chains)
      ├── Frame transport  (unified fan-out to all output protocols)
      │     ├── Spout      (GPU path — SpoutDX::SendTexture → TouchDesigner / Resolume)
      │     ├── DDP        (CPU path — staging readback → UDP to FPP / WLED controllers)
      │     └── (future)   ArtNet · sACN · DMX · NDI — same IOutputProtocol interface
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
  frame-output/          Unified frame transport module
    core/                IOutputProtocol interface, FrameOutputManager, frame types
    protocols/ddp/       DDP output (UDP to FPP/WLED controllers)
    protocols/spout/     Spout output (GPU texture sharing via SpoutDX)
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
  "spoutInput":  { "senderName": "TD_Spout_Sender" },

  // DDP outputs — map virtual canvas regions to LED controllers
  "ddpOutputs": [
    {
      "label": "LED panel A",
      "controllerAddress": "192.168.1.100",
      "port": 4048,
      "source": { "x": 0, "y": 0, "width": 32, "height": 16 }
    },
    {
      "label": "LED strip B",
      "controllerAddress": "192.168.1.101",
      "source": { "x": 32, "y": 0, "width": 1, "height": 300 },
      "zigZagRows": true
    }
  ]
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
| **F12** | Toggle browser DevTools (undocked window) |
| **Escape** | Quit cleanly |

---

## Glossary

| Term | Definition |
|---|---|
| **ANGLE** | Google's graphics abstraction layer. Translates OpenGL ES calls to platform-native APIs. Chromeyumm uses the `d3d11` backend so CEF renders via Direct3D 11 on Windows. |
| **Blitting** | Copying pixel data from one surface to another. In Chromeyumm, `CopySubresourceRegion` blits sub-regions of the shared texture to each display window's swap chain — a GPU-side copy with no CPU involvement. |
| **Bun** | A fast JavaScript/TypeScript runtime (alternative to Node.js). Chromeyumm uses Bun for its native `dlopen` FFI, single-binary compilation (`bun build --compile`), and fast startup. |
| **CEF** | Chromium Embedded Framework — a C++ library for embedding a Chromium browser in applications. Provides the full web engine (HTML, CSS, JS, WebGL, WebGPU) without the Chrome UI. Chromeyumm uses CEF in OSR mode to render web content to a GPU texture instead of a window. |
| **D3D11** | Direct3D 11, Microsoft's GPU graphics API. Used for texture creation, swap chain management, and GPU-to-GPU copies. All display output in Chromeyumm goes through D3D11. |
| **DDP** | Distributed Display Protocol — a UDP protocol for sending RGB pixel data to LED controllers. Each packet carries up to 480 pixels (1440 bytes RGB). Used by FPP (Falcon Player), WLED, and other lighting controllers. |
| **DWM** | Desktop Window Manager — the Windows compositor that manages window rendering. Chromeyumm avoids DWM overhead by using OSR mode and direct GPU blitting instead of window capture. DWM thumbnails are used only for NativeDisplayWindow mirroring. |
| **DXGI** | DirectX Graphics Infrastructure — the layer beneath D3D11 that manages swap chains, adapters, and shared texture handles. `IDXGISwapChain1` is the flip-model swap chain, and DXGI NT handles enable cross-device texture sharing. |
| **FFI** | Foreign Function Interface — a mechanism for calling native C/C++ functions from a higher-level language. Bun's `dlopen` FFI loads `libNativeWrapper.dll` and calls exported C functions directly, with no IPC or serialization overhead. |
| **FPP** | Falcon Player — open-source LED control software that runs on Raspberry Pi and BeagleBone. Accepts DDP input over the network and drives WS2812, APA102, and other LED strips/panels. |
| **Frame transport** | Chromeyumm's unified output architecture. `OnAcceleratedPaint` delivers each frame to a `FrameOutputManager` which fans it out to all registered outputs (Spout, DDP, future protocols). GPU outputs receive the texture directly; CPU outputs trigger a staging readback only when needed. |
| **IOutputProtocol** | The C++ interface that all frame output protocols implement. Methods: `Start()`, `Stop()`, `OnFrame()` (CPU path), `OnGpuFrame()` (GPU path). Adding a new output protocol means implementing this single interface. |
| **NDW** | NativeDisplayWindow — a bare Win32 window (no chrome, no taskbar entry) positioned on a physical display. Content is delivered via D3D11 blit or DWM thumbnail, not by CEF rendering into it. |
| **OnAcceleratedPaint** | A CEF callback fired every rendered frame in OSR mode with `shared_texture_enabled=1`. Delivers a DXGI NT shared texture handle — the GPU texture containing the browser's rendered content. This is the single entry point for all frame output. |
| **Optimus** | NVIDIA's hybrid GPU technology (Intel iGPU + NVIDIA dGPU). Requires `in-process-gpu: true` in CEF and careful D3D device management — the CEF compositor may render on a different GPU than the one Spout or D3D output uses. |
| **OSR** | Off-Screen Rendering — a CEF mode where the browser renders to a texture instead of a window. Combined with `shared_texture_enabled=1`, this delivers a DXGI shared texture handle every frame with zero CPU readback. |
| **Pixel mapping** | The process of mapping regions of a 2D canvas to physical LED fixtures. In Chromeyumm, each DDP output defines a `source` rect (which part of the virtual canvas to sample) and a controller address (where to send the pixel data). Multiple outputs can map different regions to different controllers. |
| **Spout** | A real-time GPU texture sharing framework for Windows. Chromeyumm sends textures via `SpoutDX::SendTexture` (output) and receives via `SpoutDX::ReceiveTexture` (input). Used for integration with TouchDesigner, Resolume, and other real-time tools. |
| **Staging texture** | A D3D11 texture with `D3D11_USAGE_STAGING` and `CPU_ACCESS_READ`. Used to copy GPU texture data to CPU memory for protocols that need pixel access (DDP). Only created when CPU-path outputs are active. |
| **Swap chain** | A DXGI object (`IDXGISwapChain1`) that manages front/back buffers for a window. Each NativeDisplayWindow has its own swap chain. `Present(0, ALLOW_TEARING)` flips the buffer to the screen without vsync. |
| **Virtual canvas** | The full rendering surface defined by `virtualCanvas` in `display-config.json`. The browser renders at this resolution; display windows and DDP outputs each sample a sub-region of it. |

---

