#include <winsock2.h>   // Must come before Windows.h
#include <ws2tcpip.h>
#include <winhttp.h>
#include <Windows.h>
#include <windowsx.h>  // For GET_X_LPARAM and GET_Y_LPARAM
#include <string>
#include <cstring>
#include <functional>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <ctime>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <windows.h>
#include "shared/pending_resize_queue.h"
#include <map>
#include <algorithm>
#include <stdint.h>
#include <shellapi.h>
#include <commctrl.h>
#include <mutex>
#include <atomic>
#include <cstdarg>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>
#include <shobjidl.h>  // For IFileOpenDialog
#include <shlobj.h>    // For SHGetKnownFolderPath, FOLDERID_Downloads
#include <shlguid.h>   // For CLSID_FileOpenDialog
#include <commdlg.h>   // For COMDLG_FILTERSPEC
#include <dcomp.h>     // For DirectComposition
#include <locale>      // For string conversion
#include <codecvt>     // For UTF-8 to wide string conversion
#include <d2d1.h>      // For Direct2D
#include <direct.h>    // For _getcwd
#include <tlhelp32.h>  // For process enumeration
#include <dwmapi.h>    // For DWM thumbnail (NativeDisplayWindow)

// Shared cross-platform utilities
#include "shared/glob_match.h"
#include "shared/callbacks.h"
#include "shared/permissions.h"
#include "shared/mime_types.h"
#include "shared/config.h"
#include "shared/preload_script.h"
#include "shared/navigation_rules.h"
#include "shared/thread_safe_map.h"
#include "shared/shutdown_guard.h"
#include "shared/ffi_helpers.h"
#include "shared/app_paths.h"
#include "shared/accelerator_parser.h"
#include "shared/chromium_flags.h"

using namespace electrobun;


// Push macro definitions to avoid conflicts with Windows headers
#pragma push_macro("GetNextSibling")
#pragma push_macro("GetFirstChild")
#undef GetNextSibling
#undef GetFirstChild

// CEF includes - always include for runtime detection
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_scheme.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_permission_handler.h"
#include "include/cef_dialog_handler.h"
#include "include/cef_download_handler.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"

// Restore macro definitions
#pragma pop_macro("GetFirstChild")
#pragma pop_macro("GetNextSibling")

// Spout2 texture sharing — uses SpoutDX (D3D11-native), statically linked.
// SDK layout (from the Spout2 CMake release package):
//   include/SpoutDX/SpoutDX.h + companion headers  — D3D11-native sender/receiver class
//   MT/lib/SpoutDX_static.lib                       — static lib (matches /MT compile flag)
// SpoutDX::SendTexture(ID3D11Texture2D*) sends a GPU texture directly to Spout receivers.
// CEF's OnAcceleratedPaint delivers a DXGI NT shared texture handle every frame (OSR mode,
// shared_texture_enabled=1). We open that handle and pass the texture directly to SpoutDX.
// No window capture (WGC) needed — zero DWM overhead, browser renders at full speed.
// If SpoutDX headers are absent the code compiles without Spout; startSpoutSender() returns false.
#if __has_include("vendor/spout/include/SpoutDX/SpoutDX.h")
#include "vendor/spout/include/SpoutDX/SpoutDX.h"
#include <d3d11_1.h>    // ID3D11Device1::OpenSharedResource1 (for DXGI NT handles)
#include <dxgi1_2.h>    // IDXGIFactory2, IDXGISwapChain1 (for DWM swap chain)
#define ELECTROBUN_HAS_SPOUT 1

#else
// SpoutDX not found — forward-declare stub so pointers in structs compile.
// Spout output will not be available; startSpoutSender() will return false.
class spoutDX;
#define ELECTROBUN_HAS_SPOUT 0
#endif

// Link required Windows libraries
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
// d3d11.lib / dxgi.lib: added by build.ts when SpoutDX is present.




// Ensure the exported functions have appropriate visibility
#define ELECTROBUN_EXPORT __declspec(dllexport)
#define WM_EXECUTE_SYNC_BLOCK (WM_USER + 1)
#define WM_EXECUTE_ASYNC_BLOCK (WM_USER + 2)
#define WM_DEVTOOLS_CREATE (WM_USER + 3)

// Forward declarations
class AbstractView;
class ContainerView;
class NSWindow;
class NSStatusItem;
class WKWebView;
class MyScriptMessageHandlerWithReply;
class StatusItemTarget;

// CEF function declarations
ELECTROBUN_EXPORT bool isCEFAvailable();

// Type definitions to match macOS types
typedef double CGFloat;

// Function pointer type definitions are in shared/callbacks.h
// Platform-specific aliases
typedef BOOL (*HandlePostMessageWin)(uint32_t webviewId, const char* message);
typedef void (*callAsyncJavascriptCompletionHandler)(const char *messageId, uint32_t webviewId, uint32_t hostWebviewId, const char *responseJSON);
typedef SnapshotCallback zigSnapshotCallback;
typedef StatusItemHandler ZigStatusItemHandler;

// Global map to store container views by window handle
static std::map<HWND, std::unique_ptr<ContainerView>> g_containerViews;

// NativeDisplayWindow: maps for DWM thumbnail-based display windows
static std::map<uint32_t, HWND>        g_windowIdToHwnd;      // BrowserWindow id → HWND
static std::map<uint32_t, HWND>        g_displayWindows;      // display window id → HWND
static std::map<uint32_t, HTHUMBNAIL>  g_displayThumbnails;   // display window id → thumbnail

// Forward declaration — log() is defined later in the file.
void log(const std::string& message);

// ---------------------------------------------------------------------------
// Spout output via CEF OSR OnAcceleratedPaint + SpoutDX::SendTexture (Phase 2d)
// ---------------------------------------------------------------------------
// Architecture: the master CEF browser runs in OSR (windowless) mode with
// shared_texture_enabled=1. CEF calls OnAcceleratedPaint every rendered frame,
// delivering a DXGI NT shared texture handle directly from its GPU compositor.
// We open that handle on our D3D11 device and pass it to SpoutDX::SendTexture.
//
// No DWM window capture (WGC) is involved — WGC was found to throttle CEF
// rendering from 60fps to 45-50fps by changing DWM's composition mode for
// the captured window. OnAcceleratedPaint has zero DWM overhead.
//
// For DWM thumbnailing (NDW display windows): we create a DXGI swap chain on
// the master HWND and Present() each frame so DWM has a surface to thumbnail.

// Per-webview Spout state (keyed by webviewId).
struct SpoutWindowState {
    HWND     hwnd   = nullptr; // master window HWND (for swap chain DWM thumbnailing)
    int      width  = 0;
    int      height = 0;
    spoutDX* sender = nullptr; // SpoutDX instance
    bool     active = false;   // true once startSpoutSender() succeeds

    ID3D11Device*        d3dDevice  = nullptr; // D3D11 device for SpoutDX + swap chain
    ID3D11DeviceContext* d3dContext = nullptr; // immediate context for CopyResource
    IDXGISwapChain1*     swapChain  = nullptr; // swap chain on master HWND for DWM
};
static std::map<uint32_t, SpoutWindowState> g_spoutWindows; // webviewId → state

// ---------------------------------------------------------------------------
// D3D output state — multi-window via direct GPU blit (no DWM thumbnails).
// Each slot corresponds to one NativeDisplayWindow; OnAcceleratedPaint copies
// the matching sub-region of the OSR shared texture into the slot's swap chain.
// Supports any virtual canvas size — no physical-monitor boundary restriction.
// ---------------------------------------------------------------------------
struct D3DOutputSlot {
    uint32_t          displayWindowId = 0;
    IDXGISwapChain1*  swapChain       = nullptr;
    int               sourceX         = 0;
    int               sourceY         = 0;
    int               sourceW         = 0;
    int               sourceH         = 0;
};
// D3D output device and context live in SpoutWindowState (g_spoutWindows).
// D3DOutputState only tracks the NDW swap chain slots.
struct D3DOutputState {
    std::vector<D3DOutputSlot> slots;
};
static std::map<uint32_t, D3DOutputState> g_d3dOutputStates; // webviewId → slot list

// ---------------------------------------------------------------------------
// Spout input receiver state
// ---------------------------------------------------------------------------
struct SpoutInputState {
    spoutDX*             receiver     = nullptr;
    ID3D11Device*        d3dDevice    = nullptr;
    ID3D11DeviceContext* d3dContext   = nullptr;
    ID3D11Texture2D*     stagingTex   = nullptr;

    // Win32 named file mapping — shared with the CEF renderer process.
    // Layout: [0-3] seq(Int32) | [4-7] width(Uint32) | [8-11] height(Uint32)
    //         | [12-15] reserved | [16+] BGRA pixels (up to 4K)
    HANDLE               hFileMapping = NULL;
    void*                mappedPtr    = nullptr;
    static constexpr size_t kMappedSize = 16 + 3840 * 2160 * 4; // ~31.6 MB
    std::string          mappingName; // "SpoutFrame_<id>"

    std::atomic<bool>    active{false};
    std::thread          recvThread;
    std::string          senderName;
};
static std::map<uint32_t, SpoutInputState*> g_spoutReceivers;
static std::atomic<uint32_t> g_nextReceiverId{1};

static void spoutReceiverThreadFn(SpoutInputState* state) {
    // Local staging dimensions — only this thread touches stagingTex, so no mutex needed.
    uint32_t stagingW = 0, stagingH = 0;

    // Typed pointers into the shared memory layout
    int32_t*  seqPtr    = (int32_t*) ((uint8_t*)state->mappedPtr + 0);
    uint32_t* widthPtr  = (uint32_t*)((uint8_t*)state->mappedPtr + 4);
    uint32_t* heightPtr = (uint32_t*)((uint8_t*)state->mappedPtr + 8);
    uint8_t*  pixelPtr  =             (uint8_t*)state->mappedPtr + 16;

    while (state->active) {
        if (state->receiver->ReceiveTexture()) {
            uint32_t w = state->receiver->GetSenderWidth();
            uint32_t h = state->receiver->GetSenderHeight();
            if (w == 0 || h == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            // Guard against frames larger than the pre-allocated mapping
            if ((size_t)w * h * 4 > SpoutInputState::kMappedSize - 16) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
            }

            // Recreate staging texture if dimensions changed (no mutex — only this thread uses stagingTex).
            if (w != stagingW || h != stagingH) {
                if (state->stagingTex) { state->stagingTex->Release(); state->stagingTex = nullptr; }
                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = w; desc.Height = h;
                desc.MipLevels = 1; desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_STAGING;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                state->d3dDevice->CreateTexture2D(&desc, nullptr, &state->stagingTex);
                stagingW = w; stagingH = h;
            }
            if (!state->stagingTex) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }

            // Copy received GPU texture → staging, then Map to CPU.
            ID3D11Texture2D* tex = state->receiver->GetSenderTexture();
            if (!tex) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            state->d3dContext->CopyResource(state->stagingTex, tex);

            D3D11_MAPPED_SUBRESOURCE mapped;
            // DO_NOT_WAIT: yield the CPU while waiting for CopyResource to finish on the GPU,
            // then retry until done. Inner loop avoids blocking the thread while still picking
            // up the frame as soon as the GPU is ready — no missed frames.
            HRESULT mapHr;
            do {
                mapHr = state->d3dContext->Map(state->stagingTex, 0, D3D11_MAP_READ,
                                               D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (mapHr == DXGI_ERROR_WAS_STILL_DRAWING) SwitchToThread();
            } while (mapHr == DXGI_ERROR_WAS_STILL_DRAWING && state->active);
            if (SUCCEEDED(mapHr)) {
                // Write width + height, then pixels, then increment seq LAST.
                // _InterlockedIncrement provides a full memory barrier on x86, ensuring
                // the renderer process always reads consistent data when it sees a new seq.
                *widthPtr  = w;
                *heightPtr = h;
                // Fast path: if the staging texture has no row padding, one memcpy covers
                // the whole frame (saves ~1000 calls at 1080p; NVIDIA BGRA textures are
                // typically contiguous so RowPitch == w * 4).
                if (mapped.RowPitch == w * 4) {
                    memcpy(pixelPtr, (uint8_t*)mapped.pData, (size_t)w * h * 4);
                } else {
                    for (uint32_t row = 0; row < h; row++)
                        memcpy(pixelPtr + row * w * 4,
                               (uint8_t*)mapped.pData + row * mapped.RowPitch, w * 4);
                }
                _InterlockedIncrement((LONG*)seqPtr);
                state->d3dContext->Unmap(state->stagingTex, 0);
            }
        } else {
            // No new frame available — yield briefly before polling again.
            // Only sleep when idle; don't add latency after a successful frame delivery.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

// Helper: create a DXGI flip swap chain on an HWND for DWM thumbnailing.
// In OSR mode CEF doesn't present to the HWND; we do it manually each frame
// so DWM has a surface to thumbnail for the NativeDisplayWindow mirrors.
static IDXGISwapChain1* createSwapChainForHwnd(ID3D11Device* device, HWND hwnd, int w, int h) {
    IDXGIDevice*   dxgiDevice  = nullptr;
    IDXGIAdapter*  dxgiAdapter = nullptr;
    IDXGIFactory2* factory2    = nullptr;

    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) return nullptr;
    dxgiDevice->GetAdapter(&dxgiAdapter);
    dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory2);

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width       = (UINT)w;
    desc.Height      = (UINT)h;
    desc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    IDXGISwapChain1* sc = nullptr;
    if (factory2) factory2->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr, &sc);

    if (factory2)    factory2->Release();
    if (dxgiAdapter) dxgiAdapter->Release();
    if (dxgiDevice)  dxgiDevice->Release();
    return sc;
}


static GetMimeType g_getMimeType = nullptr;
static GetHTMLForWebviewSync g_getHTMLForWebviewSync = nullptr;

// Global variables for CEF cache path isolation
static std::string g_electrobunChannel = "";
static std::string g_electrobunIdentifier = "";
static std::string g_electrobunName = "";

// Webview content storage (replaces JSCallback approach)
static std::map<uint32_t, std::string> webviewHTMLContent;
static std::mutex webviewHTMLMutex;

// Forward declaration for AbstractView
class AbstractView;

// Global map to track all AbstractView instances by their webviewId
static std::map<uint32_t, AbstractView*> g_abstractViews;
static std::mutex g_abstractViewsMutex;

// Forward declaration for navigation rules helper (defined after AbstractView class)
bool checkNavigationRules(AbstractView* view, const std::string& url);

// Forward declarations for HTML content management
extern "C" ELECTROBUN_EXPORT const char* getWebviewHTMLContent(uint32_t webviewId);
extern "C" ELECTROBUN_EXPORT void setWebviewHTMLContent(uint32_t webviewId, const char* htmlContent);

// Global mutex to serialize webview creation
static std::mutex g_webviewCreationMutex;

// Global map to store preload scripts by browser ID (needs to be early for load handler)
static std::map<int, std::string> g_preloadScripts;

// Global map to track browser ID to webview ID mapping (for CEF scheme handler)
static std::map<int, uint32_t> browserToWebviewMap;
static std::mutex browserMapMutex;

// Global map to store CEFViews by container window handle (using void* to avoid forward declaration issues)
static std::map<HWND, void*> g_cefViews;

// Global map to store pending CEF navigations for timing workaround - use browser ID instead of pointer
static std::map<int, std::string> g_pendingCefNavigations;
// Global map to store browser references by ID for safe access
static std::map<int, CefRefPtr<CefBrowser>> g_cefBrowsers;
// Global browser counter (moved from class static to global)
static int g_browser_count = 0;
// Global map to store pending URLs for async browser creation
static std::map<HWND, std::string> g_pendingUrls;

// Permission cache types and functions are in shared/permissions.h


static HMENU g_applicationMenu = NULL;
static std::unique_ptr<StatusItemTarget> g_appMenuTarget = nullptr;

// Global map to store menu item actions by menu ID
static std::map<UINT, std::string> g_menuItemActions;
static UINT g_nextMenuId = WM_USER + 1000;  // Start menu IDs from a safe range

// Accelerator table management for menu keyboard shortcuts
static std::vector<ACCEL> g_menuAccelerators;
static HACCEL g_hAccelTable = NULL;

// Global state for custom window dragging
static BOOL g_isMovingWindow = FALSE;
static HWND g_targetWindow = NULL;
static POINT g_initialCursorPos = {};
static POINT g_initialWindowPos = {};

// WebView positioning constants
static const int OFFSCREEN_OFFSET = -20000;

// Remote DevTools port
static int g_remoteDebugPort = 9222;

static bool IsPortAvailable(int port) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)port);

    int result = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    closesocket(sock);
    WSACleanup();
    return result == 0;
}

static int FindAvailableRemoteDebugPort(int startPort, int endPort) {
    for (int port = startPort; port <= endPort; ++port) {
        if (IsPortAvailable(port)) {
            return port;
        }
    }
    return 0;
}

// CEF global variables
static bool g_cef_initialized = false;
static CefRefPtr<CefApp> g_cef_app;
static electrobun::ChromiumFlagConfig g_userChromiumFlags;
static HANDLE g_job_object = nullptr;  // Job object to track all child processes

// Quit/shutdown coordination
static QuitRequestedHandler g_quitRequestedHandler = nullptr;
static std::atomic<bool> g_shutdownComplete{false};
static std::atomic<bool> g_eventLoopStopping{false};
static DWORD g_mainThreadId = 0;
static HANDLE g_eventLoopReadyEvent = NULL;

// Simple CEF App class for minimal implementation
// Hidden window message for CEF external message pump scheduling
#define WM_CEF_SCHEDULE_WORK (WM_USER + 100)
static HWND g_cefPumpWindow = NULL;

class ElectrobunCefApp : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }

    void OnScheduleMessagePumpWork(int64_t delay_ms) override {
        // Called by CEF when it needs CefDoMessageLoopWork to be called.
        // With external_message_pump=true, CEF does NOT internally pump Windows messages,
        // preventing it from interfering with our Windows message pump.
        if (g_cefPumpWindow) {
            if (delay_ms <= 0) {
                // Immediate work needed
                ::PostMessage(g_cefPumpWindow, WM_CEF_SCHEDULE_WORK, 0, 0);
            } else {
                // Schedule work after delay
                SetTimer(g_cefPumpWindow, 1, (UINT)delay_ms, nullptr);
            }
        }
    }

    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override {
        // Windows default flags — can be overridden via chromiumFlags in config
        static const std::vector<electrobun::DefaultFlag> defaults = {
            {"in-process-gpu", ""},
            {"disable-web-security", ""},
            {"disable-features=VizDisplayCompositor", ""},
            {"remote-allow-origins", "*"},
            {"allow-insecure-localhost", ""},
        };
        electrobun::applyDefaultFlags(defaults, g_userChromiumFlags.skip, command_line);

        // Apply user-defined chromium flags from build.json
        electrobun::applyChromiumFlags(g_userChromiumFlags, command_line);
    }

private:
    IMPLEMENT_REFCOUNTING(ElectrobunCefApp);
};

// Forward declaration for CEF client (needed for load handler)
class ElectrobunCefClient;

// CEF Load Handler for debugging navigation
class ElectrobunLoadHandler : public CefLoadHandler {
public:
    uint32_t webview_id_ = 0;
    WebviewEventHandler webview_event_handler_ = nullptr;
    CefRefPtr<ElectrobunCefClient> client_ = nullptr;

    ElectrobunLoadHandler() {}

    void SetWebviewId(uint32_t id) { webview_id_ = id; }
    void SetWebviewEventHandler(WebviewEventHandler handler) { webview_event_handler_ = handler; }
    void SetClient(CefRefPtr<ElectrobunCefClient> client) { client_ = client; }

    void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type) override;
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;
    void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl) override {
        std::cout << "[CEF] LoadError: " << static_cast<int>(errorCode)
                  << " - " << errorText.ToString()
                  << " for URL: " << failedUrl.ToString() << std::endl;
    }

private:
    IMPLEMENT_REFCOUNTING(ElectrobunLoadHandler);
};

// Global map to store CEF clients for browser connection
static std::map<HWND, CefRefPtr<ElectrobunCefClient>> g_cefClients;

// Forward declaration for helper functions (defined after class definitions)
void SetBrowserOnClient(CefRefPtr<ElectrobunCefClient> client, CefRefPtr<CefBrowser> browser);
void SetBrowserOnCEFView(HWND parentWindow, CefRefPtr<CefBrowser> browser);

// CEF Life Span Handler for async browser creation
class ElectrobunLifeSpanHandler : public CefLifeSpanHandler {
public:
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        // Note: Browser setup is now handled synchronously during CreateBrowserSync
    }

    // DoClose is called when the browser window is about to close.
    // Return true for OOPIFs to prevent CEF from closing the parent window.
    // Return false only for the main/last browser when actually quitting the app.
    bool DoClose(CefRefPtr<CefBrowser> browser) override {
        std::cout << "[CEF] DoClose: Browser ID " << browser->GetIdentifier()
                  << ", browser_count=" << g_browser_count << std::endl;

        // For OOPIFs (when there are other browsers still open, or when we're not shutting down),
        // return true to prevent CEF from sending WM_CLOSE to the parent window.
        // We handle the actual close ourselves in remove() by calling CloseBrowser.
        if (!g_eventLoopStopping.load()) {
            std::cout << "[CEF] DoClose: Returning true to prevent parent window close" << std::endl;
            return true;  // We'll handle the close - prevents CEF from closing parent
        }

        std::cout << "[CEF] DoClose: Returning false - app is shutting down" << std::endl;
        return false;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        std::cout << "[CEF] OnBeforeClose: Browser ID " << browser->GetIdentifier() << " closing" << std::endl;

        // Remove browser from global tracking
        g_cefBrowsers.erase(browser->GetIdentifier());
        {
            std::lock_guard<std::mutex> lock(browserMapMutex);
            browserToWebviewMap.erase(browser->GetIdentifier());
        }
        g_browser_count--;

        std::cout << "[CEF] Remaining browsers: " << g_browser_count << std::endl;

        // Note: Do NOT quit the message loop here when browser count reaches 0.
        // OOPIFs are CEF browsers that can be removed while the main window stays open.
        // Window/app closing is handled separately by the window close handlers.
    }

private:
    IMPLEMENT_REFCOUNTING(ElectrobunLifeSpanHandler);
};

// Forward declaration for DevTools callback
class ElectrobunCefClient;
typedef void (*RemoteDevToolsClosedCallback)(void* ctx, int target_id);
void RemoteDevToolsClosed(void* ctx, int target_id);

// Lightweight CefClient for the DevTools browser window
class RemoteDevToolsClient : public CefClient, public CefLifeSpanHandler {
public:
    RemoteDevToolsClient(RemoteDevToolsClosedCallback callback, void* ctx, int target_id)
        : callback_(callback), ctx_(ctx), target_id_(target_id) {}

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        if (callback_) {
            callback_(ctx_, target_id_);
        }
    }

private:
    RemoteDevToolsClosedCallback callback_ = nullptr;
    void* ctx_ = nullptr;
    int target_id_ = 0;
    IMPLEMENT_REFCOUNTING(RemoteDevToolsClient);
};

// DevTools window class and WndProc
struct DevToolsWindowContext {
    RemoteDevToolsClosedCallback close_callback = nullptr;
    void* ctx = nullptr;
    int target_id = 0;
    CefRefPtr<CefBrowser> browser;
};

static std::once_flag g_devtoolsClassRegistered;
static const char* DEVTOOLS_WINDOW_CLASS = "ElectrobunDevToolsClass";

static LRESULT CALLBACK DevToolsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DevToolsWindowContext* dtCtx = nullptr;

    if (msg == WM_NCCREATE) {
        CREATESTRUCTA* cs = (CREATESTRUCTA*)lParam;
        dtCtx = (DevToolsWindowContext*)cs->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)dtCtx);
    } else {
        dtCtx = (DevToolsWindowContext*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    }

    switch (msg) {
        case WM_CLOSE:
            // Hide the window instead of destroying it to avoid CEF teardown issues
            ShowWindow(hwnd, SW_HIDE);
            if (dtCtx && dtCtx->close_callback) {
                dtCtx->close_callback(dtCtx->ctx, dtCtx->target_id);
            }
            return 0;

        case WM_SIZE:
            if (dtCtx && dtCtx->browser) {
                HWND browserHwnd = dtCtx->browser->GetHost()->GetWindowHandle();
                if (browserHwnd) {
                    RECT rect;
                    GetClientRect(hwnd, &rect);
                    SetWindowPos(browserHwnd, nullptr, 0, 0,
                                 rect.right - rect.left, rect.bottom - rect.top,
                                 SWP_NOZORDER);
                }
            }
            break;

        case WM_DESTROY:
            return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void EnsureDevToolsWindowClassRegistered() {
    std::call_once(g_devtoolsClassRegistered, []() {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = DevToolsWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = DEVTOOLS_WINDOW_CLASS;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassA(&wc);
    });
}

// Forward declaration for MIME type detection
std::string getMimeTypeForFile(const std::string& path);

// CEF Response Filter for script injection
class ElectrobunResponseFilter : public CefResponseFilter {
public:
    ElectrobunResponseFilter(const std::string& script) : script_(script) {}

    bool InitFilter() override {
        return true;
    }

    FilterStatus Filter(void* data_in, size_t data_in_size, size_t& data_in_read,
                       void* data_out, size_t data_out_size, size_t& data_out_written) override {
        // Read all input data
        if (data_in_size > 0) {
            data_buffer_.append(static_cast<char*>(data_in), data_in_size);
            data_in_read = data_in_size;
        } else {
            data_in_read = 0;
        }
        
        // If no input data (end of stream), process the accumulated data
        if (data_in_size == 0 && !processed_) {
            ProcessAccumulatedData();
            processed_ = true;
        }
        
        // Output processed data
        data_out_written = 0;
        if (processed_ && output_offset_ < processed_data_.size()) {
            size_t remaining = processed_data_.size() - output_offset_;
            size_t copy_size = (data_out_size < remaining) ? data_out_size : remaining;
            memcpy(data_out, processed_data_.data() + output_offset_, copy_size);
            output_offset_ += copy_size;
            data_out_written = copy_size;
        }
        
        // Return status based on whether we have more data to output
        if (data_in_size == 0 && output_offset_ >= processed_data_.size()) {
            return RESPONSE_FILTER_DONE;
        } else {
            return RESPONSE_FILTER_NEED_MORE_DATA;
        }
    }

    void ProcessAccumulatedData() {
        // Process accumulated data and inject script
        processed_data_ = data_buffer_;

        // Look for <head> tag and inject script right after it (as first element in head)
        // This ensures preload script executes before any other scripts in the page
        size_t head_pos = processed_data_.find("<head>");
        if (head_pos != std::string::npos && !script_.empty()) {
            // Insert after the <head> tag (head_pos + 6 to skip past "<head>")
            size_t insert_pos = head_pos + 6;
            std::string script_tag = "<script>" + script_ + "</script>";
            processed_data_.insert(insert_pos, script_tag);
        } else {
            // Fallback: try case-insensitive search for <head with attributes
            size_t head_start = processed_data_.find("<head");
            if (head_start != std::string::npos && !script_.empty()) {
                // Find the end of the opening <head...> tag
                size_t head_end = processed_data_.find(">", head_start);
                if (head_end != std::string::npos) {
                    size_t insert_pos = head_end + 1;
                    std::string script_tag = "<script>" + script_ + "</script>";
                    processed_data_.insert(insert_pos, script_tag);
                }
            }
        }
    }

private:
    std::string script_;
    std::string data_buffer_;
    std::string processed_data_;
    size_t output_offset_ = 0;
    bool processed_ = false;
    IMPLEMENT_REFCOUNTING(ElectrobunResponseFilter);
};

// Forward declaration for ElectrobunCefClient
class ElectrobunCefClient;

// CEF Resource Request Handler to inject preload scripts via response filter
class ElectrobunResourceRequestHandler : public CefResourceRequestHandler {
public:
    CefRefPtr<ElectrobunCefClient> client_ = nullptr;

    ElectrobunResourceRequestHandler(CefRefPtr<ElectrobunCefClient> client) : client_(client) {}

    // Response filter to inject preload scripts into HTML before parsing
    // This ensures scripts execute BEFORE any page JavaScript
    CefRefPtr<CefResponseFilter> GetResourceResponseFilter(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request,
        CefRefPtr<CefResponse> response) override;

    IMPLEMENT_REFCOUNTING(ElectrobunResourceRequestHandler);
};

// CEF Request Handler
class ElectrobunRequestHandler : public CefRequestHandler {
public:
    uint32_t webview_id_ = 0;
    WebviewEventHandler webview_event_handler_ = nullptr;
    AbstractView* abstract_view_ = nullptr;
    CefRefPtr<ElectrobunCefClient> client_ = nullptr;

    // Static debounce timestamp for ctrl+click handling
    static double lastCtrlClickTime;

    ElectrobunRequestHandler() {}

    void SetWebviewId(uint32_t id) { webview_id_ = id; }
    void SetWebviewEventHandler(WebviewEventHandler handler) { webview_event_handler_ = handler; }
    void SetAbstractView(AbstractView* view) { abstract_view_ = view; }
    void SetClient(CefRefPtr<ElectrobunCefClient> client) { client_ = client; }

    // Return resource request handler to enable response filtering
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request,
        bool is_navigation,
        bool is_download,
        const CefString& request_initiator,
        bool& disable_default_handling) override {

        if (client_) {
            return new ElectrobunResourceRequestHandler(client_);
        }
        return nullptr;
    }

    // Handle navigation requests with Ctrl+click detection
    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       CefRefPtr<CefRequest> request,
                       bool user_gesture,
                       bool is_redirect) override {
        std::string url = request->GetURL().ToString();

        // Check if Ctrl key is held
        SHORT ctrlState = GetKeyState(VK_CONTROL);
        bool isCtrlHeld = (ctrlState & 0x8000) != 0;

        // Same-URL navigation (e.g. location.reload() while Ctrl+R is held) must not
        // be treated as a Ctrl+click — Ctrl is still physically down from the shortcut.
        std::string currentUrl = frame->GetURL().ToString();
        bool isSamePageReload = (url == currentUrl);

        printf("[CEF OnBeforeBrowse] url=%s user_gesture=%d is_redirect=%d ctrlState=0x%04X isCtrlHeld=%d isSamePageReload=%d hasHandler=%d webviewId=%u\n",
               url.c_str(), user_gesture, is_redirect, ctrlState, isCtrlHeld, isSamePageReload, webview_event_handler_ != nullptr, webview_id_);

        if (isCtrlHeld && user_gesture && !is_redirect && !isSamePageReload && webview_event_handler_) {
            // Debounce: ignore ctrl+click navigations within 500ms
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;

            printf("[CEF OnBeforeBrowse] Ctrl held! now=%.3f lastTime=%.3f diff=%.3f\n",
                   now, lastCtrlClickTime, now - lastCtrlClickTime);

            if (now - lastCtrlClickTime >= 0.5) {
                lastCtrlClickTime = now;

                // Escape URL for JSON
                std::string escapedUrl;
                for (char c : url) {
                    switch (c) {
                        case '"': escapedUrl += "\\\""; break;
                        case '\\': escapedUrl += "\\\\"; break;
                        default: escapedUrl += c; break;
                    }
                }

                std::string eventData = "{\"url\":\"" + escapedUrl +
                                       "\",\"isCmdClick\":true,\"modifierFlags\":0}";
                printf("[CEF OnBeforeBrowse] Firing new-window-open: %s\n", eventData.c_str());
                // Use strdup to create persistent copies for the FFI callback
                webview_event_handler_(webview_id_, _strdup("new-window-open"), _strdup(eventData.c_str()));
                return true;  // Cancel navigation
            } else {
                printf("[CEF OnBeforeBrowse] Debounced - too soon after last ctrl+click\n");
            }
        }

        // Check navigation rules synchronously from native-stored rules
        // Navigation is allowed by default
        bool shouldAllow = true;
        if (abstract_view_) {
            shouldAllow = checkNavigationRules(abstract_view_, url);
        }

        // Fire will-navigate event with allowed status
        if (webview_event_handler_) {
            // Escape URL for JSON
            std::string escapedUrl;
            for (char c : url) {
                switch (c) {
                    case '"': escapedUrl += "\\\""; break;
                    case '\\': escapedUrl += "\\\\"; break;
                    default: escapedUrl += c; break;
                }
            }
            std::string eventData = "{\"url\":\"" + escapedUrl + "\",\"allowed\":" +
                                   (shouldAllow ? "true" : "false") + "}";
            webview_event_handler_(webview_id_, _strdup("will-navigate"), _strdup(eventData.c_str()));
        }

        return !shouldAllow;  // Return true to cancel navigation
    }

private:
    IMPLEMENT_REFCOUNTING(ElectrobunRequestHandler);
};

// Initialize static debounce timestamp
double ElectrobunRequestHandler::lastCtrlClickTime = 0;

// CEF Context Menu Handler for devtools support
class ElectrobunContextMenuHandler : public CefContextMenuHandler {
public:
    ElectrobunContextMenuHandler() {}
    
    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefContextMenuParams> params,
                           CefRefPtr<CefMenuModel> model) override {
        // Add "Inspect Element" menu item
        model->AddSeparator();
        model->AddItem(26501, "Inspect Element");
    }
    
    // Defined out-of-line after ElectrobunCefClient (needs full class definition)
    bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefContextMenuParams> params,
                            int command_id,
                            EventFlags event_flags) override;

private:
    IMPLEMENT_REFCOUNTING(ElectrobunContextMenuHandler);
};

// CEF Permission Handler for user media and other permissions
class ElectrobunPermissionHandler : public CefPermissionHandler {
public:
    bool OnRequestMediaAccessPermission(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        const CefString& requesting_origin,
        uint32_t requested_permissions,
        CefRefPtr<CefMediaAccessCallback> callback) override {
        
        std::string origin = requesting_origin.ToString();
        printf("CEF: Media access permission requested for %s (permissions: %u)\n", origin.c_str(), requested_permissions);
        
        // Check cache first
        PermissionStatus cachedStatus = getPermissionFromCache(origin, PermissionType::USER_MEDIA);
        
        if (cachedStatus == PermissionStatus::ALLOWED) {
            printf("CEF: Using cached permission: User previously allowed media access for %s\n", origin.c_str());
            callback->Continue(requested_permissions); // Allow all requested permissions
            return true;
        } else if (cachedStatus == PermissionStatus::DENIED) {
            printf("CEF: Using cached permission: User previously blocked media access for %s\n", origin.c_str());
            callback->Cancel();
            return true;
        }
        
        // No cached permission, show dialog
        printf("CEF: No cached permission found for %s, showing dialog\n", origin.c_str());
        
        // Show Windows message box
        std::string message = "This page wants to access your camera and/or microphone.\n\nDo you want to allow this?";
        std::string title = "Camera & Microphone Access";
        
        int result = MessageBoxA(
            nullptr,
            message.c_str(),
            title.c_str(),
            MB_YESNO | MB_ICONQUESTION | MB_TOPMOST
        );
        
        // Handle response and cache the decision
        if (result == IDYES) {
            callback->Continue(requested_permissions); // Allow all requested permissions
            cachePermission(origin, PermissionType::USER_MEDIA, PermissionStatus::ALLOWED);
            printf("CEF: User allowed media access for %s (cached)\n", origin.c_str());
        } else {
            callback->Cancel();
            cachePermission(origin, PermissionType::USER_MEDIA, PermissionStatus::DENIED);
            printf("CEF: User blocked media access for %s (cached)\n", origin.c_str());
        }
        
        return true; // We handled the permission request
    }
    
    bool OnShowPermissionPrompt(
        CefRefPtr<CefBrowser> browser,
        uint64_t prompt_id,
        const CefString& requesting_origin,
        uint32_t requested_permissions,
        CefRefPtr<CefPermissionPromptCallback> callback) override {
        
        std::string origin = requesting_origin.ToString();
        printf("CEF: Permission prompt requested for %s (permissions: %u)\n", origin.c_str(), requested_permissions);
        
        // Handle different permission types
        PermissionType permType = PermissionType::OTHER;
        std::string message = "This page is requesting additional permissions.\n\nDo you want to allow this?";
        std::string title = "Permission Request";
        
        // Check for specific permission types
        if (requested_permissions & CEF_PERMISSION_TYPE_CAMERA_STREAM ||
            requested_permissions & CEF_PERMISSION_TYPE_MIC_STREAM) {
            permType = PermissionType::USER_MEDIA;
            message = "This page wants to access your camera and/or microphone.\n\nDo you want to allow this?";
            title = "Camera & Microphone Access";
        } else if (requested_permissions & CEF_PERMISSION_TYPE_GEOLOCATION) {
            permType = PermissionType::GEOLOCATION;
            message = "This page wants to access your location.\n\nDo you want to allow this?";
            title = "Location Access";
        } else if (requested_permissions & CEF_PERMISSION_TYPE_NOTIFICATIONS) {
            permType = PermissionType::NOTIFICATIONS;
            message = "This page wants to show notifications.\n\nDo you want to allow this?";
            title = "Notification Permission";
        }
        
        // Check cache first
        PermissionStatus cachedStatus = getPermissionFromCache(origin, permType);
        
        if (cachedStatus == PermissionStatus::ALLOWED) {
            printf("CEF: Using cached permission: User previously allowed %s for %s\n", title.c_str(), origin.c_str());
            callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
            return true;
        } else if (cachedStatus == PermissionStatus::DENIED) {
            printf("CEF: Using cached permission: User previously blocked %s for %s\n", title.c_str(), origin.c_str());
            callback->Continue(CEF_PERMISSION_RESULT_DENY);
            return true;
        }
        
        // No cached permission, show dialog
        printf("CEF: No cached permission found for %s, showing dialog\n", origin.c_str());
        
        // Show Windows message box
        int result = MessageBoxA(
            nullptr,
            message.c_str(),
            title.c_str(),
            MB_YESNO | MB_ICONQUESTION | MB_TOPMOST
        );
        
        // Handle response and cache the decision
        if (result == IDYES) {
            callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
            cachePermission(origin, permType, PermissionStatus::ALLOWED);
            printf("CEF: User allowed %s for %s (cached)\n", title.c_str(), origin.c_str());
        } else {
            callback->Continue(CEF_PERMISSION_RESULT_DENY);
            cachePermission(origin, permType, PermissionStatus::DENIED);
            printf("CEF: User blocked %s for %s (cached)\n", title.c_str(), origin.c_str());
        }
        
        return true; // We handled the permission request
    }
    
    void OnDismissPermissionPrompt(
        CefRefPtr<CefBrowser> browser,
        uint64_t prompt_id,
        cef_permission_request_result_t result) override {
        
        printf("CEF: Permission prompt %I64u dismissed with result %d\n", prompt_id, result);
        // Optional: Handle prompt dismissal if needed
    }

private:
    IMPLEMENT_REFCOUNTING(ElectrobunPermissionHandler);
};

// Helper functions for string conversion
std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    
    int sizeRequired = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (sizeRequired <= 0) {
        // Fallback to simple conversion (ASCII safe)
        std::wstring result;
        result.reserve(str.length());
        for (char c : str) {
            result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        return result;
    }
    
    std::wstring wstr(sizeRequired, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], sizeRequired);
    wstr.pop_back(); // Remove null terminator
    return wstr;
}

std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    
    int sizeRequired = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sizeRequired <= 0) {
        // Fallback to simple conversion (ASCII safe)
        std::string result;
        result.reserve(wstr.length());
        for (wchar_t wc : wstr) {
            if (wc <= 127) { // ASCII range
                result.push_back(static_cast<char>(wc));
            } else {
                result.push_back('?'); // Replace non-ASCII with ?
            }
        }
        return result;
    }
    
    std::string str(sizeRequired, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], sizeRequired, nullptr, nullptr);
    str.pop_back(); // Remove null terminator
    return str;
}

// CEF Dialog Handler for file dialogs
class ElectrobunDialogHandler : public CefDialogHandler {
public:
    bool OnFileDialog(CefRefPtr<CefBrowser> browser,
                      FileDialogMode mode,
                      const CefString& title,
                      const CefString& default_file_path,
                      const std::vector<CefString>& accept_filters,
                      const std::vector<CefString>& accept_extensions,
                      const std::vector<CefString>& accept_descriptions,
                      CefRefPtr<CefFileDialogCallback> callback) override {
        
        printf("CEF Windows: File dialog requested - mode: %d\n", static_cast<int>(mode));
        
        // Run file dialog on main thread using Windows native dialog
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(hr)) {
            callback->Continue(std::vector<CefString>());
            return true;
        }
        
        IFileOpenDialog* pFileDialog = nullptr;
        hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, (void**)&pFileDialog);
        if (FAILED(hr)) {
            CoUninitialize();
            callback->Continue(std::vector<CefString>());
            return true;
        }
        
        // Set dialog options based on mode
        DWORD dwFlags = 0;
        pFileDialog->GetOptions(&dwFlags);
        
        if (mode == FILE_DIALOG_OPEN_MULTIPLE) {
            dwFlags |= FOS_ALLOWMULTISELECT;
        } else if (mode == FILE_DIALOG_OPEN_FOLDER) {
            dwFlags |= FOS_PICKFOLDERS;
        }
        
        pFileDialog->SetOptions(dwFlags);
        
        // Set title if provided
        if (!title.empty()) {
            std::wstring wTitle = StringToWString(title.ToString());
            pFileDialog->SetTitle(wTitle.c_str());
        }
        
        // Set default file path if provided
        if (!default_file_path.empty()) {
            std::wstring wPath = StringToWString(default_file_path.ToString());
            
            IShellItem* pDefaultFolder = nullptr;
            hr = SHCreateItemFromParsingName(wPath.c_str(), nullptr, IID_IShellItem, (void**)&pDefaultFolder);
            if (SUCCEEDED(hr)) {
                if (mode == FILE_DIALOG_SAVE) {
                    pFileDialog->SetDefaultFolder(pDefaultFolder);
                } else {
                    pFileDialog->SetFolder(pDefaultFolder);
                }
                pDefaultFolder->Release();
            }
        }
        
        // Set file filters
        if (!accept_filters.empty()) {
            std::vector<COMDLG_FILTERSPEC> filterSpecs;
            std::vector<std::wstring> filterNames;
            std::vector<std::wstring> filterPatterns;
            
            for (const auto& filter : accept_filters) {
                std::wstring wFilter = StringToWString(filter.ToString());
                
                if (wFilter.find(L".") != 0 && wFilter != L"*" && wFilter != L"*.*") {
                    wFilter = L"." + wFilter;
                }
                
                std::wstring pattern = (wFilter == L"*" || wFilter == L"*.*") ? L"*.*" : L"*" + wFilter;
                std::wstring name = (wFilter == L"*" || wFilter == L"*.*") ? L"All files" : wFilter.substr(1) + L" files";
                
                filterNames.push_back(name);
                filterPatterns.push_back(pattern);
                
                COMDLG_FILTERSPEC spec;
                spec.pszName = filterNames.back().c_str();
                spec.pszSpec = filterPatterns.back().c_str();
                filterSpecs.push_back(spec);
            }
            
            pFileDialog->SetFileTypes(static_cast<UINT>(filterSpecs.size()), filterSpecs.data());
        }
        
        // Show the dialog
        hr = pFileDialog->Show(nullptr);
        
        std::vector<CefString> file_paths;
        if (SUCCEEDED(hr)) {
            if (mode == FILE_DIALOG_OPEN_MULTIPLE) {
                IShellItemArray* pShellItemArray = nullptr;
                hr = pFileDialog->GetResults(&pShellItemArray);
                if (SUCCEEDED(hr)) {
                    DWORD count = 0;
                    pShellItemArray->GetCount(&count);
                    
                    for (DWORD i = 0; i < count; i++) {
                        IShellItem* pShellItem = nullptr;
                        hr = pShellItemArray->GetItemAt(i, &pShellItem);
                        if (SUCCEEDED(hr)) {
                            PWSTR pszFilePath = nullptr;
                            hr = pShellItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                            if (SUCCEEDED(hr)) {
                                // Convert wide string to regular string
                                std::string path = WStringToString(pszFilePath);
                                file_paths.push_back(path);
                                CoTaskMemFree(pszFilePath);
                            }
                            pShellItem->Release();
                        }
                    }
                    pShellItemArray->Release();
                }
            } else {
                IShellItem* pShellItem = nullptr;
                hr = pFileDialog->GetResult(&pShellItem);
                if (SUCCEEDED(hr)) {
                    PWSTR pszFilePath = nullptr;
                    hr = pShellItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                    if (SUCCEEDED(hr)) {
                        // Convert wide string to regular string
                        std::string path = WStringToString(pszFilePath);
                        file_paths.push_back(path);
                        CoTaskMemFree(pszFilePath);
                    }
                    pShellItem->Release();
                }
            }
        }
        
        pFileDialog->Release();
        CoUninitialize();
        
        // Call the callback with results
        callback->Continue(file_paths);
        
        printf("CEF Windows: File dialog completed with %zu files selected\n", file_paths.size());
        return true; // We handled the dialog
    }
    
private:
    IMPLEMENT_REFCOUNTING(ElectrobunDialogHandler);
};

// CEF Download handler for Windows
class ElectrobunDownloadHandler : public CefDownloadHandler {
public:
    ElectrobunDownloadHandler() {}

    bool OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefDownloadItem> download_item,
                          const CefString& suggested_name,
                          CefRefPtr<CefBeforeDownloadCallback> callback) override {
        printf("CEF Windows: OnBeforeDownload for %s\n", suggested_name.ToString().c_str());

        // Get the Downloads folder using Windows API
        wchar_t* downloadsPath = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &downloadsPath);

        if (SUCCEEDED(hr) && downloadsPath) {
            // Convert suggested name to wide string
            std::string suggestedStr = suggested_name.ToString();
            std::wstring suggestedNameW(suggestedStr.begin(), suggestedStr.end());

            // Build the full destination path
            std::wstring destPath = downloadsPath;
            destPath += L"\\";
            destPath += suggestedNameW;

            // Handle duplicate filenames
            std::wstring basePath = destPath;
            std::wstring extension;
            size_t dotPos = destPath.find_last_of(L'.');
            size_t slashPos = destPath.find_last_of(L"\\/");
            if (dotPos != std::wstring::npos && (slashPos == std::wstring::npos || dotPos > slashPos)) {
                basePath = destPath.substr(0, dotPos);
                extension = destPath.substr(dotPos);
            }

            int counter = 1;
            while (GetFileAttributesW(destPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                destPath = basePath + L" (" + std::to_wstring(counter) + L")" + extension;
                counter++;
            }

            // Convert wide string back to UTF-8 for CEF
            int size = WideCharToMultiByte(CP_UTF8, 0, destPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string utf8Path(size - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, destPath.c_str(), -1, &utf8Path[0], size, nullptr, nullptr);

            printf("CEF Windows: Downloading to %s\n", utf8Path.c_str());

            // Continue the download to the specified path without showing a dialog
            callback->Continue(utf8Path, false);

            CoTaskMemFree(downloadsPath);
        } else {
            printf("CEF Windows: Could not get Downloads folder, using default behavior\n");
            callback->Continue("", false);
        }

        return true;  // We handled it
    }

    void OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefDownloadItem> download_item,
                           CefRefPtr<CefDownloadItemCallback> callback) override {
        if (download_item->IsComplete()) {
            printf("CEF Windows: Download complete - %s\n", download_item->GetFullPath().ToString().c_str());
        } else if (download_item->IsCanceled()) {
            printf("CEF Windows: Download canceled\n");
        } else if (download_item->IsInProgress()) {
            int percent = download_item->GetPercentComplete();
            if (percent >= 0 && percent % 25 == 0) {  // Log at 0%, 25%, 50%, 75%, 100%
                printf("CEF Windows: Download progress %d%%\n", percent);
            }
        }
    }

private:
    IMPLEMENT_REFCOUNTING(ElectrobunDownloadHandler);
};

// OSR (Off-Screen Rendering) Window for transparent CEF windows
// Renders directly to the parent layered window
class OSRWindow {
public:
    OSRWindow(HWND parent, int x, int y, int width, int height)
        : parent_(parent), pixel_buffer_(nullptr),
          buffer_width_(0), buffer_height_(0), buffer_size_(0),
          browser_(nullptr) {
    }

    ~OSRWindow() {
        if (pixel_buffer_) {
            free(pixel_buffer_);
            pixel_buffer_ = nullptr;
        }
    }

    void SetBrowser(CefRefPtr<CefBrowser> browser) {
        browser_ = browser;
    }

    void UpdateBuffer(const void* buffer, int width, int height) {
        if (!buffer || width <= 0 || height <= 0 || !parent_) {
            return;
        }

        size_t required_size = (size_t)width * (size_t)height * 4; // BGRA

        // Reallocate buffer if needed
        if (buffer_size_ < required_size) {
            if (pixel_buffer_) {
                free(pixel_buffer_);
            }
            pixel_buffer_ = (unsigned char*)malloc(required_size);
            if (!pixel_buffer_) {
                buffer_size_ = 0;
                return;
            }
            buffer_size_ = required_size;
        }

        memcpy(pixel_buffer_, buffer, required_size);
        buffer_width_ = width;
        buffer_height_ = height;

        UpdateLayeredWindow();
    }

    void UpdateLayeredWindow() {
        if (!parent_ || !pixel_buffer_ || buffer_width_ == 0 || buffer_height_ == 0) {
            return;
        }

        HDC hdc = GetDC(NULL);
        HDC memDC = CreateCompatibleDC(hdc);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = buffer_width_;
        bmi.bmiHeader.biHeight = -buffer_height_; // Top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);

        if (hBitmap && bits) {
            // Copy pixel buffer to DIB section
            memcpy(bits, pixel_buffer_, buffer_size_);

            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);

            POINT ptSrc = {0, 0};
            SIZE size = {buffer_width_, buffer_height_};
            BLENDFUNCTION blend = {};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;

            // Get the window's current position for UpdateLayeredWindow
            RECT rect;
            GetWindowRect(parent_, &rect);
            POINT ptDest = {rect.left, rect.top};

            // Update the parent window's layer with the CEF-rendered content
            ::UpdateLayeredWindow(parent_, hdc, &ptDest, &size, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

            SelectObject(memDC, oldBitmap);
            DeleteObject(hBitmap);
        }

        DeleteDC(memDC);
        ReleaseDC(NULL, hdc);
    }

    HWND GetHWND() const { return parent_; }

    // Handle mouse events and forward to CEF
    void HandleMouseEvent(UINT message, WPARAM wParam, LPARAM lParam) {
        if (!browser_) {
            printf("OSRWindow: No browser set!\n");
            return;
        }

        CefRefPtr<CefBrowserHost> host = browser_->GetHost();
        if (!host) {
            printf("OSRWindow: No browser host!\n");
            return;
        }

        CefMouseEvent mouse_event;
        mouse_event.x = GET_X_LPARAM(lParam);
        mouse_event.y = GET_Y_LPARAM(lParam);

        // Set modifiers
        mouse_event.modifiers = 0;
        if (wParam & MK_CONTROL) mouse_event.modifiers |= EVENTFLAG_CONTROL_DOWN;
        if (wParam & MK_SHIFT) mouse_event.modifiers |= EVENTFLAG_SHIFT_DOWN;
        if (GetKeyState(VK_MENU) & 0x8000) mouse_event.modifiers |= EVENTFLAG_ALT_DOWN;

        switch (message) {
            case WM_MOUSEMOVE:
                host->SendMouseMoveEvent(mouse_event, false);
                break;

            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN: {
                CefBrowserHost::MouseButtonType btn_type =
                    (message == WM_LBUTTONDOWN) ? MBT_LEFT :
                    (message == WM_RBUTTONDOWN) ? MBT_RIGHT : MBT_MIDDLE;

                printf("OSRWindow: Sending click at (%d, %d)\n", mouse_event.x, mouse_event.y);

                host->SendMouseClickEvent(mouse_event, btn_type, false, 1);
                break;
            }

            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP: {
                CefBrowserHost::MouseButtonType btn_type =
                    (message == WM_LBUTTONUP) ? MBT_LEFT :
                    (message == WM_RBUTTONUP) ? MBT_RIGHT : MBT_MIDDLE;
                host->SendMouseClickEvent(mouse_event, btn_type, true, 1);
                break;
            }

            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                host->SendMouseWheelEvent(mouse_event, 0, delta);
                break;
            }
        }
    }

    // Handle keyboard events and forward to CEF
    void HandleKeyEvent(UINT message, WPARAM wParam, LPARAM lParam) {
        if (!browser_) return;

        CefRefPtr<CefBrowserHost> host = browser_->GetHost();
        if (!host) return;

        CefKeyEvent key_event;
        key_event.windows_key_code = (int)wParam;
        key_event.native_key_code = (int)lParam;
        key_event.is_system_key = (message == WM_SYSCHAR || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP);

        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
            key_event.type = KEYEVENT_RAWKEYDOWN;
        } else if (message == WM_KEYUP || message == WM_SYSKEYUP) {
            key_event.type = KEYEVENT_KEYUP;
        } else if (message == WM_CHAR || message == WM_SYSCHAR) {
            key_event.type = KEYEVENT_CHAR;
        }

        // Set modifiers
        key_event.modifiers = 0;
        if (GetKeyState(VK_SHIFT) & 0x8000) key_event.modifiers |= EVENTFLAG_SHIFT_DOWN;
        if (GetKeyState(VK_CONTROL) & 0x8000) key_event.modifiers |= EVENTFLAG_CONTROL_DOWN;
        if (GetKeyState(VK_MENU) & 0x8000) key_event.modifiers |= EVENTFLAG_ALT_DOWN;

        host->SendKeyEvent(key_event);
    }

private:
    HWND parent_;
    unsigned char* pixel_buffer_;
    int buffer_width_;
    int buffer_height_;
    size_t buffer_size_;
    CefRefPtr<CefBrowser> browser_;
};

// CEF Render Handler for off-screen rendering (OSR) mode
class ElectrobunRenderHandler : public CefRenderHandler {
public:
    ElectrobunRenderHandler() : view_width_(800), view_height_(600), osr_window_(nullptr), window_id_(0) {}

    void SetOSRWindow(OSRWindow* window) {
        osr_window_ = window;
    }

    void SetViewSize(int width, int height) {
        view_width_ = width;
        view_height_ = height;
    }

    // Associate this render handler with a webviewId so OnAcceleratedPaint can
    // look up the corresponding SpoutWindowState entry in g_spoutWindows.
    void SetWindowId(uint32_t id) { window_id_ = id; }

    // CefRenderHandler methods
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override {
        rect.x = 0;
        rect.y = 0;
        rect.width = view_width_ > 0 ? view_width_ : 800;
        rect.height = view_height_ > 0 ? view_height_ : 600;
    }

    void OnPaint(CefRefPtr<CefBrowser> browser,
                 PaintElementType type,
                 const RectList& dirtyRects,
                 const void* buffer,
                 int width,
                 int height) override;

    // Called each frame when shared_texture_enabled = 1 in windowInfo.
    // info.shared_texture_handle is a DXGI NT shared handle to the rendered texture.
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
                            PaintElementType type,
                            const RectList& dirtyRects,
                            const CefAcceleratedPaintInfo& info) override;

private:
    int view_width_;
    int view_height_;
    OSRWindow* osr_window_;
    uint32_t window_id_; // webviewId — key into g_spoutWindows

    IMPLEMENT_REFCOUNTING(ElectrobunRenderHandler);
};

// Forward declaration
void handleApplicationMenuSelection(UINT menuId);

// CEF Keyboard Handler for menu accelerators
class ElectrobunKeyboardHandler : public CefKeyboardHandler {
public:
    // Defined out-of-line after ElectrobunCefClient (needs full class definition)
    bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                      const CefKeyEvent& event,
                      CefEventHandle os_event,
                      bool* is_keyboard_shortcut) override;

private:
    IMPLEMENT_REFCOUNTING(ElectrobunKeyboardHandler);
};

// CEF Client class with load and life span handlers
class ElectrobunCefClient : public CefClient, public CefDisplayHandler {
public:
    WebviewEventHandler webview_event_handler_ = nullptr;

    ElectrobunCefClient(uint32_t webviewId,
                       HandlePostMessage eventBridgeHandler,
                       HandlePostMessage bunBridgeHandler,
                       HandlePostMessage internalBridgeHandler,
                       bool sandbox)
        : webview_id_(webviewId),
          event_bridge_handler_(eventBridgeHandler),
          bun_bridge_handler_(bunBridgeHandler),
          webview_tag_handler_(internalBridgeHandler),
          is_sandboxed_(sandbox),
          osr_enabled_(false) {
        m_loadHandler = new ElectrobunLoadHandler();
        m_loadHandler->SetClient(this); // Set client reference for load handler
        m_lifeSpanHandler = new ElectrobunLifeSpanHandler();
        m_requestHandler = new ElectrobunRequestHandler();
        m_requestHandler->SetWebviewId(webviewId);
        m_requestHandler->SetClient(this); // Set client reference for response filter
        m_contextMenuHandler = new ElectrobunContextMenuHandler();
        m_permissionHandler = new ElectrobunPermissionHandler();
        m_dialogHandler = new ElectrobunDialogHandler();
        m_downloadHandler = new ElectrobunDownloadHandler();
        m_keyboardHandler = new ElectrobunKeyboardHandler();
        m_renderHandler = nullptr; // Created only when OSR is enabled
    }

    void EnableOSR(int width, int height) {
        osr_enabled_ = true;
        m_renderHandler = new ElectrobunRenderHandler();
        m_renderHandler->SetViewSize(width, height);
    }

    // Set the webviewId on the render handler so OnAcceleratedPaint can look up
    // the SpoutWindowState.  Call after EnableOSR() and before browser creation.
    void SetRenderHandlerWindowId(uint32_t id) {
        if (m_renderHandler) m_renderHandler->SetWindowId(id);
    }

    void SetOSRWindow(OSRWindow* window) {
        if (m_renderHandler) {
            m_renderHandler->SetOSRWindow(window);
        }
    }

    void ClearOSRWindow() {
        if (m_renderHandler) {
            m_renderHandler->SetOSRWindow(nullptr);
        }
    }

    bool IsOSREnabled() const {
        return osr_enabled_;
    }

    void SetWebviewEventHandler(WebviewEventHandler handler) {
        webview_event_handler_ = handler;
        if (m_requestHandler) {
            m_requestHandler->SetWebviewEventHandler(handler);
        }
        if (m_loadHandler) {
            m_loadHandler->SetWebviewEventHandler(handler);
            m_loadHandler->SetWebviewId(webview_id_);
        }
    }

    void SetAbstractView(AbstractView* view) {
        if (m_requestHandler) {
            m_requestHandler->SetAbstractView(view);
        }
    }

    void AddPreloadScript(const std::string& script) {
        electrobun_script_ = script;
    }

    void UpdateCustomPreloadScript(const std::string& script) {
        custom_script_ = script;
    }
    
    CefRefPtr<CefLoadHandler> GetLoadHandler() override {
        return m_loadHandler;
    }
    
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return m_lifeSpanHandler;
    }
    
    CefRefPtr<CefRequestHandler> GetRequestHandler() override {
        return m_requestHandler;
    }
    
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
        return m_contextMenuHandler;
    }
    
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override {
        return m_permissionHandler;
    }
    
    CefRefPtr<CefDialogHandler> GetDialogHandler() override {
        return m_dialogHandler;
    }

    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override {
        return m_downloadHandler;
    }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override {
        return m_renderHandler;
    }

    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override {
        return m_keyboardHandler;
    }

    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
        return this;
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefProcessId source_process,
                                 CefRefPtr<CefProcessMessage> message) override {
        std::string messageName = message->GetName().ToString();
        std::string messageContent = message->GetArgumentList()->GetString(0).ToString();
        
        char* contentCopy = strdup(messageContent.c_str());

        // eventBridge - event-only bridge (always process for all webviews, including sandboxed)
        if (messageName == "EventBridgeMessage") {
            if (event_bridge_handler_) {
                event_bridge_handler_(webview_id_, contentCopy);
            }
            return true;
        }
        // bunBridge and internalBridge - RPC bridges (only for non-sandboxed webviews)
        else if (!is_sandboxed_) {
            if (messageName == "BunBridgeMessage") {
                if (bun_bridge_handler_) {
                    bun_bridge_handler_(webview_id_, contentCopy);
                }
                return true;
            } else if (messageName == "internalMessage") {
                if (webview_tag_handler_) {
                    webview_tag_handler_(webview_id_, contentCopy);
                }
                return true;
            }
        }

        return false;
    }


    std::string GetCombinedScript() const {
        // Inject webviewId into global scope before other scripts
        std::string combined_script = "window.webviewId = " + std::to_string(webview_id_) + ";\n";
        combined_script += electrobun_script_;
        if (!custom_script_.empty()) {
            combined_script += "\n" + custom_script_;
        }
        return combined_script;
    }

    void SetBrowser(CefRefPtr<CefBrowser> browser) {
        browser_ = browser;
        // Don't execute scripts here - they should execute on each navigation
    }

    void ExecutePreloadScripts() {
        std::string script = GetCombinedScript();
        if (!script.empty() && browser_ && browser_->GetMainFrame()) {
            browser_->GetMainFrame()->ExecuteJavaScript(script, "", 0);
        }
    }

    // Track page title for DevTools target matching
    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override {
        if (browser && browser->GetMainFrame()) {
            last_title_ = title.ToString();
        }
    }

    // Open remote DevTools frontend for a specific browser (including OOPIFs)
    void OpenRemoteDevToolsFrontend(CefRefPtr<CefBrowser> browser) {
        if (!browser || !browser->GetHost()) return;

        int target_id = browser->GetIdentifier();

        // If already open, bring to front
        auto it = devtools_hosts_.find(target_id);
        if (it != devtools_hosts_.end() && it->second.is_open && it->second.window) {
            ShowWindow(it->second.window, SW_SHOW);
            SetForegroundWindow(it->second.window);
            return;
        }

        // Get the browser's URL and title for matching against /json targets
        std::string targetUrl;
        if (browser->GetMainFrame()) {
            targetUrl = browser->GetMainFrame()->GetURL().ToString();
        }
        std::string targetTitle = last_title_;
        int port = g_remoteDebugPort;

        // Keep ref to self for the background thread
        CefRefPtr<ElectrobunCefClient> self(this);

        // Fetch /json on a background thread
        std::thread([self, target_id, targetUrl, targetTitle, port]() {
            // WinHTTP synchronous GET to http://127.0.0.1:{port}/json
            HINTERNET hSession = WinHttpOpen(L"Electrobun/DevTools",
                                              WINHTTP_ACCESS_TYPE_NO_PROXY,
                                              WINHTTP_NO_PROXY_NAME,
                                              WINHTTP_NO_PROXY_BYPASS, 0);
            if (!hSession) return;

            wchar_t hostStr[64];
            swprintf_s(hostStr, L"127.0.0.1");
            HINTERNET hConnect = WinHttpConnect(hSession, hostStr, (INTERNET_PORT)port, 0);
            if (!hConnect) { WinHttpCloseHandle(hSession); return; }

            HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/json",
                                                     nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }

            BOOL bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            if (!bResults) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }

            bResults = WinHttpReceiveResponse(hRequest, nullptr);
            if (!bResults) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }

            // Read full response body
            std::string jsonBody;
            DWORD dwSize = 0;
            DWORD dwDownloaded = 0;
            do {
                dwSize = 0;
                WinHttpQueryDataAvailable(hRequest, &dwSize);
                if (dwSize == 0) break;

                std::vector<char> buf(dwSize + 1, 0);
                WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded);
                jsonBody.append(buf.data(), dwDownloaded);
            } while (dwSize > 0);

            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);

            if (jsonBody.empty()) return;

            // Simple JSON parsing for the /json array response.
            // Each target object has "url", "title", "webSocketDebuggerUrl" fields.
            // Find the target matching our browser's URL or title.

            // Parse JSON array - find objects and extract fields
            struct JsonTarget {
                std::string url;
                std::string title;
                std::string wsUrl;
            };
            std::vector<JsonTarget> targets;

            // Simple parser: split by objects in the array
            size_t pos = 0;
            while ((pos = jsonBody.find('{', pos)) != std::string::npos) {
                size_t end = jsonBody.find('}', pos);
                if (end == std::string::npos) break;

                std::string obj = jsonBody.substr(pos, end - pos + 1);
                JsonTarget t;

                // Extract "url" field
                auto extractField = [&obj](const std::string& fieldName) -> std::string {
                    std::string key = "\"" + fieldName + "\"";
                    size_t kp = obj.find(key);
                    if (kp == std::string::npos) return "";
                    size_t colon = obj.find(':', kp + key.length());
                    if (colon == std::string::npos) return "";
                    size_t qStart = obj.find('"', colon + 1);
                    if (qStart == std::string::npos) return "";
                    size_t qEnd = obj.find('"', qStart + 1);
                    if (qEnd == std::string::npos) return "";
                    return obj.substr(qStart + 1, qEnd - qStart - 1);
                };

                t.url = extractField("url");
                t.title = extractField("title");
                t.wsUrl = extractField("webSocketDebuggerUrl");
                targets.push_back(t);

                pos = end + 1;
            }

            if (targets.empty()) return;

            // Match target by URL and/or title
            const JsonTarget* selected = nullptr;
            for (const auto& t : targets) {
                bool urlMatch = !targetUrl.empty() && t.url == targetUrl;
                bool titleMatch = !targetTitle.empty() && t.title == targetTitle;

                if ((!targetUrl.empty() && !targetTitle.empty() && urlMatch && titleMatch) ||
                    (!targetUrl.empty() && urlMatch) ||
                    (!targetTitle.empty() && titleMatch)) {
                    selected = &t;
                    break;
                }
            }
            if (!selected) {
                selected = &targets[0];
            }

            if (selected->wsUrl.empty()) return;

            // Build the DevTools frontend URL
            // Strip ws:// prefix from the WebSocket URL
            std::string wsParam = selected->wsUrl;
            if (wsParam.substr(0, 5) == "ws://") {
                wsParam = wsParam.substr(5);
            }

            std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
            std::string finalUrl = baseUrl + "/devtools/inspector.html?ws=" + wsParam + "&dockSide=undocked";

            // Post back to the UI thread via CefPostTask
            class CreateDevToolsTask : public CefTask {
            public:
                CreateDevToolsTask(CefRefPtr<ElectrobunCefClient> client, int tid, const std::string& url)
                    : client_(client), target_id_(tid), url_(url) {}
                void Execute() override {
                    client_->CreateRemoteDevToolsWindow(target_id_, url_);
                }
            private:
                CefRefPtr<ElectrobunCefClient> client_;
                int target_id_;
                std::string url_;
                IMPLEMENT_REFCOUNTING(CreateDevToolsTask);
            };
            CefPostTask(TID_UI, new CreateDevToolsTask(self, target_id, finalUrl));

        }).detach();
    }

    // Create or reuse a DevTools window for a specific target
    void CreateRemoteDevToolsWindow(int target_id, const std::string& url) {
        EnsureDevToolsWindowClassRegistered();

        DevToolsHost& host = devtools_hosts_[target_id];

        if (!host.window) {
            host.dt_ctx = new DevToolsWindowContext();
            host.dt_ctx->close_callback = RemoteDevToolsClosed;
            host.dt_ctx->ctx = this;
            host.dt_ctx->target_id = target_id;

            host.window = CreateWindowExA(
                0,
                DEVTOOLS_WINDOW_CLASS,
                "DevTools",
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800,
                nullptr,  // No parent - standalone window
                nullptr,
                GetModuleHandle(NULL),
                host.dt_ctx);
        }

        ShowWindow(host.window, SW_SHOW);
        SetForegroundWindow(host.window);
        host.is_open = true;

        if (!host.client) {
            host.client = new RemoteDevToolsClient(RemoteDevToolsClosed, this, target_id);
        }

        if (host.browser) {
            // Reuse existing DevTools browser, just navigate to the new URL
            host.browser->GetMainFrame()->LoadURL(CefString(url));
            return;
        }

        // Create a new CEF browser inside the DevTools window
        RECT rect;
        GetClientRect(host.window, &rect);
        CefRect cefRect(0, 0, rect.right - rect.left, rect.bottom - rect.top);

        CefWindowInfo windowInfo;
        windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
        windowInfo.SetAsChild((CefWindowHandle)host.window, cefRect);

        CefBrowserSettings settings;
        host.browser = CefBrowserHost::CreateBrowserSync(
            windowInfo,
            host.client,
            CefString(url),
            settings,
            nullptr,
            nullptr);

        // Store the browser on the window context for WM_SIZE handling
        if (host.dt_ctx) {
            host.dt_ctx->browser = host.browser;
        }

        host.is_open = true;
    }

    void OnRemoteDevToolsClosed(int target_id) {
        auto it = devtools_hosts_.find(target_id);
        if (it == devtools_hosts_.end()) return;
        it->second.is_open = false;
        if (it->second.window) {
            ShowWindow(it->second.window, SW_HIDE);
        }
    }

    bool IsDevToolsOpen(int target_id) {
        auto it = devtools_hosts_.find(target_id);
        return it != devtools_hosts_.end() && it->second.is_open;
    }

    // Set load-end callback for deferred operations (like applying transparency after page load)
    void SetLoadEndCallback(std::function<void()> callback) {
        load_end_callback_ = callback;
    }

    // Called by load handler when page load completes
    void OnLoadEnd() {
        if (load_end_callback_) {
            load_end_callback_();
        }
    }

private:
    uint32_t webview_id_;
    HandlePostMessage event_bridge_handler_;
    HandlePostMessage bun_bridge_handler_;
    HandlePostMessage webview_tag_handler_;
    bool is_sandboxed_;
    std::string electrobun_script_;
    std::string custom_script_;
    CefRefPtr<CefBrowser> browser_;
    CefRefPtr<ElectrobunLoadHandler> m_loadHandler;
    CefRefPtr<ElectrobunLifeSpanHandler> m_lifeSpanHandler;
    CefRefPtr<ElectrobunRequestHandler> m_requestHandler;
    CefRefPtr<ElectrobunContextMenuHandler> m_contextMenuHandler;
    CefRefPtr<ElectrobunPermissionHandler> m_permissionHandler;
    CefRefPtr<ElectrobunDialogHandler> m_dialogHandler;
    CefRefPtr<ElectrobunDownloadHandler> m_downloadHandler;
    CefRefPtr<ElectrobunKeyboardHandler> m_keyboardHandler;
    CefRefPtr<ElectrobunRenderHandler> m_renderHandler;
    bool osr_enabled_;
    std::function<void()> load_end_callback_;  // Callback for page load completion

    // Remote DevTools state - tracked per CefBrowser (by identifier)
    struct DevToolsHost {
        HWND window = nullptr;
        CefRefPtr<CefBrowser> browser;
        CefRefPtr<RemoteDevToolsClient> client;
        DevToolsWindowContext* dt_ctx = nullptr;
        bool is_open = false;
    };
    std::map<int, DevToolsHost> devtools_hosts_;
    std::string last_title_;

    IMPLEMENT_REFCOUNTING(ElectrobunCefClient);
};

// Free function callback for RemoteDevToolsClient -> ElectrobunCefClient
void RemoteDevToolsClosed(void* ctx, int target_id) {
    if (!ctx) return;
    static_cast<ElectrobunCefClient*>(ctx)->OnRemoteDevToolsClosed(target_id);
}

// Out-of-line definitions for handlers that need ElectrobunCefClient to be fully defined

bool ElectrobunContextMenuHandler::OnContextMenuCommand(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefContextMenuParams> params,
    int command_id,
    EventFlags event_flags) {
    if (command_id == 26501) {
        // Open remote DevTools via the owning ElectrobunCefClient
        CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
        ElectrobunCefClient* ebClient = static_cast<ElectrobunCefClient*>(client.get());
        if (ebClient) {
            ebClient->OpenRemoteDevToolsFrontend(browser);
        }
        return true;
    }
    return false;
}

bool ElectrobunKeyboardHandler::OnPreKeyEvent(
    CefRefPtr<CefBrowser> browser,
    const CefKeyEvent& event,
    CefEventHandle os_event,
    bool* is_keyboard_shortcut) {
    // Only handle key down events
    if (event.type != KEYEVENT_RAWKEYDOWN) {
        return false;
    }

    // F12 or Ctrl+Shift+I -> open DevTools
    bool isF12 = (event.windows_key_code == 123);
    bool isCtrlShiftI = (event.windows_key_code == 'I' &&
                         (event.modifiers & EVENTFLAG_CONTROL_DOWN) &&
                         (event.modifiers & EVENTFLAG_SHIFT_DOWN));
    if (isF12 || isCtrlShiftI) {
        CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
        ElectrobunCefClient* ebClient = static_cast<ElectrobunCefClient*>(client.get());
        if (ebClient) {
            ebClient->OpenRemoteDevToolsFrontend(browser);
        }
        return true;
    }

    // Check if we have accelerator entries
    if (g_menuAccelerators.empty()) {
        return false;
    }

    // Build the current modifier state from CEF event
    BYTE modifiers = FVIRTKEY;
    if (event.modifiers & EVENTFLAG_CONTROL_DOWN) modifiers |= FCONTROL;
    if (event.modifiers & EVENTFLAG_ALT_DOWN) modifiers |= FALT;
    if (event.modifiers & EVENTFLAG_SHIFT_DOWN) modifiers |= FSHIFT;

    // Check if this key combination matches any accelerator
    WORD vkCode = (WORD)event.windows_key_code;

    for (const auto& accel : g_menuAccelerators) {
        if (accel.key == vkCode && accel.fVirt == modifiers) {
            // Found a match! Trigger the menu command directly
            handleApplicationMenuSelection(accel.cmd);
            return true;  // Prevent CEF from processing this key
        }
    }

    return false;
}

// ElectrobunRenderHandler::OnPaint implementation
// Used for transparent OSR windows only. Spout capture uses PrintWindow in a
// background thread — not this callback.
void ElectrobunRenderHandler::OnPaint(CefRefPtr<CefBrowser> browser,
                                       PaintElementType type,
                                       const RectList& dirtyRects,
                                       const void* buffer,
                                       int width,
                                       int height) {
    if (osr_window_ && buffer && width > 0 && height > 0) {
        osr_window_->UpdateBuffer(buffer, width, height);
    }
}

// ElectrobunRenderHandler::OnAcceleratedPaint implementation
// Called by CEF each rendered frame when shared_texture_enabled=1 (OSR mode).
// info.shared_texture_handle is a DXGI NT shared handle to the GPU texture.
// Two independent consumers:
//   1. D3D output mode — CopySubresourceRegion sub-regions to NativeDisplayWindow swap chains
//   2. Spout mode      — SpoutDX::SendTexture to external Spout receivers
void ElectrobunRenderHandler::OnAcceleratedPaint(
    CefRefPtr<CefBrowser> browser,
    PaintElementType type,
    const RectList& dirtyRects,
    const CefAcceleratedPaintInfo& info)
{
    if (type != PET_VIEW) return;
    if (window_id_ == 0) return;

    // Log first call to confirm OnAcceleratedPaint is firing.
    static bool s_firstPaint = true;
    if (s_firstPaint) {
        s_firstPaint = false;
        ::log("OSR: OnAcceleratedPaint fired (first frame). handle=" +
              std::to_string(reinterpret_cast<uintptr_t>(info.shared_texture_handle)));
    }

    if (!info.shared_texture_handle) {
        static bool s_warnedNull = false;
        if (!s_warnedNull) { s_warnedNull = true; ::log("OSR: shared_texture_handle is null — use-angle may not support shared textures"); }
        return;
    }

#if ELECTROBUN_HAS_SPOUT
    auto it = g_spoutWindows.find(window_id_);
    if (it == g_spoutWindows.end()) return;
    auto& state = it->second;
    if (!state.active || !state.d3dDevice || !state.d3dContext) return;

    // Open the DXGI NT shared handle that CEF's GPU compositor wrote this frame to.
    // OpenSharedResource1 (D3D 11.1) accepts NT handles; works across device instances
    // on the same adapter (and cross-adapter on WDDM 2.0+).
    ID3D11Device1* device1 = nullptr;
    state.d3dDevice->QueryInterface(__uuidof(ID3D11Device1), (void**)&device1);
    if (!device1) { ::log("Spout OSR: ID3D11Device1 QI failed"); return; }

    ID3D11Texture2D* sharedTex = nullptr;
    HRESULT hr = device1->OpenSharedResource1(
        reinterpret_cast<HANDLE>(info.shared_texture_handle),
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&sharedTex));
    device1->Release();

    if (FAILED(hr) || !sharedTex) {
        static bool s_warnedOpen = false;
        if (!s_warnedOpen) {
            s_warnedOpen = true;
            char buf[32]; sprintf_s(buf, "%08X", (unsigned)hr);
            ::log("Spout OSR: OpenSharedResource1 failed hr=0x" + std::string(buf) +
                  " — may be cross-adapter (Vulkan NVIDIA → D3D11 Intel)");
        }
        return;
    }

    // 1. Blit to swap chain so DWM can thumbnail the master HWND for NDW display windows.
    if (state.swapChain) {
        ID3D11Texture2D* backBuf = nullptr;
        if (SUCCEEDED(state.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf))) {
            state.d3dContext->CopyResource(backBuf, sharedTex);
            backBuf->Release();
        }
        state.swapChain->Present(0, 0);
    }

    // 2. Send to Spout receivers (TouchDesigner, Resolume, etc.)
    if (state.sender) {
        state.sender->SendTexture(sharedTex);
    }

    // 3. D3D output mode — blit sub-regions to NativeDisplayWindow swap chains.
    // Uses the same device and already-opened sharedTex from step 1 above.
    // state.sender is null in D3D output mode; this block is a no-op in pure Spout mode.
    {
        auto itD3D = g_d3dOutputStates.find(window_id_);
        if (itD3D != g_d3dOutputStates.end()) {
            // Query source texture dimensions once — used to clamp all slot boxes.
            D3D11_TEXTURE2D_DESC srcDesc = {};
            sharedTex->GetDesc(&srcDesc);
            const UINT texW = srcDesc.Width;
            const UINT texH = srcDesc.Height;

            static bool s_loggedFirstBlit = false;
            if (!s_loggedFirstBlit) {
                s_loggedFirstBlit = true;
                // Log source texture format (DXGI_FORMAT enum value).
                // BGRA=87, RGBA=28, BGRA_SRGB=91, RGBA_SRGB=29
                // Swap chain is always BGRA=87; mismatch causes silent black output.
                ID3D11Texture2D* firstBack = nullptr;
                DXGI_FORMAT backFmt = DXGI_FORMAT_UNKNOWN;
                if (!itD3D->second.slots.empty() && itD3D->second.slots[0].swapChain) {
                    if (SUCCEEDED(itD3D->second.slots[0].swapChain->GetBuffer(
                            0, __uuidof(ID3D11Texture2D), (void**)&firstBack))) {
                        D3D11_TEXTURE2D_DESC backDesc = {};
                        firstBack->GetDesc(&backDesc);
                        backFmt = backDesc.Format;
                        firstBack->Release();
                    }
                }
                ::log("D3DOutput: first frame — source texture " +
                      std::to_string(texW) + "x" + std::to_string(texH) +
                      " fmt=" + std::to_string(srcDesc.Format) +
                      ", backbuf fmt=" + std::to_string(backFmt) +
                      ", " + std::to_string(itD3D->second.slots.size()) + " slot(s)");

                // Pixel readback: copy 1×1 pixel from center of sharedTex to a staging
                // texture and map it to confirm CEF is rendering actual content.
                // If all samples are 0x00000000 the source texture is genuinely black.
                {
                    D3D11_TEXTURE2D_DESC stagDesc = {};
                    stagDesc.Width  = 1;
                    stagDesc.Height = 1;
                    stagDesc.MipLevels = 1;
                    stagDesc.ArraySize = 1;
                    stagDesc.Format = srcDesc.Format;
                    stagDesc.SampleDesc.Count = 1;
                    stagDesc.Usage = D3D11_USAGE_STAGING;
                    stagDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    ID3D11Texture2D* stagTex = nullptr;
                    HRESULT hrStag = state.d3dDevice->CreateTexture2D(&stagDesc, nullptr, &stagTex);
                    if (SUCCEEDED(hrStag) && stagTex) {
                        // Sample centre pixel
                        UINT cx = texW / 2, cy = texH / 2;
                        D3D11_BOX sBox = { cx, cy, 0, cx + 1, cy + 1, 1 };
                        state.d3dContext->CopySubresourceRegion(stagTex, 0, 0, 0, 0, sharedTex, 0, &sBox);
                        D3D11_MAPPED_SUBRESOURCE mapped = {};
                        if (SUCCEEDED(state.d3dContext->Map(stagTex, 0, D3D11_MAP_READ, 0, &mapped))) {
                            uint32_t px = *reinterpret_cast<uint32_t*>(mapped.pData);
                            state.d3dContext->Unmap(stagTex, 0);
                            char buf[32];
                            sprintf_s(buf, "%08X", px);
                            ::log("D3DOutput: centre pixel (BGRA) = 0x" + std::string(buf) +
                                  " (0=black/empty, non-zero=has content)");
                        } else {
                            ::log("D3DOutput: staging Map failed — cross-adapter or device mismatch?");
                        }
                        stagTex->Release();
                    } else {
                        char buf[16]; sprintf_s(buf, "%08X", (UINT)hrStag);
                        ::log("D3DOutput: staging texture create failed hr=0x" + std::string(buf));
                    }
                }
            }

            static bool s_loggedSlots = false;
            if (!s_loggedSlots) {
                s_loggedSlots = true;
                for (auto& dbgSlot : itD3D->second.slots) {
                    ::log("D3DOutput: slot displayWindowId=" + std::to_string(dbgSlot.displayWindowId) +
                          " src=(" + std::to_string(dbgSlot.sourceX) + "," + std::to_string(dbgSlot.sourceY) +
                          " " + std::to_string(dbgSlot.sourceW) + "x" + std::to_string(dbgSlot.sourceH) +
                          ") swapChain=" + (dbgSlot.swapChain ? "ok" : "null"));
                }
            }

            for (auto& slot : itD3D->second.slots) {
                if (!slot.swapChain) continue;

                // Clamp source box to actual texture bounds to prevent GPU faults.
                UINT x0 = (UINT)std::max(0, slot.sourceX);
                UINT y0 = (UINT)std::max(0, slot.sourceY);
                UINT x1 = std::min((UINT)(slot.sourceX + slot.sourceW), texW);
                UINT y1 = std::min((UINT)(slot.sourceY + slot.sourceH), texH);
                if (x1 <= x0 || y1 <= y0) {
                    static bool s_warnedOOB = false;
                    if (!s_warnedOOB) {
                        s_warnedOOB = true;
                        ::log("D3DOutput: slot sourceRect (" +
                              std::to_string(slot.sourceX) + "," + std::to_string(slot.sourceY) +
                              " " + std::to_string(slot.sourceW) + "x" + std::to_string(slot.sourceH) +
                              ") is entirely outside " + std::to_string(texW) + "x" + std::to_string(texH) +
                              " texture — skipping (check display-config.json source rects)");
                    }
                    slot.swapChain->Present(1, 0); // present black rather than stall
                    continue;
                }

                ID3D11Texture2D* backBuf = nullptr;
                if (SUCCEEDED(slot.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf))) {
                    D3D11_BOX box = { x0, y0, 0, x1, y1, 1 };
                    state.d3dContext->CopySubresourceRegion(backBuf, 0, 0, 0, 0, sharedTex, 0, &box);
                    backBuf->Release();
                }
                slot.swapChain->Present(1, 0);
            }
        }
    }

    sharedTex->Release();
#endif
}

// Helper function implementation (defined after ElectrobunCefClient class)
void SetBrowserOnClient(CefRefPtr<ElectrobunCefClient> client, CefRefPtr<CefBrowser> browser) {
    if (client && browser) {
        client->SetBrowser(browser);
        // Store preload scripts for this browser ID so load handler can access them
        std::string script = client->GetCombinedScript();
        if (!script.empty()) {
            g_preloadScripts[browser->GetIdentifier()] = script;
        }
    }
}

// ElectrobunLoadHandler method implementations (defined after ElectrobunCefClient class)
void ElectrobunLoadHandler::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type) {
    // NOTE: OnLoadStart is now a fallback - primary injection happens via GetResourceResponseFilter
    // This ensures preload scripts are in the HTML before parsing, guaranteeing execution order
}

void ElectrobunLoadHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) {
    // Fire did-navigate event
    if (frame->IsMain() && webview_event_handler_) {
        std::string url = frame->GetURL().ToString();
        webview_event_handler_(webview_id_, _strdup("did-navigate"), _strdup(url.c_str()));
    }

    // Call load end callback for deferred operations (like transparency)
    if (frame->IsMain() && client_) {
        client_->OnLoadEnd();
    }
}

// ElectrobunResourceRequestHandler method implementations (defined after ElectrobunCefClient class)
CefRefPtr<CefResponseFilter> ElectrobunResourceRequestHandler::GetResourceResponseFilter(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefResponse> response) {

    std::string url = request->GetURL().ToString();
    std::string mimeType = response->GetMimeType().ToString();
    bool isMain = frame->IsMain();
    bool hasClient = client_ != nullptr;

    // Only filter main frame HTML responses; skip logging for all other requests
    // (avoids per-frame stdout I/O for spout frame-server fetches at 60fps)
    if (isMain && hasClient && mimeType.find("html") != std::string::npos) {
        std::string combinedScript = client_->GetCombinedScript();
        std::cout << "[CEF] HTML response detected, scriptLength=" << combinedScript.length() << std::endl;

        if (!combinedScript.empty()) {
            std::cout << "[CEF] Installing response filter to inject preload scripts into HTML" << std::endl;
            return new ElectrobunResponseFilter(combinedScript);
        }
    }

    return nullptr;
}

// Runtime CEF availability detection - Windows equivalent of macOS isCEFAvailable()
bool isCEFAvailable() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) {
        *lastSlash = '\0';
    }
    
    // Check for essential CEF files
    std::string cefLibPath = std::string(exePath) + "\\libcef.dll";
    std::string icuDataPath = std::string(exePath) + "\\icudtl.dat";
    
    DWORD libAttributes = GetFileAttributesA(cefLibPath.c_str());
    DWORD icuAttributes = GetFileAttributesA(icuDataPath.c_str());
    
    bool libExists = (libAttributes != INVALID_FILE_ATTRIBUTES && !(libAttributes & FILE_ATTRIBUTE_DIRECTORY));
    bool icuExists = (icuAttributes != INVALID_FILE_ATTRIBUTES && !(icuAttributes & FILE_ATTRIBUTE_DIRECTORY));
    
    return libExists && icuExists;
}

class StatusItemTarget {
public:
    ZigStatusItemHandler zigHandler;
    uint32_t trayId;
    
    StatusItemTarget() : zigHandler(nullptr), trayId(0) {}
};



// Forward declare helper functions
std::string getMimeTypeForFile(const std::string& path);

// Load app.ico from the same directory as the running exe (cached after first call).
static HICON getAppIcon() {
    static HICON icon = nullptr;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string icoPath(exePath);
        auto slash = icoPath.find_last_of("\\/");
        if (slash != std::string::npos) icoPath = icoPath.substr(0, slash + 1);
        icoPath += "app.ico";
        icon = (HICON)LoadImageA(NULL, icoPath.c_str(),
            IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (!icon)
            ::log("Icon not found: " + icoPath + " — using default");
    }
    return icon;
}

// Apply the app icon to a window (taskbar + title bar).
static void applyAppIcon(HWND hwnd) {
    HICON icon = getAppIcon();
    if (icon) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    }
}

void log(const std::string& message) {
    // Get current time
    std::time_t now = std::time(0);
    std::string timeStr = std::ctime(&now);
    timeStr.pop_back(); // Remove newline character
    
    // Print to console
    std::cout << "[" << timeStr << "] " << message << std::endl;
    
    // Optionally write to file
    std::ofstream logFile("app.log", std::ios::app);
    if (logFile.is_open()) {
        logFile << "[" << timeStr << "] " << message << std::endl;
        logFile.close();
    }
}





class MainThreadDispatcher {
private:
    static HWND g_messageWindow;

public:
    static void initialize(HWND hwnd) {
        g_messageWindow = hwnd;
    }
    
    template<typename Func>
    static auto dispatch_sync(Func&& func) -> decltype(func()) {
        using ReturnType = decltype(func());
        
        if constexpr (std::is_void_v<ReturnType>) {
            auto promise = std::make_shared<std::promise<void>>();
            auto future = promise->get_future();
            
            auto task = new std::function<void()>([func = std::forward<Func>(func), promise]() {
                try {
                    func();
                    promise->set_value();
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
            
            PostMessage(g_messageWindow, WM_EXECUTE_SYNC_BLOCK, 0, (LPARAM)task);
            future.get(); // Will re-throw any exceptions
        } else {
            auto promise = std::make_shared<std::promise<ReturnType>>();
            auto future = promise->get_future();
            
            auto task = new std::function<void()>([func = std::forward<Func>(func), promise]() {
                try {
                    promise->set_value(func());
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
            
            PostMessage(g_messageWindow, WM_EXECUTE_SYNC_BLOCK, 0, (LPARAM)task);
            return future.get();
        }
    }
    
    static void handleSyncTask(LPARAM lParam) {
        auto task = (std::function<void()>*)lParam;
        (*task)();
        delete task;
    }
    
    template<typename Func>
    static void dispatch_async(Func&& func) {
        auto task = new std::function<void()>(std::forward<Func>(func));
        PostMessage(g_messageWindow, WM_EXECUTE_ASYNC_BLOCK, 0, (LPARAM)task);
    }
};

HWND MainThreadDispatcher::g_messageWindow = NULL;

// AbstractView base class - Windows implementation matching Mac pattern
class AbstractView {
public:
    uint32_t webviewId;
    HWND hwnd = NULL;
    bool isMousePassthroughEnabled = false;
    bool mirrorModeEnabled = false;
    bool fullSize = false;
    bool pendingStartTransparent = false;
    bool pendingStartPassthrough = false;

    // Common state
    bool isReceivingInput = true;
    std::string maskJSON;
    RECT visualBounds = {};
    bool creationFailed = false;

    // Pending resize state (cross-thread)
    std::mutex pendingResizeMutex;
    std::atomic<uint64_t> pendingResizeGeneration{0};
    uint64_t appliedResizeGeneration = 0;
    bool hasPendingResize = false;
    RECT pendingResizeFrame = {};
    std::string pendingResizeMasks;

    // Navigation rules for URL filtering
    std::vector<std::string> navigationRules;

    virtual ~AbstractView() = default;
    
    // Pure virtual methods - must be implemented by subclasses
    virtual void loadURL(const char* urlString) = 0;
    virtual void loadHTML(const char* htmlString) = 0;
    virtual void goBack() = 0;
    virtual void goForward() = 0;
    virtual void reload() = 0;
    virtual void remove() = 0;
    virtual bool canGoBack() = 0;
    virtual bool canGoForward() = 0;
    virtual void evaluateJavaScriptWithNoCompletion(const char* jsString) = 0;
    virtual void callAsyncJavascript(const char* messageId, const char* jsString, uint32_t webviewId, uint32_t hostWebviewId, void* completionHandler) = 0;
    virtual void addPreloadScriptToWebView(const char* jsString) = 0;
    virtual void updateCustomPreloadScript(const char* jsString) = 0;
    virtual void resize(const RECT& frame, const char* masksJson) = 0;
    
    // Common implementations
    virtual void setTransparent(bool transparent) {
        // Default implementation - can be overridden
    }
    
    virtual void setPassthrough(bool enable) {
        isMousePassthroughEnabled = enable;
    }
    
    virtual void setHidden(bool hidden) {
        if (hwnd) {
            ShowWindow(hwnd, hidden ? SW_HIDE : SW_SHOW);
        }
    }

    // Set navigation rules from JSON array string
    void setNavigationRulesFromJSON(const char* rulesJson) {
        navigationRules.clear();
        if (!rulesJson || strlen(rulesJson) == 0) {
            return;
        }

        // Simple JSON array parser for string arrays: ["rule1", "rule2", ...]
        std::string json(rulesJson);
        size_t pos = json.find('[');
        if (pos == std::string::npos) return;

        pos++;
        while (pos < json.length()) {
            // Find start of string
            size_t strStart = json.find('"', pos);
            if (strStart == std::string::npos) break;

            // Find end of string (handle escaped quotes)
            size_t strEnd = strStart + 1;
            while (strEnd < json.length()) {
                if (json[strEnd] == '"' && json[strEnd - 1] != '\\') break;
                strEnd++;
            }
            if (strEnd >= json.length()) break;

            // Extract string value
            std::string rule = json.substr(strStart + 1, strEnd - strStart - 1);
            navigationRules.push_back(rule);

            pos = strEnd + 1;
        }
    }

    // Check if URL should be allowed based on navigation rules
    bool shouldAllowNavigationToURL(const std::string& url) {
        if (navigationRules.empty()) {
            return true; // Default allow if no rules
        }

        bool allowed = true; // Default allow if no rules match

        for (const std::string& rule : navigationRules) {
            bool isBlockRule = !rule.empty() && rule[0] == '^';
            std::string pattern = isBlockRule ? rule.substr(1) : rule;

            if (electrobun::globMatch(pattern, url)) {
                allowed = !isBlockRule; // Last match wins
            }
        }

        return allowed;
    }

    virtual void setCreationFailed(bool failed) {
        creationFailed = failed;
    }
    
    virtual bool hasCreationFailed() const {
        return creationFailed;
    }
    
    // Check if point is in a masked (cut-out) area based on maskJSON
    bool isPointInMask(POINT localPoint) {
        if (maskJSON.empty()) {
            return false;
        }
        
        // Simple JSON parsing for mask rectangles
        // Expected format: [{"x":10,"y":20,"width":100,"height":50},...]
        size_t pos = 0;
        while ((pos = maskJSON.find("\"x\":", pos)) != std::string::npos) {
            try {
                // Extract x, y, width, height from JSON
                size_t xStart = maskJSON.find(":", pos) + 1;
                size_t xEnd = maskJSON.find(",", xStart);
                int x = std::stoi(maskJSON.substr(xStart, xEnd - xStart));
                
                size_t yPos = maskJSON.find("\"y\":", pos);
                size_t yStart = maskJSON.find(":", yPos) + 1;
                size_t yEnd = maskJSON.find(",", yStart);
                int y = std::stoi(maskJSON.substr(yStart, yEnd - yStart));
                
                size_t wPos = maskJSON.find("\"width\":", pos);
                size_t wStart = maskJSON.find(":", wPos) + 1;
                size_t wEnd = maskJSON.find(",", wStart);
                if (wEnd == std::string::npos) wEnd = maskJSON.find("}", wStart);
                int width = std::stoi(maskJSON.substr(wStart, wEnd - wStart));
                
                size_t hPos = maskJSON.find("\"height\":", pos);
                size_t hStart = maskJSON.find(":", hPos) + 1;
                size_t hEnd = maskJSON.find("}", hStart);
                int height = std::stoi(maskJSON.substr(hStart, hEnd - hStart));
                
                // Check if point is within this mask rectangle
                if (localPoint.x >= x && localPoint.x < x + width &&
                    localPoint.y >= y && localPoint.y < y + height) {
                    return true;  // Point is in a masked area
                }
                
                pos = hEnd;
            } catch (...) {
                // JSON parsing error, skip this mask
                pos++;
            }
        }
        
        return false;  // Point is not in any masked area
    }
    
    // Virtual methods for subclass-specific functionality
    virtual void applyVisualMask() = 0;
    virtual void removeMasks() = 0;
    virtual void toggleMirrorMode(bool enable) = 0;

    // Find in page methods
    virtual void findInPage(const char* searchText, bool forward, bool matchCase) = 0;
    virtual void stopFindInPage() = 0;

    // Developer tools methods
    virtual void openDevTools() = 0;
    virtual void closeDevTools() = 0;
    virtual void toggleDevTools() = 0;

    void storePendingResize(const RECT& frame, const char* masksJson) {
        std::lock_guard<std::mutex> lock(pendingResizeMutex);
        pendingResizeFrame = frame;
        pendingResizeMasks = masksJson ? masksJson : "";
        hasPendingResize = true;
        pendingResizeGeneration++;
    }

    bool consumePendingResize(RECT& outFrame, std::string& outMasks) {
        std::lock_guard<std::mutex> lock(pendingResizeMutex);
        if (!hasPendingResize) return false;
        uint64_t gen = pendingResizeGeneration.load();
        if (gen == appliedResizeGeneration) return false;
        outFrame = pendingResizeFrame;
        outMasks = pendingResizeMasks;
        appliedResizeGeneration = gen;
        hasPendingResize = false;
        return true;
    }
};

// Pending resize queue (cross-thread)
static PendingResizeQueue g_pendingResizeQueue;
static std::atomic<bool> g_pendingResizeScheduled{false};

static void drainPendingResizes() {
    g_pendingResizeScheduled.store(false);
    auto items = g_pendingResizeQueue.drain();
    for (void* item : items) {
        AbstractView* view = static_cast<AbstractView*>(item);
        if (!view) continue;
        RECT frame = {};
        std::string masks;
        if (view->consumePendingResize(frame, masks)) {
            view->resize(frame, masks.c_str());
        }
    }
}

static void schedulePendingResizeDrain() {
    if (g_pendingResizeScheduled.exchange(true)) return;
    MainThreadDispatcher::dispatch_async([]() {
        drainPendingResizes();
    });
}

// Helper function to check navigation rules
// This is defined here (after AbstractView) so it can call methods on AbstractView
bool checkNavigationRules(AbstractView* view, const std::string& url) {
    if (!view) {
        return true; // Allow navigation if no view
    }
    return view->shouldAllowNavigationToURL(url);
}


// CEFView class - implements AbstractView for CEF
class CEFView : public AbstractView {
private:
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<ElectrobunCefClient> client;
    OSRWindow* osr_window;
    bool is_osr_mode;

public:
    CEFView(uint32_t webviewId) : osr_window(nullptr), is_osr_mode(false) {
        this->webviewId = webviewId;
    }

    ~CEFView() {
        // If remove() wasn't called (e.g. window destroyed directly via WM_DESTROY
        // without explicit webview removal), clean up the browser properly.
        if (browser) {
            // Invalidate render handler's OSR pointer before we delete it
            if (client) {
                client->ClearOSRWindow();
            }

            CefRefPtr<CefBrowserHost> host = browser->GetHost();
            browser = nullptr;
            client = nullptr;

            if (host) {
                host->CloseBrowser(true);
            }
        } else if (client) {
            // remove() was called (browser is null) but client might still be set
            // in older code paths - clear the OSR pointer just in case
            client->ClearOSRWindow();
            client = nullptr;
        }

        // Clean up global maps that hold raw pointers to this object
        for (auto it = g_cefViews.begin(); it != g_cefViews.end(); ++it) {
            if (it->second == this) {
                g_cefViews.erase(it);
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_abstractViewsMutex);
            g_abstractViews.erase(this->webviewId);
        }

        if (osr_window) {
            delete osr_window;
            osr_window = nullptr;
        }
    }

    void setOSRWindow(OSRWindow* window) {
        osr_window = window;
        is_osr_mode = true;
    }

    bool isOSRMode() const {
        return is_osr_mode;
    }
    
    void loadURL(const char* urlString) override {
        if (browser) {
            browser->GetMainFrame()->LoadURL(urlString);
        }
    }
    
    void loadHTML(const char* htmlString) override {
        if (browser && htmlString) {
            // Create a data URI for the HTML content
            std::string dataUri = "data:text/html;charset=utf-8,";
            dataUri += htmlString;
            browser->GetMainFrame()->LoadURL(CefString(dataUri));
        }
    }
    
    void goBack() override {
        if (browser) {
            browser->GoBack();
        }
    }
    
    void goForward() override {
        if (browser) {
            browser->GoForward();
        }
    }
    
    void reload() override {
        if (browser) {
            browser->Reload();
        }
    }
    
    void remove() override {
        if (browser) {
            std::cout << "[CEF] CEFView::remove() called for browser ID " << browser->GetIdentifier() << std::endl;

            // Get the browser host before we clear the reference
            CefRefPtr<CefBrowserHost> host = browser->GetHost();

            // First, hide the browser window to make removal appear instant
            HWND browserHwnd = host->GetWindowHandle();
            if (browserHwnd) {
                ShowWindow(browserHwnd, SW_HIDE);
            }

            // Invalidate the render handler's OSR window pointer BEFORE async close.
            // CEF may still fire OnPaint() callbacks during the close sequence, and
            // the OSRWindow will be deleted when this CEFView is destroyed.
            if (client) {
                client->ClearOSRWindow();
            }

            // Clean up global maps to prevent stale pointer access from window messages
            for (auto it = g_cefViews.begin(); it != g_cefViews.end(); ++it) {
                if (it->second == this) {
                    g_cefViews.erase(it);
                    break;
                }
            }
            for (auto it = g_cefClients.begin(); it != g_cefClients.end();) {
                if (it->second == client) {
                    it = g_cefClients.erase(it);
                } else {
                    ++it;
                }
            }

            // Clear our references
            browser = nullptr;
            client = nullptr;

            // Defer the actual browser close to avoid synchronous window message issues
            // Use CloseBrowser(true) to force close since we return true from DoClose
            // to prevent CEF from sending WM_CLOSE to parent window
            MainThreadDispatcher::dispatch_async([host]() {
                std::cout << "[CEF] Calling CloseBrowser(true) from dispatch_async" << std::endl;
                host->CloseBrowser(true);  // force=true since DoClose returns true
            });
        }
    }
    
    bool canGoBack() override {
        if (browser) {
            return browser->CanGoBack();
        }
        return false;
    }
    
    bool canGoForward() override {
        if (browser) {
            return browser->CanGoForward();
        }
        return false;
    }
    
    void evaluateJavaScriptWithNoCompletion(const char* jsString) override {
        if (browser) {
            // Copy string to avoid lifetime issues in lambda
            std::string jsStringCopy = jsString;
            MainThreadDispatcher::dispatch_sync([this, jsStringCopy]() {
                CefRefPtr<CefFrame> frame = browser->GetMainFrame();
                if (frame) frame->ExecuteJavaScript(jsStringCopy.c_str(), "", 0);
            });
        }
    }

    void callAsyncJavascript(const char* messageId, const char* jsString, uint32_t webviewId, uint32_t hostWebviewId, void* completionHandler) override {
        if (browser) {
            CefRefPtr<CefFrame> frame = browser->GetMainFrame();
            if (frame) frame->ExecuteJavaScript(jsString, "", 0);
        }
    }

    void addPreloadScriptToWebView(const char* jsString) override {
        if (!jsString) return;
        if (browser) {
            CefRefPtr<CefFrame> frame = browser->GetMainFrame();
            if (frame) frame->ExecuteJavaScript(jsString, frame->GetURL(), 0);
        }
    }

    void updateCustomPreloadScript(const char* jsString) override {
        if (!jsString) return;
        if (browser) {
            CefRefPtr<CefFrame> frame = browser->GetMainFrame();
            if (frame) frame->ExecuteJavaScript(jsString, frame->GetURL(), 0);
        }
    }
    
    // CEF-specific methods
    void setBrowser(CefRefPtr<CefBrowser> br) {
        browser = br;
        // If OSR mode, also set the browser on the OSR window for event handling
        if (osr_window && br) {
            osr_window->SetBrowser(br);
        }
    }
    
    void setClient(CefRefPtr<ElectrobunCefClient> cl) {
        client = cl;
    }
    
    CefRefPtr<CefBrowser> getBrowser() {
        return browser;
    }
    
    CefRefPtr<ElectrobunCefClient> getClient() {
        return client;
    }
    
    void resize(const RECT& frame, const char* masksJson) override {
        if (browser) {
            // Get the CEF browser's window handle and update its position/size
            HWND browserHwnd = browser->GetHost()->GetWindowHandle();
            if (browserHwnd) {
                int width = frame.right - frame.left;
                int height = frame.bottom - frame.top;
                
                
                // Move and resize the CEF browser window, bringing it to front
                SetWindowPos(browserHwnd, HWND_TOP, frame.left, frame.top, width, height,
                           SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            
            // Notify CEF that the browser was resized
            browser->GetHost()->WasResized();
            visualBounds = frame;

            bool maskChanged = false;
            // Check if masksJson is nullptr, empty, or just "[]" (empty array)
            if (masksJson && strlen(masksJson) > 0 && strcmp(masksJson, "[]") != 0) {
                std::string newMaskJSON = masksJson;
                if (newMaskJSON != maskJSON) {
                    maskJSON = newMaskJSON;
                    maskChanged = true;
                }
            } else if (!maskJSON.empty()) {
                maskJSON = "";
                maskChanged = true;
            }

            // Only apply visual mask if mask data changed
            if (maskChanged) {
                applyVisualMask();
            }
        }
    }

    // CEF-specific implementation of mask functionality
    void applyVisualMask() override {
        if (!browser) {
            return;
        }
        
        HWND browserHwnd = browser->GetHost()->GetWindowHandle();
        if (!browserHwnd) {
            return;
        }
        
        if (maskJSON.empty()) {
            // Remove any existing mask by setting full window region
            RECT windowRect;
            GetClientRect(browserHwnd, &windowRect);
            HRGN fullRegion = CreateRectRgn(0, 0, windowRect.right, windowRect.bottom);
            SetWindowRgn(browserHwnd, fullRegion, TRUE);
            return;
        }
        
        try {
            // Get the CEF browser window bounds
            RECT bounds = visualBounds;
            int width = bounds.right - bounds.left;
            int height = bounds.bottom - bounds.top;
            
            if (width <= 0 || height <= 0) {
                return;
            }
            
            // Create base region covering entire browser window
            HRGN browserRegion = CreateRectRgn(0, 0, width, height);
            
            // Parse maskJSON and subtract mask regions (holes)
            size_t pos = 0;
            int maskCount = 0;
            while ((pos = maskJSON.find("\"x\":", pos)) != std::string::npos) {
                try {
                    // Extract mask rectangle coordinates  
                    size_t xStart = maskJSON.find(":", pos) + 1;
                    size_t xEnd = maskJSON.find(",", xStart);
                    int x = std::stoi(maskJSON.substr(xStart, xEnd - xStart));
                    
                    size_t yPos = maskJSON.find("\"y\":", pos);
                    size_t yStart = maskJSON.find(":", yPos) + 1;
                    size_t yEnd = maskJSON.find(",", yStart);
                    int y = std::stoi(maskJSON.substr(yStart, yEnd - yStart));
                    
                    size_t wPos = maskJSON.find("\"width\":", pos);
                    size_t wStart = maskJSON.find(":", wPos) + 1;
                    size_t wEnd = maskJSON.find(",", wStart);
                    if (wEnd == std::string::npos) wEnd = maskJSON.find("}", wStart);
                    int maskWidth = std::stoi(maskJSON.substr(wStart, wEnd - wStart));
                    
                    size_t hPos = maskJSON.find("\"height\":", pos);
                    size_t hStart = maskJSON.find(":", hPos) + 1;
                    size_t hEnd = maskJSON.find("}", hStart);
                    int maskHeight = std::stoi(maskJSON.substr(hStart, hEnd - hStart));
                    
                    // Create hole region and subtract from browser region
                    HRGN holeRegion = CreateRectRgn(x, y, x + maskWidth, y + maskHeight);
                    if (holeRegion) {
                        CombineRgn(browserRegion, browserRegion, holeRegion, RGN_DIFF);
                        DeleteObject(holeRegion);
                        maskCount++;
                    }
                    
                    pos = hEnd;
                } catch (const std::exception& e) {
                    pos++;
                }
            }
            
            if (maskCount > 0) {
                // Apply the region with holes to the CEF browser window
                SetWindowRgn(browserHwnd, browserRegion, TRUE);
            } else {
                // No valid masks found, clean up
                DeleteObject(browserRegion);
            }
            
        } catch (const std::exception& e) {
            // Silent error handling
        }
    }
    
    void removeMasks() override {
        if (!browser) {
            return;
        }
        
        HWND browserHwnd = browser->GetHost()->GetWindowHandle();
        if (!browserHwnd) {
            return;
        }
        
        // Remove window region to restore full visibility
        SetWindowRgn(browserHwnd, NULL, TRUE);
    }
    
    void toggleMirrorMode(bool enable) override {
        if (enable && !mirrorModeEnabled) {
            mirrorModeEnabled = true;
            // CEF-specific input disabling
            if (browser) {
                HWND browserHwnd = browser->GetHost()->GetWindowHandle();
                if (browserHwnd) {
                    // Disable input by making the window non-interactive
                    EnableWindow(browserHwnd, FALSE);
                    // char logMsg[128];
                    // sprintf_s(logMsg, "CEF mirror mode: Disabled input for browser HWND=%p", browserHwnd);
                    // ::log(logMsg);
                }
            }
        } else if (!enable && mirrorModeEnabled) {
            mirrorModeEnabled = false;
            // CEF-specific input enabling
            if (browser) {
                HWND browserHwnd = browser->GetHost()->GetWindowHandle();
                if (browserHwnd) {
                    // Enable input by making the window interactive again
                    EnableWindow(browserHwnd, TRUE);
                    // char logMsg[128];
                    // sprintf_s(logMsg, "CEF mirror mode: Enabled input for browser HWND=%p", browserHwnd);
                    // ::log(logMsg);
                }
            }
        }
    }
    
    // Override transparency implementation for CEF
    // On Windows, transparency for CEF is implemented as hiding/showing since SetLayeredWindowAttributes often fails on child windows
    void setTransparent(bool transparent) override {
        if (!browser) {
            return;
        }
        
        HWND browserHwnd = browser->GetHost()->GetWindowHandle();
        if (!browserHwnd) {
            return;
        }
        
        if (transparent) {
            // For transparency, hide the window completely
            ShowWindow(browserHwnd, SW_HIDE);
        } else {
            // For opacity, show the window
            ShowWindow(browserHwnd, SW_SHOW);
        }
    }
    
    // Override passthrough implementation for CEF
    void setPassthrough(bool enable) override {
        AbstractView::setPassthrough(enable); // Call base implementation to set the flag
        
        if (!browser) {
            return;
        }
        
        HWND browserHwnd = browser->GetHost()->GetWindowHandle();
        if (!browserHwnd) {
            return;
        }
        
        LONG exStyle = GetWindowLong(browserHwnd, GWL_EXSTYLE);
        if (enable) {
            // Make the window transparent to mouse clicks
            SetWindowLong(browserHwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
        } else {
            // Remove mouse transparency
            SetWindowLong(browserHwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
        }
    }
    
    // Override hidden implementation for CEF
    // On Windows, setHidden is an alias for setTransparent since transparency provides the desired hide + passthrough behavior
    void setHidden(bool hidden) override {
        // Use the working transparency implementation which provides hide + passthrough behavior
        setTransparent(hidden);

        // Also handle the container window using base implementation
        AbstractView::setHidden(hidden);
    }

    // Forward window messages to OSR window for event handling
    void HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (osr_window) {
            if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
                osr_window->HandleMouseEvent(message, wParam, lParam);
            } else if (message >= WM_KEYFIRST && message <= WM_KEYLAST) {
                osr_window->HandleKeyEvent(message, wParam, lParam);
            }
        }
    }

    void findInPage(const char* searchText, bool forward, bool matchCase) override {
        if (!browser) return;

        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (!host) return;

        if (!searchText || strlen(searchText) == 0) {
            host->StopFinding(true);
            return;
        }

        // Use CEF's native find functionality
        host->Find(CefString(searchText), forward, matchCase, false);
    }

    void stopFindInPage() override {
        if (!browser) return;

        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (host) {
            host->StopFinding(true); // true = clear selection
        }
    }

    void openDevTools() override {
        if (!browser || !client) return;
        client->OpenRemoteDevToolsFrontend(browser);
    }

    void closeDevTools() override {
        if (!browser || !client) return;
        int target_id = browser->GetIdentifier();
        client->OnRemoteDevToolsClosed(target_id);
    }

    void toggleDevTools() override {
        if (!browser || !client) return;
        int target_id = browser->GetIdentifier();
        if (client->IsDevToolsOpen(target_id)) {
            client->OnRemoteDevToolsClosed(target_id);
        } else {
            client->OpenRemoteDevToolsFrontend(browser);
        }
    }
};

// Helper function to set browser on CEFView (defined after CEFView class)
void SetBrowserOnCEFView(HWND parentWindow, CefRefPtr<CefBrowser> browser) {
    auto viewIt = g_cefViews.find(parentWindow);
    if (viewIt != g_cefViews.end()) {
        auto view = static_cast<CEFView*>(viewIt->second);
        if (view) {
            view->setBrowser(browser);
            
            // Trigger an immediate resize to bring CEF browser to front
            // The resize method will handle the z-ordering
            RECT currentBounds = view->visualBounds;
            view->resize(currentBounds, nullptr);
        }
    }
}


// ContainerView class definition
class ContainerView {
private:
    HWND m_hwnd;
    HWND m_parentWindow;
    std::vector<std::shared_ptr<AbstractView>> m_abstractViews;
    
    // Input management
    AbstractView* m_activeWebView = nullptr;  // Currently active webview for input
    
    // Window procedure for the container
    static LRESULT CALLBACK ContainerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ContainerView* container = nullptr;
        
        if (msg == WM_NCCREATE) {
            CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
            container = (ContainerView*)cs->lpCreateParams;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)container);
        } else {
            container = (ContainerView*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        }
        
        if (container) {
            return container->HandleMessage(msg, wParam, lParam);
        }
        
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_SIZE: {
                int width = LOWORD(lParam);
                int height = HIWORD(lParam);
                ResizeAutoSizingViews(width, height);
                break;
            }

            case WM_MOUSEMOVE: {
                POINT mousePos = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                UpdateActiveWebviewForMousePosition(mousePos);
                // Fall through to forward to OSR if present
                auto viewIt = g_cefViews.find(m_hwnd);
                if (viewIt != g_cefViews.end()) {
                    CEFView* cefView = static_cast<CEFView*>(viewIt->second);
                    if (cefView && cefView->isOSRMode()) cefView->HandleWindowMessage(msg, wParam, lParam);
                }
                break;
            }

            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_CHAR:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_SYSCHAR: {
                // Forward to OSR CEFView if registered under this container HWND.
                // In Spout/single-window mode, the CEFView is registered under the
                // container HWND (non-transparent path). This is the only way OSR
                // browsers receive input — they don't have their own HWND.
                auto viewIt = g_cefViews.find(m_hwnd);
                if (viewIt != g_cefViews.end()) {
                    CEFView* cefView = static_cast<CEFView*>(viewIt->second);
                    if (cefView && cefView->isOSRMode()) {
                        cefView->HandleWindowMessage(msg, wParam, lParam);
                    }
                }
                break;
            }

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(m_hwnd, &ps);
                // Don't draw anything - let child windows handle their own painting
                EndPaint(m_hwnd, &ps);
                return 0;
            }
        }

        return DefWindowProc(m_hwnd, msg, wParam, lParam);
    }
    
    void UpdateActiveWebviewForMousePosition(POINT mousePos) {
        AbstractView* newActiveView = nullptr;
        
        // Iterate through webviews in reverse order (top-most first)
        for (auto it = m_abstractViews.rbegin(); it != m_abstractViews.rend(); ++it) {
            auto& view = *it;
            
            if (view->isMousePassthroughEnabled) {
                // Skip passthrough webviews
                view->toggleMirrorMode(true);
                continue;
            }
            
            if (!newActiveView) {
                // Check if mouse is over this webview's bounds
                RECT viewBounds = view->visualBounds;
                
                auto cefView = std::dynamic_pointer_cast<CEFView>(view);

                if (cefView && cefView->getBrowser()) {
                    // For CEF, use the visualBounds which are set by resize
                    viewBounds = view->visualBounds;
                }
                
                if (PtInRect(&viewBounds, mousePos)) {
                    // Convert to local coordinates for mask checking
                    POINT localPoint = {
                        mousePos.x - viewBounds.left,
                        mousePos.y - viewBounds.top
                    };
                    
                    // Check if point is in a masked (cut-out) area
                    if (view->isPointInMask(localPoint)) {
                        // Point is in masked area, don't make this webview active
                        // Continue to check lower webviews
                        view->toggleMirrorMode(true);
                        continue;
                    }
                    
                    // Point is in unmasked area, make this webview active
                    newActiveView = view.get();
                    view->toggleMirrorMode(false);
                    continue;
                }
            }
            
            // All other webviews are non-interactive
            view->toggleMirrorMode(true);
        }
        
        // Update active webview for input routing
        m_activeWebView = newActiveView;
    }
    

    struct EnumChildData {
        RECT targetBounds;
        HWND containerHwnd;
    };
    
    static BOOL CALLBACK EnumChildCallback(HWND child, LPARAM lParam) {
        EnumChildData* data = (EnumChildData*)lParam;
        
        char className[256];
        GetClassNameA(child, className, sizeof(className));
        
        // Look for Chrome/CEF child windows
        if (strstr(className, "Chrome_WidgetWin") || 
            strstr(className, "Chrome_RenderWidgetHostHWND")) {
            
            RECT childRect;
            GetWindowRect(child, &childRect);
            
            // Convert to container coordinates
            POINT topLeft = {childRect.left, childRect.top};
            POINT bottomRight = {childRect.right, childRect.bottom};
            ScreenToClient(data->containerHwnd, &topLeft);
            ScreenToClient(data->containerHwnd, &bottomRight);
            
            // Check if this matches our WebView's bounds (with some tolerance)
            if (abs(topLeft.x - data->targetBounds.left) < 5 && 
                abs(topLeft.y - data->targetBounds.top) < 5) {
                // This is likely our WebView's child window
                SetWindowPos(child, HWND_TOP, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                return FALSE; // Stop enumeration
            }
        }
        return TRUE; // Continue enumeration
    }
    
    
    void BringCEFChildWindowToFront(AbstractView* view) {
        // Cast to CEFView to access browser
        auto cefView = dynamic_cast<CEFView*>(view);
        if (!cefView || !cefView->getBrowser()) return;
        
        CefRefPtr<CefBrowser> browser = cefView->getBrowser();
        if (!browser) return;
        
        // Get the CEF browser's window handle
        HWND browserHwnd = browser->GetHost()->GetWindowHandle();
        if (!browserHwnd) return;
        
        // char logMsg[256];
        // sprintf_s(logMsg, "BringCEFChildWindowToFront: Bringing CEF browser HWND=%p to front", browserHwnd);
        // ::log(logMsg);
        
        // Bring the CEF browser window to front
        SetWindowPos(browserHwnd, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

public:
    ContainerView(HWND parentWindow) : m_parentWindow(parentWindow), m_hwnd(NULL) {
        // Double-check parent window is valid
        if (!IsWindow(parentWindow)) {
            ::log("ERROR: Parent window handle is invalid in ContainerView constructor");
            return;
        }
        
        // Get parent window client area
        RECT clientRect;
        if (!GetClientRect(parentWindow, &clientRect)) {
            DWORD error = GetLastError();
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Failed to get parent window client rect, error: %lu", error);
            ::log(errorMsg);
            return;
        }
        
        // Validate that we have a reasonable client area
        int width = clientRect.right - clientRect.left;
        int height = clientRect.bottom - clientRect.top;
        
        if (width <= 0 || height <= 0) {
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Parent window has invalid client area: %dx%d", width, height);
            ::log(errorMsg);
            return;
        }
        
        // Register our custom window class for proper event handling
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSA wc = {0};
            wc.lpfnWndProc = ContainerWndProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = "ContainerViewClass";
            wc.hbrBackground = NULL; // Transparent background
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            
            if (!RegisterClassA(&wc)) {
                DWORD error = GetLastError();
                if (error != ERROR_CLASS_ALREADY_EXISTS) {
                    char errorMsg[256];
                    sprintf_s(errorMsg, "ERROR: Failed to register ContainerViewClass, error: %lu", error);
                    ::log(errorMsg);
                    // Fall back to STATIC class
                    goto use_static_class;
                }
            }
            classRegistered = true;
        }
        
        // Try creating with our custom class first
        m_hwnd = CreateWindowExA(
            0,
            "ContainerViewClass",
            "",  // No title text
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            0, 0, width, height,
            parentWindow,
            NULL,
            GetModuleHandle(NULL),
            this   // Pass this pointer for message handling
        );
        
        if (!m_hwnd) {
            ::log("Custom class failed, falling back to STATIC class");

            use_static_class:
            // Fallback to STATIC class
            m_hwnd = CreateWindowExA(
                0,
                "STATIC",
                "",  // No title text
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                0, 0, width, height,
                parentWindow,
                NULL,
                GetModuleHandle(NULL),
                NULL
            );

            if (!m_hwnd) {
                DWORD error = GetLastError();
                char errorMsg[256];
                sprintf_s(errorMsg, "ERROR: Failed to create container window even with STATIC class, error: %lu", error);
                ::log(errorMsg);
                return;
            }
            // Subclass the STATIC window so ContainerWndProc handles its messages.
            // The system's StaticWndProc would swallow all mouse/keyboard events;
            // subclassing lets HandleMessage forward them to the CEF OSR browser.
            SetWindowLongPtr(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
            SetWindowLongPtr(m_hwnd, GWLP_WNDPROC,  (LONG_PTR)ContainerWndProc);
        }
        
        if (m_hwnd) {
            // Verify the container window is valid
            if (!IsWindow(m_hwnd)) {
                ::log("ERROR: Container window creation returned handle but window is not valid");
                m_hwnd = NULL;
                return;
            }
            
            char successMsg[256];
        }
    }

    void ResizeAutoSizingViews(int width, int height) {
        for (auto& view : m_abstractViews) {
            if (view->fullSize) {
                // Resize the webview to match container
                RECT bounds = {0, 0, width, height};
                view->resize(bounds, nullptr);
                
                // char logMsg[256];
                // sprintf_s(logMsg, "Resized auto-sizing WebView %u to %dx%d", 
                //         view->webviewId, width, height);
                // ::log(logMsg);
            }
        }
    }

    void BringViewToFront(uint32_t webviewId) {
        auto it = std::find_if(m_abstractViews.begin(), m_abstractViews.end(),
            [webviewId](const std::shared_ptr<AbstractView>& view) {
                return view->webviewId == webviewId;
            });
        
        if (it != m_abstractViews.end()) {
            auto view = *it;
            // Move to front of vector (most recent first)
            m_abstractViews.erase(it);
            m_abstractViews.insert(m_abstractViews.begin(), view);
            
            // Bring the appropriate child window to front
            auto cefView = dynamic_cast<CEFView*>(view.get());

            if (cefView) {
                BringCEFChildWindowToFront(view.get());
            } else if (view->hwnd) {
                SetWindowPos(view->hwnd, HWND_TOP, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }
    }
    
    ~ContainerView() {
        // Explicitly remove each view before destroying HWNDs.
        // This lets CEFView::remove() defer CloseBrowser via dispatch_async
        // instead of ~CEFView() calling CloseBrowser(true) synchronously
        // on an already-destroyed HWND (which would crash).
        for (auto& view : m_abstractViews) {
            g_pendingResizeQueue.remove(view.get());
            view->remove();
            {
                std::lock_guard<std::mutex> lock(g_abstractViewsMutex);
                g_abstractViews.erase(view->webviewId);
            }
        }
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
        }
    }
    
    HWND GetHwnd() const { return m_hwnd; }
    
    void AddAbstractView(std::shared_ptr<AbstractView> view) {
    
        // Add to front of vector so it's top-most first
        m_abstractViews.insert(m_abstractViews.begin(), view); 
        BringViewToFront(view->webviewId);
        
        // TODO: Temporarily disable mirror mode for CEF testing
        // Start new webviews in mirror mode (input disabled)
        // They will be made interactive when mouse hovers over them
        // view->toggleMirrorMode(true);
    }
    
    void RemoveAbstractViewWithId(uint32_t webviewId) {
        m_abstractViews.erase(
            std::remove_if(m_abstractViews.begin(), m_abstractViews.end(),
                [webviewId](const std::shared_ptr<AbstractView>& view) {
                    return view->webviewId == webviewId;
                }),
            m_abstractViews.end());
    }
};

// Helper function to get or create container for a window
ContainerView* GetOrCreateContainer(HWND parentWindow) {
    // Validate the parent window handle
    if (!IsWindow(parentWindow)) {
        ::log("ERROR: Parent window handle is invalid");
        return nullptr;
    }
    
    auto it = g_containerViews.find(parentWindow);
    if (it == g_containerViews.end()) {
        
        auto container = std::make_unique<ContainerView>(parentWindow);
        ContainerView* containerPtr = container.get();
        
        // Only store if creation was successful
        if (containerPtr->GetHwnd() != NULL) {
            g_containerViews[parentWindow] = std::move(container);
            return containerPtr;
        } else {
            ::log("ERROR: Container creation failed, not storing");
            return nullptr;
        }
    }
    
    // log("Using existing container for window");
    return it->second.get();
}

// Stub classes for compatibility
class NSWindow {
public:
    void* contentView;
};



class MyScriptMessageHandlerWithReply {
public:
    HandlePostMessageWithReply zigCallback;
    uint32_t webviewId;
};

class WKWebView {
public:
    void* configuration;
};

struct NSRect {
    double x;
    double y;
    double width;
    double height;
};

struct createNSWindowWithFrameAndStyleParams {
    NSRect frame;
    uint32_t styleMask;
    const char *titleBarStyle;
};

// Define a struct to store window data
typedef struct {
    uint32_t windowId;
    WindowCloseHandler closeHandler;
    WindowMoveHandler moveHandler;
    WindowResizeHandler resizeHandler;
    WindowFocusHandler focusHandler;
    WindowBlurHandler blurHandler;
    WindowKeyHandler keyHandler;
} WindowData;


// Handle application menu item selection
void handleApplicationMenuSelection(UINT menuId) {
    auto it = g_menuItemActions.find(menuId);
    if (it != g_menuItemActions.end()) {
        const std::string& action = it->second;
        
        // char logMsg[256];
        // sprintf_s(logMsg, "Application menu action: %s", action.c_str());
        // ::log(logMsg);
        
        if (g_appMenuTarget && g_appMenuTarget->zigHandler) {
            if (action == "__quit__") {
                if (g_quitRequestedHandler && !g_eventLoopStopping.load()) {
                    g_quitRequestedHandler();
                } else {
                    PostQuitMessage(0);
                }
            } else if (action == "__undo__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, WM_UNDO, 0, 0);
                }
            } else if (action == "__redo__") {
                // Windows doesn't have a standard WM_REDO message
                // Use Ctrl+Y keypress simulation or application-specific handling
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    // Try sending Ctrl+Y keystroke
                    keybd_event(VK_CONTROL, 0, 0, 0);
                    keybd_event('Y', 0, 0, 0);
                    keybd_event('Y', 0, KEYEVENTF_KEYUP, 0);
                    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
                }
            } else if (action == "__cut__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, WM_CUT, 0, 0);
                }
            } else if (action == "__copy__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, WM_COPY, 0, 0);
                }
            } else if (action == "__paste__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, WM_PASTE, 0, 0);
                }
            } else if (action == "__pasteAndMatchStyle__") {
                // Paste as plain text: get clipboard text and paste it without formatting
                HWND focusedWindow = GetFocus();
                if (focusedWindow && OpenClipboard(NULL)) {
                    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                    if (hData) {
                        wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
                        if (pszText) {
                            // Clear clipboard and set as plain text
                            std::wstring text(pszText);
                            GlobalUnlock(hData);
                            EmptyClipboard();

                            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (text.length() + 1) * sizeof(wchar_t));
                            if (hMem) {
                                wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
                                if (pMem) {
                                    wcscpy(pMem, text.c_str());
                                    GlobalUnlock(hMem);
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                }
                            }
                        }
                    }
                    CloseClipboard();
                    // Now paste the plain text
                    SendMessage(focusedWindow, WM_PASTE, 0, 0);
                }
            } else if (action == "__delete__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, WM_CLEAR, 0, 0);
                }
            } else if (action == "__selectAll__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, EM_SETSEL, 0, -1);
                }
            } else if (action == "__minimize__") {
                HWND activeWindow = GetActiveWindow();
                if (activeWindow) {
                    ShowWindow(activeWindow, SW_MINIMIZE);
                }
            } else if (action == "__toggleFullScreen__") {
                HWND activeWindow = GetActiveWindow();
                if (activeWindow) {
                    // Toggle between maximized and normal state
                    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
                    GetWindowPlacement(activeWindow, &wp);
                    if (wp.showCmd == SW_MAXIMIZE) {
                        ShowWindow(activeWindow, SW_RESTORE);
                    } else {
                        ShowWindow(activeWindow, SW_MAXIMIZE);
                    }
                }
            } else if (action == "__zoom__") {
                HWND activeWindow = GetActiveWindow();
                if (activeWindow) {
                    // Zoom toggles between maximized and normal (same as toggleFullScreen on Windows)
                    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
                    GetWindowPlacement(activeWindow, &wp);
                    if (wp.showCmd == SW_MAXIMIZE) {
                        ShowWindow(activeWindow, SW_RESTORE);
                    } else {
                        ShowWindow(activeWindow, SW_MAXIMIZE);
                    }
                }
            } else if (action == "__close__") {
                HWND activeWindow = GetActiveWindow();
                if (activeWindow) {
                    PostMessage(activeWindow, WM_CLOSE, 0, 0);
                }
            } else {
                g_appMenuTarget->zigHandler(g_appMenuTarget->trayId, action.c_str());
            }
        }
    }
}


// Window procedure that will handle events and call your handlers
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Get our custom data
    WindowData* data = (WindowData*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    
    switch (msg) {
        
        case WM_INPUT: {
            if (g_isMovingWindow && g_targetWindow) {
                UINT dwSize = 0;
                GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
                
                LPBYTE lpb = new BYTE[dwSize];
                if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
                    RAWINPUT* raw = (RAWINPUT*)lpb;
                    
                    if (raw->header.dwType == RIM_TYPEMOUSE) {
                        // Check for mouse button release
                        if (raw->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) {
                            // Stop window move
                            RAWINPUTDEVICE rid;
                            rid.usUsagePage = 0x01;
                            rid.usUsage = 0x02;
                            rid.dwFlags = RIDEV_REMOVE;
                            rid.hwndTarget = NULL;
                            
                            RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));
                            g_isMovingWindow = FALSE;
                            g_targetWindow = NULL;
                        }
                        
                        // Handle mouse movement using cursor position tracking
                        else if (raw->data.mouse.lLastX != 0 || raw->data.mouse.lLastY != 0) {
                            POINT currentCursor;
                            GetCursorPos(&currentCursor);
                            
                            // Calculate delta from initial cursor position when drag started
                            int deltaX = currentCursor.x - g_initialCursorPos.x;
                            int deltaY = currentCursor.y - g_initialCursorPos.y;
                            
                            // Calculate new window position
                            int newX = g_initialWindowPos.x + deltaX;
                            int newY = g_initialWindowPos.y + deltaY;
                            
                            SetWindowPos(g_targetWindow, NULL, newX, newY, 0, 0, 
                                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        }
                    }
                }
                delete[] lpb;
            }
            break;
        }
        case WM_NCHITTEST:
            {
                // For layered windows, we need to handle hit testing to receive mouse events
                // Check if this is a CEF OSR window
                auto viewIt = g_cefViews.find(hwnd);
                if (viewIt != g_cefViews.end()) {
                    auto cefView = static_cast<CEFView*>(viewIt->second);
                    if (cefView && cefView->isOSRMode()) {
                        // Return HTCLIENT to indicate this is the client area and should receive mouse events
                        return HTCLIENT;
                    }
                }
            }
            break;

        case WM_COMMAND:
            // Check if this is an application menu command
            if (HIWORD(wParam) == 0) { // Menu item selected
                UINT menuId = LOWORD(wParam);
                handleApplicationMenuSelection(menuId);
                return 0;
            }
            break;

        // Forward mouse and keyboard events to CEF OSR view if present
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_SYSCHAR:
            {
                // Transparent OSR: CEFView is registered under the top-level HWND.
                // Non-transparent OSR (Spout): CEFView is registered under the container
                // child HWND. Mouse events reach the container directly because the child
                // sits on top of the client area. Keyboard events (WM_KEYDOWN etc.) go to
                // whichever window has focus — typically the top-level — so we fall back
                // to the container lookup to ensure F12 and other keys reach CEF.
                CEFView* cefView = nullptr;
                auto viewIt = g_cefViews.find(hwnd);
                if (viewIt != g_cefViews.end()) {
                    auto cv = static_cast<CEFView*>(viewIt->second);
                    if (cv && cv->isOSRMode()) cefView = cv;
                }
                if (!cefView) {
                    auto containerIt = g_containerViews.find(hwnd);
                    if (containerIt != g_containerViews.end()) {
                        HWND containerHwnd = containerIt->second->GetHwnd();
                        auto cefViewIt = g_cefViews.find(containerHwnd);
                        if (cefViewIt != g_cefViews.end()) {
                            auto cv = static_cast<CEFView*>(cefViewIt->second);
                            if (cv && cv->isOSRMode()) cefView = cv;
                        }
                    }
                }
                if (cefView) {
                    cefView->HandleWindowMessage(msg, wParam, lParam);
                }

                // Dispatch keyboard events to keyHandler callback
                if (data && data->keyHandler &&
                    (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
                    uint32_t keyCode = (uint32_t)wParam;
                    uint32_t modifiers = 0;
                    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= 1 << 0;
                    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= 1 << 1;
                    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= 1 << 2;
                    uint32_t isDown = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? 1 : 0;
                    uint32_t isRepeat = (lParam & (1 << 30)) ? 1 : 0;
                    data->keyHandler(data->windowId, keyCode, modifiers, isDown, isRepeat);
                }
            }
            break;

        case WM_CLOSE:
            if (data && data->closeHandler) {
                data->closeHandler(data->windowId);
            }
            break;
            
        case WM_MOVE:
            if (data && data->moveHandler) {
                int x = LOWORD(lParam);
                int y = HIWORD(lParam);
                data->moveHandler(data->windowId, x, y);
            }
            break;
            
        case WM_SIZE:
            {
                // Resize container to match window client area
                auto containerIt = g_containerViews.find(hwnd);
                if (containerIt != g_containerViews.end()) {
                    RECT clientRect;
                    GetClientRect(hwnd, &clientRect);
                    int width = clientRect.right - clientRect.left;
                    int height = clientRect.bottom - clientRect.top;
                    
                    // Resize the container window itself
                    SetWindowPos(containerIt->second->GetHwnd(), NULL, 
                        0, 0, width, height,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                    
                    // Resize all auto-resizing webviews in this container
                    containerIt->second->ResizeAutoSizingViews(width, height);
                }
                
                if (data && data->resizeHandler) {
                    int width = LOWORD(lParam);
                    int height = HIWORD(lParam);
                    data->resizeHandler(data->windowId, 0, 0, width, height);
                }
            }
            break;

        case WM_ACTIVATE:
            // Window activation - WA_ACTIVE or WA_CLICKACTIVE means window is being activated
            if (LOWORD(wParam) == WA_INACTIVE) {
                if (data && data->blurHandler) {
                    data->blurHandler(data->windowId);
                }
            } else {
                if (data && data->focusHandler) {
                    data->focusHandler(data->windowId);
                }
            }
            break;

        case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                // Don't need to do anything here, just validate the paint region
                EndPaint(hwnd, &ps);
            }
            return 0;
            
        case WM_TIMER:
            if (wParam == 1) {
                KillTimer(hwnd, 1);
                ::log("Timer fired - forcing window refresh");
                InvalidateRect(hwnd, NULL, TRUE);
                UpdateWindow(hwnd);
            }
            return 0;
            
        case WM_DESTROY:
            // Clean up application menu when main window is destroyed
            if (g_applicationMenu) {
                DestroyMenu(g_applicationMenu);
                g_applicationMenu = NULL;
            }
            g_appMenuTarget.reset();
            
            // Clean up container view
            g_containerViews.erase(hwnd);
            
            // Clean up window data
            if (data) {
                free(data);
                SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            }
            break;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// handles window things on Windows
LRESULT CALLBACK MessageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_EXECUTE_SYNC_BLOCK:
            MainThreadDispatcher::handleSyncTask(lParam);
            return 0;
        case WM_EXECUTE_ASYNC_BLOCK:
            MainThreadDispatcher::handleSyncTask(lParam);
            return 0;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}


class NSStatusItem {
public:
    NOTIFYICONDATA nid;
    HWND hwnd;
    uint32_t trayId;
    ZigStatusItemHandler handler;
    HMENU contextMenu;
    std::string title;
    std::string imagePath;
    
    NSStatusItem() {
        memset(&nid, 0, sizeof(NOTIFYICONDATA));
        hwnd = NULL;
        trayId = 0;
        handler = nullptr;
        contextMenu = NULL;
    }
    
    ~NSStatusItem() {
        if (contextMenu) {
            DestroyMenu(contextMenu);
        }
        // Remove from system tray
        Shell_NotifyIcon(NIM_DELETE, &nid);
    }
};

// Global map to store tray items by their window handle
static std::map<HWND, NSStatusItem*> g_trayItems;
static UINT g_trayMessageId = WM_USER + 100;

struct SimpleJsonValue {
    enum Type { STRING, BOOL, ARRAY, OBJECT, UNKNOWN };
    Type type = UNKNOWN;
    std::string stringValue;
    bool boolValue = false;
    std::vector<SimpleJsonValue> arrayValue;
    std::map<std::string, SimpleJsonValue> objectValue;
};

// Simple JSON parsing functions
std::string trimWhitespace(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

std::string extractQuotedString(const std::string& json, size_t& pos) {
    if (pos >= json.length() || json[pos] != '"') return "";
    pos++; // Skip opening quote
    
    std::string result;
    while (pos < json.length() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.length()) {
            pos++; // Skip escape character
            switch (json[pos]) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '\\': result += '\\'; break;
                case '"': result += '"'; break;
                default: result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        pos++;
    }
    
    if (pos < json.length() && json[pos] == '"') {
        pos++; // Skip closing quote
    }
    
    return result;
}

SimpleJsonValue parseJsonValue(const std::string& json, size_t& pos);

SimpleJsonValue parseJsonObject(const std::string& json, size_t& pos) {
    SimpleJsonValue obj;
    obj.type = SimpleJsonValue::OBJECT;
    
    if (pos >= json.length() || json[pos] != '{') return obj;
    pos++; // Skip '{'
    
    while (pos < json.length()) {
        // Skip whitespace
        while (pos < json.length() && isspace(json[pos])) pos++;
        
        if (pos >= json.length()) break;
        if (json[pos] == '}') {
            pos++; // Skip '}'
            break;
        }
        
        // Parse key
        std::string key = extractQuotedString(json, pos);
        
        // Skip whitespace and ':'
        while (pos < json.length() && (isspace(json[pos]) || json[pos] == ':')) pos++;
        
        // Parse value
        SimpleJsonValue value = parseJsonValue(json, pos);
        obj.objectValue[key] = value;
        
        // Skip whitespace and optional ','
        while (pos < json.length() && (isspace(json[pos]) || json[pos] == ',')) pos++;
    }
    
    return obj;
}

SimpleJsonValue parseJsonArray(const std::string& json, size_t& pos) {
    SimpleJsonValue arr;
    arr.type = SimpleJsonValue::ARRAY;
    
    if (pos >= json.length() || json[pos] != '[') return arr;
    pos++; // Skip '['
    
    while (pos < json.length()) {
        // Skip whitespace
        while (pos < json.length() && isspace(json[pos])) pos++;
        
        if (pos >= json.length()) break;
        if (json[pos] == ']') {
            pos++; // Skip ']'
            break;
        }
        
        // Parse value
        SimpleJsonValue value = parseJsonValue(json, pos);
        arr.arrayValue.push_back(value);
        
        // Skip whitespace and optional ','
        while (pos < json.length() && (isspace(json[pos]) || json[pos] == ',')) pos++;
    }
    
    return arr;
}

SimpleJsonValue parseJsonValue(const std::string& json, size_t& pos) {
    SimpleJsonValue value;
    
    // Skip whitespace
    while (pos < json.length() && isspace(json[pos])) pos++;
    
    if (pos >= json.length()) return value;
    
    if (json[pos] == '"') {
        // String value
        value.type = SimpleJsonValue::STRING;
        value.stringValue = extractQuotedString(json, pos);
    } else if (json[pos] == '{') {
        // Object value
        value = parseJsonObject(json, pos);
    } else if (json[pos] == '[') {
        // Array value
        value = parseJsonArray(json, pos);
    } else if (json.substr(pos, 4) == "true") {
        // Boolean true
        value.type = SimpleJsonValue::BOOL;
        value.boolValue = true;
        pos += 4;
    } else if (json.substr(pos, 5) == "false") {
        // Boolean false
        value.type = SimpleJsonValue::BOOL;
        value.boolValue = false;
        pos += 5;
    } else {
        // Skip unknown values
        while (pos < json.length() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']') pos++;
    }
    
    return value;
}

SimpleJsonValue parseJson(const std::string& json) {
    size_t pos = 0;
    return parseJsonValue(json, pos);
}

// Helper to parse virtual key code from key string for menu accelerators
static UINT getMenuVirtualKeyCode(const std::string& key) {
    std::string lowerKey = key;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);

    // Letters
    if (lowerKey.length() == 1 && lowerKey[0] >= 'a' && lowerKey[0] <= 'z') {
        return 'A' + (lowerKey[0] - 'a');
    }
    // Numbers
    if (lowerKey.length() == 1 && lowerKey[0] >= '0' && lowerKey[0] <= '9') {
        return '0' + (lowerKey[0] - '0');
    }
    // Function keys
    if (lowerKey[0] == 'f' && lowerKey.length() >= 2) {
        try {
            int fNum = std::stoi(lowerKey.substr(1));
            if (fNum >= 1 && fNum <= 24) return VK_F1 + (fNum - 1);
        } catch (...) {}
    }
    // Special keys
    if (lowerKey == "space" || lowerKey == " ") return VK_SPACE;
    if (lowerKey == "return" || lowerKey == "enter") return VK_RETURN;
    if (lowerKey == "tab") return VK_TAB;
    if (lowerKey == "escape" || lowerKey == "esc") return VK_ESCAPE;
    if (lowerKey == "backspace") return VK_BACK;
    if (lowerKey == "delete" || lowerKey == "del") return VK_DELETE;
    if (lowerKey == "insert") return VK_INSERT;
    if (lowerKey == "up") return VK_UP;
    if (lowerKey == "down") return VK_DOWN;
    if (lowerKey == "left") return VK_LEFT;
    if (lowerKey == "right") return VK_RIGHT;
    if (lowerKey == "home") return VK_HOME;
    if (lowerKey == "end") return VK_END;
    if (lowerKey == "pageup") return VK_PRIOR;
    if (lowerKey == "pagedown") return VK_NEXT;
    // Symbols
    if (lowerKey == "plus") return VK_OEM_PLUS;
    if (lowerKey == "minus") return VK_OEM_MINUS;
    if (lowerKey == "-") return VK_OEM_MINUS;
    if (lowerKey == "=" || lowerKey == "+") return VK_OEM_PLUS;
    if (lowerKey == "[") return VK_OEM_4;
    if (lowerKey == "]") return VK_OEM_6;
    if (lowerKey == "\\") return VK_OEM_5;
    if (lowerKey == ";") return VK_OEM_1;
    if (lowerKey == "'") return VK_OEM_7;
    if (lowerKey == ",") return VK_OEM_COMMA;
    if (lowerKey == ".") return VK_OEM_PERIOD;
    if (lowerKey == "/") return VK_OEM_2;
    if (lowerKey == "`") return VK_OEM_3;

    return 0;
}

// Parse modifiers from accelerator string for menu accelerators using the
// shared cross-platform parser. Returns FCONTROL, FALT, FSHIFT flags.
static BYTE parseMenuModifiers(const std::string& accelerator, std::string& outKey) {
    auto parts = electrobun::parseAccelerator(accelerator);
    outKey = parts.key;

    BYTE modifiers = FVIRTKEY;
    if (parts.commandOrControl || parts.command || parts.control) modifiers |= FCONTROL;
    if (parts.alt)                                                modifiers |= FALT;
    if (parts.shift)                                              modifiers |= FSHIFT;
    return modifiers;
}

// Build display string for accelerator (e.g., "Ctrl+S", "Ctrl+Shift+N")
static std::string buildAcceleratorDisplayString(const std::string& accelerator) {
    std::string keyPart;
    BYTE modifiers = parseMenuModifiers(accelerator, keyPart);

    std::string display;
    if (modifiers & FCONTROL) {
        display += "Ctrl+";
    }
    if (modifiers & FALT) {
        display += "Alt+";
    }
    if (modifiers & FSHIFT) {
        display += "Shift+";
    }

    // Capitalize the key for display
    std::string upperKey = keyPart;
    if (!upperKey.empty()) {
        upperKey[0] = toupper(upperKey[0]);
    }

    // Handle special key display names
    std::string lowerKey = keyPart;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
    if (lowerKey == "return" || lowerKey == "enter") {
        upperKey = "Enter";
    } else if (lowerKey == "escape" || lowerKey == "esc") {
        upperKey = "Esc";
    } else if (lowerKey == "delete" || lowerKey == "del") {
        upperKey = "Del";
    } else if (lowerKey == "backspace") {
        upperKey = "Backspace";
    } else if (lowerKey == "space") {
        upperKey = "Space";
    } else if (lowerKey == "pageup") {
        upperKey = "PgUp";
    } else if (lowerKey == "pagedown") {
        upperKey = "PgDn";
    } else if (lowerKey == "plus") {
        upperKey = "+";
    } else if (lowerKey == "minus") {
        upperKey = "-";
    }

    display += upperKey;
    return display;
}

// Function to create Windows menu from JSON config (equivalent to createMenuFromConfig)
HMENU createMenuFromConfig(const SimpleJsonValue& menuConfig, NSStatusItem* statusItem) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        ::log("ERROR: Failed to create popup menu");
        return NULL;
    }
    
    if (menuConfig.type != SimpleJsonValue::ARRAY) {
        ::log("ERROR: Menu config is not an array");
        return menu;
    }
    
    for (const auto& itemValue : menuConfig.arrayValue) {
        if (itemValue.type != SimpleJsonValue::OBJECT) continue;
        
        const auto& itemData = itemValue.objectValue;
        
        // Helper lambda to get string value
        auto getString = [&](const std::string& key, const std::string& defaultVal = "") -> std::string {
            auto it = itemData.find(key);
            if (it != itemData.end() && it->second.type == SimpleJsonValue::STRING) {
                return it->second.stringValue;
            }
            return defaultVal;
        };
        
        // Helper lambda to get bool value
        auto getBool = [&](const std::string& key, bool defaultVal = false) -> bool {
            auto it = itemData.find(key);
            if (it != itemData.end() && it->second.type == SimpleJsonValue::BOOL) {
                return it->second.boolValue;
            }
            return defaultVal;
        };
        
        std::string type = getString("type");
        std::string label = getString("label");
        std::string action = getString("action");
        std::string role = getString("role");
        std::string accelerator = getString("accelerator");

        bool enabled = getBool("enabled", true);
        bool checked = getBool("checked", false);
        bool hidden = getBool("hidden", false);
        std::string tooltip = getString("tooltip");

        if (hidden) {
            continue;
        } else if (type == "divider") {
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        } else {
            UINT flags = MF_STRING;
            if (!enabled) flags |= MF_GRAYED;

            UINT menuId = g_nextMenuId++;

            // Store the action for this menu ID
            if (!action.empty()) {
                g_menuItemActions[menuId] = action;
            }

            // Handle system roles (similar to macOS implementation)
            if (!role.empty()) {
                if (role == "quit") {
                    // For quit, we'll handle it specially in the menu callback
                    g_menuItemActions[menuId] = "__quit__";
                }

                // Set default accelerators for common roles if not specified
                if (accelerator.empty()) {
                    if (role == "undo") {
                        accelerator = "z";
                    } else if (role == "redo") {
                        accelerator = "y";
                    } else if (role == "cut") {
                        accelerator = "x";
                    } else if (role == "copy") {
                        accelerator = "c";
                    } else if (role == "paste") {
                        accelerator = "v";
                    } else if (role == "selectAll") {
                        accelerator = "a";
                    }
                }
            }

            // Build the label with accelerator display for context menus
            // On Windows, context menus use mnemonic keys (just the letter, not Ctrl+Letter)
            std::string displayLabel = label;
            if (!accelerator.empty()) {
                // For context menus, display just the letter (mnemonic key)
                // The user presses just the letter while the menu is open
                if (accelerator.length() == 1 && isalpha(accelerator[0])) {
                    displayLabel += "\t" + std::string(1, (char)toupper(accelerator[0]));
                } else {
                    // For complex accelerators, extract just the key part
                    std::string accelDisplay = buildAcceleratorDisplayString(accelerator);
                    // Remove "Ctrl+" prefix for context menus since they use mnemonics
                    size_t ctrlPos = accelDisplay.find("Ctrl+");
                    if (ctrlPos != std::string::npos) {
                        accelDisplay = accelDisplay.substr(ctrlPos + 5); // Skip "Ctrl+"
                    }
                    if (!accelDisplay.empty()) {
                        displayLabel += "\t" + accelDisplay;
                    }
                }
            }

            // Append the menu item
            AppendMenuA(menu, flags, menuId, displayLabel.c_str());

            if (checked) {
                CheckMenuItem(menu, menuId, MF_BYCOMMAND | MF_CHECKED);
            }

            // Handle submenus
            auto submenuIt = itemData.find("submenu");
            if (submenuIt != itemData.end() && submenuIt->second.type == SimpleJsonValue::ARRAY) {
                HMENU submenu = createMenuFromConfig(submenuIt->second, statusItem);
                if (submenu) {
                    ModifyMenuA(menu, menuId, MF_BYCOMMAND | MF_POPUP, (UINT_PTR)submenu, displayLabel.c_str());
                }
            }
        }
    }
    
    return menu;
}

// Function to handle menu item selection
void handleMenuItemSelection(UINT menuId, NSStatusItem* statusItem) {
    auto it = g_menuItemActions.find(menuId);
    if (it != g_menuItemActions.end()) {
        const std::string& action = it->second;

        if (statusItem && statusItem->handler) {
            if (action == "__quit__") {
                if (g_quitRequestedHandler && !g_eventLoopStopping.load()) {
                    g_quitRequestedHandler();
                } else {
                    PostQuitMessage(0);
                }
            } else {
                statusItem->handler(statusItem->trayId, action.c_str());
            }
        }
    }
}

// Rebuild the accelerator table from collected accelerators
static void rebuildAcceleratorTable() {
    if (g_hAccelTable) {
        DestroyAcceleratorTable(g_hAccelTable);
        g_hAccelTable = NULL;
    }

    if (!g_menuAccelerators.empty()) {
        g_hAccelTable = CreateAcceleratorTableA(g_menuAccelerators.data(), (int)g_menuAccelerators.size());
        if (g_hAccelTable) {
            // ::log("Created accelerator table with " + std::to_string(g_menuAccelerators.size()) + " entries");
        }
    }
}

// Clear all menu accelerators (call before rebuilding menu)
static void clearMenuAccelerators() {
    g_menuAccelerators.clear();
    if (g_hAccelTable) {
        DestroyAcceleratorTable(g_hAccelTable);
        g_hAccelTable = NULL;
    }
}

// Function to set accelerator keys for menu items
// Returns the display string to append to the menu label
std::string setMenuItemAccelerator(HMENU menu, UINT menuId, const std::string& accelerator, UINT modifierMask = 0) {
    if (accelerator.empty()) return "";

    std::string keyPart;
    BYTE modifiers;
    UINT vkCode;

    // Check if this is a simple single-letter accelerator (for role defaults)
    if (accelerator.length() == 1 && isalpha(accelerator[0])) {
        // Single letter with Ctrl modifier (from role defaults)
        vkCode = toupper(accelerator[0]);
        modifiers = FVIRTKEY | FCONTROL;
        keyPart = accelerator;
    } else {
        // Parse the full accelerator string
        modifiers = parseMenuModifiers(accelerator, keyPart);
        vkCode = getMenuVirtualKeyCode(keyPart);
    }

    // Apply modifierMask override if specified
    if (modifierMask > 0) {
        modifiers = FVIRTKEY;
        if (modifierMask & 1) modifiers |= FCONTROL;
        if (modifierMask & 2) modifiers |= FSHIFT;
        if (modifierMask & 4) modifiers |= FALT;
    }

    if (vkCode == 0) {
        // ::log("Failed to parse accelerator key: " + accelerator);
        return "";
    }

    // Add to accelerator table
    ACCEL accel;
    accel.fVirt = modifiers;
    accel.key = (WORD)vkCode;
    accel.cmd = (WORD)menuId;
    g_menuAccelerators.push_back(accel);

    // Build and return the display string
    if (accelerator.length() == 1 && isalpha(accelerator[0])) {
        return "Ctrl+" + std::string(1, (char)toupper(accelerator[0]));
    }
    return buildAcceleratorDisplayString(accelerator);
}

// Enhanced createMenuFromConfig for application menu
HMENU createApplicationMenuFromConfig(const SimpleJsonValue& menuConfig, StatusItemTarget* target) {
    HMENU menuBar = CreateMenu();
    if (!menuBar) {
        ::log("ERROR: Failed to create menu bar");
        return NULL;
    }
    
    if (menuConfig.type != SimpleJsonValue::ARRAY) {
        ::log("ERROR: Application menu config is not an array");
        DestroyMenu(menuBar);
        return NULL;
    }
    
    for (const auto& topLevelItem : menuConfig.arrayValue) {
        if (topLevelItem.type != SimpleJsonValue::OBJECT) continue;
        
        const auto& itemData = topLevelItem.objectValue;
        
        // Helper lambda to get string value
        auto getString = [&](const std::string& key, const std::string& defaultVal = "") -> std::string {
            auto it = itemData.find(key);
            if (it != itemData.end() && it->second.type == SimpleJsonValue::STRING) {
                return it->second.stringValue;
            }
            return defaultVal;
        };
        
        // Helper lambda to get bool value
        auto getBool = [&](const std::string& key, bool defaultVal = false) -> bool {
            auto it = itemData.find(key);
            if (it != itemData.end() && it->second.type == SimpleJsonValue::BOOL) {
                return it->second.boolValue;
            }
            return defaultVal;
        };
        
        std::string label = getString("label");
        bool hidden = getBool("hidden", false);
        
        if (hidden) continue;
        
        // Check if this has a submenu
        auto submenuIt = itemData.find("submenu");
        if (submenuIt != itemData.end() && submenuIt->second.type == SimpleJsonValue::ARRAY) {
            HMENU popupMenu = CreatePopupMenu();
            if (!popupMenu) continue;
            
            // Process submenu items
            for (const auto& subItemValue : submenuIt->second.arrayValue) {
                if (subItemValue.type != SimpleJsonValue::OBJECT) continue;
                
                const auto& subItemData = subItemValue.objectValue;
                
                // Helper lambdas for subitem data
                auto getSubString = [&](const std::string& key, const std::string& defaultVal = "") -> std::string {
                    auto it = subItemData.find(key);
                    if (it != subItemData.end() && it->second.type == SimpleJsonValue::STRING) {
                        return it->second.stringValue;
                    }
                    return defaultVal;
                };
                
                auto getSubBool = [&](const std::string& key, bool defaultVal = false) -> bool {
                    auto it = subItemData.find(key);
                    if (it != subItemData.end() && it->second.type == SimpleJsonValue::BOOL) {
                        return it->second.boolValue;
                    }
                    return defaultVal;
                };
                
                std::string subType = getSubString("type");
                std::string subLabel = getSubString("label");
                std::string subAction = getSubString("action");
                std::string subRole = getSubString("role");
                std::string subAccelerator = getSubString("accelerator");
                
                bool subEnabled = getSubBool("enabled", true);
                bool subChecked = getSubBool("checked", false);
                bool subHidden = getSubBool("hidden", false);
                
                if (subHidden) {
                    continue;
                } else if (subType == "divider") {
                    AppendMenuA(popupMenu, MF_SEPARATOR, 0, NULL);
                } else {
                    UINT flags = MF_STRING;
                    if (!subEnabled) flags |= MF_GRAYED;
                    
                    UINT menuId = g_nextMenuId++;
                    
                    // Store the action for this menu ID
                    if (!subAction.empty()) {
                        g_menuItemActions[menuId] = subAction;
                    }
                    
                    // Handle system roles
                    if (!subRole.empty()) {
                        if (subRole == "quit") {
                            g_menuItemActions[menuId] = "__quit__";
                        } else if (subRole == "undo") {
                            g_menuItemActions[menuId] = "__undo__";
                        } else if (subRole == "redo") {
                            g_menuItemActions[menuId] = "__redo__";
                        } else if (subRole == "cut") {
                            g_menuItemActions[menuId] = "__cut__";
                        } else if (subRole == "copy") {
                            g_menuItemActions[menuId] = "__copy__";
                        } else if (subRole == "paste") {
                            g_menuItemActions[menuId] = "__paste__";
                        } else if (subRole == "pasteAndMatchStyle") {
                            g_menuItemActions[menuId] = "__pasteAndMatchStyle__";
                        } else if (subRole == "delete") {
                            g_menuItemActions[menuId] = "__delete__";
                        } else if (subRole == "selectAll") {
                            g_menuItemActions[menuId] = "__selectAll__";
                        } else if (subRole == "minimize") {
                            g_menuItemActions[menuId] = "__minimize__";
                        } else if (subRole == "toggleFullScreen" || subRole == "togglefullscreen") {
                            g_menuItemActions[menuId] = "__toggleFullScreen__";
                        } else if (subRole == "zoom") {
                            g_menuItemActions[menuId] = "__zoom__";
                        } else if (subRole == "close") {
                            g_menuItemActions[menuId] = "__close__";
                        }
                        // Note: The following roles are macOS-only and not implemented on Windows:
                        // hide, hideOthers, showAll, startSpeaking, stopSpeaking, bringAllToFront

                        // Set default accelerators for common roles if not specified
                        if (subAccelerator.empty()) {
                            if (subRole == "undo") {
                                subAccelerator = "z";
                            } else if (subRole == "redo") {
                                subAccelerator = "y";
                            } else if (subRole == "cut") {
                                subAccelerator = "x";
                            } else if (subRole == "copy") {
                                subAccelerator = "c";
                            } else if (subRole == "paste" || subRole == "pasteAndMatchStyle") {
                                subAccelerator = "v";
                            } else if (subRole == "delete") {
                                subAccelerator = "Delete";
                            } else if (subRole == "selectAll") {
                                subAccelerator = "a";
                            } else if (subRole == "toggleFullScreen" || subRole == "togglefullscreen") {
                                subAccelerator = "F11";
                            }
                        }
                    }
                    
                    // Build the label with accelerator display
                    std::string displayLabel = subLabel;
                    if (!subAccelerator.empty()) {
                        std::string accelDisplay = setMenuItemAccelerator(popupMenu, menuId, subAccelerator, 0);
                        if (!accelDisplay.empty()) {
                            displayLabel += "\t" + accelDisplay;
                        }
                    }

                    // Append the menu item
                    AppendMenuA(popupMenu, flags, menuId, displayLabel.c_str());

                    if (subChecked) {
                        CheckMenuItem(popupMenu, menuId, MF_BYCOMMAND | MF_CHECKED);
                    }
                    
                    // Handle nested submenus
                    auto nestedSubmenuIt = subItemData.find("submenu");
                    if (nestedSubmenuIt != subItemData.end() && nestedSubmenuIt->second.type == SimpleJsonValue::ARRAY) {
                        HMENU nestedSubmenu = createMenuFromConfig(nestedSubmenuIt->second, reinterpret_cast<NSStatusItem*>(target));
                        if (nestedSubmenu) {
                            ModifyMenuA(popupMenu, menuId, MF_BYCOMMAND | MF_POPUP, (UINT_PTR)nestedSubmenu, subLabel.c_str());
                        }
                    }
                }
            }
            
            // Add the popup menu to the menu bar
            AppendMenuA(menuBar, MF_POPUP, (UINT_PTR)popupMenu, label.c_str());
        } else {
            // Top-level item without submenu
            UINT menuId = g_nextMenuId++;
            std::string action = getString("action");
            
            if (!action.empty()) {
                g_menuItemActions[menuId] = action;
            }
            
            UINT flags = MF_STRING;
            if (!getBool("enabled", true)) flags |= MF_GRAYED;
            
            AppendMenuA(menuBar, flags, menuId, label.c_str());
        }
    }
    
    return menuBar;
}


















// Helper function to terminate all CEF helper processes
void TerminateCEFHelperProcesses() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    
    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            // Check if this is a "bun Helper.exe" process
            if (wcsstr(pe32.szExeFile, L"bun Helper.exe") != nullptr) {
                HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                if (hProcess != nullptr) {
                    std::wcout << L"[CEF] Terminating helper process: " << pe32.szExeFile 
                              << L" (PID: " << pe32.th32ProcessID << L")" << std::endl;
                    TerminateProcess(hProcess, 0);
                    CloseHandle(hProcess);
                }
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    
    CloseHandle(hSnapshot);
}

ELECTROBUN_EXPORT bool initCEF() {
    if (g_cef_initialized) {
        return true; // Already initialized
    }
    
    // Create a job object to track all child processes
    if (!g_job_object) {
        g_job_object = CreateJobObject(nullptr, nullptr);
        if (g_job_object) {
            // Configure the job object to terminate all child processes when the main process exits
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_job_object, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
            
            // Assign the current process to the job object
            // This ensures all child processes (CEF helpers) are part of this job
            AssignProcessToJobObject(g_job_object, GetCurrentProcess());
            std::cout << "[CEF] Created job object for process tracking" << std::endl;
        }
    }

    // Get the directory where the current executable is located
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) {
        *lastSlash = '\0'; // Remove the executable name
    }

    // Set up CEF paths (resources are in ./cef relative to executable)
    std::string cefResourceDir = std::string(exePath) + "\\cef";

    // Build cache path with identifier/channel structure (consistent with CLI and updater)
    // Use %LOCALAPPDATA%\{identifier}\{channel}\CEF
    std::string userDataDir;
    char* localAppData = getenv("LOCALAPPDATA");
    if (localAppData) {
        userDataDir = buildAppDataPath(localAppData, g_electrobunIdentifier, g_electrobunChannel, "CEF", '\\');
        std::cout << "[CEF] Using path: " << userDataDir << std::endl;
    } else {
        // Fallback to executable directory if LOCALAPPDATA not available
        userDataDir = buildAppDataPath(exePath, g_electrobunIdentifier, g_electrobunChannel, "cef_cache", '\\');
    }

    // Create cache directory if it doesn't exist
    CreateDirectoryA(userDataDir.c_str(), NULL);

    // Initialize CEF
    CefMainArgs main_args(GetModuleHandle(NULL));
    
    // Create the app
    g_cef_app = new ElectrobunCefApp();

    // Read user-defined chromium flags from build.json (in exe directory)
    std::string buildJsonPath = std::string(exePath) + "\\build.json";
    std::string buildJsonContent = electrobun::readFileToString(buildJsonPath);
    if (!buildJsonContent.empty()) {
        g_userChromiumFlags = electrobun::parseChromiumFlags(buildJsonContent);
    }

    // CEF settings
    CefSettings settings;
    settings.no_sandbox = true;
    settings.multi_threaded_message_loop = false;
    settings.external_message_pump = true; // We pump CEF via OnScheduleMessagePumpWork
    settings.windowless_rendering_enabled = true; // Required for OSR/transparent windows

    // Remote DevTools port with scan for availability
    int selectedPort = FindAvailableRemoteDebugPort(9222, 9232);
    if (selectedPort == 0) {
        selectedPort = 9222;
        std::cout << "[CEF] Remote DevTools: no free port in 9222-9232, falling back to 9222" << std::endl;
    }
    g_remoteDebugPort = selectedPort;
    settings.remote_debugging_port = selectedPort;

    // Set the subprocess path to the helper executable (name derived from running exe)
    {
        char exeNameBuf[MAX_PATH];
        GetModuleFileNameA(NULL, exeNameBuf, MAX_PATH);
        std::string helperName = "chromeyumm Helper.exe"; // fallback
        if (char* sl = strrchr(exeNameBuf, '\\')) {
            std::string file = sl + 1;
            std::string stem = file.size() > 4 ? file.substr(0, file.size() - 4) : file;
            helperName = stem + " Helper.exe";
        }
        CefString(&settings.browser_subprocess_path) = std::string(exePath) + "\\" + helperName;
    }

    // Set paths — pak files and icudtl.dat are in the exe directory; locales/ is a subdir
    CefString(&settings.resources_dir_path) = std::string(exePath);
    CefString(&settings.locales_dir_path) = std::string(exePath) + "\\locales";
    CefString(&settings.cache_path) = userDataDir;
    
    // Add language settings like macOS
    CefString(&settings.accept_language_list) = "en-US,en";
    
    // Set minimal logging
    settings.log_severity = LOGSEVERITY_ERROR;
    CefString(&settings.log_file) = "";
    
    
    bool success = CefInitialize(main_args, settings, g_cef_app.get(), nullptr);
    if (success) {
        g_cef_initialized = true;

        
        // We'll start the message pump timer when we create the first browser
    } else {
        ::log("Failed to initialize CEF");
    }
    
    return success;
}


// Utility function for creating CEF request contexts with partition support
CefRefPtr<CefRequestContext> CreateRequestContextForPartition(const char* partitionIdentifier,
                                                               uint32_t webviewId) {
    printf("DEBUG CEF: CreateRequestContextForPartition called for webview %u, partition: %s\n",
           webviewId, partitionIdentifier ? partitionIdentifier : "null");

    CefRequestContextSettings settings;

    if (!partitionIdentifier || !partitionIdentifier[0]) {
        // No partition - use in-memory session
        settings.persist_session_cookies = false;
    } else {
        std::string identifier(partitionIdentifier);
        bool isPersistent = identifier.substr(0, 8) == "persist:";

        if (isPersistent) {
            // Persistent partition - create cache directory
            std::string partitionName = identifier.substr(8);

            // Get %LOCALAPPDATA% path
            char* localAppData = getenv("LOCALAPPDATA");
            if (!localAppData) {
                printf("ERROR CEF: LOCALAPPDATA not found, falling back to in-memory session\n");
                settings.persist_session_cookies = false;
            } else {
                // Build path with identifier/channel structure (consistent with CLI and updater)
                // Structure: %LOCALAPPDATA%\{identifier}\{channel}\CEF\Partitions\{partitionName}
                std::string cachePath = buildPartitionPath(localAppData, g_electrobunIdentifier, g_electrobunChannel, "CEF", partitionName, '\\');

                // Create directory if it doesn't exist
                std::wstring wideCachePath(cachePath.begin(), cachePath.end());
                SHCreateDirectoryExW(NULL, wideCachePath.c_str(), NULL);

                settings.persist_session_cookies = true;
                CefString(&settings.cache_path).FromString(cachePath);

                printf("DEBUG CEF: Persistent partition '%s' using cache path: %s\n",
                       partitionName.c_str(), cachePath.c_str());
            }
        } else {
            // Non-persistent partition - in-memory session
            settings.persist_session_cookies = false;
            printf("DEBUG CEF: In-memory partition '%s'\n", identifier.c_str());
        }
    }

    // Create the request context
    CefRefPtr<CefRequestContext> context = CefRequestContext::CreateContext(settings, nullptr);

    return context;
}

// Internal factory method for creating CEF instances
static std::shared_ptr<CEFView> createCEFView(uint32_t webviewId,
                                       HWND hwnd,
                                       const char *url,
                                       double x, double y,
                                       double width, double height,
                                       bool autoResize,
                                       const char *partitionIdentifier,
                                       DecideNavigationCallback navigationCallback,
                                       WebviewEventHandler webviewEventHandler,
                                       HandlePostMessage eventBridgeHandler,
                                       HandlePostMessage bunBridgeHandler,
                                       HandlePostMessage internalBridgeHandler,
                                       const char *electrobunPreloadScript,
                                       const char *customPreloadScript,
                                       bool transparent,
                                       bool sandbox,
                                       bool sharedTexture = false) {
    
    auto view = std::make_shared<CEFView>(webviewId);
    view->hwnd = hwnd;
    view->fullSize = autoResize;
    
    // Initialize CEF on main thread
    bool cefInitResult = MainThreadDispatcher::dispatch_sync([=]() -> bool {
        return initCEF();
    });
    
    if (!cefInitResult) {
        ::log("ERROR: Failed to initialize CEF");
        return view;
    }
    
    // CEF browser creation logic
    MainThreadDispatcher::dispatch_sync([=]() {
        auto container = GetOrCreateContainer(hwnd);
        if (!container) {
            ::log("ERROR: Failed to create container");
            return;
        }
        
        // Create CEF browser info
        CefWindowInfo windowInfo;
        windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
        CefRect cefBounds((int)x, (int)y, (int)width, (int)height);

        CefBrowserSettings browserSettings;
        // Note: web_security setting for CEF would need correct API

        // Set transparent background if requested
        if (transparent) {
            // CEF uses ARGB format: 0x00000000 = fully transparent
            browserSettings.background_color = 0;
        }

        // In OSR mode (sharedTexture / Spout path), CEF defaults to 30fps.
        // Raise to 60fps so OnAcceleratedPaint fires at the display refresh rate.
        if (sharedTexture) {
            browserSettings.windowless_frame_rate = 60;
        }

        // Create CEF client with bridge handlers
        auto client = new ElectrobunCefClient(webviewId, eventBridgeHandler, bunBridgeHandler, internalBridgeHandler, sandbox);

        // Configure OSR mode for transparent windows
        if (transparent) {
            // Enable OSR mode
            client->EnableOSR((int)width, (int)height);

            // Create OSR window for rendering
            // For OSR, the window should fill the parent window's client area (0, 0)
            OSRWindow* osrWindow = new OSRWindow(hwnd, 0, 0, (int)width, (int)height);
            view->setOSRWindow(osrWindow);
            client->SetOSRWindow(osrWindow);

            // Use windowless (off-screen) rendering
            windowInfo.SetAsWindowless(hwnd);
        } else if (sharedTexture) {
            // Spout output mode: OSR with shared_texture_enabled=1.
            // CEF renders off-screen and calls OnAcceleratedPaint each frame with a DXGI
            // NT shared texture handle from its GPU compositor. No DWM window capture (WGC)
            // is needed — WGC was found to throttle CEF from 60fps to 45-50fps by changing
            // DWM's composition mode. OnAcceleratedPaint has zero DWM overhead.
            SpoutWindowState state;
            state.hwnd   = hwnd;
            state.width  = (int)width;
            state.height = (int)height;
            g_spoutWindows[webviewId] = state;

            // Enable OSR render handler and wire up window id for OnAcceleratedPaint dispatch.
            client->EnableOSR((int)width, (int)height);
            client->SetRenderHandlerWindowId(webviewId);

            // Request shared texture delivery (DXGI NT handle per frame).
            windowInfo.shared_texture_enabled = 1;
            windowInfo.SetAsWindowless(hwnd);

            // Create an OSRWindow for mouse/keyboard event forwarding.
            // Rendering goes through OnAcceleratedPaint (shared texture), but mouse and
            // keyboard events must still be synthesized via the CEF browser host API.
            // setOSRWindow() also sets is_osr_mode=true so ContainerView routes events here.
            OSRWindow* osrWindow = new OSRWindow(hwnd, 0, 0, (int)width, (int)height);
            view->setOSRWindow(osrWindow);
            client->SetOSRWindow(osrWindow);
        } else {
            // Use windowed mode
            windowInfo.SetAsChild(container->GetHwnd(), cefBounds);
        }
        
        // Set up preload scripts
        if (electrobunPreloadScript && strlen(electrobunPreloadScript) > 0) {
            client->AddPreloadScript(std::string(electrobunPreloadScript));
        }
        if (customPreloadScript && strlen(customPreloadScript) > 0) {
            client->UpdateCustomPreloadScript(std::string(customPreloadScript));
        }
        
        // Set the webview event handler for ctrl+click handling
        client->SetWebviewEventHandler(webviewEventHandler);

        // Set the abstract view pointer for navigation rules
        client->SetAbstractView(view.get());

        view->setClient(client);

        // Set up load-end callback for deferred transparency/passthrough application
        // CEF navigation events can reset window state, so we re-apply after page load
        CEFView* viewPtr = view.get();
        client->SetLoadEndCallback([viewPtr]() {
            if (viewPtr->pendingStartTransparent) {
                viewPtr->setTransparent(true);
                viewPtr->pendingStartTransparent = false;
            }
            if (viewPtr->pendingStartPassthrough) {
                viewPtr->setPassthrough(true);
                viewPtr->pendingStartPassthrough = false;
            }
            // Re-apply passthrough if it was already set (in case navigation reset it)
            if (viewPtr->isMousePassthroughEnabled && !viewPtr->pendingStartPassthrough) {
                viewPtr->setPassthrough(true);
            }
        });

        // Create request context for partition isolation
        CefRefPtr<CefRequestContext> requestContext = CreateRequestContextForPartition(
            partitionIdentifier,
            webviewId
        );

        // Create browser synchronously (like Mac implementation)
        // Note: OnLoadStart will fire during this call, but the load handler has a direct
        // reference to the client, so preload scripts are available immediately without race condition

        // Pass sandbox flag to renderer process via extra_info
        CefRefPtr<CefDictionaryValue> extra_info = CefDictionaryValue::Create();
        extra_info->SetBool("sandbox", sandbox);

        CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(
            windowInfo, client, url ? url : "about:blank", browserSettings, extra_info, requestContext);

        if (browser) {
            // Store preload script by browser ID for compatibility with other code paths
            std::string combinedScript = client->GetCombinedScript();
            if (!combinedScript.empty()) {
                g_preloadScripts[browser->GetIdentifier()] = combinedScript;
            }

            // Map browser ID to webview ID for CEF scheme handler
            {
                std::lock_guard<std::mutex> lock(browserMapMutex);
                browserToWebviewMap[browser->GetIdentifier()] = webviewId;
            }

            // Set browser on view immediately since we have it synchronously
            view->setBrowser(browser);

            // Track browser in global map
            g_cefBrowsers[browser->GetIdentifier()] = browser;
            g_browser_count++;

            container->AddAbstractView(view);

            // Register in global AbstractView map for navigation rules
            {
                std::lock_guard<std::mutex> lock(g_abstractViewsMutex);
                g_abstractViews[view->webviewId] = view.get();
            }

            // Add client to global map.
            // Transparent (OSR) mode uses the top-level hwnd; windowed mode (including
            // sharedTexture/Spout capture mode) uses the container hwnd.
            HWND containerHwnd = container->GetHwnd();
            HWND mapKey = transparent ? hwnd : containerHwnd;

            g_cefClients[mapKey] = client;
            g_cefViews[mapKey] = view.get();

            printf("CEF: Registered view with hwnd=%p (transparent=%d, sharedTexture=%d)\n",
                   mapKey, transparent, sharedTexture);

            // Set browser on client for script execution
            client->SetBrowser(browser);

            // Set initial bounds on view before calling resize
            RECT initialBounds = {(LONG)x, (LONG)y, (LONG)(x + width), (LONG)(y + height)};
            view->visualBounds = initialBounds;

            // Handle z-ordering immediately since browser is ready
            view->resize(initialBounds, nullptr);

            // Apply deferred initial transparent/passthrough state now that browser is ready
            // Note: We apply immediately here, but also have a load-end callback to re-apply
            // after page load completes (since CEF navigation can reset window state)
            if (view->pendingStartTransparent) {
                view->setTransparent(true);
                // Don't clear yet - load-end callback will handle it after page loads
            }
            if (view->pendingStartPassthrough) {
                view->setPassthrough(true);
                // Don't clear yet - load-end callback will handle it after page loads
            }

        }
    });

    return view;
}

// Console control handler for graceful shutdown
BOOL WINAPI ConsoleControlHandler(DWORD dwCtrlType) {
    switch (dwCtrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            std::cout << "[shutdown] Received console shutdown signal" << std::endl;

            if (g_quitRequestedHandler && !g_eventLoopStopping.load()) {
                // Route through bun's quit sequence for proper beforeQuit handling
                g_quitRequestedHandler();
                // Wait for orderly shutdown (Windows gives ~5s for CTRL_CLOSE_EVENT)
                int waited = 0;
                while (!g_shutdownComplete.load() && waited < 4000) {
                    Sleep(10);
                    waited += 10;
                }
            } else {
                // Fallback: direct shutdown - post WM_QUIT to exit the message loop
                PostQuitMessage(0);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

extern "C" {

ELECTROBUN_EXPORT void startEventLoop(const char* identifier, const char* name, const char* channel) {
    g_mainThreadId = GetCurrentThreadId();

    // Store identifier, name, and channel globally for use in CEF initialization
    if (identifier && identifier[0]) {
        g_electrobunIdentifier = std::string(identifier);
    }
    if (name && name[0]) {
        g_electrobunName = std::string(name);
    }
    if (channel && channel[0]) {
        g_electrobunChannel = std::string(channel);
    }

    // Set up console control handler for graceful shutdown on Ctrl+C
    if (!SetConsoleCtrlHandler(ConsoleControlHandler, TRUE)) {
        std::cout << "[CEF] Warning: Failed to set console control handler" << std::endl;
    }
    
    // Create a hidden message-only window for dispatching
    WNDCLASSA wc = {0};  // Use ANSI version
    wc.lpfnWndProc = MessageWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "MessageWindowClass";  // Use ANSI string
    RegisterClassA(&wc);  // Use ANSI version
    
    HWND messageWindow = CreateWindowA(  // Use ANSI version
        "MessageWindowClass",  // Use ANSI string
        "", 
        0, 0, 0, 0, 0,
        HWND_MESSAGE, // This makes it a message-only window
        NULL, 
        GetModuleHandle(NULL), 
        NULL
    );
    
    // Initialize the dispatcher
    MainThreadDispatcher::initialize(messageWindow);

    // Signal initEventLoop() that the message window + dispatcher are ready
    if (g_eventLoopReadyEvent) SetEvent(g_eventLoopReadyEvent);

    // Initialize CEF if available
    if (isCEFAvailable()) {
        if (initCEF()) {
            // With external_message_pump=true, CefDoMessageLoopWork does NOT
            // internally pump Windows messages. This prevents CEF from stealing
            // our Windows messages while still processing CEF work on a timer.
            //
            // OnScheduleMessagePumpWork posts WM_CEF_SCHEDULE_WORK for immediate
            // work and uses SetTimer for delayed work. We also keep a baseline
            // timer to ensure CEF always gets serviced.
            WNDCLASSA cefPumpWc = {0};
            cefPumpWc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
                if (msg == WM_CEF_SCHEDULE_WORK || msg == WM_TIMER) {
                    CefDoMessageLoopWork();
                    return 0;
                }
                return DefWindowProc(hwnd, msg, wParam, lParam);
            };
            cefPumpWc.hInstance = GetModuleHandle(NULL);
            cefPumpWc.lpszClassName = "CefPumpWindowClass";
            RegisterClassA(&cefPumpWc);
            g_cefPumpWindow = CreateWindowA("CefPumpWindowClass", "", 0, 0, 0, 0, 0,
                                           HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL);

            // Baseline timer ensures CEF always gets serviced even if
            // OnScheduleMessagePumpWork misses a beat
            SetTimer(g_cefPumpWindow, 2, 16, nullptr);

            // Kick off initial CEF work
            CefDoMessageLoopWork();

            // Standard Windows message loop
            MSG msg;
            while (GetMessage(&msg, NULL, 0, 0)) {
                if (g_hAccelTable && TranslateAccelerator(msg.hwnd, g_hAccelTable, &msg)) {
                    continue;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            // Clean up after shutdown
            std::cout << "[CEF] CEF message loop ended, performing cleanup..." << std::endl;
            TerminateCEFHelperProcesses();

            // Close job object
            if (g_job_object) {
                CloseHandle(g_job_object);
                g_job_object = nullptr;
            }

            CefShutdown();
            g_shutdownComplete.store(true);
        } else {
            // Fall back to Windows message loop if CEF init fails
            MSG msg;
            while (GetMessage(&msg, NULL, 0, 0)) {
                // Check for menu accelerators first
                if (g_hAccelTable && TranslateAccelerator(msg.hwnd, g_hAccelTable, &msg)) {
                    continue;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            g_shutdownComplete.store(true);
        }
    } else {
        // Use Windows message loop if CEF is not available
        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            // Check for menu accelerators first
            if (g_hAccelTable && TranslateAccelerator(msg.hwnd, g_hAccelTable, &msg)) {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        g_shutdownComplete.store(true);
    }
}


// Starts the Windows message loop + CEF on a background thread.
// Blocks until the message window + dispatcher are ready, then returns.
// Must be called before any window/webview creation (dispatch_sync).
static DWORD WINAPI EventLoopThreadProc(LPVOID) {
    startEventLoop(
        g_electrobunIdentifier.c_str(),
        g_electrobunName.c_str(),
        g_electrobunChannel.c_str()
    );
    return 0;
}

ELECTROBUN_EXPORT void initEventLoop(const char* identifier, const char* name, const char* channel) {
    if (identifier && identifier[0]) g_electrobunIdentifier = std::string(identifier);
    if (name      && name[0])       g_electrobunName       = std::string(name);
    if (channel   && channel[0])    g_electrobunChannel    = std::string(channel);

    g_eventLoopReadyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, EventLoopThreadProc, NULL, 0, NULL);
    // Block until startEventLoop has set up the message window + dispatcher
    WaitForSingleObject(g_eventLoopReadyEvent, 10000);
    CloseHandle(g_eventLoopReadyEvent);
    g_eventLoopReadyEvent = NULL;
}

ELECTROBUN_EXPORT void stopEventLoop() {
    if (g_eventLoopStopping.exchange(true)) {
        return;
    }

    std::cout << "[stopEventLoop] Initiating clean event loop exit" << std::endl;

    if (isCEFAvailable() && g_cef_initialized) {
        // We use a standard Windows message loop (not CefRunMessageLoop),
        // so PostQuitMessage is the correct way to exit.
        PostQuitMessage(0);
    } else {
        // Post WM_QUIT to the main thread's message queue
        if (g_mainThreadId != 0) {
            PostThreadMessage(g_mainThreadId, WM_QUIT, 0, 0);
        }
    }
}

ELECTROBUN_EXPORT void killApp() {
    // Deprecated - delegates to stopEventLoop for backward compatibility
    stopEventLoop();
}

ELECTROBUN_EXPORT void waitForShutdownComplete(int timeoutMs) {
    int waited = 0;
    while (!g_shutdownComplete.load() && waited < timeoutMs) {
        Sleep(10);
        waited += 10;
    }
}

ELECTROBUN_EXPORT void forceExit(int code) {
    _exit(code);
}

ELECTROBUN_EXPORT void setQuitRequestedHandler(QuitRequestedHandler handler) {
    g_quitRequestedHandler = handler;
}

ELECTROBUN_EXPORT void shutdownApplication() {
    // Deprecated - use stopEventLoop() instead
    stopEventLoop();
}

// Global flags set by setNextWebviewFlags, consumed by initWebview
static struct {
    bool startTransparent;
    bool startPassthrough;
} g_nextWebviewFlags = {false, false};

ELECTROBUN_EXPORT void setNextWebviewFlags(bool startTransparent, bool startPassthrough) {
    g_nextWebviewFlags.startTransparent = startTransparent;
    g_nextWebviewFlags.startPassthrough = startPassthrough;
}

// Flag for shared-texture (OSR) mode — set by setNextWebviewSharedTexture(),
// consumed and reset to false by initWebview().
static bool g_nextWebviewSharedTexture = false;

ELECTROBUN_EXPORT void setNextWebviewSharedTexture(bool enabled) {
    g_nextWebviewSharedTexture = enabled;
}

// Clean, elegant initWebview function - Windows version matching Mac pattern
ELECTROBUN_EXPORT AbstractView* initWebview(uint32_t webviewId,
                         NSWindow *window,  // Actually HWND on Windows
                         const char *renderer,
                         const char *url,
                         double x, double y,
                         double width, double height,
                         bool autoResize,
                         const char *partitionIdentifier,
                         DecideNavigationCallback navigationCallback,
                         WebviewEventHandler webviewEventHandler,
                         HandlePostMessage eventBridgeHandler,
                         HandlePostMessage bunBridgeHandler,
                         HandlePostMessage internalBridgeHandler,
                         const char *electrobunPreloadScript,
                         const char *customPreloadScript,
                         const char *viewsRoot,
                         bool transparent,
                         bool sandbox) {

    // Read and clear pre-set flags
    bool startTransparent = g_nextWebviewFlags.startTransparent;
    bool startPassthrough = g_nextWebviewFlags.startPassthrough;
    g_nextWebviewFlags = {false, false};

    // Read and clear the shared-texture flag (set by setNextWebviewSharedTexture).
    bool sharedTexture = g_nextWebviewSharedTexture;
    g_nextWebviewSharedTexture = false;

    // Serialize webview creation to avoid conflicts
    std::lock_guard<std::mutex> lock(g_webviewCreationMutex);


    HWND hwnd = reinterpret_cast<HWND>(window);

    // Factory pattern - choose implementation based on renderer
    AbstractView* view = nullptr;

    if (renderer && strcmp(renderer, "cef") == 0 && isCEFAvailable()) {
        auto cefView = createCEFView(webviewId, hwnd, url, x, y, width, height, autoResize,
                                    partitionIdentifier, navigationCallback, webviewEventHandler,
                                    eventBridgeHandler, bunBridgeHandler, internalBridgeHandler,
                                    electrobunPreloadScript, customPreloadScript, transparent, sandbox,
                                    sharedTexture);
        view = cefView.get();
    }

    // Note: Object lifetime is managed by the ContainerView which holds shared_ptr references
    // The factories add the views to containers, so they remain alive after this function returns

    // Store initial state flags — applied later when the view is fully initialized
    // (browser/HWND may not be available yet due to async creation)
    if (view) {
        view->pendingStartTransparent = startTransparent;
        view->pendingStartPassthrough = startPassthrough;
    }

    return view;

}


ELECTROBUN_EXPORT MyScriptMessageHandlerWithReply* addScriptMessageHandlerWithReply(WKWebView *webView,
                                                              uint32_t webviewId,
                                                              const char *name,
                                                              HandlePostMessageWithReply callback) {
    // Stub implementation
    MyScriptMessageHandlerWithReply* handler = new MyScriptMessageHandlerWithReply();
    handler->zigCallback = callback;
    handler->webviewId = webviewId;
    return handler;
}
ELECTROBUN_EXPORT void loadURLInWebView(AbstractView *abstractView, const char *urlString) {
    if (!abstractView || !urlString) {
        ::log("ERROR: Invalid parameters passed to loadURLInWebView");
        return;
    }
    
    // Use virtual method which handles threading and implementation details
    
    abstractView->loadURL(urlString);
}


ELECTROBUN_EXPORT void loadHTMLInWebView(AbstractView *abstractView, const char *htmlString) {
    if (!abstractView || !htmlString) {
        ::log("ERROR: Invalid parameters passed to loadHTMLInWebView");
        return;
    }

    abstractView->loadHTML(htmlString);
}

ELECTROBUN_EXPORT void webviewGoBack(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView or webview in webviewGoBack");
        return;
    }
    
    abstractView->goBack();
}

ELECTROBUN_EXPORT void webviewGoForward(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView or webview in webviewGoForward");
        return;
    }
    
    abstractView->goForward();
}

ELECTROBUN_EXPORT void webviewReload(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView or webview in webviewReload");
        return;
    }
    
    abstractView->reload();
}

ELECTROBUN_EXPORT void webviewRemove(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView in webviewRemove");
        return;
    }

    abstractView->remove();
}

ELECTROBUN_EXPORT BOOL webviewCanGoBack(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView or webview in webviewCanGoBack");
        return FALSE;
    }
    
    return abstractView->canGoBack();
}

ELECTROBUN_EXPORT BOOL webviewCanGoForward(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView or webview in webviewCanGoForward");
        return FALSE;
    }
    
    return abstractView->canGoForward();
}

ELECTROBUN_EXPORT void evaluateJavaScriptWithNoCompletion(AbstractView *abstractView, const char *script) {
    if (!abstractView || !script) {
        ::log("ERROR: Invalid parameters passed to evaluateJavaScriptWithNoCompletion");
        return;
    }

    abstractView->evaluateJavaScriptWithNoCompletion(script);
    
}

ELECTROBUN_EXPORT void testFFI(void *ptr) {
    // Stub implementation
}

ELECTROBUN_EXPORT void callAsyncJavaScript(const char *messageId,
                        AbstractView *abstractView,
                        const char *jsString,
                        uint32_t webviewId,
                        uint32_t hostWebviewId,
                        callAsyncJavascriptCompletionHandler completionHandler) {
    // Stub implementation
    if (completionHandler) {
        completionHandler(messageId, webviewId, hostWebviewId, "\"\"");
    }
}

ELECTROBUN_EXPORT void addPreloadScriptToWebView(AbstractView *abstractView, const char *scriptContent, BOOL forMainFrameOnly) {
    if (abstractView && scriptContent) {
        MainThreadDispatcher::dispatch_sync([abstractView, scriptContent]() {
            abstractView->addPreloadScriptToWebView(scriptContent);
        });
    }
}

ELECTROBUN_EXPORT void updatePreloadScriptToWebView(AbstractView *abstractView,
                                 const char *scriptIdentifier,
                                 const char *scriptContent,
                                 BOOL forMainFrameOnly) {
    if (abstractView && scriptContent) {
        MainThreadDispatcher::dispatch_sync([abstractView, scriptContent]() {
            abstractView->updateCustomPreloadScript(scriptContent);
        });
    }
}

ELECTROBUN_EXPORT void invokeDecisionHandler(void (*decisionHandler)(int), int policy) {
    // Stub implementation
    if (decisionHandler) {
        decisionHandler(policy);
    }
}

ELECTROBUN_EXPORT const char* getUrlFromNavigationAction(void *navigationAction) {
    // Stub implementation
    static const char* defaultUrl = "about:blank";
    return defaultUrl;
}

ELECTROBUN_EXPORT const char* getBodyFromScriptMessage(void *message) {
    // Stub implementation
    static const char* emptyString = "";
    return emptyString;
}

ELECTROBUN_EXPORT void webviewSetTransparent(AbstractView *abstractView, BOOL transparent) {
    if (abstractView) {
        // UI operations must be performed on the main thread
        MainThreadDispatcher::dispatch_sync([abstractView, transparent]() {
            abstractView->setTransparent(transparent);
        });
    }
}

ELECTROBUN_EXPORT void webviewSetPassthrough(AbstractView *abstractView, BOOL enablePassthrough) {
    if (abstractView) {
        // UI operations must be performed on the main thread
        MainThreadDispatcher::dispatch_sync([abstractView, enablePassthrough]() {
            abstractView->setPassthrough(enablePassthrough);
        });
    }
}

ELECTROBUN_EXPORT void webviewSetHidden(AbstractView *abstractView, BOOL hidden) {
    if (abstractView) {
        // UI operations must be performed on the main thread
        MainThreadDispatcher::dispatch_sync([abstractView, hidden]() {
            abstractView->setTransparent(hidden);
        });
    }
}

ELECTROBUN_EXPORT void setWebviewNavigationRules(AbstractView *abstractView, const char *rulesJson) {
    if (abstractView) {
        // UI operations must be performed on the main thread
        MainThreadDispatcher::dispatch_sync([abstractView, rulesJson]() {
            abstractView->setNavigationRulesFromJSON(rulesJson);
        });
    }
}

ELECTROBUN_EXPORT void webviewFindInPage(AbstractView *abstractView, const char *searchText, bool forward, bool matchCase) {
    if (abstractView) {
        MainThreadDispatcher::dispatch_sync([abstractView, searchText, forward, matchCase]() {
            abstractView->findInPage(searchText, forward, matchCase);
        });
    }
}

// Remote DevTools helper functions for CEF on Windows
void openRemoteDevTools(uint32_t webviewId) {
    // TODO: Implement remote debugger approach for Windows CEF
    // This should trigger the remote debugger system when it's ported from macOS
    // For now, this is a placeholder that can be implemented once the 
    // remote debugger approach is fully ported to Windows
}

void closeRemoteDevTools(uint32_t webviewId) {
    // TODO: Close remote debugger window for Windows CEF
}

void toggleRemoteDevTools(uint32_t webviewId) {
    // TODO: Toggle remote debugger window for Windows CEF  
    // For now, just try to open
    openRemoteDevTools(webviewId);
}

ELECTROBUN_EXPORT void webviewStopFind(AbstractView *abstractView) {
    if (abstractView) {
        MainThreadDispatcher::dispatch_sync([abstractView]() {
            abstractView->stopFindInPage();
        });
    }
}

ELECTROBUN_EXPORT void webviewOpenDevTools(AbstractView *abstractView) {
    if (abstractView) {
        MainThreadDispatcher::dispatch_sync([abstractView]() {
            abstractView->openDevTools();
        });
    }
}

ELECTROBUN_EXPORT void webviewCloseDevTools(AbstractView *abstractView) {
    if (abstractView) {
        MainThreadDispatcher::dispatch_sync([abstractView]() {
            abstractView->closeDevTools();
        });
    }
}

ELECTROBUN_EXPORT void webviewToggleDevTools(AbstractView *abstractView) {
    if (abstractView) {
        MainThreadDispatcher::dispatch_sync([abstractView]() {
            abstractView->toggleDevTools();
        });
    }
}

ELECTROBUN_EXPORT void webviewSetPageZoom(AbstractView *abstractView, double zoomLevel) {
    // pageZoom is WebKit-specific, not available on Windows
    // TODO: implement zoom if needed
}

ELECTROBUN_EXPORT double webviewGetPageZoom(AbstractView *abstractView) {
    // pageZoom is WebKit-specific, not available on Windows
    return 1.0;
}

ELECTROBUN_EXPORT NSRect createNSRectWrapper(double x, double y, double width, double height) {
    // Stub implementation
    NSRect rect = {x, y, width, height};
    return rect;
}

ELECTROBUN_EXPORT NSWindow* createNSWindowWithFrameAndStyle(uint32_t windowId,
                                         createNSWindowWithFrameAndStyleParams config,
                                         WindowCloseHandler zigCloseHandler,
                                         WindowMoveHandler zigMoveHandler,
                                         WindowResizeHandler zigResizeHandler,
                                         WindowFocusHandler zigFocusHandler,
                                         WindowBlurHandler zigBlurHandler,
                                         WindowKeyHandler zigKeyHandler) {
    // Stub implementation
    return new NSWindow();
}

ELECTROBUN_EXPORT void testFFI2(void (*completionHandler)()) {
    // Stub implementation
    if (completionHandler) {
        completionHandler();
    }
}

ELECTROBUN_EXPORT HWND createWindowWithFrameAndStyleFromWorker(
    uint32_t windowId,
    double x, double y,
    double width, double height,
    uint32_t styleMask,
    const char* titleBarStyle,
    bool transparent,
    WindowCloseHandler zigCloseHandler,
    WindowMoveHandler zigMoveHandler,
    WindowResizeHandler zigResizeHandler,
    WindowFocusHandler zigFocusHandler,
    WindowBlurHandler zigBlurHandler,
    WindowKeyHandler zigKeyHandler) {

    // Everything GUI-related needs to be dispatched to main thread
    HWND hwnd = MainThreadDispatcher::dispatch_sync([=]() -> HWND {

        // Register window class with our custom procedure
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSA wc = {0};  // Use ANSI version
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = "BasicWindowClass";  // Use ANSI string
            RegisterClassA(&wc);  // Use ANSI version
            classRegistered = true;
        }

        // Create window data structure to store callbacks
        WindowData* data = (WindowData*)malloc(sizeof(WindowData));
        if (!data) return NULL;

        data->windowId = windowId;
        data->closeHandler = zigCloseHandler;
        data->moveHandler = zigMoveHandler;
        data->resizeHandler = zigResizeHandler;
        data->focusHandler = zigFocusHandler;
        data->blurHandler = zigBlurHandler;
        data->keyHandler = zigKeyHandler;

        // Map style mask to Windows style
        DWORD windowStyle = WS_OVERLAPPEDWINDOW; // Default
        DWORD windowExStyle = WS_EX_APPWINDOW;

        // Handle titleBarStyle options
        if (titleBarStyle && strcmp(titleBarStyle, "hidden") == 0) {
            // "hidden" = borderless window (no titlebar, no native controls)
            // This is for completely custom chrome
            windowStyle = WS_POPUP | WS_VISIBLE;
        } else if (titleBarStyle && strcmp(titleBarStyle, "hiddenInset") == 0) {
            // "hiddenInset" = window with border but custom titlebar area
            // On Windows, we can't easily do the exact macOS inset style,
            // so we provide a borderless window with shadow for similar effect
            windowStyle = WS_POPUP | WS_VISIBLE | WS_THICKFRAME;
        }
        // else: default titleBarStyle = WS_OVERLAPPEDWINDOW (standard window)

        // Handle transparent windows
        if (transparent) {
            // For transparent windows, we need WS_EX_LAYERED to support per-pixel alpha
            windowExStyle |= WS_EX_LAYERED;
        }

        // Create the window
        HWND hwnd = CreateWindowExA(  // Use CreateWindowExA to support extended styles
            windowExStyle,
            "BasicWindowClass",  // Use ANSI string
            "",
            windowStyle,
            (int)x, (int)y,
            (int)width, (int)height,
            NULL, NULL, GetModuleHandle(NULL), NULL
        );

        if (hwnd) {
            // Store our data with the window
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)data);

            // Set app icon on taskbar and title bar
            applyAppIcon(hwnd);

            // Register for NativeDisplayWindow DWM thumbnail lookup
            g_windowIdToHwnd[windowId] = hwnd;

            // Apply transparent window background if requested
            if (transparent) {
                // For transparent windows using OSR, UpdateLayeredWindow will handle
                // the rendering with per-pixel alpha. We don't use SetLayeredWindowAttributes.
                // The OSRWindow will call UpdateLayeredWindow with the CEF-rendered content.
            }

            // Don't apply application menu to transparent or custom chrome windows
            // Only apply to windows with default titleBarStyle
            bool isCustomChrome = transparent ||
                                 (titleBarStyle && strcmp(titleBarStyle, "hidden") == 0) ||
                                 (titleBarStyle && strcmp(titleBarStyle, "hiddenInset") == 0);

            if (!isCustomChrome && g_applicationMenu) {
                if (SetMenu(hwnd, g_applicationMenu)) {
                    DrawMenuBar(hwnd);
                    // char logMsg[256];
                    // sprintf_s(logMsg, "Applied application menu to new window: HWND=%p", hwnd);
                    // ::log(logMsg);
                } else {
                    ::log("Failed to apply application menu to new window");
                }
            }


            // Show the window
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        } else {
            // Clean up if window creation failed
            free(data);
        }

        return hwnd;
    });

    return hwnd;
}

ELECTROBUN_EXPORT void showWindow(void *window) {
    // On Windows, window ptr is actually HWND
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in showWindow");
        return;
    }
    
    // Dispatch to main thread to ensure thread safety
    MainThreadDispatcher::dispatch_sync([=]() {      
        // Show the window if it's hidden
        if (!IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_SHOW);
        }
        
        // Bring window to foreground - this is more complex on Windows
        // due to foreground window restrictions
        
        // First, try the simple approach
        if (SetForegroundWindow(hwnd)) {
        } else {
            // If that fails, we need to work around Windows' foreground restrictions
            DWORD currentThreadId = GetCurrentThreadId();
            DWORD foregroundThreadId = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
            
            if (currentThreadId != foregroundThreadId) {
                // Attach to the foreground thread's input queue temporarily
                if (AttachThreadInput(currentThreadId, foregroundThreadId, TRUE)) {
                    SetForegroundWindow(hwnd);
                    SetFocus(hwnd);
                    AttachThreadInput(currentThreadId, foregroundThreadId, FALSE);
                } else {
                    // Last resort - flash the window to get user attention
                    FLASHWINFO fwi = {0};
                    fwi.cbSize = sizeof(FLASHWINFO);
                    fwi.hwnd = hwnd;
                    fwi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
                    fwi.uCount = 3;
                    fwi.dwTimeout = 0;
                    FlashWindowEx(&fwi);
                    
                }
            }
        }
        
        // Ensure the window is active and focused
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
        
        // Bring to top of Z-order
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, 
                    SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        
    });
}

ELECTROBUN_EXPORT void hideWindow(void *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!IsWindow(hwnd)) return;
    MainThreadDispatcher::dispatch_sync([=]() {
        ShowWindow(hwnd, SW_HIDE);
        // Re-assert WasHidden(false) for any OSR browser whose host HWND this is,
        // so CEF keeps delivering OnAcceleratedPaint after the window is hidden.
        for (auto& kv : g_spoutWindows) {
            if (kv.second.hwnd == hwnd && kv.second.active) {
                for (auto& bkv : browserToWebviewMap) {
                    if (bkv.second == kv.first) {
                        auto bIt = g_cefBrowsers.find(bkv.first);
                        if (bIt != g_cefBrowsers.end() && bIt->second)
                            bIt->second->GetHost()->WasHidden(false);
                        break;
                    }
                }
            }
        }
    });
}

ELECTROBUN_EXPORT void setWindowTitle(NSWindow *window, const char *title) {
    // On Windows, NSWindow* is actually HWND
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in setWindowTitle");
        return;
    }
    
    // Dispatch to main thread to ensure thread safety
    MainThreadDispatcher::dispatch_sync([=]() {
        if (title && strlen(title) > 0) {
            // Convert UTF-8 to wide string for Unicode support
            int size = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
            if (size > 0) {
                std::wstring wTitle(size - 1, 0);
                MultiByteToWideChar(CP_UTF8, 0, title, -1, &wTitle[0], size);
                
                // Set the window title
                if (SetWindowTextW(hwnd, wTitle.c_str())) {
                    
                } else {
                    DWORD error = GetLastError();
                    char errorMsg[256];
                    sprintf_s(errorMsg, "Failed to set window title, error: %lu", error);
                    ::log(errorMsg);
                }
            } else {
                ::log("ERROR: Failed to convert title to wide string");
            }
        } else {
            // Set empty title
            if (SetWindowTextW(hwnd, L"")) {
            } else {
                DWORD error = GetLastError();
                char errorMsg[256];
                sprintf_s(errorMsg, "Failed to clear window title, error: %lu", error);
                ::log(errorMsg);
            }
        }
    });
}

ELECTROBUN_EXPORT void closeWindow(NSWindow *window) {
    // On Windows, NSWindow* is actually HWND
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in closeWindow");
        return;
    }

    // Dispatch to main thread to ensure thread safety
    MainThreadDispatcher::dispatch_sync([=]() {


        // Clean up any associated container views before closing
        auto containerIt = g_containerViews.find(hwnd);
        if (containerIt != g_containerViews.end()) {
            g_containerViews.erase(containerIt);
        }

        // Send WM_CLOSE message to the window
        // This will trigger the window's close handler if one is set
        if (PostMessage(hwnd, WM_CLOSE, 0, 0)) {
        } else {
            DWORD error = GetLastError();
            char errorMsg[256];
            sprintf_s(errorMsg, "Failed to send WM_CLOSE message, error: %lu", error);
            ::log(errorMsg);

            // If PostMessage fails, try DestroyWindow as a fallback
            ::log("Attempting DestroyWindow as fallback");
            if (DestroyWindow(hwnd)) {
            } else {
                DWORD destroyError = GetLastError();
                char destroyErrorMsg[256];
                sprintf_s(destroyErrorMsg, "DestroyWindow also failed, error: %lu", destroyError);
                ::log(destroyErrorMsg);
            }
        }
    });
}

ELECTROBUN_EXPORT void minimizeWindow(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in minimizeWindow");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        ShowWindow(hwnd, SW_MINIMIZE);
    });
}

ELECTROBUN_EXPORT void restoreWindow(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in restoreWindow");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        ShowWindow(hwnd, SW_RESTORE);
    });
}

ELECTROBUN_EXPORT bool isWindowMinimized(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        return false;
    }

    return IsIconic(hwnd) != 0;
}

ELECTROBUN_EXPORT void maximizeWindow(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in maximizeWindow");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        ShowWindow(hwnd, SW_MAXIMIZE);
    });
}

ELECTROBUN_EXPORT void unmaximizeWindow(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in unmaximizeWindow");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        ShowWindow(hwnd, SW_RESTORE);
    });
}

ELECTROBUN_EXPORT bool isWindowMaximized(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        return false;
    }

    return IsZoomed(hwnd) != 0;
}

ELECTROBUN_EXPORT void setWindowFullScreen(NSWindow *window, bool fullScreen) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in setWindowFullScreen");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        static std::map<HWND, WINDOWPLACEMENT> savedPlacements;
        static std::map<HWND, LONG> savedStyles;

        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        bool isCurrentlyFullScreen = (style & WS_POPUP) && !(style & WS_OVERLAPPEDWINDOW);

        if (fullScreen && !isCurrentlyFullScreen) {
            // Save current state
            WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
            GetWindowPlacement(hwnd, &wp);
            savedPlacements[hwnd] = wp;
            savedStyles[hwnd] = style;

            // Get the monitor info for the window
            HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(MONITORINFO) };
            GetMonitorInfo(monitor, &mi);

            // Remove window decorations and set to fullscreen
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW | WS_POPUP);
            SetWindowPos(hwnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        } else if (!fullScreen && isCurrentlyFullScreen) {
            // Restore saved state
            auto styleIt = savedStyles.find(hwnd);
            if (styleIt != savedStyles.end()) {
                SetWindowLong(hwnd, GWL_STYLE, styleIt->second);
                savedStyles.erase(styleIt);
            }

            auto placementIt = savedPlacements.find(hwnd);
            if (placementIt != savedPlacements.end()) {
                SetWindowPlacement(hwnd, &placementIt->second);
                savedPlacements.erase(placementIt);
            }

            SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    });
}

ELECTROBUN_EXPORT bool isWindowFullScreen(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        return false;
    }

    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    return (style & WS_POPUP) && !(style & WS_OVERLAPPEDWINDOW);
}

ELECTROBUN_EXPORT void setWindowAlwaysOnTop(NSWindow *window, bool alwaysOnTop) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in setWindowAlwaysOnTop");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        SetWindowPos(hwnd,
            alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE);
    });
}

ELECTROBUN_EXPORT bool isWindowAlwaysOnTop(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        return false;
    }

    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    return (exStyle & WS_EX_TOPMOST) != 0;
}

// ---------------------------------------------------------------------------
// NativeDisplayWindow — lightweight Win32 windows backed by DWM thumbnails.
// One master BrowserWindow renders the full virtual canvas; each display
// window shows a cropped portion via DWM composition (zero CPU, GPU-only).
// ---------------------------------------------------------------------------

static LRESULT CALLBACK DisplayWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // Prevent GDI flicker — swap chain owns the surface
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        // Suppress Alt+F4 and system close — display windows should only be
        // destroyed programmatically via destroyNativeDisplayWindow().
        return 0;
    case WM_SETCURSOR:
        // Hide the cursor over display windows in output mode.
        // When the master window is on-screen (interactive mode) the cursor
        // is handled by the CEF webview and appears normally there instead.
        SetCursor(NULL);
        return TRUE;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

ELECTROBUN_EXPORT HWND createNativeDisplayWindow(
    uint32_t displayWindowId, int x, int y, int width, int height) {
    return MainThreadDispatcher::dispatch_sync([=]() -> HWND {
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSA wc = {0};
            wc.lpfnWndProc   = DisplayWindowProc;
            wc.hInstance     = GetModuleHandle(NULL);
            wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            wc.lpszClassName = "NativeDisplayWindowClass";
            RegisterClassA(&wc);
            classRegistered = true;
        }
        // WS_EX_NOREDIRECTIONBITMAP: tell DWM not to allocate a GDI redirection
        HWND hwnd = CreateWindowExA(
            0,
            "NativeDisplayWindowClass",
            "",
            WS_POPUP | WS_VISIBLE,
            x, y, width, height,
            NULL, NULL, GetModuleHandle(NULL), NULL
        );
        if (hwnd) {
            g_displayWindows[displayWindowId] = hwnd;
            applyAppIcon(hwnd);
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }
        return hwnd;
    });
}

ELECTROBUN_EXPORT void setNativeDisplayWindowThumbnail(
    uint32_t displayWindowId, uint32_t sourceWindowId,
    double xNorm, double yNorm, double wNorm, double hNorm) {
    MainThreadDispatcher::dispatch_sync([=]() {
        auto destIt = g_displayWindows.find(displayWindowId);
        auto srcIt  = g_windowIdToHwnd.find(sourceWindowId);
        if (destIt == g_displayWindows.end() || srcIt == g_windowIdToHwnd.end()) {
            ::log("setNativeDisplayWindowThumbnail: window not found");
            return;
        }
        HWND hwndDest = destIt->second;
        HWND hwndSrc  = srcIt->second;

        // Unregister any existing thumbnail for this slot
        auto thumbIt = g_displayThumbnails.find(displayWindowId);
        if (thumbIt != g_displayThumbnails.end()) {
            DwmUnregisterThumbnail(thumbIt->second);
            g_displayThumbnails.erase(thumbIt);
        }

        HTHUMBNAIL thumb = NULL;
        HRESULT hr = DwmRegisterThumbnail(hwndDest, hwndSrc, &thumb);
        if (SUCCEEDED(hr)) {
            g_displayThumbnails[displayWindowId] = thumb;

            RECT destRect;
            GetClientRect(hwndDest, &destRect);

            // Use the physical pixel size of the source window's client area.
            // xNorm/yNorm/wNorm/hNorm are 0.0-1.0 fractions of the logical
            // canvas so we are DPI-agnostic.
            RECT srcClientRect;
            GetClientRect(hwndSrc, &srcClientRect);
            LONG physW = srcClientRect.right;
            LONG physH = srcClientRect.bottom;

            DWM_THUMBNAIL_PROPERTIES props = {};
            props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION |
                            DWM_TNP_RECTSOURCE | DWM_TNP_SOURCECLIENTAREAONLY;
            props.fVisible              = TRUE;
            props.fSourceClientAreaOnly = TRUE;
            props.rcDestination         = destRect;
            props.rcSource = {
                (LONG)(xNorm           * physW),
                (LONG)(yNorm           * physH),
                (LONG)((xNorm + wNorm) * physW),
                (LONG)((yNorm + hNorm) * physH),
            };
            DwmUpdateThumbnailProperties(thumb, &props);
        } else {
            char msg[128];
            sprintf_s(msg, "DwmRegisterThumbnail failed: 0x%08lX", hr);
            ::log(msg);
        }
    });
}

ELECTROBUN_EXPORT void destroyNativeDisplayWindow(uint32_t displayWindowId) {
    MainThreadDispatcher::dispatch_sync([=]() {
        auto thumbIt = g_displayThumbnails.find(displayWindowId);
        if (thumbIt != g_displayThumbnails.end()) {
            DwmUnregisterThumbnail(thumbIt->second);
            g_displayThumbnails.erase(thumbIt);
        }
        auto winIt = g_displayWindows.find(displayWindowId);
        if (winIt != g_displayWindows.end()) {
            DestroyWindow(winIt->second);
            g_displayWindows.erase(winIt);
        }
    });
}

ELECTROBUN_EXPORT void setNativeDisplayWindowVisible(
    uint32_t displayWindowId, bool visible) {
    MainThreadDispatcher::dispatch_sync([=]() {
        auto it = g_displayWindows.find(displayWindowId);
        if (it == g_displayWindows.end()) return;
        ShowWindow(it->second, visible ? SW_SHOW : SW_HIDE);
    });
}

ELECTROBUN_EXPORT void setNativeDisplayWindowAlwaysOnTop(
    uint32_t displayWindowId, bool alwaysOnTop) {
    MainThreadDispatcher::dispatch_sync([=]() {
        auto it = g_displayWindows.find(displayWindowId);
        if (it == g_displayWindows.end()) return;
        SetWindowPos(it->second,
            alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    });
}

ELECTROBUN_EXPORT void setNativeDisplayWindowFullScreen(
    uint32_t displayWindowId, bool fullscreen) {
    MainThreadDispatcher::dispatch_sync([=]() {
        auto it = g_displayWindows.find(displayWindowId);
        if (it == g_displayWindows.end()) return;
        HWND hwnd = it->second;
        if (fullscreen) {
            HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfo(monitor, &mi);
            SetWindowPos(hwnd, HWND_TOPMOST,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right  - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_FRAMECHANGED);
        }
    });
}

// ---------------------------------------------------------------------------
// DimOverlay — semi-transparent black Win32 window placed above a BrowserWindow
// to visually indicate "output mode" without affecting DWM thumbnail content.
//
// The overlay is a separate window (not the master), so DwmRegisterThumbnail
// reads the master's raw DirectX surface — the NDW output remains full-brightness
// regardless of the overlay alpha. WS_EX_TRANSPARENT makes clicks pass through.
// ---------------------------------------------------------------------------

static HWND g_dimOverlayHwnd = nullptr;

static LRESULT CALLBACK DimOverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

ELECTROBUN_EXPORT void createMasterDimOverlay(uint32_t sourceWindowId, uint8_t alpha) {
    MainThreadDispatcher::dispatch_sync([=]() {
        if (g_dimOverlayHwnd) return; // already created

        auto it = g_windowIdToHwnd.find(sourceWindowId);
        if (it == g_windowIdToHwnd.end()) {
            ::log("createMasterDimOverlay: source window not found");
            return;
        }
        HWND hwndMaster = it->second;

        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSA wc = {};
            wc.lpfnWndProc   = DimOverlayProc;
            wc.hInstance     = GetModuleHandle(NULL);
            wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            wc.lpszClassName = "DimOverlayClass";
            RegisterClassA(&wc);
            classRegistered = true;
        }

        RECT rc;
        GetWindowRect(hwndMaster, &rc);

        g_dimOverlayHwnd = CreateWindowExA(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            "DimOverlayClass", "",
            WS_POPUP,
            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
            NULL, NULL, GetModuleHandle(NULL), NULL
        );

        if (!g_dimOverlayHwnd) {
            ::log("createMasterDimOverlay: CreateWindowEx failed");
            return;
        }

        // Black fill + global alpha = dim. HWND_TOP keeps overlay above master
        // but below HWND_TOPMOST NativeDisplayWindows.
        SetLayeredWindowAttributes(g_dimOverlayHwnd, 0, alpha, LWA_ALPHA);
        SetWindowPos(g_dimOverlayHwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    });
}

ELECTROBUN_EXPORT void setMasterDimOverlayVisible(bool visible) {
    MainThreadDispatcher::dispatch_sync([=]() {
        if (!g_dimOverlayHwnd) return;
        ShowWindow(g_dimOverlayHwnd, visible ? SW_SHOW : SW_HIDE);
    });
}

ELECTROBUN_EXPORT void destroyMasterDimOverlay() {
    MainThreadDispatcher::dispatch_sync([=]() {
        if (g_dimOverlayHwnd) {
            DestroyWindow(g_dimOverlayHwnd);
            g_dimOverlayHwnd = nullptr;
        }
    });
}

// ---------------------------------------------------------------------------
// D3D output API — multi-window via GPU blit (replaces DWM thumbnails)
// ---------------------------------------------------------------------------

// Initialise D3D11 for D3D output mode.
//
// D3D output reuses the SpoutWindowState device path — the SAME code that
// startSpoutSender uses — because OpenSharedResource1 in OnAcceleratedPaint
// requires a device initialised through that path to reliably open the DXGI
// NT shared handle from ANGLE. A freshly created independent device fails with
// DXGI_ERROR_DEVICE_REMOVED (0x887A0005) on the first OpenSharedResource1 call.
//
// The BrowserWindow must have been created with spout:true (OSR mode).
// Then call addD3DOutputSlot() for each NativeDisplayWindow.
ELECTROBUN_EXPORT bool startD3DOutput(uint32_t webviewId) {
    auto it = g_spoutWindows.find(webviewId);
    if (it == g_spoutWindows.end()) {
        ::log("D3DOutput: no SpoutWindowState for webviewId " + std::to_string(webviewId) +
              " (did you create the BrowserWindow with spout:true?)");
        return false;
    }
    auto& state = it->second;
    if (!state.active) {
        // No Spout sender active — create our own D3D11 device for NDW blitting.
        D3D_FEATURE_LEVEL featureLevel;
        ID3D11DeviceContext* context = nullptr;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            &state.d3dDevice, &featureLevel, &context);
        if (FAILED(hr)) {
            char buf[16]; sprintf_s(buf, "%08X", (UINT)hr);
            ::log("D3DOutput: D3D11CreateDevice failed hr=0x" + std::string(buf));
            return false;
        }
        state.d3dContext = context;

        // Create swap chain on master HWND for DWM surface (same as startSpoutSender).
        state.swapChain = createSwapChainForHwnd(state.d3dDevice, state.hwnd, state.width, state.height);

        // Mark active — OnAcceleratedPaint will now open sharedTex and execute the D3D output block.
        // state.sender is null, so SendTexture is skipped.
        state.active = true;

        // Log which GPU was selected.
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(state.d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc = {};
                adapter->GetDesc(&desc);
                char name128[128] = {};
                WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, name128, sizeof(name128), nullptr, nullptr);
                ::log("D3DOutput: D3D11 device on: " + std::string(name128));
                adapter->Release();
            }
            dxgiDev->Release();
        }
    } else {
        // Spout sender already initialized the D3D device — reuse it for NDW blitting.
        // state.sender != nullptr confirms this is the Spout coexistence path (not an unexpected double-call).
        if (state.sender)
            ::log("D3DOutput: reusing Spout D3D11 device for webviewId " + std::to_string(webviewId));
        else
            ::log("D3DOutput: startD3DOutput called while already active (unexpected) — reinitializing slot list only");
    }

    // Tell CEF to keep rendering even when the host HWND is hidden.
    // Without this, ShowWindow(SW_HIDE) on the master HWND causes CEF's compositor
    // to pause OnAcceleratedPaint delivery.
    for (auto& kv : browserToWebviewMap) {
        if (kv.second == webviewId) {
            auto bIt = g_cefBrowsers.find(kv.first);
            if (bIt != g_cefBrowsers.end() && bIt->second)
                bIt->second->GetHost()->WasHidden(false);
            break;
        }
    }

    // Create empty slot list for this webviewId.
    g_d3dOutputStates[webviewId] = D3DOutputState{};

    ::log("D3DOutput: started for webviewId " + std::to_string(webviewId));
    return true;
}

// Register a NativeDisplayWindow slot for D3D output.
// hwnd is resolved from g_displayWindows[displayWindowId].
// The swap chain is sized to srcW×srcH (the NDW's client area).
// Must be called after startD3DOutput() and createNativeDisplayWindow().
ELECTROBUN_EXPORT bool addD3DOutputSlot(uint32_t webviewId, uint32_t displayWindowId,
                                         int srcX, int srcY, int srcW, int srcH) {
    // Get the D3D11 device from the SpoutWindowState (populated by startD3DOutput).
    auto spoutIt = g_spoutWindows.find(webviewId);
    if (spoutIt == g_spoutWindows.end() || !spoutIt->second.d3dDevice) {
        ::log("D3DOutput: no D3D11 device for webviewId " + std::to_string(webviewId));
        return false;
    }

    auto itD3D = g_d3dOutputStates.find(webviewId);
    if (itD3D == g_d3dOutputStates.end()) {
        ::log("D3DOutput: no slot list for webviewId " + std::to_string(webviewId));
        return false;
    }

    auto hwndIt = g_displayWindows.find(displayWindowId);
    if (hwndIt == g_displayWindows.end()) {
        ::log("D3DOutput: no HWND for displayWindowId " + std::to_string(displayWindowId));
        return false;
    }
    HWND hwnd = hwndIt->second;

    IDXGISwapChain1* swapChain = createSwapChainForHwnd(spoutIt->second.d3dDevice, hwnd, srcW, srcH);
    if (!swapChain) {
        ::log("D3DOutput: swap chain creation failed for displayWindowId " + std::to_string(displayWindowId));
        return false;
    }

    D3DOutputSlot slot;
    slot.displayWindowId = displayWindowId;
    slot.swapChain       = swapChain;
    slot.sourceX         = srcX;
    slot.sourceY         = srcY;
    slot.sourceW         = srcW;
    slot.sourceH         = srcH;
    itD3D->second.slots.push_back(slot);

    ::log("D3DOutput: slot added displayWindowId=" + std::to_string(displayWindowId) +
          " src=(" + std::to_string(srcX) + "," + std::to_string(srcY) +
          " " + std::to_string(srcW) + "x" + std::to_string(srcH) + ")");
    return true;
}

// Stop D3D output, release all NDW swap chains, and tear down the SpoutWindowState device.
ELECTROBUN_EXPORT void stopD3DOutput(uint32_t webviewId) {
    // Release NDW swap chains.
    auto itD3D = g_d3dOutputStates.find(webviewId);
    if (itD3D != g_d3dOutputStates.end()) {
        for (auto& slot : itD3D->second.slots) {
            if (slot.swapChain) { slot.swapChain->Release(); slot.swapChain = nullptr; }
        }
        g_d3dOutputStates.erase(itD3D);
    }

    // Release the SpoutWindowState device, but only if Spout is NOT also active.
    // When both Spout and D3D output are running, Spout owns state.d3dDevice/swapChain/context
    // and will release them when stopSpoutSender is called. Releasing here would invalidate
    // the Spout sender mid-session.
    auto spoutIt = g_spoutWindows.find(webviewId);
    if (spoutIt != g_spoutWindows.end()) {
        auto& state = spoutIt->second;
        state.active = false;
        if (!state.sender) {
            // D3D-output-only mode: we own the device, release it.
            if (state.swapChain) { state.swapChain->Release(); state.swapChain = nullptr; }
            if (state.d3dContext) { state.d3dContext->Release(); state.d3dContext = nullptr; }
            if (state.d3dDevice)  { state.d3dDevice->Release();  state.d3dDevice  = nullptr; }
        }
        // else: Spout owns the device; stopSpoutSender will release it.
    }

    ::log("D3DOutput: stopped for webviewId " + std::to_string(webviewId));
}

// ---------------------------------------------------------------------------
// Spout GPU texture sender API
// ---------------------------------------------------------------------------

// Start sending the browser's rendered content as a Spout sender.
// The browser must have been created with spout:true (OSR mode + shared_texture_enabled=1).
// OnAcceleratedPaint delivers each frame's DXGI NT shared texture handle; this function
// initialises D3D11 + SpoutDX so OnAcceleratedPaint can forward frames immediately.
// webviewId: the webviewId of the BrowserWindow created with spout:true.
// senderName: Spout sender name visible to receivers (TouchDesigner, Resolume, etc.).
// Returns true if the sender was successfully started.
ELECTROBUN_EXPORT bool startSpoutSender(uint32_t webviewId, const char* senderName) {
    auto it = g_spoutWindows.find(webviewId);
    if (it == g_spoutWindows.end()) {
        ::log("Spout: no state for webviewId " + std::to_string(webviewId));
        ::log("  (Did you create the BrowserWindow with spout:true?)");
        return false;
    }
    auto& state = it->second;

    if (!state.hwnd || !IsWindow(state.hwnd)) {
        ::log("Spout: invalid HWND for webviewId " + std::to_string(webviewId));
        return false;
    }

#if ELECTROBUN_HAS_SPOUT
    // Create the D3D11 device. OnAcceleratedPaint uses this to open the DXGI NT shared
    // handle that CEF's GPU compositor writes each frame to (OpenSharedResource1).
    D3D_FEATURE_LEVEL featureLevel;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &state.d3dDevice, &featureLevel, &context);
    if (FAILED(hr)) {
        auto fmtHr = [hr]{ char buf[16]; sprintf_s(buf, "%08X", (UINT)hr); return std::string(buf); };
        ::log("Spout: D3D11CreateDevice failed hr=0x" + fmtHr());
        return false;
    }
    state.d3dContext = context;

    // Create a DXGI swap chain on the master HWND for DWM thumbnailing.
    // In OSR mode CEF doesn't present to the HWND; we Present() from OnAcceleratedPaint
    // so the NativeDisplayWindow DWM thumbnails keep working.
    state.swapChain = createSwapChainForHwnd(state.d3dDevice, state.hwnd, state.width, state.height);
    if (!state.swapChain) {
        ::log("Spout: warning — swap chain creation failed; DWM thumbnails may be blank");
        // Non-fatal: Spout still works, NDW mirrors just won't show content.
    }

    // Initialise SpoutDX on the same D3D11 device so SendTexture works without a copy.
    const char* name = senderName ? senderName : "ElectrobunSpout";
    state.sender = new spoutDX();
    if (!state.sender->OpenDirectX11(state.d3dDevice)) {
        ::log("Spout: OpenDirectX11 failed");
        delete state.sender; state.sender = nullptr;
        if (state.swapChain) { state.swapChain->Release(); state.swapChain = nullptr; }
        state.d3dContext->Release(); state.d3dContext = nullptr;
        state.d3dDevice->Release(); state.d3dDevice = nullptr;
        return false;
    }
    if (!state.sender->SetSenderName(name)) {
        ::log("Spout: SetSenderName failed");
        delete state.sender; state.sender = nullptr;
        if (state.swapChain) { state.swapChain->Release(); state.swapChain = nullptr; }
        state.d3dContext->Release(); state.d3dContext = nullptr;
        state.d3dDevice->Release(); state.d3dDevice = nullptr;
        return false;
    }

    // Mark active — OnAcceleratedPaint will start forwarding frames immediately.
    state.active = true;

    // Log D3D11 adapter name so we know which GPU is being used for SpoutDX.
    {
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(state.d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc = {};
                adapter->GetDesc(&desc);
                char name128[128] = {};
                WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, name128, sizeof(name128), nullptr, nullptr);
                ::log("Spout D3D11 device on: " + std::string(name128));
                adapter->Release();
            }
            dxgiDev->Release();
        }
    }

    ::log("Spout OSR sender started: " + std::string(name));
    return true;
#else
    (void)senderName;
    ::log("Spout: built without SpoutDX headers — Spout output not available");
    return false;
#endif
}

// Stop the Spout sender for the given webviewId.
ELECTROBUN_EXPORT void stopSpoutSender(uint32_t webviewId) {
    auto it = g_spoutWindows.find(webviewId);
    if (it == g_spoutWindows.end()) return;
    auto& state = it->second;

    // Disable first so OnAcceleratedPaint stops forwarding frames.
    state.active = false;

#if ELECTROBUN_HAS_SPOUT
    if (state.sender) {
        state.sender->ReleaseSender();
        delete state.sender;
        state.sender = nullptr;
    }
    if (state.swapChain) { state.swapChain->Release(); state.swapChain = nullptr; }
    if (state.d3dContext) { state.d3dContext->Release(); state.d3dContext = nullptr; }
    if (state.d3dDevice)  { state.d3dDevice->Release();  state.d3dDevice  = nullptr; }
#endif

    ::log("Spout OSR sender stopped for webviewId " + std::to_string(webviewId));
}

// ---------------------------------------------------------------------------
// Spout input receiver exports
// ---------------------------------------------------------------------------

ELECTROBUN_EXPORT uint32_t startSpoutReceiver(const char* senderName) {
#if ELECTROBUN_HAS_SPOUT
    auto* state = new SpoutInputState();
    state->senderName = senderName ? senderName : "";

    // Prefer the NVIDIA (or first discrete) adapter to avoid cross-adapter copies.
    // With D3D_DRIVER_TYPE_HARDWARE + nullptr adapter, Windows picks the primary/default
    // adapter (Intel on Optimus laptops), which would cause PCIe cross-adapter copies for
    // every Spout frame from an NVIDIA sender. Enumerate adapters and pick NVIDIA first.
    IDXGIFactory1* dxgiFactory = nullptr;
    IDXGIAdapter* preferredAdapter = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgiFactory))) {
        IDXGIAdapter* adapter = nullptr;
        for (UINT i = 0; dxgiFactory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC desc = {};
            adapter->GetDesc(&desc);
            if (desc.VendorId == 0x10DE) { // NVIDIA
                preferredAdapter = adapter;
                break;
            }
            adapter->Release();
            adapter = nullptr;
        }
        dxgiFactory->Release();
    }

    D3D_FEATURE_LEVEL fl; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(
        preferredAdapter,
        preferredAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &state->d3dDevice, &fl, &ctx);
    if (FAILED(hr)) { if (preferredAdapter) preferredAdapter->Release(); delete state; return 0; }
    // Log which adapter was selected for the receiver device.
    {
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(state->d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&ad))) {
                DXGI_ADAPTER_DESC desc = {};
                ad->GetDesc(&desc);
                char adName[256] = {};
                wcstombs(adName, desc.Description, sizeof(adName) - 1);
                ::log("Spout receiver D3D11 device on: " + std::string(adName));
                ad->Release();
            }
            dxgiDev->Release();
        }
    }
    state->d3dContext = ctx;

    state->receiver = new spoutDX();
    state->receiver->OpenDirectX11(state->d3dDevice);
    if (!state->senderName.empty())
        state->receiver->SetReceiverName(state->senderName.c_str());

    uint32_t id = g_nextReceiverId++;

    // Create Win32 named file mapping accessible to all processes in this session.
    // The CEF renderer process opens it by name in OnContextCreated.
    std::string mappingName = "SpoutFrame_" + std::to_string(id);
    DWORD hiSize = (DWORD)(SpoutInputState::kMappedSize >> 32);
    DWORD loSize = (DWORD)(SpoutInputState::kMappedSize & 0xFFFFFFFF);
    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
        PAGE_READWRITE, hiSize, loSize, mappingName.c_str());
    if (!hMap) { delete state; return 0; }
    void* ptr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, SpoutInputState::kMappedSize);
    if (!ptr) { CloseHandle(hMap); delete state; return 0; }
    memset(ptr, 0, SpoutInputState::kMappedSize); // zero-init: seq=0, w=0, h=0
    state->hFileMapping = hMap;
    state->mappedPtr    = ptr;
    state->mappingName  = mappingName;

    g_spoutReceivers[id] = state;
    state->active = true;
    state->recvThread = std::thread(spoutReceiverThreadFn, state);
    ::log("Spout receiver started: '" + state->senderName + "' id=" + std::to_string(id) +
          " mapping=" + mappingName);
    return id;
#else
    return 0;
#endif
}

ELECTROBUN_EXPORT void stopSpoutReceiver(uint32_t receiverId) {
    auto it = g_spoutReceivers.find(receiverId);
    if (it == g_spoutReceivers.end()) return;
    auto* state = it->second;
    state->active = false;
    if (state->recvThread.joinable()) state->recvThread.join();
#if ELECTROBUN_HAS_SPOUT
    if (state->receiver) { state->receiver->ReleaseReceiver(); delete state->receiver; }
#endif
    if (state->stagingTex)   state->stagingTex->Release();
    if (state->d3dContext)   state->d3dContext->Release();
    if (state->d3dDevice)    state->d3dDevice->Release();
    if (state->mappedPtr)    { UnmapViewOfFile(state->mappedPtr); state->mappedPtr = nullptr; }
    if (state->hFileMapping) { CloseHandle(state->hFileMapping);  state->hFileMapping = NULL; }
    delete state;
    g_spoutReceivers.erase(it);
    ::log("Spout receiver stopped: id=" + std::to_string(receiverId));
}

// Returns the Win32 named file mapping name for a receiver.
// The CEF renderer process opens this mapping by name in OnContextCreated and
// exposes it to JavaScript as window.__spoutFrameBuffer (an ArrayBuffer).
// Layout: [0-3] seq(Int32) | [4-7] width(Uint32) | [8-11] height(Uint32)
//         | [12-15] reserved | [16+] BGRA pixels
ELECTROBUN_EXPORT const char* getSpoutReceiverMappingName(uint32_t receiverId) {
    auto it = g_spoutReceivers.find(receiverId);
    if (it == g_spoutReceivers.end()) return "";
    return it->second->mappingName.c_str();
}

// Copies the latest Spout input frame from shared memory into outBuf.
// Called from Bun (browser process) to serve frame data via HTTP to the renderer.
// Returns the number of bytes written (header + pixels), or 0 if no frame available.
// outBuf must be at least SpoutInputState::kMappedSize bytes.
ELECTROBUN_EXPORT int spoutReadFrame(uint32_t receiverId, uint8_t* outBuf, uint32_t bufSize) {
    auto it = g_spoutReceivers.find(receiverId);
    if (it == g_spoutReceivers.end() || !it->second || !it->second->mappedPtr) return 0;
    uint8_t* src = (uint8_t*)it->second->mappedPtr;
    uint32_t w = *(uint32_t*)(src + 4);
    uint32_t h = *(uint32_t*)(src + 8);
    if (w == 0 || h == 0) return 0;
    uint32_t dataBytes = 16 + w * h * 4;
    if (dataBytes > bufSize) return 0;
    memcpy(outBuf, src, dataBytes);
    return (int)dataBytes;
}

// Returns the current seq counter from shared memory without copying pixel data.
// Used by the Bun poll loop to detect new frames cheaply before calling spoutReadFrame.
// Returns -1 if the receiver is not found or not initialized.
ELECTROBUN_EXPORT int32_t getSpoutReceiverSeq(uint32_t receiverId) {
    auto it = g_spoutReceivers.find(receiverId);
    if (it == g_spoutReceivers.end() || !it->second || !it->second->mappedPtr) return -1;
    return *(volatile int32_t*)it->second->mappedPtr;
}

// ---------------------------------------------------------------------------

ELECTROBUN_EXPORT void setWindowVisibleOnAllWorkspaces(NSWindow *window, bool visible) {
    // Not applicable on Windows - no-op
}

ELECTROBUN_EXPORT bool isWindowVisibleOnAllWorkspaces(NSWindow *window) {
    // Not applicable on Windows
    return false;
}

ELECTROBUN_EXPORT void setWindowPosition(NSWindow *window, double x, double y) {
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!IsWindow(hwnd)) return;

    SetWindowPos(hwnd, NULL, (int)x, (int)y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

ELECTROBUN_EXPORT void setWindowSize(NSWindow *window, double width, double height) {
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!IsWindow(hwnd)) return;

    SetWindowPos(hwnd, NULL, 0, 0, (int)width, (int)height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

ELECTROBUN_EXPORT void setWindowFrame(NSWindow *window, double x, double y, double width, double height) {
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!IsWindow(hwnd)) return;

    SetWindowPos(hwnd, NULL, (int)x, (int)y, (int)width, (int)height, SWP_NOZORDER | SWP_NOACTIVATE);
}

ELECTROBUN_EXPORT void getWindowFrame(NSWindow *window, double *outX, double *outY, double *outWidth, double *outHeight) {
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!IsWindow(hwnd)) {
        *outX = 0;
        *outY = 0;
        *outWidth = 0;
        *outHeight = 0;
        return;
    }

    RECT rect;
    GetWindowRect(hwnd, &rect);
    *outX = (double)rect.left;
    *outY = (double)rect.top;
    *outWidth = (double)(rect.right - rect.left);
    *outHeight = (double)(rect.bottom - rect.top);
}

ELECTROBUN_EXPORT void resizeWebview(AbstractView *abstractView, double x, double y, double width, double height, const char *masksJson) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView in resizeWebview");
        return;
    }
    
    
    RECT bounds = {(LONG)x, (LONG)y, (LONG)(x + width), (LONG)(y + height)};
    abstractView->storePendingResize(bounds, masksJson);
    g_pendingResizeQueue.enqueue(abstractView);
    schedulePendingResizeDrain();
}

// Internal function to stop window movement (without export linkage)



ELECTROBUN_EXPORT void stopWindowMove() {
    if (g_isMovingWindow) {
        // Unregister raw input device
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x02;
        rid.dwFlags = RIDEV_REMOVE;
        rid.hwndTarget = NULL;
        
        RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));
        g_isMovingWindow = FALSE;
        g_targetWindow = NULL;
    }
}

ELECTROBUN_EXPORT void startWindowMove(NSWindow *window) {
    // On Windows, NSWindow* is actually HWND
    HWND hwnd = reinterpret_cast<HWND>(window);
    
    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in startWindowMove");
        return;
    }
    
    // Set up window dragging state
    g_targetWindow = hwnd;
    g_isMovingWindow = TRUE;
    
    // Get initial cursor and window positions
    GetCursorPos(&g_initialCursorPos);
    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);
    g_initialWindowPos.x = windowRect.left;
    g_initialWindowPos.y = windowRect.top;
    
    // Register for raw mouse input to bypass CEF event consumption
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;  // HID_USAGE_PAGE_GENERIC
    rid.usUsage = 0x02;      // HID_USAGE_GENERIC_MOUSE
    rid.dwFlags = RIDEV_INPUTSINK; // Receive input even when not in foreground
    rid.hwndTarget = hwnd;   // Send messages to our window
    
    if (!RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE))) {
        ::log("ERROR: Failed to register raw input device - error: " + std::to_string(GetLastError()));
        g_isMovingWindow = FALSE;
        g_targetWindow = NULL;
    }
}

ELECTROBUN_EXPORT BOOL moveToTrash(char *pathString) {
    if (!pathString) {
        ::log("ERROR: NULL path string passed to moveToTrash");
        return FALSE;
    }
    
    // Convert to wide string for Windows API
    int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, pathString, -1, NULL, 0);
    if (wideCharLen == 0) {
        ::log("ERROR: Failed to convert path to wide string");
        return FALSE;
    }
    
    std::vector<wchar_t> widePath(wideCharLen + 1);  // +1 for double null terminator
    MultiByteToWideChar(CP_UTF8, 0, pathString, -1, widePath.data(), wideCharLen);
    widePath[wideCharLen] = L'\0';  // Ensure double null termination
    
    // Use SHFileOperation to move to recycle bin
    SHFILEOPSTRUCTW fileOp = {};
    fileOp.hwnd = NULL;
    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = widePath.data();
    fileOp.pTo = NULL;
    fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    fileOp.fAnyOperationsAborted = FALSE;
    fileOp.hNameMappings = NULL;
    fileOp.lpszProgressTitle = NULL;
    
    int result = SHFileOperationW(&fileOp);
    
    if (result == 0 && !fileOp.fAnyOperationsAborted) {
        ::log("Successfully moved to trash: " + std::string(pathString));
        return TRUE;
    } else {
        ::log("ERROR: Failed to move to trash: " + std::string(pathString) + " (error code: " + std::to_string(result) + ")");
        return FALSE;
    }
}

ELECTROBUN_EXPORT void showItemInFolder(char *path) {
    if (!path) {
        ::log("ERROR: NULL path passed to showItemInFolder");
        return;
    }
    
    std::string pathString(path);
    if (pathString.empty()) {
        ::log("ERROR: Empty path passed to showItemInFolder");
        return;
    }
    
    // Convert to wide string for Windows API
    int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wideCharLen == 0) {
        ::log("ERROR: Failed to convert path to wide string in showItemInFolder");
        return;
    }
    
    std::vector<wchar_t> widePath(wideCharLen);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath.data(), wideCharLen);
    
    // Use ShellExecute to open Explorer and select the file
    std::wstring selectParam = L"/select,\"" + std::wstring(widePath.data()) + L"\"";
    
    HINSTANCE result = ShellExecuteW(
        NULL,                    // parent window
        L"open",                 // operation
        L"explorer.exe",         // executable
        selectParam.c_str(),     // parameters
        NULL,                    // working directory
        SW_SHOWNORMAL           // show command
    );
    
    // Check if the operation was successful
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::log("ERROR: Failed to show item in folder: " + pathString + " (error code: " + std::to_string(reinterpret_cast<INT_PTR>(result)) + ")");
    } else {
        ::log("Successfully opened folder for: " + pathString);
    }
}

// Open a URL in the default browser or appropriate application
ELECTROBUN_EXPORT BOOL openExternal(const char *urlString) {
    if (!urlString) {
        ::log("ERROR: NULL URL passed to openExternal");
        return FALSE;
    }

    std::string url(urlString);
    if (url.empty()) {
        ::log("ERROR: Empty URL passed to openExternal");
        return FALSE;
    }

    // Convert to wide string for Windows API
    int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, urlString, -1, NULL, 0);
    if (wideCharLen == 0) {
        ::log("ERROR: Failed to convert URL to wide string");
        return FALSE;
    }

    std::vector<wchar_t> wideUrl(wideCharLen);
    MultiByteToWideChar(CP_UTF8, 0, urlString, -1, wideUrl.data(), wideCharLen);

    // Use ShellExecuteW to open the URL
    HINSTANCE result = ShellExecuteW(
        NULL,           // parent window
        L"open",        // operation
        wideUrl.data(), // URL to open
        NULL,           // parameters
        NULL,           // working directory
        SW_SHOWNORMAL   // show command
    );

    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::log("ERROR: Failed to open external URL: " + url + " (error code: " + std::to_string(reinterpret_cast<INT_PTR>(result)) + ")");
        return FALSE;
    }

    ::log("Successfully opened external URL: " + url);
    return TRUE;
}

// Open a file or folder with the default application
ELECTROBUN_EXPORT BOOL openPath(const char *pathString) {
    if (!pathString) {
        ::log("ERROR: NULL path passed to openPath");
        return FALSE;
    }

    std::string path(pathString);
    if (path.empty()) {
        ::log("ERROR: Empty path passed to openPath");
        return FALSE;
    }

    // Convert to wide string for Windows API
    int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, pathString, -1, NULL, 0);
    if (wideCharLen == 0) {
        ::log("ERROR: Failed to convert path to wide string");
        return FALSE;
    }

    std::vector<wchar_t> widePath(wideCharLen);
    MultiByteToWideChar(CP_UTF8, 0, pathString, -1, widePath.data(), wideCharLen);

    // Use ShellExecuteW to open the file/folder with default application
    HINSTANCE result = ShellExecuteW(
        NULL,            // parent window
        L"open",         // operation
        widePath.data(), // file/folder to open
        NULL,            // parameters
        NULL,            // working directory
        SW_SHOWNORMAL    // show command
    );

    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::log("ERROR: Failed to open path: " + path + " (error code: " + std::to_string(reinterpret_cast<INT_PTR>(result)) + ")");
        return FALSE;
    }

    ::log("Successfully opened path: " + path);
    return TRUE;
}

// Show a native desktop notification using Shell_NotifyIcon balloon
ELECTROBUN_EXPORT void showNotification(const char *title, const char *body, const char *subtitle, BOOL silent) {
    if (!title) {
        ::log("ERROR: NULL title passed to showNotification");
        return;
    }

    // Convert strings to wide chars
    int titleLen = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
    std::vector<wchar_t> wideTitle(titleLen);
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle.data(), titleLen);

    std::wstring wideBody;
    if (body) {
        int bodyLen = MultiByteToWideChar(CP_UTF8, 0, body, -1, NULL, 0);
        std::vector<wchar_t> bodyBuf(bodyLen);
        MultiByteToWideChar(CP_UTF8, 0, body, -1, bodyBuf.data(), bodyLen);
        wideBody = bodyBuf.data();
    }

    // If subtitle is provided, prepend it to body
    if (subtitle) {
        int subtitleLen = MultiByteToWideChar(CP_UTF8, 0, subtitle, -1, NULL, 0);
        std::vector<wchar_t> subtitleBuf(subtitleLen);
        MultiByteToWideChar(CP_UTF8, 0, subtitle, -1, subtitleBuf.data(), subtitleLen);
        if (!wideBody.empty()) {
            wideBody = std::wstring(subtitleBuf.data()) + L"\n" + wideBody;
        } else {
            wideBody = subtitleBuf.data();
        }
    }

    // Create notification icon data
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = NULL;  // No window handle needed for balloon
    nid.uID = 1;
    nid.uFlags = NIF_INFO | NIF_ICON;
    nid.dwInfoFlags = NIIF_INFO | (silent ? NIIF_NOSOUND : 0);

    // Copy title (max 63 chars)
    wcsncpy_s(nid.szInfoTitle, wideTitle.data(), _TRUNCATE);

    // Copy body (max 255 chars)
    if (!wideBody.empty()) {
        wcsncpy_s(nid.szInfo, wideBody.c_str(), _TRUNCATE);
    }

    // Use app icon or default
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    // Add the notification icon (required before showing balloon)
    Shell_NotifyIconW(NIM_ADD, &nid);

    // Show the balloon notification
    Shell_NotifyIconW(NIM_MODIFY, &nid);

    // Remove the icon after a delay (fire and forget - icon will be cleaned up)
    // Note: In a real app, you might want to keep the icon around
    // For now, we schedule removal after notification timeout
    std::thread([nid]() mutable {
        Sleep(5000);  // Wait for notification to be shown
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }).detach();

    ::log("Notification shown: " + std::string(title));
}

ELECTROBUN_EXPORT const char* openFileDialog(const char *startingFolder,
                          const char *allowedFileTypes,
                          BOOL canChooseFiles,
                          BOOL canChooseDirectories,
                          BOOL allowsMultipleSelection) {
    if (!canChooseFiles && !canChooseDirectories) {
        ::log("ERROR: Both canChooseFiles and canChooseDirectories are false");
        return nullptr;
    }
    
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        ::log("ERROR: Failed to initialize COM");
        return nullptr;
    }
    
    IFileOpenDialog *pFileDialog = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, (void**)&pFileDialog);
    if (FAILED(hr)) {
        ::log("ERROR: Failed to create file dialog");
        CoUninitialize();
        return nullptr;
    }
    
    // Set dialog options
    DWORD dwFlags = 0;
    pFileDialog->GetOptions(&dwFlags);
    
    if (canChooseDirectories) {
        dwFlags |= FOS_PICKFOLDERS;
    }
    if (allowsMultipleSelection) {
        dwFlags |= FOS_ALLOWMULTISELECT;
    }
    if (!canChooseFiles) {
        dwFlags |= FOS_PICKFOLDERS;
    }
    
    pFileDialog->SetOptions(dwFlags);
    
    // Set starting folder
    if (startingFolder && strlen(startingFolder) > 0) {
        int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, startingFolder, -1, nullptr, 0);
        if (wideCharLen > 0) {
            std::vector<wchar_t> wideStartingFolder(wideCharLen);
            MultiByteToWideChar(CP_UTF8, 0, startingFolder, -1, wideStartingFolder.data(), wideCharLen);
            
            IShellItem *pStartingFolder = nullptr;
            hr = SHCreateItemFromParsingName(wideStartingFolder.data(), nullptr, IID_IShellItem, (void**)&pStartingFolder);
            if (SUCCEEDED(hr)) {
                pFileDialog->SetFolder(pStartingFolder);
                pStartingFolder->Release();
            }
        }
    }
    
    // Set file type filters
    if (allowedFileTypes && strlen(allowedFileTypes) > 0 && strcmp(allowedFileTypes, "*") != 0) {
        std::string typesStr(allowedFileTypes);
        std::vector<std::string> extensions;
        std::stringstream ss(typesStr);
        std::string extension;
        
        while (std::getline(ss, extension, ',')) {
            // Trim whitespace
            extension.erase(0, extension.find_first_not_of(" \t"));
            extension.erase(extension.find_last_not_of(" \t") + 1);
            if (!extension.empty()) {
                extensions.push_back(extension);
            }
        }
        
        if (!extensions.empty()) {
            // Create filter specification
            std::vector<COMDLG_FILTERSPEC> filterSpecs;
            std::vector<std::wstring> filterNames;
            std::vector<std::wstring> filterPatterns;
            
            for (const auto& ext : extensions) {
                std::wstring wExt = std::wstring(ext.begin(), ext.end());
                if (wExt.find(L".") != 0) {
                    wExt = L"." + wExt;
                }
                std::wstring pattern = L"*" + wExt;
                std::wstring name = wExt.substr(1) + L" files";
                
                filterNames.push_back(name);
                filterPatterns.push_back(pattern);
                
                COMDLG_FILTERSPEC spec;
                spec.pszName = filterNames.back().c_str();
                spec.pszSpec = filterPatterns.back().c_str();
                filterSpecs.push_back(spec);
            }
            
            pFileDialog->SetFileTypes(static_cast<UINT>(filterSpecs.size()), filterSpecs.data());
        }
    }
    
    // Show the dialog
    hr = pFileDialog->Show(nullptr);
    std::string result;
    
    if (SUCCEEDED(hr)) {
        if (allowsMultipleSelection) {
            IShellItemArray *pShellItemArray = nullptr;
            hr = pFileDialog->GetResults(&pShellItemArray);
            if (SUCCEEDED(hr)) {
                DWORD itemCount = 0;
                pShellItemArray->GetCount(&itemCount);
                
                std::vector<std::string> paths;
                for (DWORD i = 0; i < itemCount; i++) {
                    IShellItem *pShellItem = nullptr;
                    hr = pShellItemArray->GetItemAt(i, &pShellItem);
                    if (SUCCEEDED(hr)) {
                        PWSTR pszPath = nullptr;
                        hr = pShellItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
                        if (SUCCEEDED(hr)) {
                            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, nullptr, 0, nullptr, nullptr);
                            if (utf8Len > 0) {
                                std::vector<char> utf8Path(utf8Len);
                                WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, utf8Path.data(), utf8Len, nullptr, nullptr);
                                paths.push_back(std::string(utf8Path.data()));
                            }
                            CoTaskMemFree(pszPath);
                        }
                        pShellItem->Release();
                    }
                }
                pShellItemArray->Release();
                
                // Join paths with comma
                for (size_t i = 0; i < paths.size(); i++) {
                    if (i > 0) result += ",";
                    result += paths[i];
                }
            }
        } else {
            IShellItem *pShellItem = nullptr;
            hr = pFileDialog->GetResult(&pShellItem);
            if (SUCCEEDED(hr)) {
                PWSTR pszPath = nullptr;
                hr = pShellItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
                if (SUCCEEDED(hr)) {
                    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, nullptr, 0, nullptr, nullptr);
                    if (utf8Len > 0) {
                        std::vector<char> utf8Path(utf8Len);
                        WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, utf8Path.data(), utf8Len, nullptr, nullptr);
                        result = std::string(utf8Path.data());
                    }
                    CoTaskMemFree(pszPath);
                }
                pShellItem->Release();
            }
        }
    }
    
    pFileDialog->Release();
    CoUninitialize();
    
    if (result.empty()) {
        ::log("File dialog cancelled or no selection made");
        return nullptr;
    }
    
    return strdup(result.c_str());
}

ELECTROBUN_EXPORT int showMessageBox(const char *type,
                                     const char *title,
                                     const char *message,
                                     const char *detail,
                                     const char *buttons,
                                     int defaultId,
                                     int cancelId) {
    return MainThreadDispatcher::dispatch_sync([=]() -> int {
        // Convert strings to wide
        std::wstring wTitle, wMessage;
        if (title && strlen(title) > 0) {
            int len = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
            wTitle.resize(len - 1);
            MultiByteToWideChar(CP_UTF8, 0, title, -1, &wTitle[0], len);
        }

        // Combine message and detail
        std::string fullMsg;
        if (message && strlen(message) > 0) {
            fullMsg = message;
        }
        if (detail && strlen(detail) > 0) {
            if (!fullMsg.empty()) fullMsg += "\n\n";
            fullMsg += detail;
        }
        if (!fullMsg.empty()) {
            int len = MultiByteToWideChar(CP_UTF8, 0, fullMsg.c_str(), -1, nullptr, 0);
            wMessage.resize(len - 1);
            MultiByteToWideChar(CP_UTF8, 0, fullMsg.c_str(), -1, &wMessage[0], len);
        }

        // Determine icon based on type
        UINT uType = MB_OK;
        if (type) {
            std::string typeStr(type);
            if (typeStr == "warning") {
                uType |= MB_ICONWARNING;
            } else if (typeStr == "error" || typeStr == "critical") {
                uType |= MB_ICONERROR;
            } else if (typeStr == "question") {
                uType |= MB_ICONQUESTION;
            } else {
                uType |= MB_ICONINFORMATION;
            }
        } else {
            uType |= MB_ICONINFORMATION;
        }

        // Parse button labels to determine button type
        // MessageBox only supports predefined button combinations
        std::vector<std::string> buttonLabels;
        if (buttons && strlen(buttons) > 0) {
            std::string buttonsStr(buttons);
            std::stringstream ss(buttonsStr);
            std::string buttonLabel;
            while (std::getline(ss, buttonLabel, ',')) {
                // Trim whitespace
                buttonLabel.erase(0, buttonLabel.find_first_not_of(" \t"));
                buttonLabel.erase(buttonLabel.find_last_not_of(" \t") + 1);
                // Convert to lowercase for comparison
                std::transform(buttonLabel.begin(), buttonLabel.end(), buttonLabel.begin(), ::tolower);
                if (!buttonLabel.empty()) {
                    buttonLabels.push_back(buttonLabel);
                }
            }
        }

        // Map common button combinations to MessageBox types
        if (buttonLabels.size() == 2) {
            if ((buttonLabels[0] == "ok" && buttonLabels[1] == "cancel") ||
                (buttonLabels[0] == "yes" && buttonLabels[1] == "no")) {
                uType = (uType & ~MB_OK) | MB_OKCANCEL;
            } else if (buttonLabels[0] == "yes" && buttonLabels[1] == "no") {
                uType = (uType & ~MB_OK) | MB_YESNO;
            }
        } else if (buttonLabels.size() == 3) {
            if (buttonLabels[0] == "yes" && buttonLabels[1] == "no" && buttonLabels[2] == "cancel") {
                uType = (uType & ~MB_OK) | MB_YESNOCANCEL;
            }
        }

        int result = MessageBoxW(nullptr, wMessage.c_str(), wTitle.c_str(), uType);

        // Map MessageBox result to button index
        switch (result) {
            case IDOK:
            case IDYES:
                return 0;
            case IDNO:
                return 1;
            case IDCANCEL:
                return cancelId >= 0 ? cancelId : (buttonLabels.size() > 2 ? 2 : 1);
            default:
                return -1;
        }
    });
}

// ============================================================================
// Clipboard API
// ============================================================================

// clipboardReadText - Read text from the system clipboard
// Returns: UTF-8 string (caller must free) or NULL if no text available
ELECTROBUN_EXPORT const char* clipboardReadText() {
    return MainThreadDispatcher::dispatch_sync([=]() -> const char* {
        if (!OpenClipboard(nullptr)) {
            return nullptr;
        }

        const char* result = nullptr;
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            wchar_t* wText = static_cast<wchar_t*>(GlobalLock(hData));
            if (wText) {
                // Convert wide string to UTF-8
                int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wText, -1, nullptr, 0, nullptr, nullptr);
                if (utf8Len > 0) {
                    char* utf8Text = static_cast<char*>(malloc(utf8Len));
                    WideCharToMultiByte(CP_UTF8, 0, wText, -1, utf8Text, utf8Len, nullptr, nullptr);
                    result = utf8Text;
                }
                GlobalUnlock(hData);
            }
        }

        CloseClipboard();
        return result;
    });
}

// clipboardWriteText - Write text to the system clipboard
ELECTROBUN_EXPORT void clipboardWriteText(const char* text) {
    if (!text) return;

    MainThreadDispatcher::dispatch_sync([=]() {
        if (!OpenClipboard(nullptr)) {
            return;
        }

        EmptyClipboard();

        // Convert UTF-8 to wide string
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (wideLen > 0) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wideLen * sizeof(wchar_t));
            if (hMem) {
                wchar_t* wText = static_cast<wchar_t*>(GlobalLock(hMem));
                MultiByteToWideChar(CP_UTF8, 0, text, -1, wText, wideLen);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
        }

        CloseClipboard();
    });
}

// clipboardReadImage - Read image from clipboard as PNG data
// Returns: PNG data (caller must free) and sets outSize, or NULL if no image
ELECTROBUN_EXPORT const uint8_t* clipboardReadImage(size_t* outSize) {
    return MainThreadDispatcher::dispatch_sync([=]() -> const uint8_t* {
        if (outSize) *outSize = 0;

        if (!OpenClipboard(nullptr)) {
            return nullptr;
        }

        const uint8_t* result = nullptr;

        // Try CF_DIB format (Device Independent Bitmap)
        HANDLE hData = GetClipboardData(CF_DIB);
        if (hData) {
            BITMAPINFO* bmi = static_cast<BITMAPINFO*>(GlobalLock(hData));
            if (bmi) {
                // For now, return raw DIB data - full PNG conversion would require
                // additional libraries like libpng or GDI+
                // TODO: Implement proper PNG conversion using GDI+ or similar
                size_t dataSize = GlobalSize(hData);
                uint8_t* buffer = static_cast<uint8_t*>(malloc(dataSize));
                memcpy(buffer, bmi, dataSize);
                if (outSize) *outSize = dataSize;
                result = buffer;
                GlobalUnlock(hData);
            }
        }

        CloseClipboard();
        return result;
    });
}

// clipboardWriteImage - Write PNG image data to clipboard
ELECTROBUN_EXPORT void clipboardWriteImage(const uint8_t* pngData, size_t size) {
    if (!pngData || size == 0) return;

    MainThreadDispatcher::dispatch_sync([=]() {
        if (!OpenClipboard(nullptr)) {
            return;
        }

        EmptyClipboard();

        // For now, store as raw data - proper PNG to DIB conversion would require
        // additional libraries
        // TODO: Implement proper PNG to DIB conversion
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
        if (hMem) {
            void* data = GlobalLock(hMem);
            memcpy(data, pngData, size);
            GlobalUnlock(hMem);
            // Register a custom format for PNG data
            UINT pngFormat = RegisterClipboardFormatA("PNG");
            SetClipboardData(pngFormat, hMem);
        }

        CloseClipboard();
    });
}

// clipboardClear - Clear the clipboard
ELECTROBUN_EXPORT void clipboardClear() {
    MainThreadDispatcher::dispatch_sync([=]() {
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            CloseClipboard();
        }
    });
}

// clipboardAvailableFormats - Get available formats in clipboard
// Returns: comma-separated list of formats (caller must free)
ELECTROBUN_EXPORT const char* clipboardAvailableFormats() {
    return MainThreadDispatcher::dispatch_sync([=]() -> const char* {
        if (!OpenClipboard(nullptr)) {
            return strdup("");
        }

        std::vector<std::string> formats;

        // Check for text
        if (IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT)) {
            formats.push_back("text");
        }

        // Check for image
        if (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP)) {
            formats.push_back("image");
        }

        // Check for files
        if (IsClipboardFormatAvailable(CF_HDROP)) {
            formats.push_back("files");
        }

        // Check for HTML
        UINT htmlFormat = RegisterClipboardFormatA("HTML Format");
        if (IsClipboardFormatAvailable(htmlFormat)) {
            formats.push_back("html");
        }

        CloseClipboard();

        // Join formats with comma
        std::string result;
        for (size_t i = 0; i < formats.size(); i++) {
            if (i > 0) result += ",";
            result += formats[i];
        }

        return strdup(result.c_str());
    });
}

// Window procedure for handling tray messages
LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            // Don't allow the tray window to be closed/destroyed by default handlers
            ::log("Preventing tray window close/destroy");
            return 0;
            
        case WM_COMMAND:
        // Handle menu item clicks
        {
            auto it = g_trayItems.find(hwnd);
            if (it != g_trayItems.end()) {
                NSStatusItem* trayItem = it->second;
                UINT menuItemId = LOWORD(wParam);
                
                // Use your existing function to handle the menu selection
                handleMenuItemSelection(menuItemId, trayItem);
            }
            return 0;
        }
            
        default:
            // Check if this is our tray message
            if (msg == g_trayMessageId) {
                // Find the tray item
                auto it = g_trayItems.find(hwnd);
                if (it != g_trayItems.end()) {
                    NSStatusItem* trayItem = it->second;
                    
                    switch (LOWORD(lParam)) {
                        case WM_LBUTTONUP:
                           
                            
                        case WM_RBUTTONUP:
                            // Right click - show context menu if it exists, otherwise call handler
                            if (trayItem->contextMenu) {
                                
                                
                                POINT pt;
                                GetCursorPos(&pt);
                                
                                // This is required for the menu to work properly
                                SetForegroundWindow(hwnd);
                                
                                // Show the menu
                                BOOL menuResult = TrackPopupMenu(
                                    trayItem->contextMenu, 
                                    TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                                    pt.x, pt.y, 
                                    0, 
                                    hwnd, 
                                    NULL
                                );
                                
                                // This message helps ensure the menu closes properly
                                PostMessage(hwnd, WM_NULL, 0, 0);
                                
                                if (!menuResult) {
                                    ::log("TrackPopupMenu failed");
                                }
                            } else {
                                // No menu exists yet, call handler (this will trigger menu creation)
                                
                                
                                if (trayItem->handler) {
                                    // Use a separate thread or async call to prevent blocking
                                    std::thread([trayItem]() {
                                        try {
                                            trayItem->handler(trayItem->trayId, "");
                                        } catch (...) {
                                            ::log("Exception in tray handler");
                                        }
                                    }).detach();
                                }
                            }
                            return 0;
                            
                        default:
                            break;
                    }
                }
                return 0;
            }
            break;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

ELECTROBUN_EXPORT NSStatusItem* createTray(uint32_t trayId, const char *title, const char *pathToImage, bool isTemplate,
                        uint32_t width, uint32_t height, ZigStatusItemHandler zigTrayItemHandler) {
    
    return MainThreadDispatcher::dispatch_sync([=]() -> NSStatusItem* {
        // ::log("Creating system tray icon");
        
        NSStatusItem* statusItem = new NSStatusItem();
        statusItem->trayId = trayId;
        statusItem->handler = zigTrayItemHandler;
        
        if (title) {
            statusItem->title = std::string(title);
        }
        if (pathToImage) {
            statusItem->imagePath = std::string(pathToImage);
        }
        
        // Create a hidden window to receive tray messages
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSA wc = {0};
            wc.lpfnWndProc = TrayWindowProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = "TrayWindowClass";
            wc.hbrBackground = NULL;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.style = 0; // No special styles
            
            if (!RegisterClassA(&wc)) {
                DWORD error = GetLastError();
                if (error != ERROR_CLASS_ALREADY_EXISTS) {
                    char errorMsg[256];
                    sprintf_s(errorMsg, "Failed to register TrayWindowClass: %lu", error);
                    ::log(errorMsg);
                    delete statusItem;
                    return nullptr;
                }
            }
            classRegistered = true;
        }
        
        // Create message-only window (safer for tray operations)
        statusItem->hwnd = CreateWindowA(
            "TrayWindowClass", 
            "TrayWindow", 
            0,                    // No visible style
            0, 0, 0, 0,          // Position and size (ignored for message-only)
            HWND_MESSAGE,        // Message-only window
            NULL, 
            GetModuleHandle(NULL), 
            NULL
        );
        
        if (!statusItem->hwnd) {
            DWORD error = GetLastError();
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Failed to create tray window: %lu", error);
            ::log(errorMsg);
            delete statusItem;
            return nullptr;
        }
        
        
        
        // Store in global map before setting up the tray icon
        g_trayItems[statusItem->hwnd] = statusItem;
        
        // Set up NOTIFYICONDATA
        statusItem->nid.cbSize = sizeof(NOTIFYICONDATA);
        statusItem->nid.hWnd = statusItem->hwnd;
        statusItem->nid.uID = trayId;
        statusItem->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        statusItem->nid.uCallbackMessage = g_trayMessageId;
        
        // Set title/tooltip
        if (!statusItem->title.empty()) {
            strncpy_s(statusItem->nid.szTip, sizeof(statusItem->nid.szTip), 
                     statusItem->title.c_str(), sizeof(statusItem->nid.szTip) - 1);
        }
        
        // Load icon
        if (!statusItem->imagePath.empty()) {
            // Convert to wide string for LoadImage
            int size = MultiByteToWideChar(CP_UTF8, 0, statusItem->imagePath.c_str(), -1, NULL, 0);
            if (size > 0) {
                std::wstring wImagePath(size - 1, 0);
                MultiByteToWideChar(CP_UTF8, 0, statusItem->imagePath.c_str(), -1, &wImagePath[0], size);
                
                statusItem->nid.hIcon = (HICON)LoadImageW(NULL, wImagePath.c_str(), IMAGE_ICON,
                                                         width, height, LR_LOADFROMFILE);
                
                if (!statusItem->nid.hIcon) {
                    char errorMsg[256];
                    sprintf_s(errorMsg, "Failed to load icon from: %s", statusItem->imagePath.c_str());
                    ::log(errorMsg);
                }
            }
        }
        
        // Use default icon if loading failed
        if (!statusItem->nid.hIcon) {
            statusItem->nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
            ::log("Using default application icon");
        }
        
        // Add to system tray
        if (Shell_NotifyIcon(NIM_ADD, &statusItem->nid)) {
            // char successMsg[256];
            // sprintf_s(successMsg, "System tray icon created successfully: ID=%u, HWND=%p", trayId, statusItem->hwnd);
            // ::log(successMsg);
        } else {
            DWORD error = GetLastError();
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Failed to add icon to system tray: %lu", error);
            ::log(errorMsg);
            
            DestroyWindow(statusItem->hwnd);
            g_trayItems.erase(statusItem->hwnd);
            delete statusItem;
            return nullptr;
        }
        
        return statusItem;
    });
}

ELECTROBUN_EXPORT void setTrayTitle(NSStatusItem *statusItem, const char *title) {
    if (!statusItem) return;
    
    MainThreadDispatcher::dispatch_sync([=]() {
        
        if (title) {
            statusItem->title = std::string(title);
            strncpy_s(statusItem->nid.szTip, title, sizeof(statusItem->nid.szTip) - 1);
        } else {
            statusItem->title.clear();
            statusItem->nid.szTip[0] = '\0';
        }
        
        // Update the tray icon
        Shell_NotifyIcon(NIM_MODIFY, &statusItem->nid);
    });
}

ELECTROBUN_EXPORT void setTrayImage(NSStatusItem *statusItem, const char *image) {
    if (!statusItem) return;
    
    MainThreadDispatcher::dispatch_sync([=]() {
        
        HICON oldIcon = statusItem->nid.hIcon;
        
        if (image && strlen(image) > 0) {
            statusItem->imagePath = std::string(image);
            
            // Convert to wide string
            int size = MultiByteToWideChar(CP_UTF8, 0, image, -1, NULL, 0);
            if (size > 0) {
                std::wstring wImagePath(size - 1, 0);
                MultiByteToWideChar(CP_UTF8, 0, image, -1, &wImagePath[0], size);
                
                statusItem->nid.hIcon = (HICON)LoadImageW(NULL, wImagePath.c_str(), IMAGE_ICON,
                                                         0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
            }
        }
        
        // Use default icon if loading failed
        if (!statusItem->nid.hIcon) {
            statusItem->nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        }
        
        // Update the tray icon
        if (Shell_NotifyIcon(NIM_MODIFY, &statusItem->nid)) {
            // Clean up old icon if it's not the default
            if (oldIcon && oldIcon != LoadIcon(NULL, IDI_APPLICATION)) {
                DestroyIcon(oldIcon);
            }
        } else {
            ::log("ERROR: Failed to update tray image");
            // Restore old icon on failure
            statusItem->nid.hIcon = oldIcon;
        }
    });
}

// Updated setTrayMenuFromJSON function
ELECTROBUN_EXPORT void setTrayMenuFromJSON(NSStatusItem *statusItem, const char *jsonString) {
    if (!statusItem || !jsonString) return;
        
    MainThreadDispatcher::dispatch_sync([=]() {
        
        if (!statusItem->handler) {
            ::log("ERROR: No handler found for status item");
            return;
        }
        
        try {
            // Parse JSON using our simple parser
            SimpleJsonValue menuConfig = parseJson(std::string(jsonString));
            
            if (menuConfig.type != SimpleJsonValue::ARRAY) {
                ::log("ERROR: JSON menu configuration is not an array");
                return;
            }
            
            // Clean up existing menu
            if (statusItem->contextMenu) {
                DestroyMenu(statusItem->contextMenu);
                statusItem->contextMenu = NULL;
            }
            
            // Create new menu from JSON config
            statusItem->contextMenu = createMenuFromConfig(menuConfig, statusItem);
            
            if (statusItem->contextMenu) {
            } else {
                ::log("ERROR: Failed to create context menu from JSON configuration");
            }
            
        } catch (const std::exception& e) {
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Exception parsing JSON: %s", e.what());
            ::log(errorMsg);
        } catch (...) {
            ::log("ERROR: Unknown exception parsing JSON");
        }
    });
}

// You'll also need to update your tray click handler to process menu selections
// This should be called from your window procedure when handling tray icon messages
void handleTrayIconMessage(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    NSStatusItem* statusItem = nullptr;
    
    // Find the status item from the global map
    auto it = g_trayItems.find(hwnd);
    if (it != g_trayItems.end()) {
        statusItem = it->second;
    }
    
    switch (lParam) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            if (statusItem && statusItem->contextMenu) {
                POINT pt;
                GetCursorPos(&pt);
                
                // Required for popup menus to work correctly
                SetForegroundWindow(hwnd);
                
                UINT cmd = TrackPopupMenu(
                    statusItem->contextMenu,
                    TPM_RETURNCMD | TPM_RIGHTBUTTON,
                    pt.x, pt.y,
                    0, hwnd, NULL
                );
                
                if (cmd != 0) {
                    handleMenuItemSelection(cmd, statusItem);
                }
                
                // Required cleanup
                PostMessage(hwnd, WM_NULL, 0, 0);
            }
            break;
            
        case WM_LBUTTONUP:
            // Handle left click on tray icon
            if (statusItem && statusItem->handler) {
                statusItem->handler(statusItem->trayId, "");
            }
            break;
    }
}

ELECTROBUN_EXPORT void setTrayMenu(NSStatusItem *statusItem, const char *menuConfig) {
    // Delegate to JSON version for now
    setTrayMenuFromJSON(statusItem, menuConfig);
}

ELECTROBUN_EXPORT void removeTray(NSStatusItem *statusItem) {
    if (!statusItem) return;
    
    MainThreadDispatcher::dispatch_sync([=]() {
        // Remove from global map first
        g_trayItems.erase(statusItem->hwnd);
        
        // Clean up the tray item
        delete statusItem;
    });
}

ELECTROBUN_EXPORT const char* getTrayBounds(NSStatusItem *statusItem) {
    (void)statusItem;
    return _strdup("{\"x\":0,\"y\":0,\"width\":0,\"height\":0}");
}

ELECTROBUN_EXPORT void setApplicationMenu(const char *jsonString, ZigStatusItemHandler zigTrayItemHandler) {
    if (!jsonString) {
        ::log("ERROR: NULL JSON string passed to setApplicationMenu");
        return;
    }
    
    
    MainThreadDispatcher::dispatch_sync([=]() {
        try {
            // Parse JSON using our simple parser
            SimpleJsonValue menuConfig = parseJson(std::string(jsonString));
            
            if (menuConfig.type != SimpleJsonValue::ARRAY) {
                ::log("ERROR: Application menu JSON configuration is not an array");
                return;
            }
            
            // Create target for handling menu actions
            g_appMenuTarget = std::make_unique<StatusItemTarget>();
            g_appMenuTarget->zigHandler = zigTrayItemHandler;
            g_appMenuTarget->trayId = 0;
            
            // Clean up existing application menu and accelerators
            if (g_applicationMenu) {
                DestroyMenu(g_applicationMenu);
                g_applicationMenu = NULL;
            }
            clearMenuAccelerators();

            // Create new application menu from JSON config
            g_applicationMenu = createApplicationMenuFromConfig(menuConfig, g_appMenuTarget.get());

            // Rebuild the accelerator table after menu creation
            rebuildAcceleratorTable();
            
            if (g_applicationMenu) {
                
                // Find the main application window to set the menu
                HWND mainWindow = GetActiveWindow();
                if (!mainWindow) {
                    mainWindow = FindWindowA("BasicWindowClass", NULL);
                }
                
                if (mainWindow) {
                    if (SetMenu(mainWindow, g_applicationMenu)) {
                        DrawMenuBar(mainWindow);
                        
                       
                    } else {
                        DWORD error = GetLastError();
                        char errorMsg[256];
                        sprintf_s(errorMsg, "Failed to set application menu on window: %lu", error);
                        ::log(errorMsg);
                    }
                } else {
                    ::log("Warning: No main window found to attach application menu");
                }
            } else {
                ::log("ERROR: Failed to create application menu from JSON configuration");
            }
            
        } catch (const std::exception& e) {
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Exception in setApplicationMenu: %s", e.what());
            ::log(errorMsg);
        } catch (...) {
            ::log("ERROR: Unknown exception in setApplicationMenu");
        }
    });
}


ELECTROBUN_EXPORT void showContextMenu(const char *jsonString, ZigStatusItemHandler contextMenuHandler) {
    if (!jsonString) {
        ::log("ERROR: NULL JSON string passed to showContextMenu");
        return;
    }
    
    if (!contextMenuHandler) {
        ::log("ERROR: NULL context menu handler passed to showContextMenu");
        return;
    }
    
    MainThreadDispatcher::dispatch_sync([=]() {
        try {
            SimpleJsonValue menuConfig = parseJson(std::string(jsonString));

            std::unique_ptr<NSStatusItem> target = std::make_unique<NSStatusItem>();
            target->handler = contextMenuHandler;
            target->trayId = 0;

            HMENU menu = createMenuFromConfig(menuConfig, target.get());
            if (!menu) {
                ::log("ERROR: Failed to create context menu");
                return;
            }
            
            // Get cursor position for menu display
            POINT pt;
            GetCursorPos(&pt);
            
            // Get the foreground window or use desktop
            HWND hwnd = GetForegroundWindow();
            if (!hwnd) {
                hwnd = GetDesktopWindow();
            }
            
            // Required for proper menu operation
            SetForegroundWindow(hwnd);
                        
            // Show the context menu
            UINT cmd = TrackPopupMenu(
                menu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON,
                pt.x, pt.y,
                0, hwnd, NULL
            );
            
            // Handle menu selection
            if (cmd != 0) {
                handleMenuItemSelection(cmd, target.get());
            }
            
            // Required for proper cleanup
            PostMessage(hwnd, WM_NULL, 0, 0);
            
            // Cleanup menu
            DestroyMenu(menu);
            
        } catch (const std::exception& e) {
            ::log("ERROR: Exception in showContextMenu: " + std::string(e.what()));
        }
    });
}

ELECTROBUN_EXPORT void getWebviewSnapshot(uint32_t hostId, uint32_t webviewId,
                       WKWebView *webView,
                       zigSnapshotCallback callback) {
    // Stub implementation
    if (callback) {
        static const char* emptyDataUrl = "data:image/png;base64,";
        callback(hostId, webviewId, emptyDataUrl);
    }
}

ELECTROBUN_EXPORT void setJSUtils(GetMimeType getMimeType, GetHTMLForWebviewSync getHTMLForWebviewSync) {
    ::log("setJSUtils called but using map-based approach instead of callbacks");
}

// MARK: - Webview HTML Content Management (replaces JSCallback approach)

extern "C" ELECTROBUN_EXPORT void setWebviewHTMLContent(uint32_t webviewId, const char* htmlContent) {
    std::lock_guard<std::mutex> lock(webviewHTMLMutex);
    if (htmlContent) {
        webviewHTMLContent[webviewId] = std::string(htmlContent);
        char logMsg[256];
        sprintf_s(logMsg, "setWebviewHTMLContent: Set HTML for webview %u", webviewId);
        ::log(logMsg);
    } else {
        webviewHTMLContent.erase(webviewId);
        char logMsg[256];
        sprintf_s(logMsg, "setWebviewHTMLContent: Cleared HTML for webview %u", webviewId);
        ::log(logMsg);
    }
}

extern "C" ELECTROBUN_EXPORT const char* getWebviewHTMLContent(uint32_t webviewId) {
    std::lock_guard<std::mutex> lock(webviewHTMLMutex);
    auto it = webviewHTMLContent.find(webviewId);
    if (it != webviewHTMLContent.end()) {
        char* result = _strdup(it->second.c_str());
        char logMsg[256];
        sprintf_s(logMsg, "getWebviewHTMLContent: Retrieved HTML for webview %u", webviewId);
        ::log(logMsg);
        return result;
    } else {
        char logMsg[256];
        sprintf_s(logMsg, "getWebviewHTMLContent: No HTML found for webview %u", webviewId);
        ::log(logMsg);
        return nullptr;
    }
}

// Adding a few Windows-specific functions for interop if needed
ELECTROBUN_EXPORT uint32_t getWindowStyle(
    bool Borderless,
    bool Titled,
    bool Closable,
    bool Miniaturizable,
    bool Resizable,
    bool UnifiedTitleAndToolbar,
    bool FullScreen,
    bool FullSizeContentView,
    bool UtilityWindow,
    bool DocModalWindow,
    bool NonactivatingPanel,
    bool HUDWindow) {
    // Stub implementation that returns a composite style mask
    uint32_t mask = 0;
    if (Borderless) mask |= 1;
    if (Titled) mask |= 2;
    if (Closable) mask |= 4;
    if (Resizable) mask |= 8;
    return mask;
}

} // extern "C"

// Shared MIME type detection function
// Based on Bun runtime supported file types and web development standards
std::string getMimeTypeForFile(const std::string& path) {
    // Web/Code Files (Bun native support)
    if (path.find(".html") != std::string::npos || path.find(".htm") != std::string::npos) {
        return "text/html";
    } else if (path.find(".js") != std::string::npos || path.find(".mjs") != std::string::npos || path.find(".cjs") != std::string::npos) {
        return "text/javascript";
    } else if (path.find(".ts") != std::string::npos || path.find(".mts") != std::string::npos || path.find(".cts") != std::string::npos) {
        return "text/typescript";
    } else if (path.find(".jsx") != std::string::npos) {
        return "text/jsx";
    } else if (path.find(".tsx") != std::string::npos) {
        return "text/tsx";
    } else if (path.find(".css") != std::string::npos) {
        return "text/css";
    } else if (path.find(".json") != std::string::npos) {
        return "application/json";
    } else if (path.find(".xml") != std::string::npos) {
        return "application/xml";
    } else if (path.find(".md") != std::string::npos) {
        return "text/markdown";
    } else if (path.find(".txt") != std::string::npos) {
        return "text/plain";
    } else if (path.find(".toml") != std::string::npos) {
        return "application/toml";
    } else if (path.find(".yaml") != std::string::npos || path.find(".yml") != std::string::npos) {
        return "application/x-yaml";
    
    // Image Files
    } else if (path.find(".png") != std::string::npos) {
        return "image/png";
    } else if (path.find(".jpg") != std::string::npos || path.find(".jpeg") != std::string::npos) {
        return "image/jpeg";
    } else if (path.find(".gif") != std::string::npos) {
        return "image/gif";
    } else if (path.find(".webp") != std::string::npos) {
        return "image/webp";
    } else if (path.find(".svg") != std::string::npos) {
        return "image/svg+xml";
    } else if (path.find(".ico") != std::string::npos) {
        return "image/x-icon";
    } else if (path.find(".avif") != std::string::npos) {
        return "image/avif";
    
    // Font Files
    } else if (path.find(".woff") != std::string::npos) {
        return "font/woff";
    } else if (path.find(".woff2") != std::string::npos) {
        return "font/woff2";
    } else if (path.find(".ttf") != std::string::npos) {
        return "font/ttf";
    } else if (path.find(".otf") != std::string::npos) {
        return "font/otf";
    
    // Media Files
    } else if (path.find(".mp3") != std::string::npos) {
        return "audio/mpeg";
    } else if (path.find(".mp4") != std::string::npos) {
        return "video/mp4";
    } else if (path.find(".webm") != std::string::npos) {
        return "video/webm";
    } else if (path.find(".ogg") != std::string::npos) {
        return "audio/ogg";
    } else if (path.find(".wav") != std::string::npos) {
        return "audio/wav";
    
    // Document Files
    } else if (path.find(".pdf") != std::string::npos) {
        return "application/pdf";
    
    // WebAssembly (Bun support)
    } else if (path.find(".wasm") != std::string::npos) {
        return "application/wasm";
    
    // Compressed Files
    } else if (path.find(".zip") != std::string::npos) {
        return "application/zip";
    } else if (path.find(".gz") != std::string::npos) {
        return "application/gzip";
    }

    return "application/octet-stream"; // default
}

/*
 * =============================================================================
 * GLOBAL KEYBOARD SHORTCUTS
 * =============================================================================
 */

// Callback type for global shortcut triggers
typedef void (*GlobalShortcutCallback)(const char* accelerator);
static GlobalShortcutCallback g_globalShortcutCallback = nullptr;

// Custom Windows messages for hotkey thread communication
#define WM_REGISTER_HOTKEY (WM_USER + 100)
#define WM_UNREGISTER_HOTKEY (WM_USER + 101)
#define WM_UNREGISTER_ALL_HOTKEYS (WM_USER + 102)

// Structure to pass hotkey registration data between threads
struct HotkeyRegisterData {
    int hotkeyId;
    UINT modifiers;
    UINT vkCode;
    std::string accelerator;
    BOOL* result;  // Output: success/failure
    HANDLE completionEvent;  // Signal when operation is complete
};

// Per-hotkey registration info so we can re-register after focus changes
struct HotkeyRegistration {
    int hotkeyId;
    UINT modifiers;
    UINT vkCode;
    std::string accelerator;
};

// Storage for registered shortcuts: accelerator string -> hotkey ID
static std::map<std::string, int> g_globalShortcuts;
static std::map<int, std::string> g_hotkeyIdToAccelerator;
static std::map<int, HotkeyRegistration> g_hotkeyRegistrations;  // hotkeyId -> registration data for re-register
static bool g_hotkeysCurrentlyRegistered = false;  // true when OS-level hotkeys are active
static int g_nextHotkeyId = 1;
static HWND g_hotkeyWindow = NULL;
static std::thread g_hotkeyThread;
static bool g_hotkeyThreadRunning = false;
static std::mutex g_hotkeyMutex;  // Protect access to g_globalShortcuts and g_hotkeyIdToAccelerator

// Helper to parse virtual key code from key string
static UINT getVirtualKeyCode(const std::string& key) {
    std::string lowerKey = key;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);

    // Letters
    if (lowerKey.length() == 1 && lowerKey[0] >= 'a' && lowerKey[0] <= 'z') {
        return 'A' + (lowerKey[0] - 'a');
    }
    // Numbers
    if (lowerKey.length() == 1 && lowerKey[0] >= '0' && lowerKey[0] <= '9') {
        return '0' + (lowerKey[0] - '0');
    }
    // Function keys
    if (lowerKey[0] == 'f' && lowerKey.length() >= 2) {
        int fNum = std::stoi(lowerKey.substr(1));
        if (fNum >= 1 && fNum <= 24) return VK_F1 + (fNum - 1);
    }
    // Special keys
    if (lowerKey == "space" || lowerKey == " ") return VK_SPACE;
    if (lowerKey == "return" || lowerKey == "enter") return VK_RETURN;
    if (lowerKey == "tab") return VK_TAB;
    if (lowerKey == "escape" || lowerKey == "esc") return VK_ESCAPE;
    if (lowerKey == "backspace") return VK_BACK;
    if (lowerKey == "delete") return VK_DELETE;
    if (lowerKey == "up") return VK_UP;
    if (lowerKey == "down") return VK_DOWN;
    if (lowerKey == "left") return VK_LEFT;
    if (lowerKey == "right") return VK_RIGHT;
    if (lowerKey == "home") return VK_HOME;
    if (lowerKey == "end") return VK_END;
    if (lowerKey == "pageup") return VK_PRIOR;
    if (lowerKey == "pagedown") return VK_NEXT;
    // Symbols
    if (lowerKey == "-") return VK_OEM_MINUS;
    if (lowerKey == "=") return VK_OEM_PLUS;
    if (lowerKey == "[") return VK_OEM_4;
    if (lowerKey == "]") return VK_OEM_6;
    if (lowerKey == "\\") return VK_OEM_5;
    if (lowerKey == ";") return VK_OEM_1;
    if (lowerKey == "'") return VK_OEM_7;
    if (lowerKey == ",") return VK_OEM_COMMA;
    if (lowerKey == ".") return VK_OEM_PERIOD;
    if (lowerKey == "/") return VK_OEM_2;
    if (lowerKey == "`") return VK_OEM_3;

    return 0;
}

// Parse modifiers from accelerator string for global shortcuts using the
// shared cross-platform parser. Returns MOD_CONTROL, MOD_ALT, MOD_SHIFT flags.
static UINT parseModifiers(const std::string& accelerator, std::string& outKey) {
    auto parts = electrobun::parseAccelerator(accelerator);
    outKey = parts.key;

    UINT modifiers = 0;
    if (parts.commandOrControl || parts.command || parts.control) modifiers |= MOD_CONTROL;
    if (parts.alt)                                                modifiers |= MOD_ALT;
    if (parts.shift)                                              modifiers |= MOD_SHIFT;
    if (parts.super)                                              modifiers |= MOD_WIN;
    return modifiers;
}

// Window procedure for hotkey window
static LRESULT CALLBACK HotkeyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY) {
        // Hotkeys are only registered while our app has focus (see focus hook below),
        // so no process ID check needed — if we get WM_HOTKEY, we're focused.
        int hotkeyId = (int)wParam;
        std::lock_guard<std::mutex> lock(g_hotkeyMutex);
        auto it = g_hotkeyIdToAccelerator.find(hotkeyId);
        if (it != g_hotkeyIdToAccelerator.end() && g_globalShortcutCallback) {
            g_globalShortcutCallback(it->second.c_str());
        }
        return 0;
    }
    else if (msg == WM_REGISTER_HOTKEY) {
        HotkeyRegisterData* data = reinterpret_cast<HotkeyRegisterData*>(lParam);
        BOOL success = RegisterHotKey(hwnd, data->hotkeyId, data->modifiers, data->vkCode);
        if (success) {
            std::lock_guard<std::mutex> lock(g_hotkeyMutex);
            g_globalShortcuts[data->accelerator] = data->hotkeyId;
            g_hotkeyIdToAccelerator[data->hotkeyId] = data->accelerator;
            // Store registration data for re-registering on focus gain
            HotkeyRegistration reg;
            reg.hotkeyId = data->hotkeyId;
            reg.modifiers = data->modifiers;
            reg.vkCode = data->vkCode;
            reg.accelerator = data->accelerator;
            g_hotkeyRegistrations[data->hotkeyId] = reg;
            g_hotkeysCurrentlyRegistered = true;
            ::log("GlobalShortcut registered successfully: '" + data->accelerator + "' (id=" + std::to_string(data->hotkeyId) + ", total=" + std::to_string(g_globalShortcuts.size()) + ")");
        } else {
            DWORD error = GetLastError();
            ::log("ERROR: Failed to register hotkey '" + data->accelerator + "' - Win32 error: " + std::to_string(error));
        }
        *data->result = success;
        SetEvent(data->completionEvent);
        return 0;
    }
    else if (msg == WM_UNREGISTER_HOTKEY) {
        int hotkeyId = (int)wParam;
        UnregisterHotKey(hwnd, hotkeyId);
        {
            std::lock_guard<std::mutex> lock(g_hotkeyMutex);
            g_hotkeyRegistrations.erase(hotkeyId);
        }
        return 0;
    }
    else if (msg == WM_UNREGISTER_ALL_HOTKEYS) {
        std::lock_guard<std::mutex> lock(g_hotkeyMutex);
        for (const auto& pair : g_globalShortcuts) {
            UnregisterHotKey(hwnd, pair.second);
        }
        g_globalShortcuts.clear();
        g_hotkeyIdToAccelerator.clear();
        g_hotkeyRegistrations.clear();
        g_hotkeysCurrentlyRegistered = false;
        ::log("GlobalShortcut: Unregistered all shortcuts");
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Suspend hotkeys at the OS level (unregister without clearing our maps).
// Called when the app loses focus so other apps can use those key combos.
static void suspendHotkeys() {
    if (!g_hotkeyWindow) return;
    std::lock_guard<std::mutex> lock(g_hotkeyMutex);
    if (!g_hotkeysCurrentlyRegistered) return;
    for (const auto& pair : g_hotkeyRegistrations) {
        UnregisterHotKey(g_hotkeyWindow, pair.first);
    }
    g_hotkeysCurrentlyRegistered = false;
    ::log("GlobalShortcut: Suspended (app lost focus, " + std::to_string(g_hotkeyRegistrations.size()) + " hotkeys)");
}

// Resume hotkeys at the OS level (re-register from stored data).
// Called when the app gains focus.
static void resumeHotkeys() {
    if (!g_hotkeyWindow) return;
    std::lock_guard<std::mutex> lock(g_hotkeyMutex);
    if (g_hotkeysCurrentlyRegistered) return;
    for (const auto& pair : g_hotkeyRegistrations) {
        const HotkeyRegistration& reg = pair.second;
        RegisterHotKey(g_hotkeyWindow, reg.hotkeyId, reg.modifiers, reg.vkCode);
    }
    g_hotkeysCurrentlyRegistered = true;
    ::log("GlobalShortcut: Resumed (app gained focus, " + std::to_string(g_hotkeyRegistrations.size()) + " hotkeys)");
}

// WinEvent hook callback — monitors foreground window changes to suspend/resume hotkeys
static void CALLBACK ForegroundChangeProc(
    HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD idEventThread, DWORD dwmsEventTime)
{
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    DWORD fgPid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &fgPid);
    if (fgPid == GetCurrentProcessId()) {
        resumeHotkeys();
    } else {
        suspendHotkeys();
    }
}

// Message loop thread for hotkey window
static void hotkeyMessageLoop() {
    // Create a message-only window
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = HotkeyWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"ElectrobunHotkeyWindow";

    RegisterClassExW(&wc);

    g_hotkeyWindow = CreateWindowExW(0, L"ElectrobunHotkeyWindow", L"",
        0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL);

    if (!g_hotkeyWindow) {
        ::log("ERROR: Failed to create hotkey window");
        return;
    }

    // Install a WinEvent hook to monitor foreground window changes.
    // When our app loses focus, we unregister all hotkeys so other apps
    // can use those key combos. When we regain focus, we re-register them.
    HWINEVENTHOOK hFocusHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, ForegroundChangeProc, 0, 0,
        WINEVENT_OUTOFCONTEXT);

    MSG msg;
    while (g_hotkeyThreadRunning && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hFocusHook) UnhookWinEvent(hFocusHook);

    DestroyWindow(g_hotkeyWindow);
    g_hotkeyWindow = NULL;
}

// Set the callback for global shortcut events
extern "C" ELECTROBUN_EXPORT void setGlobalShortcutCallback(GlobalShortcutCallback callback) {
    g_globalShortcutCallback = callback;

    // Start the hotkey message loop thread if not running
    if (!g_hotkeyThreadRunning && callback) {
        g_hotkeyThreadRunning = true;
        g_hotkeyThread = std::thread(hotkeyMessageLoop);
        // Wait for window to be created
        while (!g_hotkeyWindow && g_hotkeyThreadRunning) {
            Sleep(10);
        }
    }
}

// Register a global keyboard shortcut
extern "C" ELECTROBUN_EXPORT BOOL registerGlobalShortcut(const char* accelerator) {
    if (!accelerator) {
        ::log("ERROR: Cannot register shortcut - invalid accelerator");
        return FALSE;
    }

    // Wait for hotkey window to be ready (with timeout)
    int waitCount = 0;
    const int maxWaitMs = 5000; // 5 second timeout

    while (!g_hotkeyWindow && waitCount < maxWaitMs) {
        Sleep(10);
        waitCount += 10;
    }

    if (!g_hotkeyWindow) {
        ::log("ERROR: Cannot register shortcut - hotkey window not ready after " + std::to_string(waitCount) + "ms");
        return FALSE;
    }

    std::string accelStr(accelerator);

    // Check if already registered (with mutex protection)
    {
        std::lock_guard<std::mutex> lock(g_hotkeyMutex);
        if (g_globalShortcuts.find(accelStr) != g_globalShortcuts.end()) {
            ::log("GlobalShortcut already registered: " + accelStr);
            return FALSE;
        }
    }

    // Parse the accelerator
    std::string key;
    UINT modifiers = parseModifiers(accelStr, key);
    UINT vkCode = getVirtualKeyCode(key);

    if (vkCode == 0) {
        ::log("ERROR: Unknown key: " + key);
        return FALSE;
    }

    // Prepare registration data
    int hotkeyId = g_nextHotkeyId++;
    BOOL result = FALSE;
    HANDLE completionEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    HotkeyRegisterData data;
    data.hotkeyId = hotkeyId;
    data.modifiers = modifiers | MOD_NOREPEAT;
    data.vkCode = vkCode;
    data.accelerator = accelStr;
    data.result = &result;
    data.completionEvent = completionEvent;

    ::log("GlobalShortcut: Posting registration request for '" + accelStr + "' with modifiers=" + std::to_string(modifiers) + " vkCode=" + std::to_string(vkCode));

    // Post message to hotkey thread to register the hotkey
    PostMessage(g_hotkeyWindow, WM_REGISTER_HOTKEY, 0, reinterpret_cast<LPARAM>(&data));

    // Wait for registration to complete (with timeout)
    DWORD waitResult = WaitForSingleObject(completionEvent, 5000);
    CloseHandle(completionEvent);

    if (waitResult != WAIT_OBJECT_0) {
        ::log("ERROR: Registration timeout for '" + accelStr + "'");
        return FALSE;
    }

    return result;
}

// Unregister a global keyboard shortcut
extern "C" ELECTROBUN_EXPORT BOOL unregisterGlobalShortcut(const char* accelerator) {
    if (!accelerator) return FALSE;

    std::string accelStr(accelerator);
    int hotkeyId = -1;

    {
        std::lock_guard<std::mutex> lock(g_hotkeyMutex);
        auto it = g_globalShortcuts.find(accelStr);
        if (it != g_globalShortcuts.end()) {
            hotkeyId = it->second;
            g_hotkeyIdToAccelerator.erase(hotkeyId);
            g_globalShortcuts.erase(it);
        }
    }

    if (hotkeyId != -1 && g_hotkeyWindow) {
        PostMessage(g_hotkeyWindow, WM_UNREGISTER_HOTKEY, hotkeyId, 0);
        ::log("GlobalShortcut unregistered: " + accelStr);
        return TRUE;
    }

    return FALSE;
}

// Unregister all global keyboard shortcuts
extern "C" ELECTROBUN_EXPORT void unregisterAllGlobalShortcuts() {
    if (g_hotkeyWindow) {
        PostMessage(g_hotkeyWindow, WM_UNREGISTER_ALL_HOTKEYS, 0, 0);
    }
}

// Check if a shortcut is registered
extern "C" ELECTROBUN_EXPORT BOOL isGlobalShortcutRegistered(const char* accelerator) {
    if (!accelerator) return FALSE;

    std::string accelStr(accelerator);
    std::lock_guard<std::mutex> lock(g_hotkeyMutex);
    bool found = g_globalShortcuts.find(accelStr) != g_globalShortcuts.end();
    ::log("GlobalShortcut.isRegistered: Checking '" + accelStr + "' - " + (found ? "FOUND" : "NOT FOUND") + " (total shortcuts=" + std::to_string(g_globalShortcuts.size()) + ")");
    return found;
}

/*
 * =============================================================================
 * SCREEN API
 * =============================================================================
 */

// Structure to collect monitor info during enumeration
struct MonitorEnumData {
    std::vector<std::string> displays;
};

// Callback for EnumDisplayMonitors
static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MonitorEnumData* data = reinterpret_cast<MonitorEnumData*>(dwData);

    MONITORINFOEX monitorInfo;
    monitorInfo.cbSize = sizeof(MONITORINFOEX);

    if (GetMonitorInfo(hMonitor, &monitorInfo)) {
        // Get DPI/scale factor using GetDpiForMonitor if available (Windows 8.1+)
        double scaleFactor = 1.0;

        // Try to get DPI - load dynamically as it may not be available on all Windows versions
        typedef HRESULT(WINAPI *GetDpiForMonitorFunc)(HMONITOR, int, UINT*, UINT*);
        HMODULE shcore = LoadLibraryW(L"Shcore.dll");
        if (shcore) {
            GetDpiForMonitorFunc getDpi = (GetDpiForMonitorFunc)GetProcAddress(shcore, "GetDpiForMonitor");
            if (getDpi) {
                UINT dpiX, dpiY;
                // MDT_EFFECTIVE_DPI = 0
                if (SUCCEEDED(getDpi(hMonitor, 0, &dpiX, &dpiY))) {
                    scaleFactor = dpiX / 96.0;  // 96 DPI is 100% scaling
                }
            }
            FreeLibrary(shcore);
        }

        // Check if primary
        bool isPrimary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;

        // Build JSON for this display
        std::ostringstream json;
        json << "{";
        json << "\"id\":" << reinterpret_cast<uintptr_t>(hMonitor) << ",";
        json << "\"bounds\":{";
        json << "\"x\":" << monitorInfo.rcMonitor.left << ",";
        json << "\"y\":" << monitorInfo.rcMonitor.top << ",";
        json << "\"width\":" << (monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left) << ",";
        json << "\"height\":" << (monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top);
        json << "},";
        json << "\"workArea\":{";
        json << "\"x\":" << monitorInfo.rcWork.left << ",";
        json << "\"y\":" << monitorInfo.rcWork.top << ",";
        json << "\"width\":" << (monitorInfo.rcWork.right - monitorInfo.rcWork.left) << ",";
        json << "\"height\":" << (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
        json << "},";
        json << "\"scaleFactor\":" << scaleFactor << ",";
        json << "\"isPrimary\":" << (isPrimary ? "true" : "false");
        json << "}";

        data->displays.push_back(json.str());
    }

    return TRUE;  // Continue enumeration
}

// Get all displays as JSON array
extern "C" ELECTROBUN_EXPORT const char* getAllDisplays() {
    MonitorEnumData data;

    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&data));

    // Build JSON array
    std::ostringstream result;
    result << "[";
    for (size_t i = 0; i < data.displays.size(); i++) {
        if (i > 0) result << ",";
        result << data.displays[i];
    }
    result << "]";

    return _strdup(result.str().c_str());
}

// Callback for finding primary display
struct PrimaryMonitorData {
    std::string json;
    bool found;
};

static BOOL CALLBACK PrimaryMonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    PrimaryMonitorData* data = reinterpret_cast<PrimaryMonitorData*>(dwData);

    MONITORINFOEX monitorInfo;
    monitorInfo.cbSize = sizeof(MONITORINFOEX);

    if (GetMonitorInfo(hMonitor, &monitorInfo)) {
        if (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) {
            // Get DPI/scale factor
            double scaleFactor = 1.0;
            HMODULE shcore = LoadLibraryW(L"Shcore.dll");
            if (shcore) {
                typedef HRESULT(WINAPI *GetDpiForMonitorFunc)(HMONITOR, int, UINT*, UINT*);
                GetDpiForMonitorFunc getDpi = (GetDpiForMonitorFunc)GetProcAddress(shcore, "GetDpiForMonitor");
                if (getDpi) {
                    UINT dpiX, dpiY;
                    if (SUCCEEDED(getDpi(hMonitor, 0, &dpiX, &dpiY))) {
                        scaleFactor = dpiX / 96.0;
                    }
                }
                FreeLibrary(shcore);
            }

            std::ostringstream json;
            json << "{";
            json << "\"id\":" << reinterpret_cast<uintptr_t>(hMonitor) << ",";
            json << "\"bounds\":{";
            json << "\"x\":" << monitorInfo.rcMonitor.left << ",";
            json << "\"y\":" << monitorInfo.rcMonitor.top << ",";
            json << "\"width\":" << (monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left) << ",";
            json << "\"height\":" << (monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top);
            json << "},";
            json << "\"workArea\":{";
            json << "\"x\":" << monitorInfo.rcWork.left << ",";
            json << "\"y\":" << monitorInfo.rcWork.top << ",";
            json << "\"width\":" << (monitorInfo.rcWork.right - monitorInfo.rcWork.left) << ",";
            json << "\"height\":" << (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
            json << "},";
            json << "\"scaleFactor\":" << scaleFactor << ",";
            json << "\"isPrimary\":true";
            json << "}";

            data->json = json.str();
            data->found = true;
            return FALSE;  // Stop enumeration
        }
    }

    return TRUE;  // Continue enumeration
}

// Get primary display as JSON
extern "C" ELECTROBUN_EXPORT const char* getPrimaryDisplay() {
    PrimaryMonitorData data;
    data.found = false;

    EnumDisplayMonitors(NULL, NULL, PrimaryMonitorEnumProc, reinterpret_cast<LPARAM>(&data));

    if (data.found) {
        return _strdup(data.json.c_str());
    }

    return _strdup("{}");
}

// Get current cursor position as JSON: {"x": 123, "y": 456}
extern "C" ELECTROBUN_EXPORT const char* getCursorScreenPoint() {
    POINT cursorPos;
    if (GetCursorPos(&cursorPos)) {
        std::ostringstream json;
        json << "{\"x\":" << cursorPos.x << ",\"y\":" << cursorPos.y << "}";
        return _strdup(json.str().c_str());
    }

    return _strdup("{\"x\":0,\"y\":0}");
}

extern "C" ELECTROBUN_EXPORT uint64_t getMouseButtons() {
    uint64_t buttons = 0;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) buttons |= 1ull << 0;
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) buttons |= 1ull << 1;
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) buttons |= 1ull << 2;
    return buttons;
}


// URL scheme handler - macOS only, stub for Windows
extern "C" ELECTROBUN_EXPORT void setURLOpenHandler(void (*callback)(const char*)) {
    (void)callback;
    // Not supported on Windows - stub to prevent dlopen failure
    // Windows URL protocol handling is done via registry
}

// App reopen handler - macOS only, stub for Windows
extern "C" ELECTROBUN_EXPORT void setAppReopenHandler(void (*callback)()) {
    (void)callback;
    // Not supported on Windows - stub to prevent dlopen failure
}

// Dock icon visibility - macOS only, stubs for Windows
extern "C" ELECTROBUN_EXPORT void setDockIconVisible(bool visible) {
    (void)visible;
    // Not supported on Windows - stub to prevent dlopen failure
}

extern "C" ELECTROBUN_EXPORT bool isDockIconVisible() {
    // Not supported on Windows
    return true;
}
