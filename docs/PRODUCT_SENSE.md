# Product Sense

## Target Users

1. **Live visual artists / VJs** — Run web-based visuals across multiple projectors or LED walls at events, installations, and performances. Need reliable multi-output with zero frame drops.

2. **Creative technologists** — Integrate web rendering into larger AV pipelines (TouchDesigner, Resolume, MadMapper) via Spout. Need GPU→GPU texture sharing without CPU overhead.

3. **Installation designers** — Deploy permanent or semi-permanent multi-display installations in museums, retail, or public spaces. Need config-driven layout, auto-recovery from monitor topology changes.

## Value Propositions

| Value | How Delivered |
|---|---|
| Zero-CPU frame delivery | OSR `OnAcceleratedPaint` → DXGI shared texture → `CopySubresourceRegion` (D3D) or `SpoutDX::SendTexture` (Spout). No pixel readback on the hot path. |
| Web content everywhere | Full Chromium engine — any WebGL/WebGPU/Canvas content. Use existing Three.js/R3F/p5.js/etc. or standard HTML/CSS. |
| Multi-display, no limits | Virtual canvas of any size. Each display window samples a sub-region. No monitor boundary restrictions. |
| AV tool integration | Spout I/O for bidirectional GPU texture sharing with TouchDesigner, Resolume, OBS (via Spout plugin), etc. |
| JSON-driven layout | `display-config.json` defines everything. No code changes to reconfigure for a different venue. |
| Lightweight distrib | ~350MB vs Electron's ~500MB+. Bun runtime + CEF prebuilt. |

## Product Tradeoffs

| Tradeoff | Decision | Rationale |
|---|---|---|
| Windows only | Accepted | DXGI shared textures, Spout, and Win32 NDWs are all Windows APIs. Cross-platform would require different GPU sharing approaches per OS. |
| No RPC / IPC abstraction | Accepted | Direct FFI is simpler, faster, and sufficient for a single-app use case. No multi-window messaging needed. |
| Electrobun heritage debt | Accepted (being stripped) | ~900 lines of unused code (ASAR, WGPU shims, update/packaging). Planned removal. |
| Spout input CPU cost | Accepted | +10% Intel / +30% NVIDIA is inherent to CPU-mediated readback. GPU-direct paths investigated but blocked by cross-device EGL limitations. |
| OSR master always hidden in output mode | Accepted | Master HWND is black in OSR — content only in shared texture. Ctrl+M toggles to interactive mode to see the master window. |
| Interactive mode is not a full browser | Accepted | OSR requires manual input forwarding — CEF never receives events natively. Common inputs work, but native dropdowns, drag-and-drop, IME, and context menus are absent or degraded. Chromeyumm is not suitable as an interactive UI surface; interactive mode exists for development convenience only. |

## Non-Goals

- Cross-platform support (macOS, Linux)
- General-purpose browser or Electron replacement
- Multi-page / tab browsing
- Node.js addon compatibility (Bun FFI only)
- Chromium extension support

## Related

- [DESIGN.md](DESIGN.md) — Design principles
- [ARCHITECTURE.md](../ARCHITECTURE.md) — System architecture
- [product-specs/](product-specs/index.md) — Feature specifications
