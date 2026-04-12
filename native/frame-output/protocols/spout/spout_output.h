#pragma once

#include "../../core/output_protocol.h"

#include <string>

struct ID3D11Device;
class spoutDX;

namespace chromeyumm::frame_output {

struct SpoutOutputConfig {
    std::string senderName;
    ID3D11Device* device = nullptr;  // borrowed pointer, not owned
};

class SpoutOutput final : public IOutputProtocol {
public:
    explicit SpoutOutput(const SpoutOutputConfig& config);
    ~SpoutOutput() override;

    const char* Name() const override;
    bool Start() override;
    void Stop() override;
    bool IsRunning() const override;

    bool NeedsCpuFrame() const override { return false; }
    bool NeedsGpuFrame() const override { return true; }
    void OnFrame(const FrameContext&, const BgraFrameView&) override {}
    void OnGpuFrame(const FrameContext& frameCtx, const GpuFrameView& frame) override;

private:
    SpoutOutputConfig config_;
    spoutDX* sender_ = nullptr;
    bool running_ = false;
};

} // namespace chromeyumm::frame_output
