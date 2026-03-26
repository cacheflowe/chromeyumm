# Decision: Direct FFI over RPC Abstraction

## Status
Accepted

## Context
The Electrobun upstream uses a `ffi.request` abstraction layer and event emitter classes for TypeScript ↔ C++ communication. This was designed for a general-purpose framework. Chromeyumm is a single-purpose app.

## Options Considered

### Keep Electrobun's RPC/event system
- **Pro**: Familiar if coming from Electrobun
- **Con**: Extra abstraction layer (request/response serialization) for calls that are always synchronous
- **Con**: Event emitter class overhead for simple per-webview callbacks

### Direct `native.symbols.*` calls
- **Pro**: Zero abstraction overhead — TypeScript calls C++ exports directly via Bun `dlopen`
- **Pro**: No serialization — FFI handles type marshaling natively
- **Pro**: Easier to understand — one file (`ffi.ts`) declares all available C++ functions
- **Con**: No request/response lifecycle (but we don't need one)

## Decision
Direct FFI. All C++ exports are declared in `src/chromeyumm/ffi.ts` and called via `native.symbols.*`. Event routing uses a simple `Map<webviewId, Map<event, handler[]>>` instead of a class hierarchy.

## Implementation
- `cs()` helper for string conversion (replaces `toCString()`)
- `nullCallback` JSCallback passed for unused bridge handlers
- Per-webview event handling via `addWebviewListener()` + `eventBridgeCallback`
- No `BuildConfig` — renderer is always `"cef"`, no dynamic config

## Consequences
- Adding a new FFI symbol requires editing `ffi.ts` (declare types) + using `native.symbols.name()`
- No async FFI support (all calls are synchronous) — acceptable for this use case
- Simpler debugging — stack traces go directly from TS to native symbols

## Related
- [BACKEND.md](../BACKEND.md) — Full FFI symbol map
- [references/ffi-patterns.md](../references/ffi-patterns.md) — FFI coding patterns
