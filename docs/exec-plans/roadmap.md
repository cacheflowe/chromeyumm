# Roadmap

## Current Focus

(No current focus — see Backlog for next priorities.)

## Planned

### Rename Helper Processes
Once Bun adds support for custom subprocess names, add `chromeyumm Helper (GPU).exe` and friends to `helperNames` in `src/app/index.ts` for the GPU-preference registry writes. Currently only `chromeyumm.exe` and `chromeyumm Helper.exe` are registered.

## Backlog
- ~~CI build pipeline~~ → moved to Completed
- DDP output to FPP from p5 example? 
- f12 key no longer working in CEF DevTools — investigate and re-enable
- Crash/error logging to disk
- Auto-detect monitor topology changes (replace manual Ctrl+Shift+M)
- Replace `RegisterHotKey` with window-level accelerators (`TranslateAccelerator` or `WM_KEYDOWN`) — current approach suspends/resumes hotkeys on focus change which works well, but window-level shortcuts would be architecturally cleaner

## Completed

- CI build + release pipeline — GitHub Actions workflow (`.github/workflows/release.yml`) triggers on `v*` tag push or manual dispatch. Caches ~1.5 GB CEF vendors, runs full MSVC build, packages zip, creates GitHub release with auto-generated notes from git log. Local one-command release via `bun run release:publish` (auto-bumps patch, builds, packages, tags, pushes, publishes with changelog).
- Feature-check mode (`bun run feature-check` / `dist/chromeyumm.exe --feature-check`) — launches feature detection page in a 900×800 interactive window, bypassing display-config.json. Tests WebGL/WebGPU, video codecs (with MSE + canPlayType dual detection), hardware APIs, webcam selector with live preview, and Chromeyumm state.
- Relative contentUrl resolution — paths like `src/views/feature-check/index.html` in display-config.json are auto-resolved to `file:///` URLs relative to cwd. Full URLs (`http://`, `file:///`) still work as before.
- Video codec documentation — added codec support table and ffmpeg VP9 conversion guide to FRONTEND.md. CEF standard builds exclude H.264/AAC; use VP9/WebM for video content.
- FFI DLL path resolution — `ffi.ts` now searches exe dir → `dist/` → cwd for `libNativeWrapper.dll`, fixing dev-mode runs.
- Automated smoke test (`bun run test` / `bun scripts/smoke-test.ts`) — verifies DLL loads and all 34 FFI symbols resolve, cross-checks against dumpbin exports when available.
- Permissive chromium flags — added `use-angle=d3d11`, `enable-gpu-rasterization`, `allow-file-access-from-files`, `allow-running-insecure-content`, `disable-site-isolation-trials`, `autoplay-policy=no-user-gesture-required`, `use-fake-ui-for-media-stream`, `enable-usermedia-screen-capturing`, `enable-experimental-web-platform-features`, `enable-webgpu-developer-features`. All overridable via `build.json` chromiumFlags.
- NDW interactive input forwarding — `interactiveWindows: true` in display-config.json enables mouse event forwarding from borderless display windows to CEF (visitor-safe alternative to Ctrl+M interactive mode)
- Hotkey suspend/resume hardening — removed flag-based early returns in `suspendHotkeys`/`resumeHotkeys`, resume now unconditionally unregisters+re-registers to clear stale OS state. Bun keepalive interval reduced from ~12 days to 250ms for reliable threadsafe callback delivery.
- Phase 1: PrintWindow multi-window output
- Phase 2c: Windows Graphics Capture (Spout output)
- Phase 2d: OSR OnAcceleratedPaint (Spout output) — **0% Intel, 15% NVIDIA**
- Phase 5: D3D output (OSR + CopySubresourceRegion)
- Spout input two-tier shared memory bridge
- Detach from Electrobun upstream
- Docs system (harness-engineering model)
- WebView2 removal (stripped all WebView2 code from cef-wrapper.cpp)
- Spout + D3D multi-window coexistence (both output modes run simultaneously, sharing D3D device)
- CEF auto-upgrade (`--latest` / `--check-latest` flags in `setup-vendors.ts`, `bun run upgrade:cef` / `bun run check-updates` in `package.json`)
- GitHub Releases (`scripts/release.ts` — tag, package, publish)
- Strip Electrobun heritage code (WebView2 ~630 lines, WGPU shims ~295 lines, dead includes, orphaned headers)
- Consolidated Debug Panel (merged `<slot-overlay>` into `<debug-panel>`, data-driven hotkeys, auto-injection, `window.__chromeyumm` state object)
- Convert ELECTROBUN references to Chromeyumm — renamed C++ class names, macros, include guards, namespaces, JS bridge globals, and TS identifiers. Historical docs preserved.
- D3D output frame pacing — batched CopySubresourceRegion + Flush + Present(0, ALLOW_TEARING) eliminates vsync stalls and DWM interference (taskbar hover stuttering)

## Related

- [tech-debt-tracker.md](tech-debt-tracker.md) — Detailed debt items
- [../QUALITY_SCORE.md](../QUALITY_SCORE.md) — Quality gaps
- [../design-docs/index.md](../design-docs/index.md) — Architecture decisions
