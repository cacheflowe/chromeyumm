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
struct BgraFrameView {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int strideBytes = 0;
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
