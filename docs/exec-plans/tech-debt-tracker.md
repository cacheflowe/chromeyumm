# Tech Debt Tracker

| Item | Impact | Priority | Owner | Target | Status |
|---|---|---|---|---|---|
| ASAR reading code (~200 lines) | Dead code in cef-wrapper.cpp | P1 | — | — | Not started |
| WGPU shims (~400 lines) | Dead code (already `#ifdef`'d, but still maintained) | P2 | — | — | Not started |
| Update/packaging machinery (~300 lines) | Dead code from Electrobun | P1 | — | — | Not started |
| `cef-wrapper.cpp` monolith (12k lines) | Hard to navigate, single TU compilation time | P2 | — | — | Not started |
| No automated tests | Manual verification only | P1 | — | — | Not started |
| `loadURL()` broken | Must use `location.reload()` workaround | P2 | — | — | Known, workaround in place |
| Helper process names incomplete | Only 2 of 6 CEF helpers get GPU preference | P3 | — | Blocked on Bun | Waiting |
| No crash telemetry | Silent failures in production | P2 | — | — | Not started |
| Scattered debug panel code | `debugEl` + stats.js + slot overlay | P2 | — | — | Not started |

## Stripping Plan

Strip `cef-wrapper.cpp` heritage code in order:
1. Update/packaging machinery — self-contained section
3. ASAR reading — referenced in `<scheme>Handler::Open`, replace with simpler path
4. WGPU shims — already `#ifdef`'d, just remove the blocks

After each removal: `bun build.ts` + manual smoke test.

## Related

- [roadmap.md](roadmap.md) — Project roadmap
- [../QUALITY_SCORE.md](../QUALITY_SCORE.md) — Quality gaps
