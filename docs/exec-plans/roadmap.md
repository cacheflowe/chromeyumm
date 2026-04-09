# Roadmap

## Current Focus

### Strip Electrobun Heritage Code
Remove ~900 lines of unused code from `cef-wrapper.cpp`:
- ASAR reading (~200 lines)
- WGPU shims (~400 lines, already `#ifdef`'d)
- Update/packaging machinery (~300 lines)

Approach: strip section by section with a build test after each.

See: [tech-debt-tracker.md](tech-debt-tracker.md)

## Planned

### GitHub Releases
Write `scripts/release.ts` to tag, package, and push versioned releases. Will replace the old `installation-browser/scripts/release.ts`.

### Consolidated Debug Panel
Single Ctrl+D panel with: fps, draw calls, canvas size, mouse coords, Spout status, mode info, key reference. Replaces scattered `debugEl` + stats.js + slot overlay.

### Rename Helper Processes
Once Bun adds support for custom subprocess names, add `chromeyumm Helper (GPU).exe` and friends to `helperNames` in `src/app/index.ts` for the GPU-preference registry writes. Currently only `chromeyumm.exe` and `chromeyumm Helper.exe` are registered.

## Backlog

- Automated smoke test (DLL loads, FFI symbols resolve)
- CI build pipeline (GitHub Actions, self-hosted runner for MSVC + CEF)
- Crash/error logging to disk
- Add automated system to pull latest Spout / CEF versions into codebase
- Auto-detect monitor topology changes (replace manual Ctrl+Shift+M)

## Completed

- Phase 1: PrintWindow multi-window output
- Phase 2c: Windows Graphics Capture (Spout output)
- Phase 2d: OSR OnAcceleratedPaint (Spout output) — **0% Intel, 15% NVIDIA**
- Phase 5: D3D output (OSR + CopySubresourceRegion)
- Spout input two-tier shared memory bridge
- Detach from Electrobun upstream
- Docs system (harness-engineering model)

## Related

- [tech-debt-tracker.md](tech-debt-tracker.md) — Detailed debt items
- [../QUALITY_SCORE.md](../QUALITY_SCORE.md) — Quality gaps
- [../design-docs/index.md](../design-docs/index.md) — Architecture decisions
