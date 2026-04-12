#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace chromeyumm::frame_output {

struct HostServices {
    bool           (*ensureHostRuntime)   (uint32_t webviewId) = nullptr;
    ID3D11Device*  (*getHostDevice)       (uint32_t webviewId) = nullptr;
    void           (*notifyOutputsStopped)(uint32_t webviewId) = nullptr;
    void           (*wakeBrowser)         (uint32_t webviewId) = nullptr;
};

void          SetHostServices(const HostServices& services);
bool          EnsureHostRuntime(uint32_t webviewId);
ID3D11Device* GetHostDevice(uint32_t webviewId);
void          NotifyOutputsStopped(uint32_t webviewId);
void          WakeBrowser(uint32_t webviewId);

struct DdpOutputStartConfig {
    const char* controllerAddress = nullptr;
    uint16_t port = 4048;
    uint8_t destinationId = 0x01;
    int pixelStart = 0;
    int srcX = 0;
    int srcY = 0;
    int srcW = 0;
    int srcH = 0;
    bool zigZagRows = false;
    bool flipH = false;
    bool flipV = false;
    int rotate = 0; // 0, 90, 180, 270
};

struct SpoutOutputStartConfig {
    const char* senderName = nullptr;
    ID3D11Device* device = nullptr;
};

// Runtime API used by cef-wrapper.cpp. Backed by an internal OOP runtime.
bool StartDdpOutput(uint32_t webviewId, const DdpOutputStartConfig& config, bool clearExisting);
bool StartSpoutOutput(uint32_t webviewId, const SpoutOutputStartConfig& config);
void StopOutputs(uint32_t webviewId);
void StopOutputsByName(uint32_t webviewId, const char* protocolName);
bool HasOutputs(uint32_t webviewId);
const char* GetDdpStatsJson(uint32_t webviewId);

void ProcessSharedFrame(
    uint32_t webviewId,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* sharedTex);

} // namespace chromeyumm::frame_output
