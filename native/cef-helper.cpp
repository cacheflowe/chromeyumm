#include "include/cef_app.h"
#include "include/cef_v8.h"
#include <Windows.h>
#include <map>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// Helper: extract a query parameter value from a URL string.
// ---------------------------------------------------------------------------
static std::string GetQueryParam(const std::string& url, const std::string& key) {
    auto qpos = url.find('?');
    if (qpos == std::string::npos) return "";
    std::string query = url.substr(qpos + 1);
    std::string search = key + "=";
    auto pos = query.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = query.find('&', pos);
    return query.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// ---------------------------------------------------------------------------
// Release callback: unmaps the Win32 view when the zero-copy ArrayBuffer is
// GC'd by V8. Only used when V8 sandbox is disabled (fast path).
// ---------------------------------------------------------------------------
class MappedMemoryReleaseCallback : public CefV8ArrayBufferReleaseCallback {
public:
    MappedMemoryReleaseCallback(HANDLE hMap) : hMap_(hMap) {}
    void ReleaseBuffer(void* buffer) override {
        UnmapViewOfFile(buffer);
        CloseHandle(hMap_);
    }
private:
    HANDLE hMap_;
    IMPLEMENT_REFCOUNTING(MappedMemoryReleaseCallback);
};

// ---------------------------------------------------------------------------
// SpoutFrameHandler: CefV8Handler for window.__spoutGetFrame().
//
// Called by the renderer each requestAnimationFrame.
// Reads just 4 bytes (seq) from shared memory; if the frame is new, copies
// the entire frame into V8 heap via CreateArrayBufferWithCopy.
// This is sandbox-safe and avoids all network overhead.
//
// Layout of shared memory (matches SpoutInputState in nativeWrapper.cpp):
//   [0-3]  seq      — Int32LE; C++ increments after writing pixels
//   [4-7]  width    — Uint32LE
//   [8-11] height   — Uint32LE
//   [12-15] reserved
//   [16+]  BGRA pixels (up to 3840×2160×4 bytes)
// ---------------------------------------------------------------------------
class SpoutFrameHandler : public CefV8Handler {
public:
    static constexpr size_t kMappedSize = 16 + 3840 * 2160 * 4;

    SpoutFrameHandler(HANDLE hMap, void* ptr)
        : hMap_(hMap), mappedPtr_(ptr), lastSeq_(0) {}

    ~SpoutFrameHandler() {
        if (mappedPtr_) { UnmapViewOfFile(mappedPtr_); mappedPtr_ = nullptr; }
        if (hMap_)      { CloseHandle(hMap_);           hMap_ = NULL; }
    }

    bool Execute(const CefString& name,
                 CefRefPtr<CefV8Value> object,
                 const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& retval,
                 CefString& exception) override {
        if (!mappedPtr_) {
            retval = CefV8Value::CreateNull();
            return true;
        }

        // Cheap seq check — 4-byte read, no pixel copy
        int32_t seq = *(volatile int32_t*)mappedPtr_;
        if (seq == 0 || seq == lastSeq_) {
            retval = CefV8Value::CreateNull(); // no new frame
            return true;
        }

        uint8_t*  src = (uint8_t*)mappedPtr_;
        uint32_t  w   = *(uint32_t*)(src + 4);
        uint32_t  h   = *(uint32_t*)(src + 8);
        size_t frameBytes = 16 + (size_t)w * h * 4;

        if (w == 0 || h == 0 || frameBytes > kMappedSize) {
            retval = CefV8Value::CreateNull();
            return true;
        }

        // Preferred path: caller passes a pre-allocated ArrayBuffer as the write target.
        // window.__spoutGetFrame(persistentBuf) → true on new frame, null if unchanged.
        // Zero allocation per frame — no GC pressure, no V8 heap churn.
        if (!arguments.empty() && arguments[0]->IsArrayBuffer()) {
            void*  dst    = arguments[0]->GetArrayBufferData();
            size_t dstLen = arguments[0]->GetArrayBufferByteLength();
            if (dst && dstLen >= frameBytes) {
                memcpy(dst, src, frameBytes);
                lastSeq_ = seq;
                retval = CefV8Value::CreateBool(true);
                return true;
            }
        }

        // Fallback: no pre-allocated buffer supplied — copy into a new V8 ArrayBuffer.
        // Causes ~8 MB/frame heap allocation (GC pressure). Kept for backward compatibility.
        CefRefPtr<CefV8Value> ab = CefV8Value::CreateArrayBufferWithCopy(src, frameBytes);
        if (!ab) {
            retval = CefV8Value::CreateNull();
            return true;
        }

        lastSeq_ = seq;
        retval = ab;
        return true;
    }

private:
    HANDLE   hMap_;
    void*    mappedPtr_;
    int32_t  lastSeq_;
    IMPLEMENT_REFCOUNTING(SpoutFrameHandler);
};

class HelperApp : public CefApp, public CefRenderProcessHandler {
public:
    // CefApp methods:
    virtual void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
        registrar->AddCustomScheme("views",
            CEF_SCHEME_OPTION_STANDARD |
            CEF_SCHEME_OPTION_CORS_ENABLED |
            CEF_SCHEME_OPTION_SECURE | // treat it like https
            CEF_SCHEME_OPTION_CSP_BYPASSING | // allow things like crypto.subtle
            CEF_SCHEME_OPTION_FETCH_ENABLED);
    }

    virtual CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
        return this;
    }

    // CefRenderProcessHandler methods:

    // Called when a browser is created - receive sandbox flag via extra_info
    virtual void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefDictionaryValue> extra_info) override {
        if (extra_info && extra_info->HasKey("sandbox")) {
            bool sandbox = extra_info->GetBool("sandbox");
            std::lock_guard<std::mutex> lock(sandbox_map_mutex_);
            sandbox_map_[browser->GetIdentifier()] = sandbox;
        }
    }

    // Called when a browser is destroyed - cleanup sandbox flag
    virtual void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
        std::lock_guard<std::mutex> lock(sandbox_map_mutex_);
        sandbox_map_.erase(browser->GetIdentifier());
    }

    virtual void OnContextCreated(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefRefPtr<CefV8Context> context) override {
        // Check if this browser is sandboxed
        bool is_sandboxed = false;
        {
            std::lock_guard<std::mutex> lock(sandbox_map_mutex_);
            auto it = sandbox_map_.find(browser->GetIdentifier());
            if (it != sandbox_map_.end()) {
                is_sandboxed = it->second;
            }
        }

        // Get the global window object
        CefRefPtr<CefV8Context> v8Context = frame->GetV8Context();
        v8Context->Enter();

        CefRefPtr<CefV8Value> window = context->GetGlobal();

        // Create eventBridge - event-only bridge (always available for all webviews, including sandboxed)
        CefRefPtr<CefV8Value> eventBridge = CefV8Value::CreateObject(nullptr, nullptr);
        CefRefPtr<CefV8Value> eventPostMessage = CreatePostMessageFunction(browser, "EventBridgeMessage");
        eventBridge->SetValue("postMessage", eventPostMessage, V8_PROPERTY_ATTRIBUTE_NONE);
        window->SetValue("__electrobunEventBridge", eventBridge, V8_PROPERTY_ATTRIBUTE_NONE);

        // Only create bunBridge and internalBridge for non-sandboxed webviews
        if (!is_sandboxed) {
            // Create bunBridge - user RPC bridge
            CefRefPtr<CefV8Value> bunBridge = CefV8Value::CreateObject(nullptr, nullptr);
            CefRefPtr<CefV8Value> bunPostMessage = CreatePostMessageFunction(browser, "BunBridgeMessage");
            bunBridge->SetValue("postMessage", bunPostMessage, V8_PROPERTY_ATTRIBUTE_NONE);
            window->SetValue("__electrobunBunBridge", bunBridge, V8_PROPERTY_ATTRIBUTE_NONE);

            // Create internalBridge - internal RPC bridge
            CefRefPtr<CefV8Value> internalBridge = CefV8Value::CreateObject(nullptr, nullptr);
            CefRefPtr<CefV8Value> internalPostMessage = CreatePostMessageFunction(browser, "internalMessage");
            internalBridge->SetValue("postMessage", internalPostMessage, V8_PROPERTY_ATTRIBUTE_NONE);
            window->SetValue("__electrobunInternalBridge", internalBridge, V8_PROPERTY_ATTRIBUTE_NONE);
        }

        // Expose Spout shared memory to the renderer for the main frame.
        // Two tiers:
        //   1. window.__spoutFrameBuffer — zero-copy external ArrayBuffer (fast path,
        //      requires V8 sandbox to be disabled; CreateArrayBuffer returns nullptr otherwise)
        //   2. window.__spoutGetFrame()  — per-frame copy via CreateArrayBufferWithCopy
        //      (sandbox-safe; called synchronously in requestAnimationFrame)
        if (frame->IsMain()) {
            std::string url = frame->GetURL().ToString();
            std::string idStr = GetQueryParam(url, "spoutReceiverId");

            if (!idStr.empty()) {
                std::string mappingName = "SpoutFrame_" + idStr;
                constexpr size_t kMapped = SpoutFrameHandler::kMappedSize;
                std::string diag = "idStr=" + idStr;

                HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, mappingName.c_str());
                diag += "; OpenFileMappingA=" + std::string(hMap ? "ok" : "FAILED(err=" + std::to_string(GetLastError()) + ")");

                if (hMap) {
                    void* ptr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, kMapped);
                    diag += "; MapViewOfFile=" + std::string(ptr ? "ok" : "FAILED(err=" + std::to_string(GetLastError()) + ")");

                    if (ptr) {
                        // Tier 1: try zero-copy external ArrayBuffer
                        CefRefPtr<CefV8Value> ab = CefV8Value::CreateArrayBuffer(
                            ptr, kMapped, new MappedMemoryReleaseCallback(hMap));
                        diag += "; CreateArrayBuffer=" + std::string(ab ? "ok(fast-path)" : "nullptr(V8-sandbox)");

                        if (ab) {
                            // V8 sandbox is off — zero-copy fast path
                            window->SetValue("__spoutFrameBuffer", ab,
                                             V8_PROPERTY_ATTRIBUTE_READONLY);
                            // MappedMemoryReleaseCallback owns hMap + ptr; will clean up on GC
                        } else {
                            // Tier 2: V8 sandbox active — expose __spoutGetFrame()
                            // SpoutFrameHandler takes ownership of hMap + ptr
                            CefRefPtr<CefV8Handler> handler = new SpoutFrameHandler(hMap, ptr);
                            CefRefPtr<CefV8Value> fn =
                                CefV8Value::CreateFunction("__spoutGetFrame", handler);
                            bool set = window->SetValue("__spoutGetFrame", fn,
                                             V8_PROPERTY_ATTRIBUTE_READONLY);
                            diag += "; __spoutGetFrame registered=" + std::string(set ? "ok" : "FAILED");
                            // SpoutFrameHandler destructor closes hMap + ptr when fn is GC'd
                        }
                    } else {
                        CloseHandle(hMap);
                    }
                }

                window->SetValue("__spoutDiag", CefV8Value::CreateString(diag),
                                 V8_PROPERTY_ATTRIBUTE_NONE);
            }
        }

        v8Context->Exit();
    }

private:
    // Map of browser ID to sandbox flag
    std::map<int, bool> sandbox_map_;
    std::mutex sandbox_map_mutex_;
    // Helper class to handle V8 function calls
    class V8Handler : public CefV8Handler {
    public:
        V8Handler(CefRefPtr<CefBrowser> browser, const CefString& messageName)
            : browser_(browser), message_name_(messageName) {}

        virtual bool Execute(const CefString& name,
                           CefRefPtr<CefV8Value> object,
                           const CefV8ValueList& arguments,
                           CefRefPtr<CefV8Value>& retval,
                           CefString& exception) override {
            if (arguments.size() > 0 && arguments[0]->IsString()) {
                // Create and send process message to the main process
                CefRefPtr<CefFrame> mainFrame = browser_->GetMainFrame();
                if (mainFrame) {
                    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(message_name_);
                    message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
                    mainFrame->SendProcessMessage(PID_BROWSER, message);
                }
                return true;
            }
            return false;
        }

    private:
        CefRefPtr<CefBrowser> browser_;
        CefString message_name_;
        IMPLEMENT_REFCOUNTING(V8Handler);
    };

    CefRefPtr<CefV8Value> CreatePostMessageFunction(CefRefPtr<CefBrowser> browser,
                                                   const CefString& messageName) {
        return CefV8Value::CreateFunction(
            "postMessage",
            new V8Handler(browser, messageName)
        );
    }

    IMPLEMENT_REFCOUNTING(HelperApp);
};

// Entry point function for Windows sub-processes.
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // Provide CEF with command-line arguments.
    CefMainArgs main_args(hInstance);

    CefRefPtr<CefApp> app(new HelperApp);

    // Execute the sub-process.
    return CefExecuteProcess(main_args, app, nullptr);
}

// Alternative entry point for console applications
int main() {
    return wWinMain(GetModuleHandle(NULL), NULL, GetCommandLineW(), SW_HIDE);
}
