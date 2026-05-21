#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <windowsx.h>  // GET_X/Y_LPARAM
#include <winsock2.h>
#include <ws2tcpip.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <d3dcompiler.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "vendor/nlohmann/json.hpp"
#include "frame-output/protocols/ddp/ddp_output.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// UDP probe
// ---------------------------------------------------------------------------
static void ProbeUdpSocket(const std::string& host, uint16_t port) {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        fprintf(stderr, "  probe: socket() failed: %d\n", WSAGetLastError());
        return;
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &dest.sin_addr) != 1) {
        fprintf(stderr, "  probe: inet_pton failed for '%s'\n", host.c_str());
        closesocket(s);
        return;
    }

    if (connect(s, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == 0) {
        sockaddr_in local{};
        int len = sizeof(local);
        getsockname(s, reinterpret_cast<sockaddr*>(&local), &len);
        char localIp[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &local.sin_addr, localIp, sizeof(localIp));
        printf("  probe: routing %s:%d  via local interface %s\n", host.c_str(), port, localIp);
    } else {
        fprintf(stderr, "  probe: connect() failed: %d  (no route to host?)\n", WSAGetLastError());
    }

    uint8_t ddpProbe[13]{};
    ddpProbe[0] = 0x41; ddpProbe[1] = 0x01; ddpProbe[2] = 0x0b; ddpProbe[3] = 0x01;
    ddpProbe[8] = 0x00; ddpProbe[9] = 0x03;
    ddpProbe[10] = 0xff;
    const int sent = sendto(s, reinterpret_cast<const char*>(ddpProbe), sizeof(ddpProbe), 0,
                            reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    if (sent == SOCKET_ERROR)
        fprintf(stderr, "  probe: sendto failed: %d\n", WSAGetLastError());
    else
        printf("  probe: sent 1-pixel DDP test packet -> %s:%d\n", host.c_str(), port);

    closesocket(s);
}

// ---------------------------------------------------------------------------
// BMP save
// ---------------------------------------------------------------------------
static bool writeBgraToBmp(const std::string& path, const uint8_t* bgra, int w, int h) {
    if (!bgra || w <= 0 || h <= 0) return false;
    const size_t rowBytes = (size_t)w * 4;
    const size_t pixelBytes = rowBytes * (size_t)h;

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType    = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize    = (DWORD)(fh.bfOffBits + pixelBytes);
    ih.biSize    = sizeof(BITMAPINFOHEADER);
    ih.biWidth   = w;
    ih.biHeight  = h; // bottom-up
    ih.biPlanes  = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage   = (DWORD)pixelBytes;

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    out.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
    for (int y = h - 1; y >= 0; --y)
        out.write(reinterpret_cast<const char*>(bgra + y * rowBytes), rowBytes);
    return out.good();
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
using namespace chromeyumm::frame_output;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

static std::atomic<bool> g_stop{false};
static std::atomic<int>  g_capX{0};
static std::atomic<int>  g_capY{0};
static std::atomic<HWND> g_overlayHwnd{nullptr};

BOOL WINAPI CtrlHandler(DWORD) {
    g_stop.store(true, std::memory_order_relaxed);
    HWND overlay = g_overlayHwnd.load(std::memory_order_relaxed);
    if (overlay) PostMessage(overlay, WM_CLOSE, 0, 0);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Shader compiler helper
// ---------------------------------------------------------------------------
static ComPtr<ID3DBlob> CompileShader(const std::string& source, const char* entrypoint, const char* target) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(
        source.c_str(), source.size(), nullptr, nullptr, nullptr,
        entrypoint, target, 0, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) {
            fprintf(stderr, "Shader compilation error: %s\n", (const char*)errors->GetBufferPointer());
        } else {
            fprintf(stderr, "Shader compilation error HRESULT: 0x%08X\n", hr);
        }
        return nullptr;
    }
    return code;
}

// ---------------------------------------------------------------------------
// Capture region overlay window
// ---------------------------------------------------------------------------
static constexpr int      BORDER_PX    = 6;
static constexpr COLORREF BORDER_COLOR = RGB(255, 0, 220);  // magenta
static constexpr COLORREF CHROMA_KEY   = RGB(1, 1, 1);      // near-black -> transparent

struct OverlayCtx {
    int monOffX, monOffY;
    int capW, capH;
    int desktopW, desktopH;
    std::string configPath;
};

static void SaveCapturePos(const std::string& configPath, int x, int y) {
    std::ifstream fi(configPath);
    if (!fi.is_open()) return;
    json cfg;
    try { cfg = json::parse(fi, nullptr, true, true); } catch (...) { return; }
    fi.close();
    if (!cfg.contains("screenCapture") || !cfg["screenCapture"].contains("captureRect")) return;
    cfg["screenCapture"]["captureRect"]["x"] = x;
    cfg["screenCapture"]["captureRect"]["y"] = y;
    std::ofstream fo(configPath);
    if (fo.is_open()) fo << cfg.dump(2);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OverlayCtx* ctx = reinterpret_cast<OverlayCtx*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_ERASEBKGND:
        return TRUE;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(CHROMA_KEY);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        HBRUSH border = CreateSolidBrush(BORDER_COLOR);
        RECT edges[4] = {
            {0,                  0,                  rc.right,          BORDER_PX},
            {0,                  rc.bottom-BORDER_PX, rc.right,         rc.bottom},
            {0,                  0,                  BORDER_PX,         rc.bottom},
            {rc.right-BORDER_PX, 0,                  rc.right,          rc.bottom},
        };
        for (auto& e : edges) FillRect(hdc, &e, border);
        DeleteObject(border);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        RECT rc; GetClientRect(hwnd, &rc);
        if (pt.x < BORDER_PX || pt.y < BORDER_PX ||
            pt.x >= rc.right - BORDER_PX || pt.y >= rc.bottom - BORDER_PX)
            return HTCAPTION;
        return HTTRANSPARENT;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCAPTION) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        return DefWindowProc(hwnd, msg, wp, lp);

    case WM_EXITSIZEMOVE: {
        if (ctx) {
            RECT rc; GetWindowRect(hwnd, &rc);
            int nx = rc.left - ctx->monOffX;
            int ny = rc.top  - ctx->monOffY;
            nx = std::max(0, std::min(nx, ctx->desktopW - ctx->capW));
            ny = std::max(0, std::min(ny, ctx->desktopH - ctx->capH));
            // Snap the window to the clamped position.
            SetWindowPos(hwnd, nullptr, ctx->monOffX + nx, ctx->monOffY + ny, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            g_capX.store(nx, std::memory_order_relaxed);
            g_capY.store(ny, std::memory_order_relaxed);
            printf("screen-ddp: region moved -> [%d,%d %dx%d]  (saved to config)\n",
                   nx, ny, ctx->capW, ctx->capH);
            SaveCapturePos(ctx->configPath, nx, ny);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_stop.store(true, std::memory_order_relaxed);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void RunOverlay(OverlayCtx ctx, int initX, int initY) {
    HICON hIcon   = (HICON)LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(1), IMAGE_ICON,
                                     0, 0, LR_DEFAULTSIZE);
    HICON hIconSm = (HICON)LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(1), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = L"ScreenDdpOverlay";
    wc.hIcon         = hIcon;
    wc.hIconSm       = hIconSm;
    if (!RegisterClassExW(&wc)) {
        fprintf(stderr, "screen-ddp: overlay RegisterClassExW failed: %d\n", GetLastError());
        return;
    }

    const int wx = ctx.monOffX + initX;
    const int wy = ctx.monOffY + initY;
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_APPWINDOW,
        L"ScreenDdpOverlay", L"DDP Screen Capture",
        WS_POPUP,
        wx, wy, ctx.capW, ctx.capH,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!hwnd) {
        fprintf(stderr, "screen-ddp: overlay CreateWindowExW failed: %d  (pos=%d,%d size=%dx%d)\n",
                GetLastError(), wx, wy, ctx.capW, ctx.capH);
        return;
    }

    static OverlayCtx s_ctx;
    s_ctx = ctx;
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s_ctx));
    SetLayeredWindowAttributes(hwnd, CHROMA_KEY, 0, LWA_COLORKEY);
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    printf("screen-ddp: overlay window at (%d,%d) size %dx%d - magenta border should be visible there\n",
           wx, wy, ctx.capW, ctx.capH);
    g_overlayHwnd.store(hwnd, std::memory_order_relaxed);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_overlayHwnd.store(nullptr, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Physical-pixel coordinates throughout - required so the overlay window
    // aligns with the DXGI capture rect (which is always in physical pixels).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleTitleW(L"DDP Screen Capture");

    const char* configPath = "display-config.json";
    std::string saveFramePath;
    bool showRegion = false;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--save-frame") {
            saveFramePath = (i+1 < argc && argv[i+1][0] != '-') ? argv[++i] : "screen-ddp-capture.bmp";
        } else if (arg == "--show-region") {
            showRegion = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (configPath == std::string("display-config.json")) {
            configPath = argv[i];
        }
    }

    std::ifstream f(configPath);
    if (!f.is_open()) {
        fprintf(stderr, "screen-ddp: cannot open config: %s\n", configPath);
        return 1;
    }
    json cfg;
    try {
        cfg = json::parse(f, nullptr, true, true);
    } catch (const std::exception& e) {
        fprintf(stderr, "screen-ddp: config parse error: %s\n", e.what());
        return 1;
    }

    if (!cfg.contains("screenCapture")) {
        fprintf(stderr, "screen-ddp: config missing 'screenCapture' block\n");
        return 1;
    }

    auto& sc      = cfg["screenCapture"];
    const int mon = sc.value("monitor", 0);
    const int cvW = sc.value("canvasWidth", 0);
    const int cvH = sc.value("canvasHeight", 0);
    const int fps = std::max(1, sc.value("targetFps", 60));
    int capX = 0, capY = 0, capW = 0, capH = 0;
    if (sc.contains("captureRect")) {
        auto& cr = sc["captureRect"];
        capX = cr.value("x", 0);
        capY = cr.value("y", 0);
        capW = cr.value("width", 0);
        capH = cr.value("height", 0);
    }

    if (cvW <= 0 || cvH <= 0 || capW <= 0 || capH <= 0) {
        fprintf(stderr, "screen-ddp: invalid dimensions in screenCapture\n");
        return 1;
    }

    std::vector<std::unique_ptr<DdpOutput>> outputs;
    if (cfg.contains("ddpOutputs")) {
        for (auto& o : cfg["ddpOutputs"]) {
            if (!o.value("enabled", true)) continue;
            DdpOutputConfig dc;
            dc.controllerAddress = o.value("controllerAddress", "");
            dc.port              = (uint16_t)o.value("port", 4048);
            const uint8_t rawDst = (uint8_t)o.value("destinationId", 0);
            dc.destinationId     = rawDst ? rawDst : 0x01; // 0 -> default 1, matches chromeyumm
            dc.pixelStart        = o.value("pixelStart", 0);
            dc.zigZagRows        = o.value("zigZagRows", false);
            dc.flipH             = o.value("flipH", false);
            dc.flipV             = o.value("flipV", false);
            dc.rotate            = o.value("rotate", 0);
            if (o.contains("source")) {
                auto& s = o["source"];
                dc.sourceRect.x      = s.value("x", 0);
                dc.sourceRect.y      = s.value("y", 0);
                dc.sourceRect.width  = s.value("width", 0);
                dc.sourceRect.height = s.value("height", 0);
            }
            dc.debugLog = verbose;
            if (dc.controllerAddress.empty()) continue;
            printf("  output: %-30s -> %s:%d\n",
                o.value("label", dc.controllerAddress).c_str(),
                dc.controllerAddress.c_str(), dc.port);
            outputs.push_back(std::make_unique<DdpOutput>(dc));
        }
    }

    if (outputs.empty()) {
        fprintf(stderr, "screen-ddp: no DDP outputs found in config\n");
        return 1;
    }

    auto& first = cfg["ddpOutputs"][0];
    ProbeUdpSocket(first.value("controllerAddress", ""), (uint16_t)first.value("port", 4048));

    for (auto& out : outputs) {
        if (!out->Start()) {
            fprintf(stderr, "screen-ddp: failed to start a DDP output\n");
            return 1;
        }
    }

    // Find the adapter that owns monitor 'mon' by counting outputs across all
    // adapters. On laptops with hybrid GPUs each adapter only exposes its own
    // outputs, so D3D_DRIVER_TYPE_HARDWARE (default adapter) may not have mon > 0.
    ComPtr<IDXGIFactory1> dxgiFactory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) {
        fprintf(stderr, "screen-ddp: CreateDXGIFactory1 failed: 0x%08X\n", hr);
        return 1;
    }

    ComPtr<IDXGIAdapter> targetAdapter;
    ComPtr<IDXGIOutput>  dxgiOut;
    int totalOutputs = 0;
    for (UINT ai = 0; dxgiFactory->EnumAdapters(ai, &targetAdapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        for (UINT oi = 0; ; ++oi) {
            ComPtr<IDXGIOutput> out;
            if (FAILED(targetAdapter->EnumOutputs(oi, &out))) break;
            if (totalOutputs == mon) { dxgiOut = out; goto found_output; }
            ++totalOutputs;
        }
        targetAdapter.Reset();
    }
    fprintf(stderr, "screen-ddp: monitor %d not found (%d output(s) detected across all adapters)\n",
            mon, totalOutputs);
    return 1;
found_output:

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    // Must use D3D_DRIVER_TYPE_UNKNOWN when supplying an explicit adapter.
    hr = D3D11CreateDevice(
        targetAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "screen-ddp: D3D11CreateDevice failed: 0x%08X\n", hr);
        return 1;
    }

    ComPtr<IDXGIOutput1> dxgiOut1;  dxgiOut.As(&dxgiOut1);

    // Get the monitor's virtual-desktop offset so we can position the overlay window.
    DXGI_OUTPUT_DESC outDesc{};
    dxgiOut->GetDesc(&outDesc);
    const int monOffX = outDesc.DesktopCoordinates.left;
    const int monOffY = outDesc.DesktopCoordinates.top;

    int desktopW = 0, desktopH = 0;
    auto initDup = [&](ComPtr<IDXGIOutputDuplication>& dup) -> bool {
        HRESULT r = dxgiOut1->DuplicateOutput(device.Get(), &dup);
        if (FAILED(r)) {
            fprintf(stderr, "screen-ddp: DuplicateOutput failed: 0x%08X\n", r);
            return false;
        }
        DXGI_OUTDUPL_DESC desc{};
        dup->GetDesc(&desc);
        if (desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            fprintf(stderr, "screen-ddp: unsupported desktop format 0x%X\n", desc.ModeDesc.Format);
            return false;
        }
        desktopW = (int)desc.ModeDesc.Width;
        desktopH = (int)desc.ModeDesc.Height;
        return true;
    };

    ComPtr<IDXGIOutputDuplication> dup;
    if (!initDup(dup)) return 1;

    D3D11_TEXTURE2D_DESC stgDesc{};
    stgDesc.Width            = (UINT)cvW;
    stgDesc.Height           = (UINT)cvH;
    stgDesc.MipLevels        = stgDesc.ArraySize = 1;
    stgDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    stgDesc.SampleDesc.Count = 1;
    stgDesc.Usage            = D3D11_USAGE_STAGING;
    stgDesc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    hr = device->CreateTexture2D(&stgDesc, nullptr, &staging);
    if (FAILED(hr)) {
        fprintf(stderr, "screen-ddp: staging texture failed: 0x%08X\n", hr);
        return 1;
    }

    D3D11_TEXTURE2D_DESC tempDesc{};
    tempDesc.Width            = (UINT)capW;
    tempDesc.Height           = (UINT)capH;
    tempDesc.MipLevels        = tempDesc.ArraySize = 1;
    tempDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    tempDesc.SampleDesc.Count = 1;
    tempDesc.Usage            = D3D11_USAGE_DEFAULT;
    tempDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tempTex;
    hr = device->CreateTexture2D(&tempDesc, nullptr, &tempTex);
    if (FAILED(hr)) {
        fprintf(stderr, "screen-ddp: failed to create intermediate texture: 0x%08X\n", hr);
        return 1;
    }

    ComPtr<ID3D11ShaderResourceView> tempSRV;
    hr = device->CreateShaderResourceView(tempTex.Get(), nullptr, &tempSRV);
    if (FAILED(hr)) {
        fprintf(stderr, "screen-ddp: failed to create temp SRV: 0x%08X\n", hr);
        return 1;
    }

    D3D11_TEXTURE2D_DESC rtDesc{};
    rtDesc.Width            = (UINT)cvW;
    rtDesc.Height           = (UINT)cvH;
    rtDesc.MipLevels        = rtDesc.ArraySize = 1;
    rtDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Usage            = D3D11_USAGE_DEFAULT;
    rtDesc.BindFlags        = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> rtTex;
    hr = device->CreateTexture2D(&rtDesc, nullptr, &rtTex);
    if (FAILED(hr)) {
        fprintf(stderr, "screen-ddp: failed to create render target texture: 0x%08X\n", hr);
        return 1;
    }

    ComPtr<ID3D11RenderTargetView> rtRTV;
    hr = device->CreateRenderTargetView(rtTex.Get(), nullptr, &rtRTV);
    if (FAILED(hr)) {
        fprintf(stderr, "screen-ddp: failed to create RTV: 0x%08X\n", hr);
        return 1;
    }

    const std::string shaderCode = R"(
        struct VS_OUTPUT {
            float4 position : SV_POSITION;
            float2 texcoord : TEXCOORD;
        };
        VS_OUTPUT VSMain(uint id : SV_VertexID) {
            VS_OUTPUT output;
            output.texcoord = float2((id << 1) & 2, id & 2);
            output.position = float4(output.texcoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
            return output;
        }

        Texture2D shaderTexture : register(t0);
        SamplerState sampleType : register(s0);

        float4 PSMain(VS_OUTPUT input) : SV_TARGET {
            return shaderTexture.Sample(sampleType, input.texcoord);
        }
    )";

    ComPtr<ID3DBlob> vsBlob = CompileShader(shaderCode, "VSMain", "vs_4_0");
    ComPtr<ID3DBlob> psBlob = CompileShader(shaderCode, "PSMain", "ps_4_0");
    if (!vsBlob || !psBlob) {
        return 1;
    }

    ComPtr<ID3D11VertexShader> vertexShader;
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader);
    if (FAILED(hr)) {
        fprintf(stderr, "screen-ddp: creating VertexShader failed: 0x%08X\n", hr);
        return 1;
    }

    ComPtr<ID3D11PixelShader> pixelShader;
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader);
    if (FAILED(hr)) {
        fprintf(stderr, "screen-ddp: creating PixelShader failed: 0x%08X\n", hr);
        return 1;
    }

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> samplerState;
    hr = device->CreateSamplerState(&sampDesc, &samplerState);
    if (FAILED(hr)) {
        fprintf(stderr, "screen-ddp: failed to create sampler state: 0x%08X\n", hr);
        return 1;
    }

    std::vector<uint8_t> canvas((size_t)cvW * cvH * 4);
    FrameContext frameCtx{};
    frameCtx.canvasWidth  = cvW;
    frameCtx.canvasHeight = cvH;

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // Initial check for bounds
    if ((capX + capW) > desktopW || (capY + capH) > desktopH) {
        fprintf(stderr, "screen-ddp: captureRect extends beyond monitor bounds (%dx%d)\n",
                desktopW, desktopH);
        return 1;
    }

    printf("screen-ddp: monitor %d  desktop %dx%d  capture [%d,%d %dx%d] -> canvas %dx%d  %zu output(s)  %d fps\n",
        mon, desktopW, desktopH,
        capX, capY, capW, capH,
        cvW, cvH, outputs.size(), fps);

    g_capX.store(capX, std::memory_order_relaxed);
    g_capY.store(capY, std::memory_order_relaxed);

    // Raise Windows timer resolution to 1 ms so sleep_for is precise at 60 fps.
    // Default is 15.6 ms which causes significant frame-time jitter.
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    if (showRegion) {
        OverlayCtx octx{monOffX, monOffY, capW, capH, desktopW, desktopH, configPath};
        std::thread([octx, capX, capY]() { RunOverlay(octx, capX, capY); }).detach();
        printf("screen-ddp: region overlay active - drag the magenta border to reposition\n");
    }

    printf("Ctrl+C to stop.\n\n");

    const auto frameDur   = std::chrono::microseconds(1'000'000 / fps);
    auto       statsTimer = std::chrono::steady_clock::now();
    uint64_t   framesAcquired = 0;
    uint64_t   timeouts       = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto t0 = std::chrono::steady_clock::now();

        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        hr = dup->AcquireNextFrame(100, &fi, &res);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { ++timeouts; continue; }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            dup.Reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!initDup(dup)) break;
            continue;
        }
        if (FAILED(hr)) {
            fprintf(stderr, "screen-ddp: AcquireNextFrame: 0x%08X\n", hr);
            break;
        }

        ++framesAcquired;

        // LastPresentTime == 0 means only the cursor moved - no pixel content changed.
        // DdpOutput's keepalive handles retransmission; skip the copy/scale/send pipeline.
        if (fi.LastPresentTime.QuadPart == 0) {
            dup->ReleaseFrame();
            continue;
        }

        ComPtr<ID3D11Texture2D> deskTex;
        if (FAILED(res.As(&deskTex))) {
            fprintf(stderr, "screen-ddp: failed to get desktop texture\n");
            dup->ReleaseFrame();
            continue;
        }

        // Read the live capture position - may have been updated by overlay drag.
        const int cx = g_capX.load(std::memory_order_relaxed);
        const int cy = g_capY.load(std::memory_order_relaxed);

        D3D11_BOX box{};
        box.left   = (UINT)cx;
        box.top    = (UINT)cy;
        box.front  = 0;
        box.right  = (UINT)(cx + capW);
        box.bottom = (UINT)(cy + capH);
        box.back   = 1;
        ctx->CopySubresourceRegion(tempTex.Get(), 0, 0, 0, 0, deskTex.Get(), 0, &box);
        dup->ReleaseFrame();

        // Downscale subregion to canvas size on the GPU
        ctx->OMSetRenderTargets(1, rtRTV.GetAddressOf(), nullptr);

        D3D11_VIEWPORT vp{};
        vp.Width = (float)cvW;
        vp.Height = (float)cvH;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        ctx->RSSetViewports(1, &vp);

        ID3D11ShaderResourceView* srvs[] = { tempSRV.Get() };
        ctx->PSSetShaderResources(0, 1, srvs);
        ctx->PSSetSamplers(0, 1, samplerState.GetAddressOf());
        ctx->VSSetShader(vertexShader.Get(), nullptr, 0);
        ctx->PSSetShader(pixelShader.Get(), nullptr, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ctx->Draw(3, 0);

        // Reset bindings
        ID3D11ShaderResourceView* nullSRVs[] = { nullptr };
        ctx->PSSetShaderResources(0, 1, nullSRVs);

        // Copy render target results to the cpu-readable staging texture
        ctx->CopyResource(staging.Get(), rtTex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            fprintf(stderr, "screen-ddp: Map failed: 0x%08X\n", hr);
            continue;
        }

        // Direct row copy of downscaled frame from the staging texture, accounting for row padding
        const auto* src = static_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < cvH; ++y) {
            memcpy(canvas.data() + (size_t)y * cvW * 4,
                   src + (size_t)y * mapped.RowPitch, (size_t)cvW * 4);
        }
        ctx->Unmap(staging.Get(), 0);

        if (!saveFramePath.empty()) {
            uint32_t sum = 0;
            const int chk = std::min(64, cvW * cvH);
            for (int i = 0; i < chk; ++i) { const uint8_t* p = canvas.data()+i*4; sum += p[0]+p[1]+p[2]; }
            if (sum > 0) {
                if (writeBgraToBmp(saveFramePath, canvas.data(), cvW, cvH))
                    printf("screen-ddp: saved frame -> %s  (%dx%d)\n", saveFramePath.c_str(), cvW, cvH);
                else
                    fprintf(stderr, "screen-ddp: failed to write BMP: %s\n", saveFramePath.c_str());
                saveFramePath.clear();
            }
        }

        BgraFrameView view{};
        view.data        = canvas.data();
        view.width       = cvW;
        view.height      = cvH;
        view.strideBytes = cvW * 4;
        for (auto& out : outputs) out->OnFrame(frameCtx, view);
        ++frameCtx.frameId;

        const auto now = std::chrono::steady_clock::now();
        if (verbose && now - statsTimer >= std::chrono::seconds(1)) {
            const auto s = outputs[0]->GetStats();
            uint32_t brightness = 0;
            const int samplePx = std::min(16, cvW * cvH);
            for (int i = 0; i < samplePx; ++i) {
                const uint8_t* p = canvas.data() + i * 4;
                brightness += p[2] + p[1] + p[0];
            }
            brightness /= (uint32_t)samplePx;
            printf("  acquired %-5llu  timeouts %-5llu  ddp frames %-4llu  udp packets %-7llu  keepalives %-5llu  errors %-3llu  brightness ~%u\n",
                (unsigned long long)framesAcquired,
                (unsigned long long)timeouts,
                (unsigned long long)s.framesSent,
                (unsigned long long)s.packetsSent,
                (unsigned long long)s.keepaliveFramesSent,
                (unsigned long long)s.sendErrors,
                brightness);
            framesAcquired = 0;
            timeouts       = 0;
            statsTimer     = now;
        }

        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < frameDur)
            std::this_thread::sleep_for(frameDur - elapsed);
    }

    timeEndPeriod(1);
    printf("screen-ddp: stopping\n");
    for (auto& out : outputs) out->Stop();
    return 0;
}
