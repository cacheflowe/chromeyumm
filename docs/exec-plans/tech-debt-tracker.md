# Tech Debt Tracker

| Item | Impact | Priority | Owner | Target | Status |
|---|---|---|---|---|---|
| `cef-wrapper.cpp` monolith (~9k lines) | Hard to navigate, single TU compilation time | P2 | — | — | Not started |
| No automated tests | Only an FFI symbol smoke test (`bun run test`) exists — no behavioral coverage | P1 | — | — | Not started |
| `loadURL()` broken | Must use `location.reload()` workaround | P2 | — | — | Known, workaround in place |
| Helper process names incomplete | Only 2 of 6 CEF helpers get GPU preference | P3 | — | Blocked on Bun | Waiting |
| No crash telemetry | Silent failures in production | P2 | — | — | Not started |

## Related

- [roadmap.md](roadmap.md) — Project roadmap (see Completed section for resolved debt)
- [../QUALITY_SCORE.md](../QUALITY_SCORE.md) — Quality gaps
