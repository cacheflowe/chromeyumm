# Chromeyumm — Architecture

## System Context

Chromeyumm is a specialized CEF (Chromium Embedded Framework) browser for live visual installations. It renders a single web page to a virtual canvas and distributes sub-regions to physical displays via GPU blitting, or shares the full texture via Spout for integration with VJ/AV tools, or streams pixels to LED controllers over DDP/UDP.

```
┌─────────────────────────────────────────────────────────────────┐
│  Bun Runtime (bun.exe / chromeyumm.exe)                        │
│  ┌──────────────────────────┐  ┌─────────────────────────────┐ │
│  │  TypeScript App           │  │  libNativeWrapper.dll       │ │
│  │  src/app/index.ts         │  │  native/cef-wrapper.cpp     │ │
│  │  src/chromeyumm/ (FFI)    │──│  CEF, D3D11, Spout, Win32   │ │
│  └──────────────────────────┘  └─────────────────────────────┘ │
│                                         │                       │
│               ┌─────────────────────────┼──────────────────┐   │
│               │                         │                  │   │
│               ▼                         ▼                  ▼   │
│     ┌──────────────────┐   ┌──────────────────┐  ┌─────────────┐│
│     │  CEF OSR Browser │   │  D3D11 Output    │  │ Frame-Output││
│     │  (shared texture)│──▶│  NativeDisplay   │  │ Module      ││
│     │                  │   │  Windows ×N      │  │ (DDP, Spout)││
│     └──────────────────┘   └──────────────────┘  └─────────────┘│
└─────────────────────────────────────────────────────────────────┘
              │                      │                  │
              ▼                      ▼                  ▼
       (content render)    Physical Displays    Spout receivers /
                           (Win32 HWNDs)        LED controllers
```

## Transport Outputs

Three independent transports read from the same rendered CEF texture and can all run simultaneously. Each is enabled by its presence in `display-config.json`.

### D3D Multi-Window Output

```
Master BrowserWindow (OSR, use-angle=d3d11, shared_texture_enabled=1)
  └── renders full virtual canvas (any size, no monitor boundary limit)
  └── OnAcceleratedPaint → DXGI NT shared texture handle every frame
  └── Batch CopySubresourceRegion for all slots → Flush → Present(0, ALLOW_TEARING)

NativeDisplayWindow × N (lightweight Win32 HWNDs)
  └── D3D11 swap chain per window; content driven by GPU blit
  └── alwaysOnTop, suppresses cursor and close events
  └── zero CPU per frame — pure GPU CopySubresourceRegion
```

### Spout Output

```
OnAcceleratedPaint → DXGI NT shared texture handle
  └── OpenSharedResource1 → ID3D11Texture2D on SpoutDX device
  └── SpoutDX::SendTexture → GPU→GPU zero-copy to any Spout receiver
```

GPU result: Intel 0%, NVIDIA 15% at 60fps on Optimus hardware. No CPU involvement.

### DDP Output

```
OnAcceleratedPaint → frame-output module (CPU path)
  └── Staging texture readback: GPU → CPU (shared across all CPU-path protocols)
  └── BuildRgbPayload: crop source rect + apply zigzag/flip/rotate → RGB bytes
  └── SendDdpPackets: UDP packetization (480px/packet, PUSH on last)
  └── Skip if frame unchanged; keepalive every 1s
```

CPU path: one staging-texture readback shared across all DDP outputs. Each output crops and packetizes its region independently.

## Data Ownership

| Data | Owner | Location |
|---|---|---|
| Display layout & config | `display-config.json` | Project root (found via cwd walk-up at runtime) |
| Virtual canvas geometry | `src/app/config.ts` | Computed from config or auto-detected |
| CEF browser lifecycle | `src/chromeyumm/browser-window.ts` | TypeScript wrapper over FFI |
| D3D11 device & swap chains | `native/cef-wrapper.cpp` | `D3DOutputState`, `D3DOutputSlot` structs |
| Frame output host state | `native/cef-wrapper.cpp` | `FrameOutputHostState` — shared D3D device for all transport outputs |
| Spout sender state | `native/frame-output/protocols/spout/` | `SpoutOutput` protocol |
| DDP output state | `native/frame-output/protocols/ddp/` | `DdpOutput` per output; `TransportSession` per webview |
| Spout receiver state | `native/cef-wrapper.cpp` | `SpoutInputState` struct |
| Shared memory (Spout input) | `native/cef-wrapper.cpp` | Win32 named file mapping `SpoutFrame_<id>` |
| V8 bindings (Spout input) | `native/cef-helper.cpp` | `OnContextCreated` tier 1/2 injection |
| Global shortcuts | `src/chromeyumm/shortcut.ts` | FFI to C++ `setGlobalShortcutCallback` |
| Event routing (C++ → TS) | `src/chromeyumm/ffi.ts` | `eventBridgeCallback` → per-webview listener map |

## End-to-End Flow: Frame Render to Output

1. **CEF renders** the web page (Three.js / R3F / any HTML) to a GPU texture via OSR with `shared_texture_enabled=1` and `use-angle=d3d11`.
2. **`OnAcceleratedPaint`** fires in `cef-wrapper.cpp` with a DXGI NT shared texture handle.
3. **`OpenSharedResource1`** opens the shared texture as an `ID3D11Texture2D` on the shared D3D11 device (`FrameOutputHostState`).
4. **D3D output path**: For each `D3DOutputSlot`, `CopySubresourceRegion` with a source box blits the sub-region to the slot's swap chain, then `Present`.
5. **Spout output path**: `SpoutDX::SendTexture` shares the texture GPU→GPU with any Spout receiver — no CPU copy.
6. **DDP output path**: One staging-texture readback copies the shared texture to CPU. Each `DdpOutput` crops its source region, converts BGRA→RGB (with optional zigzag/flip/rotate), and sends UDP packets. Frames are skipped when pixel data is unchanged; keepalive sends fire every 1 second to hold LED controller state.

## Domain Boundaries

```
src/
├── chromeyumm/          Framework layer (FFI, classes, event routing)
│   ├── ffi.ts           Bun dlopen → libNativeWrapper.dll
│   ├── browser-window.ts  BrowserWindow lifecycle + D3D/Spout/DDP control
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
└── views/               Demo view projects (each is a standalone Vite project)
    ├── threejs/         Three.js full-canvas renderer
    ├── r3f/             React Three Fiber app
    ├── p5/              p5.js + Matter.js demo
    ├── spout-demo/      Spout input demo
    └── spout-video/     Video + Spout demo

native/
├── cef-wrapper.cpp      Main DLL: CEF, D3D11, Spout input, Win32 (the "engine")
├── cef-helper.cpp       CEF renderer process helper (V8 bindings for Spout input)
├── shared/              C++ headers (callbacks, config, parsers, utilities)
├── frame-output/        Frame transport module (DDP, Spout sender, staging readback)
└── vendor/              CEF, Spout vendor dirs (gitignored)
```

## Key Architecture Decisions

| Decision | Rationale | Ref |
|---|---|---|
| CEF + Bun over Electron | ~350MB distrib vs ~500MB+, ~50ms cold start, cleaner FFI via Bun dlopen | [docs/design-docs/cef-over-electron.md](design-docs/cef-over-electron.md) |
| OSR with shared_texture_enabled | Only way to get DXGI texture handles for GPU→GPU sharing | [docs/design-docs/osr-shared-texture.md](design-docs/osr-shared-texture.md) |
| Single master browser + GPU blit | One render → N displays via CopySubresourceRegion; no per-window browser overhead | [docs/design-docs/single-master-gpu-blit.md](design-docs/single-master-gpu-blit.md) |
| in-process-gpu on Optimus | GPU subprocess crashes at ~30s without it; GPU thread must run where GpuPreference=2 applies | [DEVELOPMENT.md](DEVELOPMENT.md) |
| Direct FFI (no RPC) | `native.symbols.*` calls directly; no event emitter/bridge abstraction overhead | [docs/design-docs/direct-ffi.md](design-docs/direct-ffi.md) |
| Spout input via shared memory | Two-tier: zero-copy MapViewOfFile (V8 sandbox off) or persistent-buffer memcpy (sandbox safe) | [docs/design-docs/spout-input-shared-memory.md](design-docs/spout-input-shared-memory.md) |
| Frame-output module | All CPU/GPU transport protocols share one staging readback and one D3D device per webview, registered via HostServices callbacks | [docs/BACKEND.md](BACKEND.md) |

## Related Documents

- [AGENTS.md](../AGENTS.md) — Root agent map, quick start, key directories
- [DEVELOPMENT.md](DEVELOPMENT.md) — Project history, GPU findings, C++ internals
- [docs/COMMANDS.md](COMMANDS.md) — All build/run/dev commands
- [docs/references/app-layer-diagram.md](references/app-layer-diagram.md) — Layered Mermaid diagrams
- [docs/BACKEND.md](BACKEND.md) — C++ DLL internals, FFI patterns
- [docs/FRONTEND.md](FRONTEND.md) — Views architecture, browser-side patterns
- [docs/RELIABILITY.md](RELIABILITY.md) — Failure modes, GPU crash recovery
