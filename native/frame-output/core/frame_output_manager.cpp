#include "frame_output_manager.h"

#include <algorithm>
#include <cstring>

namespace chromeyumm::frame_output {

void FrameOutputManager::RegisterOutput(std::unique_ptr<IOutputProtocol> output) {
    if (!output) return;
    outputs_.push_back(std::move(output));
}

void FrameOutputManager::ClearOutputs() {
    StopAll();
    outputs_.clear();
}

void FrameOutputManager::StopOutputsByName(const char* name) {
    auto it = std::remove_if(outputs_.begin(), outputs_.end(),
        [name](const std::unique_ptr<IOutputProtocol>& output) {
            if (std::strcmp(output->Name(), name) == 0) {
                if (output->IsRunning()) output->Stop();
                return true;
            }
            return false;
        });
    outputs_.erase(it, outputs_.end());
}

bool FrameOutputManager::StartAll() {
    bool allStarted = true;
    for (const auto& output : outputs_) {
        if (!output->IsRunning() && !output->Start()) {
            allStarted = false;
        }
    }
    return allStarted;
}

void FrameOutputManager::StopAll() {
    for (const auto& output : outputs_) {
        if (output->IsRunning()) {
            output->Stop();
        }
    }
}

void FrameOutputManager::OnFrame(const FrameContext& frameCtx, const BgraFrameView& frame) {
    for (const auto& output : outputs_) {
        if (!output->IsRunning() || !output->NeedsCpuFrame()) continue;
        output->OnFrame(frameCtx, frame);
    }
}

void FrameOutputManager::OnGpuFrame(const FrameContext& frameCtx, const GpuFrameView& frame) {
    for (const auto& output : outputs_) {
        if (!output->IsRunning() || !output->NeedsGpuFrame()) continue;
        output->OnGpuFrame(frameCtx, frame);
    }
}

bool FrameOutputManager::HasCpuOutputs() const {
    for (const auto& output : outputs_) {
        if (output->IsRunning() && output->NeedsCpuFrame()) return true;
    }
    return false;
}

bool FrameOutputManager::HasGpuOutputs() const {
    for (const auto& output : outputs_) {
        if (output->IsRunning() && output->NeedsGpuFrame()) return true;
    }
    return false;
}

bool FrameOutputManager::HasOutputs() const {
    return !outputs_.empty();
}

const std::vector<std::unique_ptr<IOutputProtocol>>& FrameOutputManager::Outputs() const {
    return outputs_;
}

} // namespace chromeyumm::frame_output
