#include "ddp_output.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <chrono>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace chromeyumm::frame_output {

namespace {
constexpr int kDdpHeaderLength = 10;
constexpr int kMaxPixelsPerPacket = 480;
constexpr int kMaxDataLength = kMaxPixelsPerPacket * 3;
constexpr int64_t kKeepaliveMs = 1000;
constexpr uint8_t kVersion1 = 0x40;
constexpr uint8_t kPush = 0x01;
constexpr uint8_t kDataTypeRgb888 = 0x0b;

bool EnsureWinsockInitialized() {
    static bool initialized = false;
    static bool ok = false;
    if (!initialized) {
        WSADATA wsaData{};
        ok = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
        initialized = true;
    }
    return ok;
}

} // namespace

DdpOutput::DdpOutput(const DdpOutputConfig& config)
    : config_(config) {}

DdpOutput::~DdpOutput() {
    Stop();
}

const char* DdpOutput::Name() const {
    return "ddp";
}

bool DdpOutput::Start() {
    if (running_) return true;
    if (!config_.enabled) return false;
    if (config_.controllerAddress.empty()) return false;
    if (!EnsureWinsockInitialized()) return false;

    SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;

    int sendBufferSize = 1024 * 1024;
    ::setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBufferSize), sizeof(sendBufferSize));

    socket_ = static_cast<uintptr_t>(sock);
    running_ = true;
    return true;
}

void DdpOutput::Stop() {
    if (!running_) return;
    if (socket_ != static_cast<uintptr_t>(~0ULL)) {
        const SOCKET sock = static_cast<SOCKET>(socket_);
        ::closesocket(sock);
    }
    socket_ = static_cast<uintptr_t>(~0ULL);
    running_ = false;
    previousRgbPayload_.clear();
    previousPayloadWidth_ = 0;
}

bool DdpOutput::IsRunning() const {
    return running_;
}

void DdpOutput::OnFrame(const FrameContext&, const BgraFrameView& frame) {
    if (!running_) return;
    framesReceived_.fetch_add(1, std::memory_order_relaxed);

    std::vector<uint8_t> rgbPayload;
    int payloadPixelWidth = 0;
    if (!BuildRgbPayload(frame, rgbPayload, payloadPixelWidth)) return;

    const int64_t nowMs = NowMs();
    const bool keepaliveDue = lastSendTimeMs_ <= 0 || (nowMs - lastSendTimeMs_) >= kKeepaliveMs;

    // Full-frame comparison: skip if pixel data is identical to the last successful send
    // and a keepalive is not due. When sending, always send the complete frame — partial
    // row sends are incompatible with common DDP controllers (WLED, FPP) that clear their
    // pixel buffer at the start of each new frame, turning unsent rows black.
    const bool frameUnchanged =
        !previousRgbPayload_.empty() &&
        previousRgbPayload_.size() == rgbPayload.size() &&
        previousPayloadWidth_ == payloadPixelWidth &&
        std::memcmp(rgbPayload.data(), previousRgbPayload_.data(), rgbPayload.size()) == 0;

    if (frameUnchanged && !keepaliveDue) return;

    uint64_t packetsOut = 0;
    uint64_t bytesOut = 0;
    const bool ok = SendDdpPackets(rgbPayload.data(), rgbPayload.size(), packetsOut, bytesOut);

    if (ok) {
        framesSent_.fetch_add(1, std::memory_order_relaxed);
        if (frameUnchanged) keepaliveFramesSent_.fetch_add(1, std::memory_order_relaxed);
        packetsSent_.fetch_add(packetsOut, std::memory_order_relaxed);
        bytesSent_.fetch_add(bytesOut, std::memory_order_relaxed);
        lastSendTimeMs_.store(nowMs, std::memory_order_relaxed);
        previousRgbPayload_ = std::move(rgbPayload);
        previousPayloadWidth_ = payloadPixelWidth;
    } else {
        sendErrors_.fetch_add(1, std::memory_order_relaxed);
    }
}

DdpOutputStats DdpOutput::GetStats() const {
    DdpOutputStats stats{};
    stats.framesReceived      = framesReceived_.load(std::memory_order_relaxed);
    stats.framesSent          = framesSent_.load(std::memory_order_relaxed);
    stats.keepaliveFramesSent = keepaliveFramesSent_.load(std::memory_order_relaxed);
    stats.packetsSent         = packetsSent_.load(std::memory_order_relaxed);
    stats.bytesSent           = bytesSent_.load(std::memory_order_relaxed);
    stats.sendErrors          = sendErrors_.load(std::memory_order_relaxed);
    stats.lastSendTimeMs      = lastSendTimeMs_.load(std::memory_order_relaxed);
    return stats;
}

bool DdpOutput::BuildRgbPayload(const BgraFrameView& frame, std::vector<uint8_t>& outRgb, int& outPixelWidth) const {
    if (!frame.data || frame.width <= 0 || frame.height <= 0 || frame.strideBytes <= 0) {
        return false;
    }

    int srcX = std::max(0, config_.sourceRect.x);
    int srcY = std::max(0, config_.sourceRect.y);
    int srcW = config_.sourceRect.width > 0 ? config_.sourceRect.width : frame.width;
    int srcH = config_.sourceRect.height > 0 ? config_.sourceRect.height : frame.height;

    if (srcX >= frame.width || srcY >= frame.height) return false;
    srcW = std::min(srcW, frame.width - srcX);
    srcH = std::min(srcH, frame.height - srcY);
    if (srcW <= 0 || srcH <= 0) return false;

    // Output dimensions swap for 90°/270° rotations.
    const bool swapDims = (config_.rotate == 90 || config_.rotate == 270);
    const int outW = swapDims ? srcH : srcW;
    const int outH = swapDims ? srcW : srcH;

    outPixelWidth = outW;
    outRgb.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 3);

    for (int oy = 0; oy < outH; ++oy) {
        for (int bufX = 0; bufX < outW; ++bufX) {
            // ZigZag: for even output rows the strip runs right-to-left, so the
            // logical column for buffer position bufX is its mirror.
            const int col = (config_.zigZagRows && oy % 2 == 0) ? outW - 1 - bufX : bufX;

            // Apply flip to logical output position (col, oy).
            const int ix = config_.flipH ? outW - 1 - col : col;
            const int iy = config_.flipV ? outH - 1 - oy  : oy;

            // Map intermediate coords to source coords via rotation.
            int sx, sy;
            switch (config_.rotate) {
                case 90:  sx = iy;             sy = srcH - 1 - ix; break;
                case 180: sx = srcW - 1 - ix;  sy = srcH - 1 - iy; break;
                case 270: sx = srcW - 1 - iy;  sy = ix;            break;
                default:  sx = ix;             sy = iy;             break;
            }

            const uint8_t* srcRow = frame.data + static_cast<size_t>(srcY + sy) * frame.strideBytes;
            const size_t readIdx  = static_cast<size_t>(srcX + sx) * 4;
            const size_t writeIdx = (static_cast<size_t>(oy) * outW + static_cast<size_t>(bufX)) * 3;

            // BGRA -> RGB
            outRgb[writeIdx + 0] = srcRow[readIdx + 2];
            outRgb[writeIdx + 1] = srcRow[readIdx + 1];
            outRgb[writeIdx + 2] = srcRow[readIdx + 0];
        }
    }

    return true;
}

bool DdpOutput::SendDdpPackets(const uint8_t* data, size_t dataSize, uint64_t& packetsOut, uint64_t& bytesOut) {
    packetsOut = 0;
    bytesOut = 0;
    if (!data || dataSize == 0) return false;
    if (socket_ == static_cast<uintptr_t>(~0ULL)) return false;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.controllerAddress.c_str(), &address.sin_addr) != 1) {
        return false;
    }

    const int packetCount = static_cast<int>((dataSize + kMaxDataLength - 1) / kMaxDataLength);
    const int dataOffsetBytes = std::max(0, config_.pixelStart) * 3;
    bool allSent = true;

    // Grow the reusable packet buffer once to max packet size; no per-packet allocation.
    const size_t maxPacketSize = static_cast<size_t>(kDdpHeaderLength + kMaxDataLength);
    if (packetBuf_.size() < maxPacketSize) packetBuf_.resize(maxPacketSize);

    for (int packetIndex = 0; packetIndex < packetCount; ++packetIndex) {
        const int packetOffset = packetIndex * kMaxDataLength;
        const int payloadLength = std::min(kMaxDataLength, static_cast<int>(dataSize) - packetOffset);
        const bool isLastPacket = (packetIndex == packetCount - 1);
        const int absoluteOffset = dataOffsetBytes + packetOffset;

        packetBuf_[0] = static_cast<uint8_t>(kVersion1 | (isLastPacket ? kPush : 0));
        packetBuf_[1] = NextSequence();
        packetBuf_[2] = kDataTypeRgb888;
        packetBuf_[3] = config_.destinationId;
        packetBuf_[4] = static_cast<uint8_t>((absoluteOffset >> 24) & 0xff);
        packetBuf_[5] = static_cast<uint8_t>((absoluteOffset >> 16) & 0xff);
        packetBuf_[6] = static_cast<uint8_t>((absoluteOffset >> 8) & 0xff);
        packetBuf_[7] = static_cast<uint8_t>(absoluteOffset & 0xff);
        packetBuf_[8] = static_cast<uint8_t>((payloadLength >> 8) & 0xff);
        packetBuf_[9] = static_cast<uint8_t>(payloadLength & 0xff);

        std::memcpy(packetBuf_.data() + kDdpHeaderLength, data + packetOffset, static_cast<size_t>(payloadLength));

        const SOCKET sock = static_cast<SOCKET>(socket_);
        const int packetSize = kDdpHeaderLength + payloadLength;
        const int sent = ::sendto(sock,
                                  reinterpret_cast<const char*>(packetBuf_.data()),
                                  packetSize,
                                  0,
                                  reinterpret_cast<const sockaddr*>(&address),
                                  sizeof(address));
        if (sent == SOCKET_ERROR) {
            allSent = false;
            continue;
        }

        packetsOut++;
        bytesOut += static_cast<uint64_t>(sent);
    }

    return allSent;
}

uint8_t DdpOutput::NextSequence() {
    sequence_ = sequence_ >= 15 ? 1 : static_cast<uint8_t>(sequence_ + 1);
    return sequence_;
}

int64_t DdpOutput::NowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace chromeyumm::frame_output
