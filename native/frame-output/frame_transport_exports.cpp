#include "frame_transport_runtime.h"

#include <cstdint>
#include <string>

#define CHROMEYUMM_EXPORT
void log(const std::string& message);

extern "C" {

// clearExisting=true: replace all DDP outputs for this webview (startDdpOutput behaviour).
// clearExisting=false: append a new DDP output alongside any existing ones (addDdpOutput behaviour).
CHROMEYUMM_EXPORT bool startDdpOutput(
    uint32_t webviewId,
    const char* controllerAddress,
    uint16_t port,
    uint8_t destinationId,
    int pixelStart,
    int srcX,
    int srcY,
    int srcW,
    int srcH,
    bool zigZagRows,
    bool flipH,
    bool flipV,
    int rotate,
    bool clearExisting)
{
    if (!chromeyumm::frame_output::EnsureHostRuntime(webviewId)) return false;

    chromeyumm::frame_output::DdpOutputStartConfig config{};
    config.controllerAddress = controllerAddress;
    config.port = port;
    config.destinationId = destinationId;
    config.pixelStart = pixelStart;
    config.srcX = srcX;
    config.srcY = srcY;
    config.srcW = srcW;
    config.srcH = srcH;
    config.zigZagRows = zigZagRows;
    config.flipH = flipH;
    config.flipV = flipV;
    config.rotate = rotate;

    const bool ok = chromeyumm::frame_output::StartDdpOutput(webviewId, config, clearExisting);
    if (ok) {
        chromeyumm::frame_output::WakeBrowser(webviewId);
    }
    return ok;
}

CHROMEYUMM_EXPORT void stopDdpOutput(uint32_t webviewId) {
    chromeyumm::frame_output::StopOutputsByName(webviewId, "ddp");
    chromeyumm::frame_output::NotifyOutputsStopped(webviewId);
    ::log("DDP: stopped for webviewId " + std::to_string(webviewId));
}

CHROMEYUMM_EXPORT const char* getDdpOutputStats(uint32_t webviewId) {
    return chromeyumm::frame_output::GetDdpStatsJson(webviewId);
}

// ── Spout output sender ─────────────────────────────────────────────────────

CHROMEYUMM_EXPORT bool startSpoutSender(uint32_t webviewId, const char* senderName) {
    if (!chromeyumm::frame_output::EnsureHostRuntime(webviewId)) return false;

    ID3D11Device* device = chromeyumm::frame_output::GetHostDevice(webviewId);
    if (!device) {
        ::log("SpoutSender: host D3D device not available for webviewId " + std::to_string(webviewId));
        return false;
    }

    const char* name = (senderName && senderName[0]) ? senderName : "ChromeyummSpout";
    chromeyumm::frame_output::SpoutOutputStartConfig config{};
    config.senderName = name;
    config.device = device;

    const bool ok = chromeyumm::frame_output::StartSpoutOutput(webviewId, config);
    if (ok) chromeyumm::frame_output::WakeBrowser(webviewId);
    return ok;
}

CHROMEYUMM_EXPORT void stopSpoutSender(uint32_t webviewId) {
    chromeyumm::frame_output::StopOutputsByName(webviewId, "spout");
    chromeyumm::frame_output::NotifyOutputsStopped(webviewId);
    ::log("Spout sender stopped for webviewId " + std::to_string(webviewId));
}

}
