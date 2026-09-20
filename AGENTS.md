# Chromeyumm — Agent Map

Minimal Windows CEF browser for live installations. Renders web content across multiple displays via D3D11 GPU blitting, with Spout texture I/O for TouchDesigner/Resolume integration.

## Source Accuracy & Drafting Protocol

NEVER fabricate statistics, data points, or claims not explicitly present in source documents. If a
fact cannot be verified from provided sources, flag it as '[NEEDS SOURCE]' rather than including it.
Cross-reference all data attributions to ensure they match the correct source document and author.

### When drafting documents or conducting research from source materials:

1. ** Read first, write second .** Read all provided source documents fully before drafting. Do not
begin writing until all sources are loaded.
2. ** Maintain a source map .** Track every factual claim, metric, name, or date back to its source.
Present the draft clean (no inline tags), with a "Source Map" appendix listing each claim and its
origin (document name, section/heading).
3. ** Verify before delivering .** For substantive documents (strategy docs, external-facing reports,
review comments, posts, presentations), spawn a verification agent that re-reads each source and
checks every claim in the source map. Mark any unverifiable claim as [UNVERIFIED].
4. ** Separate verified from unverified .** Present the clean draft with unverified claims removed, plus
a separate list of removed claims so I can decide whether to add them back with proper sourcing.
5. ** No invention .** Never generate statistics, percentages, quotes, or specific details not found in
the sources - even if they seem plausible or "directionally correct."

## Quick Start

```bash
bun install
bun scripts/setup-vendors.ts # download CEF + Spout (first time / upgrade)
bun build.ts              # full build (C++ DLL + TS + CEF runtime)
bun build.ts --skip-native  # TS-only rebuild
bun build.ts --dev        # dev build (no minification)
dist/chromeyumm.exe       # run (from project root)
bun scripts/release.ts    # package release zip
```

## Key Directories

| Path | Purpose |
|---|---|
| `native/cef-wrapper.cpp` | Main C++ DLL: CEF, D3D11, Spout, NativeDisplayWindow |
| `native/frame-output/` | Frame transport module (DDP output, Spout sender, staging readback) |
| `native/cef-helper.cpp` | CEF renderer process helper (Spout input V8 bindings) |
| `native/shared/` | Shared C++ headers |
| `native/vendor/` | CEF + Spout vendor dirs (gitignored, see `scripts/setup-vendors.ts`) |
| `src/chromeyumm/` | TypeScript framework layer (FFI bindings, BrowserWindow, etc.) |
| `src/app/` | Application entry point, config loader, Spout input lifecycle |
| `src/components/` | Reusable web components (debug panel, slot overlay, Spout receiver) |
| `src/views/` | Example views (Three.js, R3F, Spout demos) — served via dev servers, not bundled |
| `build.ts` | Build script |
| `scripts/release.ts` | Release packaging — version bump, zip, GitHub publish |
| `.github/workflows/release.yml` | CI: build + release on `v*` tag push |
| `display-config.json` | Display layout, canvas size, content URL, Spout settings |

## Environment

- **OS**: Windows 10/11 x64 only
- **Runtime**: [Bun](https://bun.sh)
- **C++ toolchain**: Visual Studio 2022 with MSVC (C++ Desktop workload)
- **CEF**: Chromium Embedded Framework (vendor drop-in, see `native/README.md`)
- **GPU**: ANGLE d3d11 backend required; `in-process-gpu: true` required on Optimus
- **Spout**: Optional — SpoutDX for GPU texture sharing (vendor, see `native/README.md`)

## Architecture & Docs

| Document | Purpose |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | System context, data flow, domain boundaries |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | Deep C++ internals, GPU findings, project history |
| [docs/COMMANDS.md](docs/COMMANDS.md) | All build/run/dev commands |
| [docs/DESIGN.md](docs/DESIGN.md) | Product and design principles |
| [docs/FRONTEND.md](docs/FRONTEND.md) | Views, browser-side architecture |
| [docs/BACKEND.md](docs/BACKEND.md) | C++ DLL, FFI, native layer patterns |
| [docs/PRODUCT_SENSE.md](docs/PRODUCT_SENSE.md) | Users, value props, tradeoffs |
| [docs/QUALITY_SCORE.md](docs/QUALITY_SCORE.md) | Quality rubric and current gaps |
| [docs/RELIABILITY.md](docs/RELIABILITY.md) | Failure modes, recovery patterns |
| [docs/SECURITY.md](docs/SECURITY.md) | Sandbox, permissions, registry writes |
| [docs/design-docs/](docs/design-docs/index.md) | Architecture decision records |
| [docs/product-specs/](docs/product-specs/index.md) | Feature specifications |
| [docs/exec-plans/](docs/exec-plans/roadmap.md) | Roadmap, active plans, tech debt |
| [docs/references/](docs/references/cef-upgrade.md) | Implementation guides |

## Conventions

- All FFI symbols declared in `src/chromeyumm/ffi.ts`; use `native.symbols.*` directly
- `cs()` helper for FFI string conversion (not `toCString()`)
- No RPC system — `nullCallback` for bridge handlers
- C++ exports verified via `dumpbin /exports dist/libNativeWrapper.dll`
- Config via `display-config.json` (JSON with `_comment` fields, not JSONC)
