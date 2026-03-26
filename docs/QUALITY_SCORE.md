# Quality Score

## Quality Rubric

| Dimension | Weight | Score (1-5) | Notes |
|---|---|---|---|
| **GPU Performance** | High | 5 | Zero-CPU frame path achieved. Intel 0% / NVIDIA 15% at 60fps (Spout output). D3D output ~25% Intel (inherent DWM floor). |
| **Reliability** | High | 3 | GPU crash recovery via `in-process-gpu`; Ctrl+Shift+M resets displays. No automated watchdog yet. |
| **Build System** | Medium | 4 | Single `bun build.ts` handles everything. `--skip-native` for fast iteration. Vendor setup is manual junctions. |
| **Configuration** | Medium | 4 | `display-config.json` covers layout, content, Spout settings. Auto-detect fallback works. |
| **Code Health** | Medium | 3 | ~1700 lines of Electrobun heritage to strip. `cef-wrapper.cpp` is monolithic at ~12k lines. |
| **Documentation** | Medium | 4 | DEVELOPMENT.md is thorough. Docs system now structured per harness-engineering model. |
| **Testing** | Low | 1 | No automated tests. Manual verification only. |
| **Error Handling** | Low | 2 | Console logging for failures. No structured error reporting or crash telemetry. |
| **Monitoring** | Low | 1 | Debug panel exists but requires page-side JS. No runtime metrics collection. |

## Current Gaps

### P0 — Must Fix

- **No automated tests.** Build verification is manual (`bun build.ts` + run). At minimum, need a smoke test that verifies DLL loads and FFI symbols resolve.

### P1 — Should Fix

- **Heritage code removal.** ~900 lines of unused Electrobun code (ASAR, WGPU shims, packaging) increases maintenance surface and compilation time.
- **Monolithic C++ DLL.** `cef-wrapper.cpp` at 12k lines. Consider splitting D3D output, Spout I/O, and window management into separate translation units (while keeping the single DLL).

### P2 — Nice to Have

- **Consolidated debug panel.** Single Ctrl+D panel with fps, draw calls, canvas size, Spout status, mode info. Replaces scattered `debugEl` + stats.js.
- **Crash telemetry.** Log GPU crashes, recovery attempts, and Spout connection failures to disk.
- **CI build.** GitHub Actions pipeline for `bun build.ts` (requires MSVC + CEF vendor — may need self-hosted runner).

## Performance Benchmarks

| Scenario | Intel GPU | NVIDIA GPU | FPS |
|---|---|---|---|
| Spout output | 0% | 15% | 60 |
| D3D output (multi-window) | ~25% | ~15% | 60 |
| D3D + Spout input | ~35% | ~65% | 60 |

Measured on Optimus laptop (RTX 4090 + Iris Xe). See [DEVELOPMENT.md](../DEVELOPMENT.md) for full GPU performance history.

## Related

- [RELIABILITY.md](RELIABILITY.md) — Failure modes and recovery
- [exec-plans/tech-debt-tracker.md](exec-plans/tech-debt-tracker.md) — Tech debt items
- [DEVELOPMENT.md](../DEVELOPMENT.md) — GPU performance analysis
