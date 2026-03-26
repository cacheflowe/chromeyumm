# Feature: Spout Input

## User Story
As a creative technologist, I want to receive Spout textures from external tools (TouchDesigner, Resolume) and use them as textures in my web-based visual content, so I can create bidirectional GPU pipelines between web rendering and traditional AV tools.

## Activation
Add `"spoutInput"` to `display-config.json`:
```json
{
  "spoutInput": { "senderName": "TD_Spout_Sender" }
}
```

Works in both D3D output and Spout output modes.

## Data Flow

```
External Spout Sender (TouchDesigner, etc.)
  │
  ▼ SpoutDX::ReceiveTexture (NVIDIA D3D11 device)
  │
  ▼ CopyResource → staging texture
  │
  ▼ Map(DO_NOT_WAIT) retry loop → memcpy → Win32 shared memory
  │
  ▼ _InterlockedIncrement(seq) — release barrier
  │
  ▼ CEF renderer (cef-helper.cpp OnContextCreated)
  │  parse ?spoutReceiverId → OpenFileMappingA
  │
  ├── Tier 1: CefV8Value::CreateArrayBuffer (MapViewOfFile-backed)
  │           → window.__spoutFrameBuffer (zero-copy, V8 sandbox off)
  │
  └── Tier 2: SpoutFrameHandler::Execute
              → window.__spoutGetFrame() (persistent-buf memcpy, sandbox safe)
  │
  ▼ Browser JavaScript
    createSpoutInputReceiver() → poll() each requestAnimationFrame
```

## Shared Memory Layout
```
[0-3]   seq      — Int32LE; incremented after pixels written
[4-7]   width    — Uint32LE
[8-11]  height   — Uint32LE
[12-15] reserved
[16+]   BGRA pixels (~31.6 MB at 3840×2160)
```

## Performance Cost
| Metric | Without Spout Input | With Spout Input | Delta |
|---|---|---|---|
| Intel GPU | ~25% | ~35% | +10% |
| NVIDIA GPU | ~35% | ~65% | +30% |

The +30% NVIDIA cost is `CopyResource` + staging `Map/memcpy` readback. The +10% Intel is `texSubImage2D` upload. Two PCIe crossings (NVIDIA readback + WebGL upload) are unavoidable without GPU-direct texture import.

## Edge Cases
- **Spout sender not running**: Receiver thread loops waiting. No crash, no timeout. Frame data stays stale.
- **V8 sandbox on**: Tier 1 fails gracefully (CreateArrayBuffer returns nullptr), falls back to tier 2.
- **Sender resolution changes**: C++ updates width/height in shared memory header. Browser code must check dimensions on each poll.
- **Multiple receivers**: Not supported — one receiver per app instance.

## Critical Implementation Detail
The `Map(DO_NOT_WAIT)` retry must be an **inner loop** that does NOT call `ReceiveTexture()` again. Calling `ReceiveTexture()` in an outer retry loop resets the internal sequence and loses the texture data.

## Out of Scope
- GPU-direct texture import (blocked by cross-device EGL limitations on Optimus)
- Multiple simultaneous Spout inputs
- Spout input resolution scaling/remapping

## Related
- [spout-output.md](spout-output.md) — Spout output feature
- [../design-docs/spout-input-shared-memory.md](../design-docs/spout-input-shared-memory.md) — Architecture decision
- [../FRONTEND.md](../FRONTEND.md) — Browser-side API docs
