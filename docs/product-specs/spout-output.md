# Feature: Spout Output

## User Story
As a creative technologist, I want to share the rendered web canvas as a Spout texture, so I can receive it in TouchDesigner, Resolume, or any Spout-compatible application with zero CPU overhead.

## Activation
Add `"spoutOutput"` to `display-config.json`:
```json
{
  "spoutOutput": { "senderName": "Chromeyumm" }
}
```

When present, D3D multi-window output is disabled (no NativeDisplayWindows created).

## Data Flow

```
Master BrowserWindow (OSR, shared_texture_enabled=1)
  │
  ▼ OnAcceleratedPaint → DXGI NT shared texture handle
  │
  ▼ OpenSharedResource1 → ID3D11Texture2D (on SpoutDX NVIDIA device)
  │
  ▼ SpoutDX::SendTexture → GPU→GPU zero-copy
  │
  ▼ Any Spout receiver (TouchDesigner, Resolume, OBS, etc.)
```

## Performance
- Intel GPU: 0% utilization
- NVIDIA GPU: 15% at 60fps
- Zero CPU involvement, zero PCIe traffic
- Measured on Optimus laptop (RTX 4090 + Iris Xe)

## Build Requirement
SpoutDX static library must be present at `native/vendor/spout/MT/lib/SpoutDX_static.lib`. If absent, `build.ts` compiles without Spout support and `startSpoutSender()` returns false.

## Edge Cases
- **SpoutDX not built**: `startSpoutSender()` returns false. Console warning logged. App continues without Spout output.
- **No Spout receivers running**: Sender runs but texture is not consumed. No performance impact.
- **Sender name collision**: Spout handles this — latest sender wins the name.

## Out of Scope
- Multiple Spout senders (one sender per app instance)
- Spout sender name changes at runtime
- Spout output + D3D multi-window output simultaneously

## Related
- [multi-window-d3d-output.md](multi-window-d3d-output.md) — Alternative output mode
- [spout-input.md](spout-input.md) — Receiving Spout textures
- [../design-docs/osr-shared-texture.md](../design-docs/osr-shared-texture.md) — Why OSR
