# Tech Debt Tracker

| Item | Impact | Priority | Owner | Target | Status |
|---|---|---|---|---|---|
| WebView2 code (~630 lines) | Dead code in cef-wrapper.cpp | P1 | — | — | **Done** |
| ASAR reading code (~200 lines) | Dead code in cef-wrapper.cpp | P1 | — | — | **Done** (already stripped / never ported) |
| WGPU shims (~295 lines) | Dead code — WGPUView class + FFI exports | P2 | — | — | **Done** |
| Update/packaging machinery (~300 lines) | Dead code from Electrobun | P1 | — | — | **Done** (already stripped / never ported) |
| Orphaned headers (asar.h, download_event.h, json_menu_parser.h) | Dead files in native/shared | P2 | — | — | **Done** |
| `cef-wrapper.cpp` monolith (~9k lines) | Hard to navigate, single TU compilation time | P2 | — | — | Not started |
| No automated tests | Manual verification only | P1 | — | — | Not started |
| `loadURL()` broken | Must use `location.reload()` workaround | P2 | — | — | Known, workaround in place |
| Global hotkeys via `RegisterHotKey` | Hijacks system shortcuts; suspend/resume on focus approach works but proper fix is `TranslateAccelerator` / `WM_KEYDOWN` | P3 | — | — | Mitigated (suspend on blur, resume on focus) |
| Helper process names incomplete | Only 2 of 6 CEF helpers get GPU preference | P3 | — | Blocked on Bun | Waiting |
| No crash telemetry | Silent failures in production | P2 | — | — | Not started |
| Scattered debug panel code | `debugEl` + stats.js + slot overlay | P2 | — | — | Not started |

## Stripping Plan

All heritage code has been stripped from `cef-wrapper.cpp`:
1. ~~WebView2 code~~ — **Done** (removed ~630 lines)
2. ~~Update/packaging machinery~~ — **Done** (already stripped / never ported; removed dead includes)
3. ~~ASAR reading~~ — **Done** (already stripped / never ported; removed orphaned `asar.h`)
4. ~~WGPU shims~~ — **Done** (removed ~295 lines: WGPUView class + FFI exports)

After each removal: `bun build.ts` + manual smoke test.

## Related

- [roadmap.md](roadmap.md) — Project roadmap
- [../QUALITY_SCORE.md](../QUALITY_SCORE.md) — Quality gaps
