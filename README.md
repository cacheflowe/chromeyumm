# Chromeyumm

A minimal Windows CEF browser built for live installations. Renders web content across multiple displays via direct D3D11 GPU blitting, with Spout texture I/O for integration with TouchDesigner, Resolume, and other real-time environments.

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

### Prerequisites

- Windows 10/11 x64
- [Bun](https://bun.sh) runtime
- Visual Studio 2022 with MSVC (C++ Desktop workload)
- CEF prebuilt vendor (see `native/README.md`)
- Spout vendor (optional, for Spout output — see `native/README.md`)

### Build

```bash
bun install

# Full build — compiles C++ DLL + bundles TypeScript + copies CEF runtime
bun build.ts

# TypeScript only (no C++ compile) — fast, for TS-only changes
bun build.ts --skip-native

# Development (no minification)
bun build.ts --dev
```

### Run

```bash
dist/chromeyumm.exe
```

Configure `display-config.json` in the project root (found via cwd walk-up at runtime).

---

## Project Structure

```
native/
  cef-wrapper.cpp        Main C++ DLL (CEF, D3D11, Spout, NativeDisplayWindow)
  cef-helper.cpp         CEF renderer process helper (Spout input V8 bindings)
  shared/                Shared C++ headers
  vendor/                CEF + Spout vendor dirs (gitignored — see native/README.md)

src/
  chromeyumm/            Framework layer (replaces electrobun/bun)
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

build.ts                 Build script (replaces Electrobun CLI)
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

  // Spout output (comment out to use multi-window D3D output)
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

## CEF Version Upgrades

1. Download a new CEF prebuilt from [cef-builds.spotifycdn.com](https://cef-builds.spotifycdn.com/index.html) (Windows 64-bit, Standard Distribution)
2. Replace `native/vendor/cef/`
3. Rebuild `libcef_dll_wrapper.lib` (CMake — see `native/README.md`)
4. Run `bun build.ts`
5. The CEF C++ API (`OnAcceleratedPaint`, `CefBrowserSettings`, etc.) is very stable; breaking changes are rare
