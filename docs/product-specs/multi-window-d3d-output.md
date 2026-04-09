# Feature: Multi-Window D3D Output

## User Story
As a live visual artist, I want to render a single web canvas and distribute sub-regions to multiple physical displays, so I can run an installation across any number of projectors/screens from one render pass.

## Activation
Enabled whenever display windows are configured (either via `display-config.json` windows or auto-detected displays). Works alongside Spout output — when both are active, `startD3DOutput` reuses the D3D11 device created by the Spout sender.

## Data Flow

```
display-config.json → slots[] (window + source geometry)
  │
  ▼
Master BrowserWindow (OSR, shared_texture_enabled=1)
  │ renders full virtual canvas (totalWidth × totalHeight)
  │
  ▼ OnAcceleratedPaint → DXGI NT shared texture handle
  │
  ▼ OpenSharedResource1 → ID3D11Texture2D
  │
  ├── D3DOutputSlot[0]: CopySubresourceRegion(srcBox) → SwapChain[0] → Present
  ├── D3DOutputSlot[1]: CopySubresourceRegion(srcBox) → SwapChain[1] → Present
  └── D3DOutputSlot[N]: ...
```

## Configuration

```json
{
  "virtualCanvas": { "width": 3840, "height": 1080 },
  "windows": [
    {
      "label": "Left display",
      "slot": 0,
      "window": { "x": 0, "y": 0, "width": 1920, "height": 1080 },
      "source": { "x": 0, "y": 0, "width": 1920, "height": 1080 }
    },
    {
      "label": "Right display",
      "slot": 1,
      "window": { "x": 1920, "y": 0, "width": 1920, "height": 1080 },
      "source": { "x": 1920, "y": 0, "width": 1920, "height": 1080 }
    }
  ]
}
```

- `window` = where the HWND appears on screen (OS logical pixels)
- `source` = which region of the virtual canvas this window samples (virtual pixels)
- `source` is optional — defaults to the window's position relative to the minimum window x/y

## Edge Cases

- **No config file**: Auto-detects connected displays. Single display → simulation mode (two half-size windows side by side).
- **Source box exceeds texture**: First-frame `D3D11_TEXTURE2D_DESC` query → clamp source coordinates. Logs out-of-bounds warning.
- **Monitor topology change**: Must manually reset with Ctrl+Shift+M.
- **DWM compositing overhead**: ~25% Intel GPU on Optimus. Inherent floor for N swap chains. Use Spout output for 0% Intel.

## Out of Scope

- Automatic monitor topology detection and window repositioning
- Per-display content (all displays sample the same virtual canvas)
- Display rotation or scaling transforms

## Related

- [spout-output.md](spout-output.md) — Alternative output mode
- [display-config.md](display-config.md) — Configuration format
- [../design-docs/single-master-gpu-blit.md](../design-docs/single-master-gpu-blit.md) — Architecture decision
