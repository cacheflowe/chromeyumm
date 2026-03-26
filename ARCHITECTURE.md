# Chromeyumm — Architecture

## System Context

Chromeyumm is a specialized CEF (Chromium Embedded Framework) browser for live visual installations. It renders a single web page to a virtual canvas and distributes sub-regions to physical displays via GPU blitting, or shares the full texture via Spout for integration with VJ/AV tools.

```
┌─────────────────────────────────────────────────────────────────┐
│  Bun Runtime (bun.exe / chromeyumm.exe)                        │
│  ┌──────────────────────────┐  ┌─────────────────────────────┐ │
│  │  TypeScript App           │  │  libNativeWrapper.dll       │ │
│  │  src/app/index.ts         │  │  native/cef-wrapper.cpp     │ │
│  │  src/chromeyumm/ (FFI)    │──│  CEF, D3D11, Spout, Win32   │ │
│  └──────────────────────────┘  └─────────────────────────────┘ │
│                                         │                       │
│                    ┌────────────────────┼────────────────────┐  │
│                    │                    │                    │  │
│                    ▼                    ▼                    ▼  │
│           ┌──────────────┐   ┌──────────────┐   ┌───────────┐ │
│           │ CEF OSR      │   │ D3D11 Output │   │ SpoutDX   │ │
│           │ Browser      │   │ NativeDisplay│   │ Sender/   │ │
│           │ (shared tex) │──▶│ Windows ×N   │   │ Receiver  │ │
│           └──────────────┘   └──────────────┘   └───────────┘ │
└─────────────────────────────────────────────────────────────────┘
                                    │                    │
                                    ▼                    ▼
                            Physical Displays    TouchDesigner /
                            (Win32 HWNDs)        Resolume / etc.
```

## Two Operating Modes

Mode is determined by presence/absence of `"spoutOutput"` in `display-config.json`.

### Multi-Window D3D Output (default)

```
Master BrowserWindow (OSR, shared_texture_enabled=1)
  └── renders full virtual canvas (arbitrary size)
  └── OnAcceleratedPaint → DXGI NT shared texture handle
  └── CopySubresourceRegion(srcBox) → each NDW's D3D11 swap chain → Present

NativeDisplayWindow × N (lightweight Win32 HWNDs)
  └── D3D11 swap chain per window
  └── content driven by GPU blit from master OSR texture
  └── alwaysOnTop, suppresses cursor and close events
  └── zero CPU per frame — pure GPU CopySubresourceRegion
```

### Spout Output

```
Master BrowserWindow (OSR, shared_texture_enabled=1)
  └── OnAcceleratedPaint → DXGI NT shared texture handle
  └── OpenSharedResource1 → ID3D11Texture2D on SpoutDX device
  └── SpoutDX::SendTexture → GPU→GPU zero-copy to receivers
```

GPU result: Intel 0%, NVIDIA 15% at 60fps on Optimus hardware. No CPU involvement.

## Data Ownership

| Data | Owner | Location |
|---|---|---|
| Display layout & config | `display-config.json` | Project root (found via cwd walk-up at runtime) |
| Virtual canvas geometry | `src/app/config.ts` | Computed from config or auto-detected |
| CEF browser lifecycle | `src/chromeyumm/browser-window.ts` | TypeScript wrapper over FFI |
| D3D11 device & swap chains | `native/cef-wrapper.cpp` | `D3DOutputState`, `D3DOutputSlot` structs |
| Spout sender/receiver state | `native/cef-wrapper.cpp` | `SpoutWindowState`, `SpoutInputState` structs |
| Shared memory (Spout input) | `native/cef-wrapper.cpp` | Win32 named file mapping `SpoutFrame_<id>` |
| V8 bindings (Spout input) | `native/cef-helper.cpp` | `OnContextCreated` tier 1/2 injection |
| Global shortcuts | `src/chromeyumm/shortcut.ts` | FFI to C++ `setGlobalShortcutCallback` |
| Event routing (C++ → TS) | `src/chromeyumm/ffi.ts` | `eventBridgeCallback` → per-webview listener map |

## End-to-End Flow: Frame Render to Display

1. **CEF renders** the web page (Three.js / R3F / any HTML) to a GPU texture via OSR with `shared_texture_enabled=1` and `use-angle=d3d11`.
2. **`OnAcceleratedPaint`** fires in `cef-wrapper.cpp` with a DXGI NT shared texture handle.
3. **`OpenSharedResource1`** opens the shared texture as an `ID3D11Texture2D` on the D3D11 device.
4. **D3D output path**: For each `D3DOutputSlot`, `CopySubresourceRegion` with a source box blits the relevant sub-region to the slot's swap chain back buffer, then `Present`.
5. **Spout output path**: `SpoutDX::SendTexture` shares the texture GPU→GPU with any Spout receiver.
6. **Result**: Physical displays show their portion of the virtual canvas with zero CPU overhead per frame.

## Domain Boundaries

```
src/
├── chromeyumm/          Framework layer (FFI, classes, event routing)
│   ├── ffi.ts           Bun dlopen → libNativeWrapper.dll (single source of truth for FFI)
│   ├── browser-window.ts  BrowserWindow lifecycle + D3D/Spout control
│   ├── webview.ts       Webview (JS execution, event listeners)
│   ├── native-display-window.ts  NativeDisplayWindow (Win32 HWND wrapper)
│   ├── shortcut.ts      GlobalShortcut registration
│   ├── screen.ts        Screen.getAllDisplays()
│   └── spout.ts         SpoutReceiver start/stop
│
├── components/          Reusable web components (drop into any page)
│   ├── debug-panel.js   <debug-panel> — debug overlay with perf, Spout, keys
│   ├── slot-overlay.js  <slot-overlay> — coordinate grid + slot boundaries
│   ├── spout-receiver.js <spout-receiver> — Spout input with WebGL rendering
│   ├── spout-video.js   <spout-video> — Spout frames as <video> element
│   └── layout-params.js parseLayoutParams() utility
│
├── app/                 Application logic (not reusable framework)
│   ├── index.ts         Entry point: config → windows → shortcuts → event loop
│   ├── config.ts        display-config.json loader + virtual canvas resolver
│   └── spout-input.ts   SpoutInput lifecycle wrapper
│
└── views/               Demo view projects (run via Vite dev server)
    ├── threejs/         Three.js full-canvas renderer
    ├── r3f/             React Three Fiber app (Vite-built)
    ├── spout-demo/      Spout input demo
    └── spout-video/     Video + Spout demo

native/
├── cef-wrapper.cpp      Main DLL: CEF, D3D11, Spout, Win32 (the "engine")
├── cef-helper.cpp       CEF renderer process helper (V8 bindings)
├── shared/              C++ headers (callbacks, config, parsers, utilities)
└── vendor/              CEF, Spout vendor dirs (gitignored)
```

## Key Architecture Decisions

| Decision | Rationale | Ref |
|---|---|---|
| CEF + Bun over Electron | ~350MB distrib vs ~500MB+, ~50ms cold start, cleaner FFI via Bun dlopen | [docs/design-docs/cef-over-electron.md](docs/design-docs/cef-over-electron.md) |
| OSR with shared_texture_enabled | Only way to get DXGI texture handles for GPU→GPU sharing | [docs/design-docs/osr-shared-texture.md](docs/design-docs/osr-shared-texture.md) |
| Single master browser + GPU blit | One render → N displays via CopySubresourceRegion; no per-window browser overhead | [docs/design-docs/single-master-gpu-blit.md](docs/design-docs/single-master-gpu-blit.md) |
| in-process-gpu on Optimus | GPU subprocess crashes at ~30s without it; GPU thread must run where GpuPreference=2 applies | [DEVELOPMENT.md](DEVELOPMENT.md) |
| Direct FFI (no RPC) | `native.symbols.*` calls directly; no event emitter/bridge abstraction overhead | [docs/design-docs/direct-ffi.md](docs/design-docs/direct-ffi.md) |
| Spout input via shared memory | Two-tier: zero-copy MapViewOfFile (V8 sandbox off) or persistent-buffer memcpy (sandbox safe) | [docs/design-docs/spout-input-shared-memory.md](docs/design-docs/spout-input-shared-memory.md) |

## Related Documents

- [AGENTS.md](AGENTS.md) — Root agent map, quick start, key directories
- [DEVELOPMENT.md](DEVELOPMENT.md) — Deep C++ internals, GPU performance history, build details
- [docs/COMMANDS.md](docs/COMMANDS.md) — All build/run/dev commands
- [docs/BACKEND.md](docs/BACKEND.md) — C++ DLL internals, FFI patterns
- [docs/FRONTEND.md](docs/FRONTEND.md) — Views architecture, browser-side patterns
- [docs/RELIABILITY.md](docs/RELIABILITY.md) — Failure modes, GPU crash recovery
