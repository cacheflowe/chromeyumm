# Decision: Spout Input via Win32 Shared Memory

## Status
Accepted

## Context
Need to receive Spout textures (from TouchDesigner, Resolume, etc.) and make them available as pixel data in the browser's JavaScript context. The challenge: Spout textures live on the NVIDIA GPU, but WebGL textures live on whatever GPU ANGLE is using (often Intel on Optimus).

## Options Considered

### GPU-direct import (EGL_ANGLE_d3d_texture_client_buffer)
- **Pro**: Zero-copy GPU texture sharing
- **Con**: Requires both Spout and ANGLE to use the same D3D11 device. On Optimus, Spout runs on NVIDIA, ANGLE runs on Intel (different GPU). Cross-device texture sharing is not supported by `EGL_ANGLE_d3d_texture_client_buffer`.
- **Con**: `EGL_ANGLE_device_d3d` could theoretically force ANGLE to the NVIDIA device, but would require CEF source modifications.
- **Result**: Not viable without CEF changes.

### CPU-mediated shared memory bridge
- **Pro**: Works across any GPU configuration
- **Pro**: Two tiers handle V8 sandbox on/off automatically
- **Con**: Two PCIe crossings per frame (NVIDIA readback + Intel WebGL upload)
- **Con**: +10% Intel / +30% NVIDIA overhead

## Decision
CPU-mediated shared memory bridge with two tiers.

## Implementation

### C++ side (`cef-wrapper.cpp`)
- Receiver thread: `SpoutDX::ReceiveTexture` → `CopyResource` to staging → `Map(DO_NOT_WAIT)` retry loop → `memcpy` to shared memory → `_InterlockedIncrement(seq)`
- NVIDIA adapter selected via DXGI enumeration (VendorId `0x10DE`) to avoid cross-adapter penalty
- Win32 named file mapping: `SpoutFrame_<id>`

### CEF renderer side (`cef-helper.cpp`)
- `OnContextCreated`: parse `?spoutReceiverId` from URL → open file mapping
- **Tier 1**: `CefV8Value::CreateArrayBuffer` backed by `MapViewOfFile` → `window.__spoutFrameBuffer` (zero-copy, but returns nullptr when V8 sandbox is on)
- **Tier 2**: `SpoutFrameHandler::Execute` → copies into persistent ArrayBuffer → `window.__spoutGetFrame()` (sandbox-safe)

### Browser side (`spout-input-receiver.ts`)
- `createSpoutInputReceiver()` tries tier 1, falls back to tier 2
- `poll()` called each `requestAnimationFrame` — checks sequence counter, reads frame if new

## Consequences
- ~30% NVIDIA overhead is inherent to CPU readback — acceptable tradeoff
- Tier selection is automatic — no configuration needed
- Shared memory name is predictable (`SpoutFrame_<id>`) — fine for single-user installations

## Related
- [osr-shared-texture.md](osr-shared-texture.md) — Spout output uses the same OSR texture
- [FRONTEND.md](../FRONTEND.md) — Browser-side Spout API
- [BACKEND.md](../BACKEND.md) — C++ implementation notes
