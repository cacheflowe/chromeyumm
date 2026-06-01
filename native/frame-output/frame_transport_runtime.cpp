#include "frame_transport_runtime.h"

#include <d3d11.h>

#include <climits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/frame_output_manager.h"
#include "protocols/ddp/ddp_output.h"
#include "protocols/spout/spout_output.h"

// Forward declaration — implemented in cef-wrapper.cpp.
void log(const std::string& message);

namespace chromeyumm::frame_output {

static HostServices g_hostServices{};

void SetHostServices(const HostServices& services) {
    g_hostServices = services;
}

bool EnsureHostRuntime(uint32_t webviewId) {
    if (!g_hostServices.ensureHostRuntime) {
        ::log("FrameTransport: host services not registered (ensureHostRuntime)");
        return false;
    }
    return g_hostServices.ensureHostRuntime(webviewId);
}

void NotifyOutputsStopped(uint32_t webviewId) {
    if (g_hostServices.notifyOutputsStopped) {
        g_hostServices.notifyOutputsStopped(webviewId);
    }
}

ID3D11Device* GetHostDevice(uint32_t webviewId) {
    if (!g_hostServices.getHostDevice) {
        ::log("FrameTransport: host services not registered (getHostDevice)");
        return nullptr;
    }
    return g_hostServices.getHostDevice(webviewId);
}

void WakeBrowser(uint32_t webviewId) {
    if (g_hostServices.wakeBrowser) {
        g_hostServices.wakeBrowser(webviewId);
    }
}

class TransportSession {
public:
    ~TransportSession() {
        StopOutputs();
    }

    bool StartDdpOutput(const DdpOutputStartConfig& config, bool clearExisting, uint32_t webviewId) {
        if (!config.controllerAddress || !config.controllerAddress[0]) {
            ::log("DDP: controllerAddress is empty");
            return false;
        }

        // Stop and clear existing DDP outputs before replacing them.
        // StopOutputsByName resets manager_ if it becomes empty, so re-create after.
        if (clearExisting) StopOutputsByName("ddp");
        EnsureManager();

        DdpOutputConfig ddp{};
        ddp.enabled = true;
        ddp.controllerAddress = config.controllerAddress;
        ddp.port = config.port ? config.port : 4048;
        ddp.destinationId = config.destinationId ? config.destinationId : 0x01;
        ddp.pixelStart = config.pixelStart;
        ddp.sourceRect.x = config.srcX;
        ddp.sourceRect.y = config.srcY;
        ddp.sourceRect.width = config.srcW;
        ddp.sourceRect.height = config.srcH;
        ddp.zigZagRows = config.zigZagRows;
        ddp.flipH = config.flipH;
        ddp.flipV = config.flipV;
        ddp.rotate = config.rotate;
        ddp.targetFps = config.targetFps;

        // Start before registering so a failed Start() leaves the manager clean.
        auto output = std::make_unique<DdpOutput>(ddp);
        if (!output->Start()) {
            ::log("DDP: failed to start output for webviewId " + std::to_string(webviewId));
            return false;
        }
        auto* ddpPtr = output.get();
        manager_->RegisterOutput(std::move(output));
        ddpOutputs_.push_back(ddpPtr);
        ddpSourceRects_.push_back(ddp.sourceRect);
        RecomputeDdpCrop();

        ::log("DDP: started for webviewId " + std::to_string(webviewId) +
              " -> " + ddp.controllerAddress + ":" + std::to_string(ddp.port));
        return true;
    }

    bool StartSpoutOutput(const SpoutOutputStartConfig& config, uint32_t webviewId) {
        if (!config.senderName || !config.senderName[0]) {
            ::log("SpoutOutput: senderName is empty");
            return false;
        }
        if (!config.device) {
            ::log("SpoutOutput: D3D device is null");
            return false;
        }

        EnsureManager();

        SpoutOutputConfig spout{};
        spout.senderName = config.senderName;
        spout.device = config.device;

        // Start before registering so a failed Start() leaves the manager clean.
        auto output = std::make_unique<SpoutOutput>(spout);
        if (!output->Start()) {
            ::log("SpoutOutput: failed to start for webviewId " + std::to_string(webviewId));
            return false;
        }
        manager_->RegisterOutput(std::move(output));

        ::log("SpoutOutput: registered for webviewId " + std::to_string(webviewId) +
              " sender=\"" + spout.senderName + "\"");
        return true;
    }

    void StopOutputs() {
        if (manager_) {
            manager_->StopAll();
            manager_->ClearOutputs();
            manager_.reset();
        }
        ddpOutputs_.clear();
        ddpSourceRects_.clear();
        ddpCropX_ = 0; ddpCropY_ = 0; ddpCropW_ = 0; ddpCropH_ = 0;
        ReleaseStagingTextures();
        frameCounter_ = 0;
    }

    void StopOutputsByName(const char* name) {
        if (!manager_) return;
        if (std::strcmp(name, "ddp") == 0) {
            ddpOutputs_.clear();
            ddpSourceRects_.clear();
            ddpCropX_ = 0; ddpCropY_ = 0; ddpCropW_ = 0; ddpCropH_ = 0;
        }
        manager_->StopOutputsByName(name);
        // If no outputs remain, clean up staging resources.
        if (!manager_->HasOutputs()) {
            manager_.reset();
            ReleaseStagingTextures();
        }
    }

    bool HasOutputs() const {
        return manager_ != nullptr && manager_->HasOutputs();
    }

    std::string BuildDdpStatsJson(uint32_t webviewId) const {
        std::ostringstream out;
        out << "{"
            << "\"webviewId\":" << webviewId
            << ",\"active\":" << (HasOutputs() ? "true" : "false")
            << ",\"frameCounter\":" << frameCounter_
            << ",\"outputCount\":" << ddpOutputs_.size()
            << ",\"outputs\":[";

        for (size_t i = 0; i < ddpOutputs_.size(); ++i) {
            const auto* output = ddpOutputs_[i];
            if (!output) continue;
            const auto stats = output->GetStats();
            if (i > 0) out << ",";
            out << "{"
                << "\"index\":" << i
                << ",\"framesReceived\":" << stats.framesReceived
                << ",\"framesSent\":" << stats.framesSent
                << ",\"keepaliveFramesSent\":" << stats.keepaliveFramesSent
                << ",\"packetsSent\":" << stats.packetsSent
                << ",\"bytesSent\":" << stats.bytesSent
                << ",\"sendErrors\":" << stats.sendErrors
                << ",\"lastSendTimeMs\":" << stats.lastSendTimeMs
                << "}";
        }

        out << "]}";
        return out.str();
    }

    void ProcessSharedFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* sharedTex) {
        if (!manager_ || !device || !context || !sharedTex) return;

        D3D11_TEXTURE2D_DESC srcDesc = {};
        sharedTex->GetDesc(&srcDesc);

        FrameContext frameCtx{};
        frameCtx.frameId = ++frameCounter_;
        frameCtx.canvasWidth = static_cast<int>(srcDesc.Width);
        frameCtx.canvasHeight = static_cast<int>(srcDesc.Height);

        // GPU frame path first — Spout and similar GPU-native outputs.
        // No staging readback needed for these.
        if (manager_->HasGpuOutputs()) {
            GpuFrameView gpu{};
            gpu.texture = sharedTex;
            manager_->OnGpuFrame(frameCtx, gpu);
        }

        if (!manager_->HasCpuOutputs()) return;
        if (ddpCropW_ <= 0 || ddpCropH_ <= 0) return;

        // Clamp crop to actual canvas bounds (guards against misconfigured source rects).
        const int cropX = std::max(0, ddpCropX_);
        const int cropY = std::max(0, ddpCropY_);
        const int cropW = std::min(ddpCropW_, static_cast<int>(srcDesc.Width)  - cropX);
        const int cropH = std::min(ddpCropH_, static_cast<int>(srcDesc.Height) - cropY);
        if (cropW <= 0 || cropH <= 0) return;

        // Ensure staging texture exists and matches the crop dimensions.
        if (!stagingTex_ || stagingW_ != cropW || stagingH_ != cropH) {
            if (stagingTex_) { stagingTex_->Release(); stagingTex_ = nullptr; }

            D3D11_TEXTURE2D_DESC stagingDesc = srcDesc;
            stagingDesc.Width          = static_cast<UINT>(cropW);
            stagingDesc.Height         = static_cast<UINT>(cropH);
            stagingDesc.BindFlags      = 0;
            stagingDesc.MiscFlags      = 0;
            stagingDesc.Usage          = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex_))) return;
            stagingW_ = cropW;
            stagingH_ = cropH;
        }

        // Copy only the DDP bounding box — much less data than the full canvas.
        D3D11_BOX srcBox{};
        srcBox.left   = static_cast<UINT>(cropX);
        srcBox.top    = static_cast<UINT>(cropY);
        srcBox.front  = 0;
        srcBox.right  = static_cast<UINT>(cropX + cropW);
        srcBox.bottom = static_cast<UINT>(cropY + cropH);
        srcBox.back   = 1;
        context->CopySubresourceRegion(stagingTex_, 0, 0, 0, 0, sharedTex, 0, &srcBox);

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(stagingTex_, 0, D3D11_MAP_READ, 0, &mapped))) return;

        {
            BgraFrameView bgra{};
            bgra.data        = reinterpret_cast<const uint8_t*>(mapped.pData);
            bgra.width       = cropW;
            bgra.height      = cropH;
            bgra.strideBytes = static_cast<int>(mapped.RowPitch);
            bgra.cropOriginX = cropX;
            bgra.cropOriginY = cropY;

            manager_->OnFrame(frameCtx, bgra);
        }

        context->Unmap(stagingTex_, 0);
    }

private:
    void EnsureManager() {
        if (!manager_) {
            manager_ = std::make_unique<FrameOutputManager>();
        }
    }

    void ReleaseStagingTextures() {
        if (stagingTex_) { stagingTex_->Release(); stagingTex_ = nullptr; }
        stagingW_ = 0;
        stagingH_ = 0;
    }

    // Recompute the union bounding box of all registered DDP source rects.
    // Called whenever outputs are added or removed so the staging crop stays tight.
    void RecomputeDdpCrop() {
        if (ddpSourceRects_.empty()) {
            ddpCropX_ = 0; ddpCropY_ = 0; ddpCropW_ = 0; ddpCropH_ = 0;
            ReleaseStagingTextures();
            return;
        }
        int minX = INT_MAX, minY = INT_MAX, maxX = 0, maxY = 0;
        for (const auto& r : ddpSourceRects_) {
            minX = std::min(minX, r.x);
            minY = std::min(minY, r.y);
            maxX = std::max(maxX, r.x + r.width);
            maxY = std::max(maxY, r.y + r.height);
        }
        ddpCropX_ = minX;
        ddpCropY_ = minY;
        ddpCropW_ = maxX - minX;
        ddpCropH_ = maxY - minY;
        // Invalidate staging textures — size may have changed.
        ReleaseStagingTextures();
    }

private:
    std::unique_ptr<FrameOutputManager> manager_;
    std::vector<DdpOutput*>  ddpOutputs_;
    std::vector<SourceRect>  ddpSourceRects_;
    int ddpCropX_ = 0;
    int ddpCropY_ = 0;
    int ddpCropW_ = 0;
    int ddpCropH_ = 0;
    ID3D11Texture2D* stagingTex_ = nullptr;
    int stagingW_ = 0;
    int stagingH_ = 0;
    uint64_t frameCounter_ = 0;
};

class FrameTransportRuntime {
public:
    static FrameTransportRuntime& Instance() {
        static FrameTransportRuntime instance;
        return instance;
    }

    bool StartDdpOutput(uint32_t webviewId, const DdpOutputStartConfig& config, bool clearExisting) {
        return sessions_[webviewId].StartDdpOutput(config, clearExisting, webviewId);
    }

    bool StartSpoutOutput(uint32_t webviewId, const SpoutOutputStartConfig& config) {
        return sessions_[webviewId].StartSpoutOutput(config, webviewId);
    }

    void StopOutputs(uint32_t webviewId) {
        auto it = sessions_.find(webviewId);
        if (it == sessions_.end()) return;
        it->second.StopOutputs();
        sessions_.erase(it);
    }

    void StopOutputsByName(uint32_t webviewId, const char* protocolName) {
        auto it = sessions_.find(webviewId);
        if (it == sessions_.end()) return;
        it->second.StopOutputsByName(protocolName);
        // Remove session entirely if no outputs remain.
        if (!it->second.HasOutputs()) {
            sessions_.erase(it);
        }
    }

    bool HasOutputs(uint32_t webviewId) const {
        auto it = sessions_.find(webviewId);
        return it != sessions_.end() && it->second.HasOutputs();
    }

    std::string GetDdpStatsJson(uint32_t webviewId) const {
        auto it = sessions_.find(webviewId);
        if (it == sessions_.end()) {
            return "{\"webviewId\":" + std::to_string(webviewId) +
                   ",\"active\":false,\"frameCounter\":0,\"outputCount\":0,\"outputs\":[]}";
        }
        return it->second.BuildDdpStatsJson(webviewId);
    }

    void ProcessSharedFrame(
        uint32_t webviewId,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* sharedTex)
    {
        auto it = sessions_.find(webviewId);
        if (it == sessions_.end()) return;
        it->second.ProcessSharedFrame(device, context, sharedTex);
    }

private:
    FrameTransportRuntime() = default;
    FrameTransportRuntime(const FrameTransportRuntime&) = delete;
    FrameTransportRuntime& operator=(const FrameTransportRuntime&) = delete;

private:
    std::map<uint32_t, TransportSession> sessions_;
};

bool StartDdpOutput(uint32_t webviewId, const DdpOutputStartConfig& config, bool clearExisting) {
    return FrameTransportRuntime::Instance().StartDdpOutput(webviewId, config, clearExisting);
}

bool StartSpoutOutput(uint32_t webviewId, const SpoutOutputStartConfig& config) {
    return FrameTransportRuntime::Instance().StartSpoutOutput(webviewId, config);
}

void StopOutputs(uint32_t webviewId) {
    FrameTransportRuntime::Instance().StopOutputs(webviewId);
}

void StopOutputsByName(uint32_t webviewId, const char* protocolName) {
    FrameTransportRuntime::Instance().StopOutputsByName(webviewId, protocolName);
}

bool HasOutputs(uint32_t webviewId) {
    return FrameTransportRuntime::Instance().HasOutputs(webviewId);
}

const char* GetDdpStatsJson(uint32_t webviewId) {
    static thread_local std::string s_statsJson;
    s_statsJson = FrameTransportRuntime::Instance().GetDdpStatsJson(webviewId);
    return s_statsJson.c_str();
}

void ProcessSharedFrame(
    uint32_t webviewId,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* sharedTex)
{
    FrameTransportRuntime::Instance().ProcessSharedFrame(webviewId, device, context, sharedTex);
}

} // namespace chromeyumm::frame_output
