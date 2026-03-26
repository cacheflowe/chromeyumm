# Decision: CEF + Bun over Electron

## Status
Accepted

## Context
Needed a browser runtime for live visual installations that renders web content across multiple displays with GPU texture sharing (Spout). Electron is the default choice for embedding Chromium, but has tradeoffs for this use case.

## Options Considered

### Electron
- **Pro**: Mature ecosystem, large community, well-documented APIs
- **Con**: ~500MB+ distributable, ~200ms cold start, Node.js native addons for C++ interop (napi, node-gyp complexity)
- **Con**: No direct access to CEF's `OnAcceleratedPaint` or `shared_texture_enabled` — Electron abstracts these away

### CEF + Bun (via Electrobun fork)
- **Pro**: ~350MB distributable, ~50ms cold start
- **Pro**: Bun FFI (`dlopen`) is cleaner than Node native addons — declare symbols in TS, call directly
- **Pro**: Direct CEF API access — `OnAcceleratedPaint` with `shared_texture_enabled=1` gives DXGI texture handles
- **Con**: Own more plumbing (window management, event loop, etc.)
- **Con**: Smaller community, less documentation

## Decision
CEF + Bun. The GPU texture sharing requirement is the deciding factor — Electron doesn't expose `OnAcceleratedPaint` or shared texture handles. The lighter distribution and cleaner FFI are bonuses. Owning the plumbing is acceptable because the plumbing IS the product.

## Consequences
- Must maintain C++ DLL directly (~12k lines)
- No Electron ecosystem (no electron-builder, electron-forge, etc.)
- Must handle CEF version upgrades manually (vendor drop-in)
- Bun FFI requires explicit type declarations for every C++ export

## Related
- [ARCHITECTURE.md](../../ARCHITECTURE.md) — System architecture
- [osr-shared-texture.md](osr-shared-texture.md) — Why OSR is required
