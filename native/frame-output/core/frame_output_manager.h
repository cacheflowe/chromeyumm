#pragma once

#include <memory>
#include <vector>

#include "output_protocol.h"

namespace chromeyumm::frame_output {

class FrameOutputManager {
public:
    void RegisterOutput(std::unique_ptr<IOutputProtocol> output);
    void ClearOutputs();
    void StopOutputsByName(const char* name);

    bool StartAll();
    void StopAll();

    void OnFrame(const FrameContext& frameCtx, const BgraFrameView& frame);
    void OnGpuFrame(const FrameContext& frameCtx, const GpuFrameView& frame);

    bool HasCpuOutputs() const;
    bool HasGpuOutputs() const;
    bool HasOutputs() const;
    const std::vector<std::unique_ptr<IOutputProtocol>>& Outputs() const;

private:
    std::vector<std::unique_ptr<IOutputProtocol>> outputs_;
};

} // namespace chromeyumm::frame_output
