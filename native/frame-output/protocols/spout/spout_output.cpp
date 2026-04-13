// SpoutOutput — GPU-native output protocol for SpoutDX texture sharing.
// Conditionally compiled: when SpoutDX headers are absent, Start() returns false.

#if __has_include("SpoutDX.h")
#include "SpoutDX.h"
#define CHROMEYUMM_HAS_SPOUT_OUTPUT 1
#else
class spoutDX;
#define CHROMEYUMM_HAS_SPOUT_OUTPUT 0
#endif

#include "spout_output.h"

#include <d3d11.h>
#include <string>

void log(const std::string& message);

namespace chromeyumm::frame_output {

SpoutOutput::SpoutOutput(const SpoutOutputConfig& config)
    : config_(config) {}

SpoutOutput::~SpoutOutput() {
    Stop();
}

const char* SpoutOutput::Name() const {
    return "spout";
}

bool SpoutOutput::Start() {
#if CHROMEYUMM_HAS_SPOUT_OUTPUT
    if (running_) return true;
    if (config_.senderName.empty()) return false;
    if (!config_.device) return false;

    sender_ = new spoutDX();
    if (!sender_->OpenDirectX11(config_.device)) {
        ::log("SpoutOutput: OpenDirectX11 failed");
        delete sender_; sender_ = nullptr;
        return false;
    }
    if (!sender_->SetSenderName(config_.senderName.c_str())) {
        ::log("SpoutOutput: SetSenderName failed");
        delete sender_; sender_ = nullptr;
        return false;
    }

    running_ = true;
    ::log("SpoutOutput: started sender \"" + config_.senderName + "\"");
    return true;
#else
    ::log("SpoutOutput: built without SpoutDX — not available");
    return false;
#endif
}

void SpoutOutput::Stop() {
#if CHROMEYUMM_HAS_SPOUT_OUTPUT
    if (!running_) return;
    if (sender_) {
        sender_->ReleaseSender();
        delete sender_;
        sender_ = nullptr;
    }
    running_ = false;
    ::log("SpoutOutput: stopped sender \"" + config_.senderName + "\"");
#endif
}

bool SpoutOutput::IsRunning() const {
    return running_;
}

void SpoutOutput::OnGpuFrame(const FrameContext&, const GpuFrameView& frame) {
#if CHROMEYUMM_HAS_SPOUT_OUTPUT
    if (!running_ || !sender_ || !frame.texture) return;
    sender_->SendTexture(frame.texture);
#else
    (void)frame;
#endif
}

} // namespace chromeyumm::frame_output
