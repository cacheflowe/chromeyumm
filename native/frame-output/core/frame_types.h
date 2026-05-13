#pragma once

#include <cstdint>

struct ID3D11Texture2D;

namespace chromeyumm::frame_output {

struct SourceRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// CPU view of a BGRA8 frame (matching common D3D readback format).
// cropOriginX/Y: the canvas-space coordinate of this view's (0,0) pixel — set
// when only a sub-region of the canvas was read back, so outputs whose source
// rects are expressed in full-canvas coordinates can subtract the origin.
struct BgraFrameView {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int strideBytes = 0;
    int cropOriginX = 0;
    int cropOriginY = 0;
};

// GPU view of a D3D11 texture (for Spout, NDI, and similar GPU-native outputs).
struct GpuFrameView {
    ID3D11Texture2D* texture = nullptr;
};

struct FrameContext {
    uint64_t frameId = 0;
    int canvasWidth = 0;
    int canvasHeight = 0;
};

} // namespace chromeyumm::frame_output
