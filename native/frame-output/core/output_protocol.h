#pragma once

#include "frame_types.h"

namespace chromeyumm::frame_output {

class IOutputProtocol {
public:
    virtual ~IOutputProtocol() = default;

    virtual const char* Name() const = 0;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;

    // Override to indicate which frame path(s) this output needs.
    virtual bool NeedsCpuFrame() const { return true; }
    virtual bool NeedsGpuFrame() const { return false; }

    // CPU frame path — protocols requiring pixel readback (DDP, ArtNet, etc.)
    virtual void OnFrame(const FrameContext& frameCtx, const BgraFrameView& frame) { (void)frameCtx; (void)frame; }

    // GPU frame path — protocols that operate on D3D11 textures (Spout, NDI, etc.)
    virtual void OnGpuFrame(const FrameContext& frameCtx, const GpuFrameView& frame) { (void)frameCtx; (void)frame; }
};

} // namespace chromeyumm::frame_output
