# App Layer Diagram

This page explains the main runtime layers in Chromeyumm and how pixels and control messages move between them.

## Layered View

```mermaid
flowchart TB
  subgraph Browser[CEF Browser Content]
    HTML[HTML / CSS / JS View Three.js / p5 / R3F / DOM]
    Injected[Injected Runtime State window.__chromeyumm debug panel helpers]
    WebAPIs[Canvas / WebGL / WebGPU / Video]
    HTML --> WebAPIs
    Injected --> HTML
  end

  subgraph CEF[CEF Offscreen Rendering]
    Render[Chromium renderer + GPU pipeline]
    OSR[OnAcceleratedPaint shared DXGI texture handle]
    Render --> OSR
  end

  subgraph TS[TypeScript App Layer]
    App[src/app/index.ts config + startup + orchestration]
    Framework[src/chromeyumm/* BrowserWindow / Webview / shortcuts / Screen]
    FFI[src/chromeyumm/ffi.ts Bun dlopen symbol table]
    App --> Framework
    Framework --> FFI
  end

  subgraph Native[Native DLL Layer]
    Wrapper[native/cef-wrapper.cpp CEF + Win32 + D3D11 + Spout]
    FrameOutput[native/frame-output/* protocol output manager]
    Wrapper --> FrameOutput
  end

  subgraph Outputs[Output / Transport Layer]
    D3D[D3D display transport CopySubresourceRegion to display windows]
    Spout[Spout transport SendTexture to external apps]
    DDP[Protocol transport DDP UDP output]
  end

  HTML -. rendered by .-> Render
  OSR --> Wrapper
  FFI <--> Wrapper
  Wrapper --> D3D
  Wrapper --> Spout
  FrameOutput --> DDP
```

## What Each Layer Owns

| Layer | Responsibility |
|---|---|
| Browser content | Your installation UI, animation code, shaders, canvases, DOM, media playback |
| CEF OSR | Renders the page offscreen and exposes the GPU texture to the native host |
| TypeScript app | Loads config, creates windows/webviews, registers hotkeys, starts outputs |
| FFI bridge | Typed symbol boundary between Bun/TS and `libNativeWrapper.dll` |
| Native wrapper | Owns Win32 windows, CEF integration, D3D device/context, Spout receiver, accelerated paint hook, D3D display blitting |
| Frame-output module | Owns all transport outputs: GPU-native protocols (Spout sender) and CPU-readback protocols (DDP, etc.). Performs staging readback at most once per frame, shared across CPU-path outputs. |
| Output transports | Move pixels to physical displays, Spout consumers, or network protocols |

## Frame Flow

```mermaid
flowchart LR
  A[HTML view draws frame] --> B[CEF GPU render]
  B --> C[OnAcceleratedPaint]
  C --> D[Shared D3D texture opened in native layer]
  D --> E1[D3D display path sub-rect blits to NativeDisplayWindow swap chains]
  D --> E2[Spout path SendTexture to TouchDesigner / Resolume / etc.]
  D --> E3[Frame-output path copy to staging texture + CPU readback]
  E3 --> F[DDP packetization]
  F --> G[FPP / DDP receiver]
```

## Control Flow

```mermaid
sequenceDiagram
  participant Config as display-config.json
  participant App as src/app/index.ts
  participant FFI as ffi.ts
  participant Native as libNativeWrapper.dll
  participant CEF as CEF browser
  participant Output as D3D / Spout / DDP

  Config->>App: load layout + output settings
  App->>FFI: create window / webview / outputs
  FFI->>Native: call exported symbols
  Native->>CEF: create OSR browser
  CEF-->>Native: accelerated frame texture
  Native-->>Output: route pixels to active transports
  Native-->>FFI: navigation / event callbacks
  FFI-->>App: did-navigate and other events
  App-->>CEF: inject state + debug helpers
```

## Transport Modes

This view separates the four pixel transport paths that are easy to conflate:

- Spout input: external GPU texture into Chromeyumm, then into browser JavaScript
- Spout output: Chromeyumm-rendered texture out to external GPU consumers
- D3D display output: Chromeyumm-rendered texture out to local display windows
- DDP output: Chromeyumm-rendered texture read back and packetized for network transport

```mermaid
flowchart LR
  subgraph ExternalIn[External Producer]
    TDIn[TouchDesigner / Resolume / Spout sender]
  end

  subgraph Native[Native Layer]
    SpoutRx[Spout receiver GPU receive + shared memory publish]
    SharedMem[Win32 shared memory SpoutFrame_<id>]
    OSR[CEF shared texture from OnAcceleratedPaint]
    SpoutTx[Spout sender SendTexture]
    D3DOut[D3D transport CopySubresourceRegion]
    Readback[Staging texture + CPU readback]
    DDPPack[DDP packetizer]
  end

  subgraph Browser[Browser Content]
    View[HTML / JS view]
    SpoutAPI[Injected Spout input API __spoutFrameBuffer / __spoutGetFrame]
  end

  subgraph ExternalOut[Destinations]
    Displays[NativeDisplayWindow swap chains physical displays]
    TDOut[TouchDesigner / Resolume / Spout receiver]
    FPP[FPP / DDP controller]
  end

  TDIn --> SpoutRx
  SpoutRx --> SharedMem
  SharedMem --> SpoutAPI
  SpoutAPI --> View

  View --> OSR
  OSR --> D3DOut
  D3DOut --> Displays

  OSR --> SpoutTx
  SpoutTx --> TDOut

  OSR --> Readback
  Readback --> DDPPack
  DDPPack --> FPP
```

## Reading The Transport Diagram

1. Spout input is the only path that starts outside the app and ends inside the browser content.
2. D3D output, Spout output, and DDP output all start from the same rendered CEF texture.
3. Spout output stays GPU-native — it runs inside the frame-output manager's GPU path, no CPU round-trip.
4. DDP output requires a staging-texture readback (CPU path) before packetization. The readback is done once and shared across all active CPU-path outputs.
5. The HTML view does not send DDP or Spout directly. It only renders pixels or consumes injected input data.
6. All output transports are independent and can be active simultaneously for the same webview.

## OnAcceleratedPaint Internals

This is the lower-level native path inside `native/cef-wrapper.cpp` after CEF hands over an accelerated frame.

```mermaid
flowchart TD
  A[CEF calls OnAcceleratedPaint with DXGI shared texture handle] --> B[Look up per-webview native state SpoutWindowState / D3DOutputState]
  B --> C[Open shared handle as ID3D11Texture2D on native D3D device]

  C --> FOM

  subgraph FOM[Step 0 — Frame-output manager ProcessSharedFrame]
    FOM_GPU{GPU-path outputs active e.g. Spout?}
    FOM_GPU -- yes --> FOM_SPOUT[Dispatch GpuFrameView to GPU outputs SendTexture etc.]
    FOM_GPU -- no --> FOM_CPU

    FOM_CPU{CPU-path outputs active e.g. DDP?}
    FOM_CPU -- yes --> FOM_STAGE[Ensure staging texture matches canvas size]
    FOM_STAGE --> FOM_COPY[CopyResource sharedTex to stagingTex]
    FOM_COPY --> FOM_MAP[Map staging texture for CPU read]
    FOM_MAP --> FOM_DISPATCH[Dispatch BgraFrameView + FrameContext to CPU outputs]
    FOM_DISPATCH --> FOM_UNMAP[Unmap staging texture]
    FOM_CPU -- no --> FOM_DONE[Skip CPU readback]
  end

  FOM --> SC{Step 1 — Swap chain for DWM thumbnailing}
  SC -- active --> SC2[CopyResource into master swap chain back buffer]
  SC2 --> SC3[Present with tearing flag]
  SC -- inactive --> D3D

  SC3 --> D3D{Step 2 — D3D display slots active?}
  D3D -- yes --> D3D2[For each slot CopySubresourceRegion to NDW back buffer]
  D3D2 --> D3D3[Present NDW swap chain]
  D3D3 --> END[End frame]
  D3D -- no --> END
```

## Reading The Native Frame Path

1. The shared texture is the central artifact — every output path branches from the same GPU texture.
2. The frame-output manager (step 0) owns both GPU-native protocols (Spout) and CPU-readback protocols (DDP). It performs at most one staging-texture readback per frame, shared across all active CPU-path outputs.
3. Spout output stays GPU-to-GPU — `SendTexture` inside the frame-output manager, no CPU round-trip.
4. D3D display output (step 2) is separate from the frame-output manager — it uses `CopySubresourceRegion` directly from the shared texture into each NativeDisplayWindow swap chain.
5. All three steps happen on the same frame — Spout, DDP, and D3D multi-window can all be active simultaneously.

## Key Distinction

There are two different "bridges" in this architecture:

1. The control bridge is the FFI boundary between Bun/TypeScript and the native DLL. This is how the app starts windows, outputs, and native services.
2. The graphics bridge is the shared texture path from CEF OSR into the native D3D layer. This is how rendered pixels move efficiently without going through JSON, base64, or browser-side networking.

That distinction matters because Spout and DDP do not originate in the HTML layer directly. The HTML layer only renders pixels. The native layer decides how those pixels are transported onward.

## Related

- [../../ARCHITECTURE.md](../../ARCHITECTURE.md) — System context and primary architecture notes
- [../BACKEND.md](../BACKEND.md) — Native wrapper, FFI, and output implementation details
- [../FRONTEND.md](../FRONTEND.md) — Browser-side view architecture