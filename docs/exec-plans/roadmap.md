# Roadmap

## Current Focus

(No current focus — heritage stripping complete. See Backlog for next priorities.)

## Planned

### Consolidated Debug Panel
Single Ctrl+D panel with: fps, draw calls, canvas size, mouse coords, Spout status, mode info, key reference. Replaces scattered `debugEl` + stats.js + slot overlay.

### Rename Helper Processes
Once Bun adds support for custom subprocess names, add `chromeyumm Helper (GPU).exe` and friends to `helperNames` in `src/app/index.ts` for the GPU-preference registry writes. Currently only `chromeyumm.exe` and `chromeyumm Helper.exe` are registered.

## Backlog

- Automated smoke test (DLL loads, FFI symbols resolve)
  - Can we test video codes, webcam access, webgpu, serial device api, other related web features we want to ensure?
- CI build pipeline (GitHub Actions, self-hosted runner for MSVC + CEF)
- Crash/error logging to disk
- Add automated system to pull latest Spout / CEF versions into codebase
- Convert ELECTROBUN references to Chromeyumm (e.g., `ELECTROBUN_VERSION` → `CHROMEYUMM_VERSION`)
- Auto-detect monitor topology changes (replace manual Ctrl+Shift+M)

## Completed

- Phase 1: PrintWindow multi-window output
- Phase 2c: Windows Graphics Capture (Spout output)
- Phase 2d: OSR OnAcceleratedPaint (Spout output) — **0% Intel, 15% NVIDIA**
- Phase 5: D3D output (OSR + CopySubresourceRegion)
- Spout input two-tier shared memory bridge
- Detach from Electrobun upstream
- Docs system (harness-engineering model)
- WebView2 removal (stripped all WebView2 code from cef-wrapper.cpp)
- Spout + D3D multi-window coexistence (both output modes run simultaneously, sharing D3D device)
- GitHub Releases (`scripts/release.ts` — tag, package, publish)
- Strip Electrobun heritage code (WebView2 ~630 lines, WGPU shims ~295 lines, dead includes, orphaned headers)

## Related

- [tech-debt-tracker.md](tech-debt-tracker.md) — Detailed debt items
- [../QUALITY_SCORE.md](../QUALITY_SCORE.md) — Quality gaps
- [../design-docs/index.md](../design-docs/index.md) — Architecture decisions
