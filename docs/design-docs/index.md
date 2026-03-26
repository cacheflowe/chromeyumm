# Design Docs — Index

Architecture decision records for Chromeyumm. Each doc captures context, options considered, decision, and consequences.

| Doc | Status | Last Reviewed | Summary |
|---|---|---|---|
| [cef-over-electron.md](cef-over-electron.md) | Accepted | 2025-01 | CEF + Bun over Electron for size, startup, and FFI |
| [osr-shared-texture.md](osr-shared-texture.md) | Accepted | 2025-01 | OSR with `shared_texture_enabled=1` for DXGI texture access |
| [single-master-gpu-blit.md](single-master-gpu-blit.md) | Accepted | 2025-01 | One render → N displays via GPU blit |
| [direct-ffi.md](direct-ffi.md) | Accepted | 2025-01 | Direct FFI calls over RPC abstraction |
| [spout-input-shared-memory.md](spout-input-shared-memory.md) | Accepted | 2025-01 | Two-tier shared memory bridge for Spout input |
| [harness-engineering.md](harness-engineering.md) | Reference | 2025-01 | Docs system methodology |

## Related

- [ARCHITECTURE.md](../../ARCHITECTURE.md) — System architecture
- [DESIGN.md](../DESIGN.md) — Design principles
