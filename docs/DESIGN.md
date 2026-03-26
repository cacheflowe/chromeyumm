# Design Principles

## Product Philosophy

Chromeyumm exists at the intersection of web rendering and live AV. The guiding principles:

1. **The plumbing is the product.** Unlike Electron apps where the framework is invisible, Chromeyumm's value is in the GPU pipeline — CEF OSR → DXGI shared textures → D3D11 blit or Spout. Owning the plumbing is an advantage.

2. **GPU-first, CPU-never.** Every frame path must be zero CPU overhead. If a pixel touches the CPU (staging readback, memcpy), it's a compromise to be minimized or eliminated. The performance table in DEVELOPMENT.md is the scorecard.

3. **One browser, many outputs.** The master BrowserWindow renders once. Outputs (display windows, Spout senders) are GPU blits from the shared texture. This scales to N displays with constant render cost.

4. **Configuration over code.** Display layout, canvas size, content URL, and mode are all in `display-config.json`. Changing a 4-display installation to a 2-display one is a JSON edit, not a code change.

5. **Minimal framework surface.** The `src/chromeyumm/` layer exposes only FFI symbols and class wrappers actually used. No abstraction for abstraction's sake. Direct `native.symbols.*` calls over RPC.

## Technical Design Rules

- **ANGLE d3d11 always.** Required for OSR `shared_texture_enabled=1`. Other backends (gl, vulkan) cause VIZ OOM crashes.
- **in-process-gpu on Optimus.** GPU subprocess crashes at ~30s without it. Non-negotiable on NVIDIA+Intel laptops.
- **`dispatch_sync` for all window/webview ops.** C++ window creation must run on the message-loop thread. The `MainThreadDispatcher` enforces this.
- **No cross-platform guards.** This is Windows-only. No `process.platform` checks. Simpler code, fewer branches to test.
- **Vendor dirs are junctions, not copies.** CEF is ~1.5GB. Use `New-Item -ItemType Junction` to point at existing vendor dirs.

## Code Style

- TypeScript: Bun runtime, ESM imports, no transpiler config beyond `tsconfig.json`
- C++: Single-file DLL (`cef-wrapper.cpp`), Win32 API directly, no C++ framework
- FFI: Bun `dlopen` with explicit type declarations in `ffi.ts`
- Config: JSON with `_comment` fields (not JSONC — parsed with `JSON.parse`)
- Naming: `camelCase` in TS, `snake_case` for C++ exports, `PascalCase` for C++ structs/classes

## Related

- [ARCHITECTURE.md](../ARCHITECTURE.md) — System context and data flow
- [PRODUCT_SENSE.md](PRODUCT_SENSE.md) — Users and value props
- [docs/design-docs/](design-docs/index.md) — Individual architecture decisions
