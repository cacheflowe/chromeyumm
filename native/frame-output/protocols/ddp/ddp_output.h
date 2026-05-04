#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../core/output_protocol.h"

namespace chromeyumm::frame_output {

struct DdpOutputConfig {
    bool enabled = true;
    std::string controllerAddress;
    uint16_t port = 4048;
    uint8_t destinationId = 0x01;
    int pixelStart = 0;
    SourceRect sourceRect{};
    bool zigZagRows = false;
    bool flipH = false;       // mirror output horizontally
    bool flipV = false;       // mirror output vertically
    int rotate = 0;           // clockwise rotation in degrees: 0, 90, 180, 270
};

struct DdpOutputStats {
    uint64_t framesReceived = 0;
    uint64_t framesSent = 0;
    uint64_t keepaliveFramesSent = 0;
    uint64_t packetsSent = 0;
    uint64_t bytesSent = 0;
    uint64_t sendErrors = 0;
    int64_t lastSendTimeMs = 0;
};

class DdpOutput final : public IOutputProtocol {
public:
    explicit DdpOutput(const DdpOutputConfig& config);
    ~DdpOutput() override;

    const char* Name() const override;
    bool Start() override;
    void Stop() override;
    bool IsRunning() const override;

    bool NeedsCpuFrame() const override { return true; }
    bool NeedsGpuFrame() const override { return false; }
    void OnFrame(const FrameContext& frameCtx, const BgraFrameView& frame) override;

    DdpOutputStats GetStats() const;

private:
    bool BuildRgbPayload(const BgraFrameView& frame, std::vector<uint8_t>& outRgb, int& outPixelWidth) const;
    bool SendDdpPackets(const uint8_t* data, size_t dataSize, uint64_t& packetsOut, uint64_t& bytesOut);
    uint8_t NextSequence();
    static int64_t NowMs();
    void KeepaliveLoop();

private:
    DdpOutputConfig config_;
    bool running_ = false;
    uintptr_t socket_ = static_cast<uintptr_t>(~0ULL);
    uint8_t sequence_ = 0;
    // Stats are written on the render thread and read from the JS stats polling
    // timer — use relaxed atomics so reads are never torn on any platform.
    std::atomic<uint64_t> framesReceived_{0};
    std::atomic<uint64_t> framesSent_{0};
    std::atomic<uint64_t> keepaliveFramesSent_{0};
    std::atomic<uint64_t> packetsSent_{0};
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> sendErrors_{0};
    std::atomic<int64_t>  lastSendTimeMs_{0};
    // Protects previousRgbPayload_, previousPayloadWidth_, packetBuf_, sequence_,
    // and stopKeepalive_ — accessed from both the CEF render thread and keepaliveThread_.
    mutable std::mutex sendMutex_;
    std::condition_variable keepaliveCv_;
    bool stopKeepalive_ = false;
    std::thread keepaliveThread_;
    std::vector<uint8_t> previousRgbPayload_;
    int previousPayloadWidth_ = 0;
    // Pre-allocated send buffer — avoids per-packet heap allocation in the hot path.
    std::vector<uint8_t> packetBuf_;
};

} // namespace chromeyumm::frame_output
