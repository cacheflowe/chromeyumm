# Decision: Single Master Browser with GPU Blit

## Status
Accepted

## Context
Need to render a single virtual canvas and display sub-regions on multiple physical displays. Two approaches: run N browser instances (one per display), or render once and distribute via GPU.

## Options Considered

### N browser instances
- **Pro**: Simple — each browser window shows its region
- **Con**: N× render cost. If content is a Three.js scene, it renders N times.
- **Con**: Synchronization between instances is complex
- **Con**: Each instance has its own GPU memory allocation

### One master browser + GPU blit
- **Pro**: Single render. One Three.js scene, one GPU context, one `requestAnimationFrame`.
- **Pro**: `CopySubresourceRegion` with source box samples any sub-region — constant cost per display.
- **Pro**: Virtual canvas can be any size (not limited to monitor boundaries).
- **Con**: More plumbing — needs D3D11 swap chain per display window, source box clamping logic.

## Decision
Single master browser with GPU blit via `CopySubresourceRegion`. The performance advantage is decisive — one render regardless of display count.

## Implementation
- Master `BrowserWindow` renders to full virtual canvas (e.g., 3840×1080 for two 1920×1080 displays)
- Each `NativeDisplayWindow` is a bare Win32 HWND with a D3D11 swap chain
- `OnAcceleratedPaint` iterates `D3DOutputSlot`s: `GetBuffer(0)` → clamp source box → `CopySubresourceRegion` → `Present`
- Source box clamping queries `D3D11_TEXTURE2D_DESC` on first frame to prevent out-of-bounds copies

## Consequences
- Master window is hidden in output mode (content only in shared texture)
- Ctrl+M toggle needed for interactive/output mode switching
- Display window reset (Ctrl+Shift+M) needed for monitor topology changes

## Related
- [osr-shared-texture.md](osr-shared-texture.md) — Where the texture comes from
- [ARCHITECTURE.md](../../ARCHITECTURE.md) — System diagram
