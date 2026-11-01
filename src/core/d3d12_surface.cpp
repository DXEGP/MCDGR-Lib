module;

#include "_d3d12_module_prelude.h"
#include "mcdr_dlss_impl.h"  // reflex 前/后 Present 钩子
#include "mcdr_hdr_state.h"  // HDR 模式/激活状态（surface 与 DLSS 两个 TU 共享）
#include "mcdr_surface_opts_state.h"  // allowTearing / presentChild（配置推来）
#include "mcdr_window_host.h"  // SeamlessWindowHost
#include <algorithm>          // std::clamp
#include <cwctype>            // towlower（模块名小写化）
#include <tlhelp32.h>         // Module32First / 进程模块快照

module mcdr.d3d12.device;

import mcdr.error;
// dx12-mc 原生 D3D12 层：DXGI 交换链（GpuSurfaceBackend 对应物）。
// PresentMode 序号 = 官方枚举 ordinal：IMMEDIATE=0, MAILBOX=1, FIFO=2, FIFO_RELAXED=3。
// DXGI FLIP 模型支持 {IMMEDIATE, FIFO, FIFO_RELAXED}，MAILBOX 无直接对应。见 docs/surface/README.md

namespace d3d12_mc {

namespace {

std::string hrText(HRESULT hr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// 抽干当前线程消息队列 ms 毫秒。CreateSwapChainForHwnd 返回 E_ACCESSDENIED
// (0x80070005) 多为瞬态（DWM 未完成重定向 / 覆盖层占用），泵消息后重试即可。
void pumpWindowMessages(int ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(ms > 0 ? ms : 0);
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount() >= deadline) break;
        Sleep(8);
    }
}

// swapchain 创建结果（含 DirectComposition 回退资源）。
struct SwapChainBundle {
    ComPtr<IDXGISwapChain1> swapChain;
    bool composition = false;   // true = 由 CreateSwapChainForComposition 创建
    bool allowTearing = false;  // 仅 HWND + ALLOW_TEARING tier 为 true
    ComPtr<IDCompositionDevice> dcompDevice;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;
};

// 动态解析 dcomp.dll 的 DCompositionCreateDevice（避免链接期依赖 dcomp.lib）。
typedef HRESULT(WINAPI* PFN_DCompositionCreateDevice)(IUnknown*, REFIID, void**);
PFN_DCompositionCreateDevice resolveDCompCreateDevice() {
    static PFN_DCompositionCreateDevice fn = []() -> PFN_DCompositionCreateDevice {
        HMODULE mod = LoadLibraryW(L"dcomp.dll");
        if (!mod) return nullptr;
        return reinterpret_cast<PFN_DCompositionCreateDevice>(
            GetProcAddress(mod, "DCompositionCreateDevice"));
    }();
    return fn;
}

// DXGI 工厂统一走原生 CreateDXGIFactory1，再升级成 SL 的代理工厂（eUseDXGIFactoryProxy 要求调用方
// 拿代理工厂建链、接口创建后立刻升级）。不升级 SL 的钩子永不触发：帧生成报 eOk 但实际 0 帧。

// 统一工厂入口：原生创建 + 立刻升级成代理（SL 未启用时是空操作）。
// 包装型 FG 后端（FSR-FG / XeSS-FG）要求应用原生链：fgWantsAppNativeChain() 为真时不升级。
template <typename T>
HRESULT createDxgiFactory(ComPtr<T>& factory) {
    const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    if (fgWantsAppNativeChain()) {
        // 不升级：这条链要交给包装型 FG 后端。SR 侧若选 DLSS+Streamline，由 caps 各自报不可用。
        fgNoteChainOwner(MCDR_FG_CHAIN_APP_NATIVE);
        return hr;
    }
    void* raw = factory.Get();
    if (mcdr_sl_upgrade_interface(&raw) != 0 && raw != nullptr) {
        // 成功时是 replace in place：原生那一个引用已经变成代理的引用，Attach 不 AddRef。
        factory.Attach(static_cast<T*>(raw));
        // 把链归属登记成 SL 代理（供 mcdr_fg_caps.chainOwner 查询）：代理工厂不动 v-table，Present 仍解析到本链。
        fgNoteChainOwner(MCDR_FG_CHAIN_SL_PROXY);
    }
    return hr;
}

// 解绑/绑定 composition visual 的内容。DirectComposition 持有 swapchain 的 backbuffer 引用，
// 因此 ResizeBuffers / 释放 swapchain 前必须先 SetContent(nullptr) + Commit，否则 INVALID_CALL。
mcdr::Result<void> setCompositionContent(IDCompositionVisual* visual, IDCompositionDevice* dev,
                                        IUnknown* content) {
    if (!visual || !dev) {
        return mcdr::err("setCompositionContent", "composition visual/device null");
    }
    HRESULT hr = visual->SetContent(content);
    if (FAILED(hr)) {
        return mcdr::err("setCompositionContent", "IDCompositionVisual::SetContent failed " + hrText(hr));
    }
    hr = dev->Commit();
    if (FAILED(hr)) {
        return mcdr::err("setCompositionContent", "IDCompositionDevice::Commit failed " + hrText(hr));
    }
    return {};
}

// 为本 HWND 建立 DirectComposition 合成链（device/target/visual）。
mcdr::Result<void> initComposition(HWND win, SwapChainBundle& b) {
    PFN_DCompositionCreateDevice createDevice = resolveDCompCreateDevice();
    if (!createDevice) {
        return mcdr::err("initComposition", "dcomp.dll / DCompositionCreateDevice unavailable");
    }
    HRESULT hr = createDevice(nullptr, __uuidof(IDCompositionDevice),
                              reinterpret_cast<void**>(b.dcompDevice.GetAddressOf()));
    if (FAILED(hr)) {
        return mcdr::err("initComposition", "DCompositionCreateDevice failed " + hrText(hr));
    }
    hr = b.dcompDevice->CreateTargetForHwnd(win, TRUE, b.dcompTarget.GetAddressOf());
    if (FAILED(hr)) {
        return mcdr::err("initComposition",
                         "IDCompositionDevice::CreateTargetForHwnd failed " + hrText(hr));
    }
    hr = b.dcompDevice->CreateVisual(b.dcompVisual.GetAddressOf());
    if (FAILED(hr)) {
        return mcdr::err("initComposition", "IDCompositionDevice::CreateVisual failed " + hrText(hr));
    }
    hr = b.dcompTarget->SetRoot(b.dcompVisual.Get());
    if (FAILED(hr)) {
        return mcdr::err("initComposition", "IDCompositionTarget::SetRoot failed " + hrText(hr));
    }
    return {};
}

// 创建 composition swapchain 并把内容绑定到 visual 上。
mcdr::Result<void> createCompositionSwapChain(IDXGIFactory2* factory2, ID3D12CommandQueue* queue, UINT width,
                                             UINT height, DXGI_FORMAT format, SwapChainBundle& b) {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width > 0 ? width : 1;
    sd.Height = height > 0 ? height : 1;
    sd.Format = format;
    sd.Stereo = FALSE;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kSurfaceBufferCount;
    sd.Scaling = DXGI_SCALING_STRETCH;                 // composition 仅支持 STRETCH
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;  // composition 支持 SEQUENTIAL/DISCARD
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;      // composition 不接受 UNSPECIFIED
    sd.Flags = 0;                                      // composition 不支持 ALLOW_TEARING
    HRESULT hr = factory2->CreateSwapChainForComposition(queue, &sd, nullptr, &b.swapChain);
    if (FAILED(hr)) {
        return mcdr::err("createCompositionSwapChain",
                         "CreateSwapChainForComposition failed " + hrText(hr));
    }
    auto bind = setCompositionContent(b.dcompVisual.Get(), b.dcompDevice.Get(), b.swapChain.Get());
    if (!bind) return bind;
    b.composition = true;
    b.allowTearing = false;
    return {};
}

// 呈现子窗口（RTSS/OSD 修复）：被 GL 呈现过的 HWND 会让 CreateSwapChainForHwnd 永久返回
// E_ACCESSDENIED，故给呈现单独造一个子窗口（窗口类不含 CS_OWNDC）。它的 GetDesc1().OutputWindow
// 非空，RTSS/Afterburner 才能把 OSD 画进 back buffer（composition 链的 OutputWindow 恒为空）。
constexpr wchar_t kPresentWindowClass[] = L"MCDR_D3D12_PresentChild";

// 启动/退出"白幕"：把窗口客户区刷成黑色。两个窗口在"尚未呈现任何内容"的窗口期里显示的是
// 未绘制的白面，而 swapchain 画面无法搬回 GDI 表面 —— 黑幕是安全默认值。
static void paintWindowBlack(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) return;
    HDC dc = GetDC(hwnd);
    if (dc == nullptr) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    ReleaseDC(hwnd, dc);
}

// 销毁呈现子窗口前把父窗口刷黑：子窗口消失后露出的是父窗口从未绘制过的客户区。
static void paintParentBlackBeforeDestroy(HWND child) {
    if (child == nullptr) return;
    paintWindowBlack(GetParent(child));
}

static void logWindowState(const char* tag, HWND hwnd) {
    if (hwnd == nullptr) {
        dbgLogInfo("%s: hwnd=null", tag);
        return;
    }
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    auto brush = reinterpret_cast<void*>(GetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND));
    dbgLogInfo("%s: hwnd=%p %ls %ld,%ld %ldx%ld vis=%d iconic=%d brush=%p", tag,
               reinterpret_cast<void*>(hwnd), cls, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
               static_cast<int>(IsWindowVisible(hwnd)), static_cast<int>(IsIconic(hwnd)), brush);
}

// 呈现子窗口的消息处理。三条都必须有，否则子窗口会破坏游戏输入/画面：
//   - WM_NCHITTEST == HTTRANSPARENT：WS_EX_TRANSPARENT 不参与命中测试，否则子窗口吃掉全部
//     鼠标消息（"游戏界面点不动"）。
//   - WM_ERASEBKGND == 1：不擦背景，否则 resize 时 swapchain 重新出画前整片闪黑。
//   - WM_MOUSEACTIVATE == MA_NOACTIVATE：点击穿透时不要抢父窗口（GLFW）的激活焦点。
LRESULT CALLBACK presentationWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

HWND createPresentationWindow(HWND parent, UINT width, UINT height) {
    if (!parent) return nullptr;
    static const bool registered = []() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;  // 关键：不能带 CS_OWNDC
        wc.lpfnWndProc = presentationWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 不设背景刷子：内容只来自 swapchain，任何系统擦除都会先露出黑底。
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kPresentWindowClass;
        if (RegisterClassExW(&wc)) return true;
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) return nullptr;
    const int w = width > 0 ? static_cast<int>(width) : 1;
    const int h = height > 0 ? static_cast<int>(height) : 1;
    // WS_EX_TRANSPARENT：命中测试跳过本窗口，鼠标消息仍由父窗口（GLFW）接收。
    HWND child = CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY, kPresentWindowClass,
                                 L"MCDR D3D12 present", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, w,
                                 h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!child) return nullptr;
    // 第一帧 Present 之前是一块未绘制的白面，先刷黑让启动期显示黑幕而不是白幕。
    paintWindowBlack(child);
    paintWindowBlack(parent);
    SetWindowPos(child, HWND_TOP, 0, 0, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return child;
}

void resizePresentationWindow(HWND child, UINT width, UINT height) {
    if (!child || width == 0 || height == 0) return;
    SetWindowPos(child, HWND_TOP, 0, 0, static_cast<int>(width), static_cast<int>(height),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RECT rc{};
    GetWindowRect(child, &rc);
    dbgLogDebug("resizePresentationWindow: %p %ux%u vis=%d rect=%dx%d",
           reinterpret_cast<void*>(child), width, height, IsWindowVisible(child) ? 1 : 0,
           static_cast<int>(rc.right - rc.left), static_cast<int>(rc.bottom - rc.top));
}

// 为窗口创建 swapchain（三档回退）：1) CreateSwapChainForHwnd 多 tier（FLIP_DISCARD±TEARING /
// FLIP_SEQUENTIAL）重试 3 轮；2) 全被拒时回退 CreateSwapChainForComposition + DirectComposition。
// BufferCount 恒为 3；composition 只能复用不能重建（重复 CreateTargetForHwnd 返回 ALREADY_COMPOSED）。
static void logOverlayModules(const char* tag) {
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (::Module32FirstW(snap, &me) != 0) {
        do {
            std::wstring lower(me.szModule);
            for (auto& c : lower) c = static_cast<wchar_t>(::towlower(c));
            if (lower.find(L"d3d12") == std::wstring::npos &&
                lower.find(L"dxgi") == std::wstring::npos &&
                lower.find(L"rtss") == std::wstring::npos &&
                lower.find(L"interposer") == std::wstring::npos &&
                lower.find(L"nvngx") == std::wstring::npos) {
                continue;
            }
            dbgLog("%s module %ls base=%p size=%lu path=%ls", tag, me.szModule,
                   reinterpret_cast<void*>(me.modBaseAddr), static_cast<unsigned long>(me.modBaseSize),
                   me.szExePath);
        } while (::Module32NextW(snap, &me) != 0);
    }
    ::CloseHandle(snap);
}

static void logSwapChainPresentHook(IDXGISwapChain* sc) {
    if (sc == nullptr) return;
    void** vtbl = *reinterpret_cast<void***>(sc);
    if (vtbl == nullptr) return;
    void* present = vtbl[8];
    if (present == nullptr) return;
    HMODULE owner = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(present), &owner);
    std::wstring ownerPath(MAX_PATH, L'\0');
    const DWORD n = owner != nullptr ? ::GetModuleFileNameW(owner, ownerPath.data(), MAX_PATH) : 0;
    ownerPath.resize(n);
    std::wstring_view sv(ownerPath);
    const size_t slash = sv.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) sv.remove_prefix(slash + 1);
    const auto* b = reinterpret_cast<const unsigned char*>(present);
    dbgLog("present slot %p owner=%ls %02X%02X%02X%02X",
           present, sv.empty() ? L"(unknown)" : std::wstring(sv).c_str(),
           b[0], b[1], b[2], b[3]);

    const HMODULE rtssHooks = ::GetModuleHandleW(L"RTSSHooks64.dll");
    const HMODULE rtssLoader = ::GetModuleHandleW(L"RTSSHooksLoader64.dll");
    DXGI_SWAP_CHAIN_DESC desc{};
    const bool haveDesc = SUCCEEDED(sc->GetDesc(&desc));
    dbgLog("overlay %p/%p ok=%d out=%p "
           "Format=%d BufferCount=%u SwapEffect=%d Windowed=%d Refresh=%u/%u",
           reinterpret_cast<void*>(rtssHooks), reinterpret_cast<void*>(rtssLoader), haveDesc ? 1 : 0,
           haveDesc ? reinterpret_cast<void*>(desc.OutputWindow) : nullptr,
           haveDesc ? static_cast<int>(desc.BufferDesc.Format) : -1,
           haveDesc ? desc.BufferCount : 0u, haveDesc ? static_cast<int>(desc.SwapEffect) : -1,
           haveDesc ? (desc.Windowed ? 1 : 0) : -1,
           haveDesc ? desc.BufferDesc.RefreshRate.Numerator : 0u,
           haveDesc ? desc.BufferDesc.RefreshRate.Denominator : 0u);
    logOverlayModules("P252");
}

// 查某显示器的「SDR 白」亮度（nits）。DXGI_OUTPUT_DESC1 没有这个值，它属于 display config
// （DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL，单位 1/1000 nits）。
float querySdrWhiteNits(HMONITOR monitor) {
    if (monitor == nullptr) return 0.0f;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return 0.0f;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return 0.0f;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        return 0.0f;
    }
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) return 0.0f;
        return static_cast<float>(white.SDRWhiteLevel) / 1000.0f;
    }
    return 0.0f;
}

// 窗口所在输出的高级颜色状态。只信 DXGI：SDL 的同名属性也是从这份输出信息推出来的。
struct HdrProbe {
    bool hdrOn = false;      // Windows 高级颜色已开启（输出色彩空间是 PQ/HDR10）
    float sdrWhiteNits = 200.0f;  // 系统「SDR 内容亮度」，scRGB 换算的分母
    float maxLuminanceNits = 0.0f;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool found = false;      // 是否成功定位到输出（false 时上面的值都是默认）
};

HdrProbe probeHdrForWindow(IDXGIFactory4* factory, HWND win) {
    HdrProbe probe{};
    if (factory == nullptr || win == nullptr) return probe;

    // 窗口客户区中心点落在哪块输出上 —— 多显示器下这是"这个窗口现在在哪块屏"的判据。
    POINT center{0, 0};
    RECT cr{};
    if (GetClientRect(win, &cr)) {
        center.x = (cr.right - cr.left) / 2;
        center.y = (cr.bottom - cr.top) / 2;
        ClientToScreen(win, &center);
    }

    ComPtr<IDXGIOutput> matched;
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(factory->EnumAdapters(a, adapter.GetAddressOf()))) break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(o, output.GetAddressOf()))) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;
            const RECT& r = desc.DesktopCoordinates;
            if (center.x >= r.left && center.x < r.right && center.y >= r.top && center.y < r.bottom) {
                matched = output;
                break;
            }
        }
        if (matched) break;
    }
    if (!matched) return probe;

    ComPtr<IDXGIOutput6> output6;
    if (FAILED(matched.As(&output6))) return probe;

    DXGI_OUTPUT_DESC1 desc1{};
    if (FAILED(output6->GetDesc1(&desc1))) return probe;

    probe.found = true;
    probe.colorSpace = desc1.ColorSpace;
    probe.maxLuminanceNits = desc1.MaxLuminance;
    // SDR 白点来自 display config；查不到时退回 200 nits（Windows HDR 滑块 50% 的默认值）。
    const float sdrWhite = querySdrWhiteNits(desc1.Monitor);
    probe.sdrWhiteNits = sdrWhite > 1.0f ? sdrWhite : 200.0f;
    probe.hdrOn = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                  desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
    return probe;
}

mcdr::Result<void> createSwapChainForWindow(IDXGIFactory4* factory, ID3D12CommandQueue* queue, HWND win,
                                           HWND presentWin, UINT width, UINT height,
                                           const SwapChainBundle* reuse, SwapChainBundle& out,
                                           DXGI_FORMAT format) {
    static constexpr struct {
        DXGI_SWAP_EFFECT effect;
        UINT flags;
        const char* label;
    } kTiers[] = {
        {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, "FLIP_DISCARD+TEARING"},
        {DXGI_SWAP_EFFECT_FLIP_DISCARD, 0, "FLIP_DISCARD"},
        {DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 0, "FLIP_SEQUENTIAL"},
    };

    // RTSS/OSD 修复：优先把 swapchain 建在自建呈现子窗口上（GL 呈现过的父窗口会被永久拒绝）。
    const HWND chainWin = presentWin != nullptr ? presentWin : win;

    if (!reuse || !reuse->composition) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        // resize 修复（关键）：必须用调用方给的目标尺寸建链，不能用 1x1 占位 —— flip + 异步 Present 下
        // ResizeBuffers 会一直 INVALID_CALL，重建出的 1x1 链被拉伸铺满窗口（"resize 后黑屏"）。
        sd.Width = width > 0 ? width : 1;
        sd.Height = height > 0 ? height : 1;
        sd.Format = format;
        sd.Stereo = FALSE;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kSurfaceBufferCount;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

        const int kPasses = 3;
        HRESULT lastHr = E_FAIL;
        for (int pass = 0; pass < kPasses && !out.swapChain; ++pass) {
            if (pass > 0) {
                dbgLogError("no hwnd tier, hr=0x%08X, "
                       "retry pass=%d (pump %dms)",
                       static_cast<unsigned>(lastHr), pass, pass * 60);
                pumpWindowMessages(pass * 60);
            }
            for (const auto& tier : kTiers) {
                // 配置关掉 tearing 时跳过第一档：链只能进 DWM 合成翻页（RTSS 的 OSD 只在合成翻页下画得出来）。
                if ((tier.flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0 &&
                    mcdr_surface_opts_allow_tearing() == 0) {
                    continue;
                }
                sd.SwapEffect = tier.effect;
                sd.Flags = tier.flags;
                HRESULT hr =
                    factory->CreateSwapChainForHwnd(queue, chainWin, &sd, nullptr, nullptr, &out.swapChain);
                if (SUCCEEDED(hr)) {
                    out.allowTearing = (tier.flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
                    dbgLogInfo("swapchain ok: tier=%s pass=%d hwnd=%s",
                               tier.label, pass,
                               presentWin != nullptr ? "present-child" : "game-window");
                    // MakeWindowAssociation 只能带 DXGI_MWA_NO_ALT_ENTER（Alt+Enter 会去对子窗口做 SetFullscreenState）。
                    // **不能**带 DXGI_MWA_NO_WINDOW_CHANGES：SetFullscreenState(TRUE) 靠改窗口样式进全屏，
                    // 带上它独占请求直接失败（0x887A0022）。
                    const HRESULT assocHr = factory->MakeWindowAssociation(chainWin, DXGI_MWA_NO_ALT_ENTER);
                    dbgLog("window association hr=%08X",
                           static_cast<unsigned>(assocHr));
                    logSwapChainPresentHook(out.swapChain.Get());
                    break;
                }
                lastHr = hr;
                dbgLog("tier %s failed hr=%08X pass=%d %s", tier.label,
                       static_cast<unsigned>(hr), pass,
                       presentWin != nullptr ? "present-child" : "game-window");
            }
        }
    } else {
        dbgLogInfo("composition chain: recreate directly");
    }

    if (out.swapChain) return {};

    // DirectComposition 回退
    if (win == nullptr) {
        return mcdr::err("createSwapChainForWindow",
                         "swapchain creation failed: HWND invalid, cannot use composition fallback");
    }
    ComPtr<IDXGIFactory2> factory2;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
        return mcdr::err("createSwapChainForWindow",
                         "swapchain creation failed (HWND tiers) and factory lacks IDXGIFactory2");
    }

    if (reuse && reuse->composition) {
        out.dcompDevice = reuse->dcompDevice;
        out.dcompTarget = reuse->dcompTarget;
        out.dcompVisual = reuse->dcompVisual;
        (void)setCompositionContent(out.dcompVisual.Get(), out.dcompDevice.Get(), nullptr);
    } else {
        auto comp = initComposition(win, out);
        if (!comp) {
            return mcdr::err("createSwapChainForWindow",
                             "swapchain HWND tiers failed; composition fallback failed: " +
                                 comp.error().message);
        }
    }
    auto created = createCompositionSwapChain(factory2.Get(), queue, width, height, format, out);
    if (!created) {
        return mcdr::err("createSwapChainForWindow",
                         "swapchain HWND tiers failed; composition fallback failed: " +
                             created.error().message);
    }
    dbgLogInfo("composition swapchain OK (%ux%u)", width, height);
    return {};
}

}  // namespace

mcdr::Result<void*> createSurface(uintptr_t hwnd) {
    DeviceContext& ctx = deviceContext();
    if (!ctx.device || !ctx.queue) {
        return mcdr::err("createSurface", "device not initialized (call dx12CreateDevice first)");
    }

    DXGI_ADAPTER_DESC devDesc{};
    if (!ctx.adapter) {
        return mcdr::err("createSurface", "createSurface: ctx.adapter is null");
    }
    ctx.adapter->GetDesc(&devDesc);
    dbgLogDebug("createSurface: devLuid=%08X%08X desc=%S", devDesc.AdapterLuid.LowPart,
                devDesc.AdapterLuid.HighPart, devDesc.Description);
    HWND win = hwnd != 0 ? reinterpret_cast<HWND>(hwnd) : nullptr;

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = createDxgiFactory(factory);
    if (FAILED(hr)) {
        return mcdr::err("createSurface", "CreateDXGIFactory1 failed " + hrText(hr));
    }
    {
        LUID devLuid = ctx.device->GetAdapterLuid();
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter> adj;
            if (FAILED(factory->EnumAdapters(i, &adj))) break;
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adj->GetDesc(&desc)) && desc.AdapterLuid.LowPart == devLuid.LowPart
                && desc.AdapterLuid.HighPart == devLuid.HighPart) {
                ctx.adapter = adj;
                break;
            }
        }
    }

    // 初始尺寸：composition 回退需要真实客户区尺寸；HWND 路径用 1x1 占位（configureSurface 再 Resize）。
    UINT initW = 1, initH = 1;
    if (win != nullptr) {
        RECT crc{};
        if (GetClientRect(win, &crc) && crc.right > crc.left && crc.bottom > crc.top) {
            initW = static_cast<UINT>(crc.right - crc.left);
            initH = static_cast<UINT>(crc.bottom - crc.top);
        }
    }

    // HDR 判定必须在建链之前 —— 格式是 CreateSwapChainForHwnd 的参数。只信 DXGI 输出；
    // 配置只有 auto/on/off 三态，on 用于系统未开 HDR 时做管线自检。
    const int hdrMode = mcdr_hdr_state_mode();
    const HdrProbe probe = probeHdrForWindow(factory.Get(), win);
    const bool wantHdr = hdrMode == 1 || (hdrMode == 0 && probe.hdrOn);
    // 记录“真正用掉的”模式：JVM 侧拿它和当前选项比较，决定视频设置里要不要提示重启。
    mcdr_hdr_state_set_applied_mode(hdrMode);
    // scRGB：R16G16B16A16_FLOAT + RGB_FULL_G10_NONE_P709（1.0 = 80 nits），线性、无需 PQ 编码。
    const DXGI_FORMAT chainFormat =
        wantHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    dbgLogInfo("hdr mode=%d found=%d on=%d cs=%d sdr=%.1f "
               "max=%.1f nits -> wantHdr=%d chainFormat=%d",
               hdrMode, probe.found ? 1 : 0, probe.hdrOn ? 1 : 0, static_cast<int>(probe.colorSpace),
               probe.sdrWhiteNits, probe.maxLuminanceNits, wantHdr ? 1 : 0,
               static_cast<int>(chainFormat));

    // present 链优先建在 SDL 顶层窗口上：只有顶层窗口才可能进真独占全屏（子窗口会被弹窗化）。
    // 呈现子窗口降级为回退：顶层窗口建链失败时才创建它再试。
    MCDR_WindowHost* host = nullptr;
    HWND presentChild = nullptr;
    if (win != nullptr && mcdr_surface_opts_force_present_child() != 0) {
        host = mcdr_host_create(win, static_cast<int32_t>(initW), static_cast<int32_t>(initH));
        if (host != nullptr) {
            presentChild = reinterpret_cast<HWND>(mcdr_host_child(host));
            dbgLogInfo("seamless host child=%p (%ux%u) parent=%p",
                       reinterpret_cast<void*>(presentChild), initW, initH,
                       reinterpret_cast<void*>(win));
            // 子窗口要等第一帧 Present 才显示，这段窗口期先把父窗口刷黑，避免白闪。
            paintWindowBlack(win);
        } else {
            dbgLogWarn("no seamless host: err=%lu, using game window",
                       static_cast<unsigned long>(GetLastError()));
        }
    }

    // Physical isolation: unbind the current GL context while DXGI creates the chain so the
    // HWND it receives has never been used with OpenGL.
    const bool glWasSuspended = mcdr_host_suspend_gl() != 0;
    SwapChainBundle bundle;
    auto swapChainRes = createSwapChainForWindow(factory.Get(), ctx.queue.Get(), win, presentChild,
                                                initW, initH, nullptr, bundle, chainFormat);
    if (!swapChainRes && presentChild != nullptr) {
        dbgLog("seamless chain failed %s, retry on game window",
               swapChainRes.error().message.c_str());
        mcdr_host_release(host);
        host = nullptr;
        presentChild = nullptr;
        swapChainRes = createSwapChainForWindow(factory.Get(), ctx.queue.Get(), win, nullptr, initW,
                                                initH, nullptr, bundle, chainFormat);
    }
    if (glWasSuspended) mcdr_host_restore_gl();
    if (bundle.composition && presentChild != nullptr) {
        // DirectComposition fallback: the visual composites onto the main window and a child
        // window on top would hide it. Release the host and keep the legacy path.
        dbgLogInfo("composition fallback: release seamless host");
        mcdr_host_release(host);
        host = nullptr;
        presentChild = nullptr;
    }
    ComPtr<IDXGISwapChain1> swapChain1 = bundle.swapChain;

    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain1.As(&swapChain3))) {
        return mcdr::err("createSurface", "swapchain does not support IDXGISwapChain3");
    }

    // 把链切到 scRGB。CheckColorSpaceSupport 必须先过：少数驱动/覆盖层会拒绝，被拒时**只降级不重建链**
    // （float 链在 G22 色彩空间下就是普通 SDR 呈现）。
    bool hdrActive = false;
    if (wantHdr) {
        UINT support = 0;
        const HRESULT csHr = swapChain3->CheckColorSpaceSupport(
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support);
        const bool canPresent =
            SUCCEEDED(csHr) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;
        const HRESULT setHr = canPresent
            ? swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
            : E_FAIL;
        if (SUCCEEDED(setHr)) {
            hdrActive = true;
        } else {
            dbgLogWarn("scRGB rejected 0x%08X/0x%X/0x%08X "
                       "presenting SDR through the float chain",
                       static_cast<unsigned>(csHr), support, static_cast<unsigned>(setHr));
        }
    }

    Dx12Surface* s = new Dx12Surface();
    s->hwnd = hwnd;
    s->presentWindow = presentChild;
    s->host = host;  // 非空 = swapchain 建在自建子窗口上（HWND 路径）
    s->swapChain = swapChain3;
    s->compositionMode = bundle.composition;
    s->dcompDevice = bundle.dcompDevice;
    s->dcompTarget = bundle.dcompTarget;
    s->dcompVisual = bundle.dcompVisual;
    // 记录实际创建是否带 ALLOW_TEARING（composition 链不支持 tearing）。
    s->allowTearing = bundle.allowTearing;
    // HDR 状态与 blit 用的换算系数：scRGB 的 1.0 = 80 nits，SDR 内容要乘
    // clamp(SDRWhiteLevel/80, 1, 10) 才是正确亮度。
    s->format = chainFormat;
    s->swapChainFormat = chainFormat;
    s->hdrActive = hdrActive;
    s->hdrMaxLuminance = probe.maxLuminanceNits;
    s->hdrSdrWhiteScale = hdrActive ? std::clamp(probe.sdrWhiteNits / 80.0f, 1.0f, 10.0f) : 1.0f;
    mcdr_hdr_state_set_active(hdrActive ? 1 : 0);
    if (hdrActive) {
        dbgLogInfo("hdr on: scRGB sdr=%.1f nits "
                   "(scale %.3f), max luminance %.1f nits, presentation window %s",
                   probe.sdrWhiteNits, s->hdrSdrWhiteScale, probe.maxLuminanceNits,
                   presentChild ? "present-child" : "game-window");
    } else {
        dbgLogInfo("HDR inactive: SDR presentation (format=%d)",
                   static_cast<int>(chainFormat));
    }
    // 不在此处 setActiveSurface：surface 必须在 configureSurface 完成后才 active，
    // 否则渲染线程会在 backBuffers 为空时 acquire 导致无限循环。
    return static_cast<void*>(s);
}

// 真独占全屏：26.3 的 Window.applyExclusiveFullscreen() 只调 SDL 改显示模式，DXGI 的链从不进入
// fullscreen state。这里真正调 SetFullscreenState（ON→TRUE / OFF→FALSE）；失败不致命，只记 WARN。
mcdr::Result<void> setExclusiveFullscreen(Dx12Surface* s, bool on) {
    if (s == nullptr) return mcdr::err("setExclusiveFullscreen", "surface is null");
    if (!s->swapChain) return mcdr::err("setExclusiveFullscreen", "surface has no swap chain");
    if (s->compositionMode) {
        return mcdr::err("setExclusiveFullscreen",
                         "composition swap chain cannot enter exclusive fullscreen");
    }
    if (s->host != nullptr) {
        // The chain hangs off a child window; only a top-level swap chain can take the output.
        return mcdr::err("setExclusiveFullscreen",
                         "exclusive fullscreen needs a top-level swap chain (seamless host active)");
    }
    const BOOL want = on ? TRUE : FALSE;
    const HRESULT hr = s->swapChain->SetFullscreenState(want, nullptr);
    BOOL actual = FALSE;
    s->swapChain->GetFullscreenState(&actual, nullptr);
    if (FAILED(hr)) {
        dbgLogWarn("exclusive(%d): hr=0x%08X "
                   "(fullscreenState=%d) — staying windowed",
                   on ? 1 : 0, static_cast<unsigned>(hr), actual ? 1 : 0);
        return mcdr::err("setExclusiveFullscreen", "SetFullscreenState failed: " + hrText(hr));
    }
    s->exclusive = on;
    // 真独占下 DXGI 要求链的窗口是前台窗口；我们绕过 SDL 改状态，SDL 不会提窗 —— 留在别的程序后面时
    // Present 被 DWM 丢弃（画面冻结、presented 计数不动）。显式提到前台。
    if (on && s->hwnd != 0) {
        HWND hwnd = reinterpret_cast<HWND>(s->hwnd);
        ShowWindow(hwnd, SW_SHOW);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
    }
    // SetFullscreenState 之后必须重建 back buffer：旧 flip 序列作废，继续 Present 返回 S_OK 但不再翻页
    // （画面冻结在进全屏前那一帧）。同步做完再返回。
    if (s->width > 0 && s->height > 0) {
        auto resized = configureSurface(s, static_cast<int>(s->width), static_cast<int>(s->height),
                                        s->presentMode, /*force=*/true);
        if (!resized) {
            dbgLogWarn("exclusive(%d): ResizeBuffers failed: %s", on ? 1 : 0,
                       resized.error().message.c_str());
        }
    }
    dbgLogInfo("exclusive(%d) ok state=%d window=%p",
               on ? 1 : 0, actual ? 1 : 0, reinterpret_cast<void*>(s->presentWindow));
    return {};
}

std::vector<int> surfacePresentModes() {
    // IMMEDIATE=0, FIFO=2, FIFO_RELAXED=3（MAILBOX=1 在 DXGI FLIP 下无直接对应）
    return {0, 2, 3};
}

// 链刚建好时问一次 provider 要不要包装：1 = swapChain 已换成代理链；0 = 没包装（应用链保留）；
// -1 = 链已被 SDK 接管但初始化失败（swapChain 已空，调用方必须放弃这台 surface）。
int32_t wrapChainForFrameGen(Dx12Surface* s, DeviceContext& ctx) {

    if (s == nullptr || s->swapChain == nullptr || !ctx.device || !ctx.queue) {
        return 0;
    }
    // 只有"接管交换链"的后端才会吃掉应用链，先问清楚再 Detach；无条件 Detach 会让
    // acquireSurface 的 GetCurrentBackBufferIndex() 撞空指针。
    const int32_t picked = fgPickedBackend();
    if (picked != MCDR_FG_BACKEND_XESSFG && picked != MCDR_FG_BACKEND_FSRFG) {
        return 0;
    }
    // 必须先 Detach() 把所有权交出去：SDK 会 Release 应用链并令该指针失效（计数必须为 1，
    // 否则 init 返回 HRESULT_FAILURE，而我们手里的 ComPtr 也会在 SDK Release 之后悬空）。
    auto* appChain = s->swapChain.Detach();
    void* wrapped = nullptr;
    const int32_t ok = fgWrapPresentChain(
        ctx.device.Get(), ctx.queue.Get(), appChain, static_cast<int32_t>(s->width),
        static_cast<int32_t>(s->height), static_cast<int32_t>(s->swapChainFormat), &wrapped);
    if (ok != 1 || wrapped == nullptr) {
        // 失败后指针同样失效：链没了，本帧不能再 Present，交上层按"surface 需要重建"处理。
        dbgLogWarn("fg skipped app chain; swapchain rebuild");
        s->backBuffers.clear();
        s->currentImageIndex = -1;
        s->lastBlitIndex = -1;
        s->suboptimal = true;
        return -1;
    }
    // 代理链是**另一个**对象：GetSwapChainPtr 交出的是**已加引用**的指针，不能再 QueryInterface，
    // 否则 xefgSwapChainDestroy 报 "reference counter is not 0"，native 链不释放 → 同 HWND 重建 INVALID_CALL。
    // 返回之后所有交换链交互（GetBuffer / CreateRenderTargetView）都必须走 proxy。
    auto* rawProxy = static_cast<IDXGISwapChain4*>(wrapped);
    s->swapChain.Attach(rawProxy);
    // 代理链的 tearing 能力未必与我们请求的一致：Present 的 flags 直接读 s->allowTearing ——
    // 链上没有 ALLOW_TEARING 却传 DXGI_PRESENT_ALLOW_TEARING，DXGI 判 INVALID_CALL。
    DXGI_SWAP_CHAIN_DESC1 proxyDesc{};
    if (SUCCEEDED(rawProxy->GetDesc1(&proxyDesc))) {
        const bool proxyTearing = (proxyDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
        if (proxyTearing != s->allowTearing) {
            dbgLogInfo("proxy allowTearing=%d (app had %d), following proxy",
                       static_cast<int>(proxyTearing), static_cast<int>(s->allowTearing));
        }
        s->allowTearing = proxyTearing;
        dbgLogInfo("proxy chain %ux%u buffers=%u format=%d effect=%d", proxyDesc.Width,
                   proxyDesc.Height, proxyDesc.BufferCount, static_cast<int>(proxyDesc.Format),
                   static_cast<int>(proxyDesc.SwapEffect));
    }
    dbgLogInfo("presenting through the wrapped chain (%ux%u)", s->width, s->height);
    // 包成功后必须把链归属登记成对应的包装家族（createDxgiFactory 只在建链时登记 APP_NATIVE /
    // SL_PROXY）；不补记则每次推设置都会再触发一次"重建链"，同一条 HWND 上反复易主直到 INVALID_CALL。
    if (picked == MCDR_FG_BACKEND_FSRFG) {
        fgNoteChainOwner(MCDR_FG_CHAIN_FSR_WRAP);
    } else {
        fgNoteChainOwner(MCDR_FG_CHAIN_XESS_WRAP);
    }
    return 1;
}
mcdr::Result<void> configureSurface(Dx12Surface* s, int width, int height, int presentMode, bool force) {
    DeviceContext& ctx = deviceContext();
    const double p98t0 = nowMs();
    double p98waitRenderMs = -1.0;
    double p98waitPresentsMs = -1.0;
    double p98idleMs = -1.0;
    if (!s || !s->swapChain) {
        return mcdr::err("configureSurface", "surface not created");
    }
    // 窗口最小化/边框切换瞬间 WM_SIZE 可能传 0 尺寸：ResizeBuffers 对 0 返回 INVALID_CALL，
    // 保持旧尺寸继续，不视为错误。
    if (width <= 0 || height <= 0) {
        s->presentMode = presentMode;
        return {};
    }
    // 防御：DWM 切换/热插拔瞬间会返回错误尺寸。新高度偏差超过 2 倍且宽度不变时视为无效回调，
    // 保留旧尺寸（否则链被重建为错误比例 → 黑屏）。比较前显式收成 int，避免 /W4 C4389。
    const int curW = static_cast<int>(s->width);
    const int curH = static_cast<int>(s->height);
    if (curW > 0 && curH > 0 && width == curW && height > curH * 2) {
        dbgLog("reject height %d (current %d), keep old", height, s->height);
        s->presentMode = presentMode;
        return {};
    }
    s->presentMode = presentMode;
    // 呈现子窗口跟随客户区尺寸（它是 swapchain 的实际宿主窗口）。
    if (s->host != nullptr) {
        mcdr_host_resize(static_cast<MCDR_WindowHost*>(s->host), width, height);
    } else {
        resizePresentationWindow(s->presentWindow, static_cast<UINT>(width), static_cast<UINT>(height));
    }

    // 新尺寸与链一致时跳过 ResizeBuffers。进出真独占时尺寸不变但必须重建 back buffer
    // （SetFullscreenState 后旧 flip 序列作废），由 force 绕过跳过。
    if (!force && static_cast<UINT>(width) == s->width && static_cast<UINT>(height) == s->height) {
        dbgLogInfo("size unchanged %dx%d, skip resize (%.1f ms)", width, height,
               nowMs() - p98t0);
        return {};
    }

    // ResizeBuffers 前必须等 GPU 完全空闲，且把呈现管线静止下来：DWM 仍持有 backbuffer 引用、或异步
    // 渲染线程正在提交时会并发访问同一条链 → NOT_CURRENTLY_AVAILABLE / INVALID_CALL。
    const double p98tWaitRender0 = nowMs();
    waitForRenderThreadSubmit();          // 等异步渲染线程完成当前帧提交
    p98waitRenderMs = nowMs() - p98tWaitRender0;
    if (!waitForPendingPresents(2000)) {  // 等已提交但尚未 Present 完成的帧
        dbgLog("presents not drained before resize");
    }
    p98waitPresentsMs = nowMs() - p98tWaitRender0 - p98waitRenderMs;
    const double p98tIdle0 = nowMs();
    {
        auto idleRes = deviceWaitIdle();
        if (!idleRes) {
            return mcdr::err("configureSurface", "deviceWaitIdle before ResizeBuffers failed: " +
                                                     idleRes.error().message);
        }
    }
    p98idleMs = nowMs() - p98tIdle0;
    dbgLogDebug("configureSurface: idle ok, ResizeBuffers %dx%d mode=%d", width, height, presentMode);
    // 防御：acquire 与 present 之间被 configure 会残留"已 acquire 未 present"的 backbuffer，
    // ResizeBuffers 因此失败 → surface 永久 invalid（画面冻结）。先 Present 释放。
    // 判据必须是原子量 bufferHeld；currentImageIndex 由渲染线程维护，主线程读它是数据竞争，
    // 会对已 present 的 buffer 再推一帧（叠帧 + OSD 双份）。
    if (s->bufferHeld.exchange(false)) {
        dbgLogDebug("release backbuffer idx=%d before resize",
               s->currentImageIndex);
        s->swapChain->Present(0, 0);
        s->currentImageIndex = -1;
    }
    // composition 模式：ResizeBuffers / 释放链前必须先解绑 visual 内容并 Commit，否则 INVALID_CALL。
    if (s->compositionMode) {
        auto unbind = setCompositionContent(s->dcompVisual.Get(), s->dcompDevice.Get(), nullptr);
        if (!unbind) {
            dbgLogError("configureSurface: unbind composition content failed: %s",
                        unbind.error().message.c_str());
        }
    }
    // ResizeBuffers 之前必须释放所有对 back buffer 的引用（DXGI 硬性要求），否则固定返回
    // INVALID_CALL (0x887A0001)；把 s->backBuffers 留到重试之后是"10 次全败再重建"的根因。
    for (auto& bb : s->backBuffers) {
        if (bb) bb.Reset();
    }
    s->backBuffers.clear();
    // 用建链时真正的格式（HDR 下是 R16G16B16A16_FLOAT）；硬编码 R8G8B8A8_UNORM 会让 ResizeBuffers 失败。
    const DXGI_FORMAT scFmt = s->swapChainFormat;
    // ResizeBuffers 的 flags 必须与创建时一致：曾无条件传 ALLOW_TEARING，而"不带 TEARING"的 tier
    // 成功时 allowTearing=false → 每次 INVALID_CALL。composition 链同样必须传 0。
    const UINT scFlags =
        s->compositionMode ? 0 : (s->allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    HRESULT hr = S_OK;
    // deviceWaitIdle 后 DWM 仍可能异步持有 backbuffer 引用（flip + 窗口/全屏切换竞态）：
    // 多次重试 + 递增等待，最多 ~1s。
    dbgLogInfo("configureSurface: target=%ux%u presentWindow=%p", static_cast<unsigned>(width),
               static_cast<unsigned>(height), reinterpret_cast<void*>(s->presentWindow));
    logWindowState("configureSurface child", s->presentWindow);
    logWindowState("configureSurface parent", s->presentWindow ? GetParent(s->presentWindow) : nullptr);
    for (int retry = 0; retry < 10; ++retry) {
        if (retry > 0) {
            int waitMs = (1 << retry);  // 2,4,8,16,32,64,128,256,512 ms
            dbgLogDebug("configureSurface: retry %d after %dms", retry, waitMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        }
        hr = s->swapChain->ResizeBuffers(kSurfaceBufferCount, static_cast<UINT>(width),
                                         static_cast<UINT>(height), scFmt, scFlags);
        if (SUCCEEDED(hr)) {
            if (retry > 0) {
                dbgLogInfo("configureSurface: ResizeBuffers ok after %d retries", retry);
            } else {
                dbgLogDebug("configureSurface: ResizeBuffers ok (retry=0)");
            }
            break;
        }
        // 重试属预期内的 DWM/flip 竞态：首次失败打可见日志，后续降 DEBUG，避免拖动窗口刷满日志。
        if (retry == 0) {
            dbgLog("resize failed %s flags=0x%X tearing=%d, retry",
                   hrText(hr).c_str(), scFlags, static_cast<int>(s->allowTearing));
        } else {
            dbgLogDebug("configureSurface: ResizeBuffers failed %s (retry=%d)", hrText(hr).c_str(),
                        retry);
        }
        // INVALID_CALL 与 NOT_CURRENTLY_AVAILABLE 同源（DWM 异步持有引用），统一重试。
        if (hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE && hr != DXGI_ERROR_INVALID_CALL) break;
    }
    if (FAILED(hr)) {
        // 全部失败时按 VulkanGpuSurface.configure() 的做法销毁旧链再重建（同尺寸、同 HWND）：
        // 必须先 Release 旧链，否则 CreateSwapChainForHwnd 返回 E_ACCESSDENIED。
        dbgLogError("resize failed after 10 retries, recreating");
        // 先 Present 释放残留的 acquired backbuffer（同样只用 bufferHeld 判定）。
        if (s->bufferHeld.exchange(false)) {
            s->swapChain->Present(0, 0);
            s->currentImageIndex = -1;
        }
        (void)deviceWaitIdle();
        // provider 的 context 绑在这条链上，销毁前必须先解包（幂等）。顺序关键：先放掉本进程对链的全部
        // 引用（surface 的 ComPtr + back buffer），再让 provider 销毁；否则 SDK 报 "reference counter is
        // not 0"，native 链不释放、同 HWND 重建即 INVALID_CALL。
        s->backBuffers.clear();
        s->swapChain.Reset();
        fgUnwrapPresentChain();
        // 不清 rtvHandles：句柄在 rtvHeap 上是 bump 分配、可复用；每次重建都重新分配会把
        // kRtvHeapSize 用尽（"RTV descriptor heap exhausted"）。
        s->currentImageIndex = -1;
        s->lastBlitIndex = -1;

        // 重建 swapchain（与 createSurface 同策略：HWND 多 tier + 重试，全被拒则回退 composition）。
        ComPtr<IDXGIFactory4> factory;
        hr = createDxgiFactory(factory);
        if (FAILED(hr)) {
            return mcdr::err("configureSurface", "CreateDXGIFactory1 failed " + hrText(hr));
        }
        // 已是 composition 模式时复用现有 composition 对象（重复 CreateTargetForHwnd 会 WINDOW_ALREADY_COMPOSED）。
        SwapChainBundle reuse;
        if (s->compositionMode) {
            reuse.composition = true;
            reuse.dcompDevice = s->dcompDevice;
            reuse.dcompTarget = s->dcompTarget;
            reuse.dcompVisual = s->dcompVisual;
        }
        SwapChainBundle bundle;
        auto recreated = createSwapChainForWindow(factory.Get(), ctx.queue.Get(),
                                                  reinterpret_cast<HWND>(s->hwnd), s->presentWindow,
                                                  static_cast<UINT>(width), static_cast<UINT>(height),
                                                  s->compositionMode ? &reuse : nullptr, bundle,
                                                  s->swapChainFormat);
        if (!recreated) {
            return mcdr::err("configureSurface", recreated.error().message + " (recreate)");
        }
        if (bundle.composition && s->presentWindow != nullptr) {
            // 重建时退化成 composition：呈现子窗口只会盖住合成画面，销毁它。
            paintParentBlackBeforeDestroy(s->presentWindow);
            DestroyWindow(s->presentWindow);
            s->presentWindow = nullptr;
        }
        if (FAILED(bundle.swapChain.As(&s->swapChain))) {
            return mcdr::err("configureSurface", "swapchain does not support IDXGISwapChain3");
        }
        // As() 只是 QueryInterface（同一对象计数 2）：立刻放掉临时句柄，保证交给 FG 时计数为 1。
        bundle.swapChain.Reset();
        s->compositionMode = bundle.composition;
        s->dcompDevice = bundle.dcompDevice;
        s->dcompTarget = bundle.dcompTarget;
        s->dcompVisual = bundle.dcompVisual;
        s->allowTearing = bundle.allowTearing;
        dbgLogInfo("configureSurface: recreated swapchain %dx%d (composition=%d)", width, height,
               static_cast<int>(s->compositionMode));
    }

    // composition 模式：把 visual 内容重新绑回。
    if (s->compositionMode) {
        auto rebind = setCompositionContent(s->dcompVisual.Get(), s->dcompDevice.Get(), s->swapChain.Get());
        if (!rebind) {
            return mcdr::err("configureSurface",
                             "configureSurface: rebind composition content failed: " +
                                 rebind.error().message);
        }
    }

    // 仅在成功路径更新尺寸：失败时保持旧尺寸，避免后续拿错误尺寸反复 ResizeBuffers。
    s->width = static_cast<UINT>(width);
    s->height = static_cast<UINT>(height);

    // 重新取 back buffers + RTV。
    // 包装必须发生在取 back buffer **之前**：SDK 接管后 GetBuffer / Present 都要走代理链。
    if (wrapChainForFrameGen(s, ctx) < 0) {
        // 包装失败时应用链已被 SDK 销毁，不能直接返回错误（surface 会永久留在"没有链"的状态）。
        // 关掉帧生成（picked 清成 NONE 后不再尝试包装），按纯应用链重建。
        fgAbortWrapAfterFailure(
            "frame generation could not take the swap chain and was turned off; "
            "the swap chain was rebuilt without it");
        auto plain = rebuildSwapChainForFg(s, "wrap-failed");
        if (!plain) {
            return mcdr::err("configureSurface",
                             "recreating a plain swap chain after a failed frame generation wrap "
                             "failed: " + plain.error().message);
        }
        return {};
    }
    s->backBuffers.resize(kSurfaceBufferCount);
    s->surfaceFences.assign(kSurfaceBufferCount, 0);  // 每 back buffer 一份 fence
    // RTV 句柄持久复用（见上文重建分支）。只在本对象还没有句柄时分配。
    if (s->rtvHandles.size() != kSurfaceBufferCount) {
        s->rtvHandles.clear();
        for (UINT i = 0; i < kSurfaceBufferCount; ++i) {
            auto rtv = allocRtvHandle();
            if (!rtv) return std::unexpected<mcdr::Error>(std::move(rtv.error()));
            s->rtvHandles.push_back(rtv.value());
        }
    }
    for (UINT i = 0; i < kSurfaceBufferCount; ++i) {
        hr = s->swapChain->GetBuffer(i, IID_PPV_ARGS(&s->backBuffers[i]));
        if (FAILED(hr)) {
            return mcdr::err("configureSurface", "GetBuffer failed " + hrText(hr));
        }
        ctx.device->CreateRenderTargetView(s->backBuffers[i].Get(), nullptr, s->rtvHandles[i]);
    }
    // 校验：重建/调整后链的真实尺寸必须等于请求尺寸。
    if (!s->backBuffers.empty() && s->backBuffers[0]) {
        const D3D12_RESOURCE_DESC bd0 = s->backBuffers[0]->GetDesc();
        dbgLogDebug("buffer0 %llux%u, requested %dx%d",
               static_cast<unsigned long long>(bd0.Width), static_cast<unsigned>(bd0.Height),
               static_cast<int>(width), static_cast<int>(height));
    }
    // 配置完成后才设为 active surface，确保渲染线程不会在 backBuffers 未就绪时 acquire。
    setActiveSurface(s);
    // resize 是 provider 必须知道的事件（FSR-FG / XeSS-FG 的内部历史要按新尺寸重建），放在 setActiveSurface 之后。
    fgOnResize(static_cast<int32_t>(width), static_cast<int32_t>(height));
    dbgLogInfo("configureSurface: registered active surface=%p bbCount=%u", reinterpret_cast<void*>(s),
               static_cast<UINT>(s->backBuffers.size()));
    {
        const double p98total = nowMs() - p98t0;
        const double p98other = p98total - (p98waitRenderMs < 0.0 ? 0.0 : p98waitRenderMs) -
                                (p98waitPresentsMs < 0.0 ? 0.0 : p98waitPresentsMs) -
                                (p98idleMs < 0.0 ? 0.0 : p98idleMs);
        dbgLogInfo("%dx%d %.1f ms (render=%.1f pres=%.1f idle=%.1f rest=%.1f)",
               width, height, p98total, p98waitRenderMs, p98waitPresentsMs, p98idleMs, p98other);
    }
    return {};
}

// 开关帧生成时按官方检查表重建交换链（同尺寸、同 HWND、同呈现模式），不复用 configureSurface 的
// resize 兜底（那条路径"尺寸没变就跳过"，而 FG 场景恰是尺寸不变只换链）。调用点必须是帧首，
// 不能在 Present 里（会丢掉刚画完的这一帧）；独占全屏一律拒绝（重建 + SetFullscreenState 会冻结）。

mcdr::Result<void> rebuildSwapChainForFg(Dx12Surface* s, const char* why) {
    // 这里**允许** swapChain == nullptr：包装失败时 SDK 已把应用链拿走并销毁（wrapChainForFrameGen
    // 里 Detach 过），调用方正要在这个状态下重建一条纯应用链。
    if (s == nullptr) {
        return mcdr::err("rebuildSwapChainForFg", "surface not created");
    }
    if (s->hwnd == 0) {
        return mcdr::err("rebuildSwapChainForFg", "surface has no window");
    }
    if (s->exclusive) {
        return mcdr::err("rebuildSwapChainForFg",
                         "refused in exclusive fullscreen (rebuild + fullscreen can freeze)");
    }
    DeviceContext& ctx = deviceContext();
    // 与 configureSurface 的重建分支同一收尾顺序：先把残留 acquired back buffer 用 Present(0,0) 交还
    // （只用 bufferHeld 判定），再等 GPU 真空闲 —— 旧链 buffer 仍被引用时 DWM 会拒绝同一 HWND 上的新链。
    const bool held = s->bufferHeld.exchange(false);
    if (held && s->swapChain != nullptr) {
        s->swapChain->Present(0, 0);
    }
    s->currentImageIndex = -1;
    (void)deviceWaitIdle();
    // 同 configureSurface：先放掉本进程全部引用（surface + back buffer），再让 provider 销毁 SDK 对象。
    s->backBuffers.clear();
    s->swapChain.Reset();
    fgUnwrapPresentChain();
    // 不清 rtvHandles：bump 分配可复用，清了反复重建会用尽 kRtvHeapSize。
    s->lastBlitIndex = -1;

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = createDxgiFactory(factory);
    if (FAILED(hr)) {
        return mcdr::err("rebuildSwapChainForFg", "CreateDXGIFactory1 failed " + hrText(hr));
    }
    const UINT w = s->width == 0 ? 1u : s->width;
    const UINT h = s->height == 0 ? 1u : s->height;
    // 包装失败不能把 surface 留在"没有链"的状态（游戏照跑、画面一帧不动）。失败就关掉帧生成，再用同一
    // 条 HWND 重建纯应用链；同 HWND 上旧链销毁是异步的，重试之间要稍等（等不到就 0x887A0001）。
    for (int attempt = 0;; ++attempt) {
        // composition 对象属于 HWND，只能复用不能重建（重复 CreateTargetForHwnd 会 WINDOW_ALREADY_COMPOSED）。
        // 每次尝试都按当前状态重建这份复用句柄：上一次尝试可能刚把模式从 flip 退化成 composition。
        SwapChainBundle reuse;
        if (s->compositionMode) {
            reuse.composition = true;
            reuse.dcompDevice = s->dcompDevice;
            reuse.dcompTarget = s->dcompTarget;
            reuse.dcompVisual = s->dcompVisual;
        }
        SwapChainBundle bundle;
        auto made = createSwapChainForWindow(factory.Get(), ctx.queue.Get(),
                                             reinterpret_cast<HWND>(s->hwnd), s->presentWindow, w, h,
                                             s->compositionMode ? &reuse : nullptr, bundle,
                                             s->swapChainFormat);
        if (!made) {
            return mcdr::err("rebuildSwapChainForFg", made.error().message);
        }
        if (bundle.composition && s->presentWindow != nullptr) {
            // 重建时退化成 composition：呈现子窗口只会盖住合成画面，销毁它。
            paintParentBlackBeforeDestroy(s->presentWindow);
            DestroyWindow(s->presentWindow);
            s->presentWindow = nullptr;
        }
        if (FAILED(bundle.swapChain.As(&s->swapChain))) {
            return mcdr::err("rebuildSwapChainForFg", "swapchain does not support IDXGISwapChain3");
        }
        // As() 之后临时句柄还持有同一对象，放掉它计数才是 1。
        bundle.swapChain.Reset();
        s->compositionMode = bundle.composition;
        s->dcompDevice = bundle.dcompDevice;
        s->dcompTarget = bundle.dcompTarget;
        s->dcompVisual = bundle.dcompVisual;
        s->allowTearing = bundle.allowTearing;
        if (s->compositionMode) {
            auto rebind =
                setCompositionContent(s->dcompVisual.Get(), s->dcompDevice.Get(), s->swapChain.Get());
            if (!rebind) {
                return mcdr::err("rebuildSwapChainForFg", "rebind composition content failed: " +
                                                              rebind.error().message);
            }
        }
        // 重新取 back buffer + RTV（句柄持久复用）。
        // 包装在取 buffer 之前，理由同 configureSurface。
        if (wrapChainForFrameGen(s, ctx) >= 0) {
            break;
        }
        if (attempt >= 2) {
            return mcdr::err("rebuildSwapChainForFg",
                             "frame generation kept failing to take the swap chain");
        }
        fgAbortWrapAfterFailure(
            "frame generation could not take the swap chain and was turned off; "
            "the swap chain was rebuilt without it");
        // 包装失败时 SDK 已经把我们的链销毁了（wrapChainForFrameGen 里 Detach 过）。
        s->backBuffers.clear();
        s->swapChain.Reset();
        (void)deviceWaitIdle();
        Sleep(static_cast<DWORD>(60 * (attempt + 1)));
    }
    s->backBuffers.resize(kSurfaceBufferCount);
    s->surfaceFences.assign(kSurfaceBufferCount, 0);
    if (s->rtvHandles.size() != kSurfaceBufferCount) {
        s->rtvHandles.clear();
        for (UINT i = 0; i < kSurfaceBufferCount; ++i) {
            auto rtv = allocRtvHandle();
            if (!rtv) return std::unexpected<mcdr::Error>(std::move(rtv.error()));
            s->rtvHandles.push_back(rtv.value());
        }
    }
    for (UINT i = 0; i < kSurfaceBufferCount; ++i) {
        const HRESULT gb = s->swapChain->GetBuffer(i, IID_PPV_ARGS(&s->backBuffers[i]));
        if (FAILED(gb)) {
            return mcdr::err("rebuildSwapChainForFg", "GetBuffer failed " + hrText(gb));
        }
        ctx.device->CreateRenderTargetView(s->backBuffers[i].Get(), nullptr, s->rtvHandles[i]);
    }
    setActiveSurface(s);
    // FG 开关引发的重建对 provider 也是一次 resize（同尺寸、只是换了链），provider 必须能接受这种调用。
    fgOnResize(static_cast<int32_t>(w), static_cast<int32_t>(h));
    dbgLogInfo("fg rebuild %ux%u done why=%s comp=%d tearing=%d owner=%d",
               static_cast<unsigned>(w), static_cast<unsigned>(h), why == nullptr ? "?" : why,
               static_cast<int>(s->compositionMode), static_cast<int>(s->allowTearing),
               static_cast<int>(fgChainOwner()));
    return {};
}

mcdr::Result<void> acquireSurface(Dx12Surface* s) {
    {
        static double sP98LastAcquireMs = 0.0;
        const double p98now = nowMs();
        if (sP98LastAcquireMs > 0.0) {
            const double p98gap = p98now - sP98LastAcquireMs;
            if (p98gap > 200.0) {
                dbgLogInfo("main-thread frame hitch: %.1f ms since previous acquire", p98gap);
            }
        }
        sP98LastAcquireMs = p98now;
    }
    if (!s || !s->swapChain) {
        return mcdr::err("acquireSurface", "surface not created");
    }
    // 帧生成挂起的链重建必须在这里消费（帧首、上一帧已上屏、本帧还没开画）。失败不自动重试：
    // 撤销请求后，用户下次改开关才会再试一次。
    if (fgChainRebuildPending()) {
        auto rebuiltFgChain = rebuildSwapChainForFg(s, "fg-toggle");
        if (!rebuiltFgChain) {
            // 重建的前半程已经 Reset 过旧链：此刻没有任何 back buffer，继续往下会崩在 GetCurrentBackBufferIndex。
            // 返回失败让 JVM 跳过本帧 blit，并置 suboptimal 走 surface 重建。
            s->suboptimal = true;
            const std::string why = rebuiltFgChain.error().message;
            fgNoteChainRebuildDone(fgChainOwner(), 0, why.c_str());
            return mcdr::err("acquireSurface",
                             "frame generation swapchain rebuild failed: " + why);
        }
        fgNoteChainRebuildDone(fgChainOwner(), 1, nullptr);
    }
    // 帧首钩子：必须在重建消费之后（marker 的帧号要跟新链的第一次 Present 走），
    // 也必须在本帧开画之前（RENDERSUBMIT_START = 应用开始提交这一帧的渲染）。
    fgFrameStart();
    // 多帧飞行：Present 未完成时 GetCurrentBackBufferIndex 不轮转，会返回上一帧已 Present 的 buffer
    // （两帧写同一处 → 闪帧）；真实 acquire 已推迟到 blit 之前，故通常 0ms。
    // 同步失败必须当致命处理：否则写到"Present 后会翻转走"的 buffer → D3D12 ERROR #907 →
    // RemoveDevice(0x887A0005 ACCESS_DENIED)。同步不上就放弃本帧，下一帧报 SurfaceException 让 MC 重建。
    if (!waitForPendingPresents(2000)) {
        s->currentImageIndex = -1;
        return mcdr::err("acquireSurface",
                         "acquireSurface: present sync timeout (render thread stalled) — frame skipped to "
                         "avoid writing a non-current back buffer");
    }
    // 逐帧诊断降为 DEBUG（默认级别下 stderr+文件 flush 是帧关键路径上的纯开销）。
    dbgLogDebug("acquireSurface: enter surface=%p", reinterpret_cast<void*>(s));
    UINT rawIdx = s->swapChain->GetCurrentBackBufferIndex();
    s->currentImageIndex = static_cast<int>(rawIdx);
    UINT bbCount = static_cast<UINT>(s->backBuffers.size());
    dbgLogDebug("acquireSurface: rawIdx=%u bbCount=%u currentImageIndex=%d", rawIdx, bbCount,
                s->currentImageIndex);
    if (s->currentImageIndex < 0 || s->currentImageIndex >= static_cast<int>(bbCount)) {
        // 容错：backBuffers 为空说明 surface 尚未配置完，返回 false 让渲染线程稍后重试；
        // index 异常时再用 GetBufferCount 交叉验证。
        if (bbCount == 0) {
            return mcdr::err("acquireSurface",
                             "acquireSurface: backBuffers empty (surface not configured yet)");
        }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        HRESULT hr = s->swapChain->GetDesc1(&sd);
        return mcdr::err("acquireSurface",
                         "GetCurrentBackBufferIndex returned invalid index=" + std::to_string(rawIdx) +
                             " bbCount=" + std::to_string(bbCount) + " scGetDesc_hr=" + hrText(hr) +
                             " BufferCount=" + std::to_string(sd.BufferCount));
    }
    // 重用了上一帧的 back buffer 时（上次 blit 可能还没完成）等它对应的 fence，避免 CPU 写入 GPU
    // 仍在读的 buffer；3-buffer 正常情况下等待为 0。
    {
        UINT64 needed =
            s->surfaceFences.empty() ? 0 : s->surfaceFences[static_cast<size_t>(s->currentImageIndex)];
        DeviceContext& ctx = deviceContext();
        if (needed > 0 && ctx.queueFence) {
            UINT64 cv = ctx.queueFence->GetCompletedValue();
            if (cv < needed) {
                dbgLogDebug("acquireSurface: wait fence idx=%d needed=%llu cv=%llu", s->currentImageIndex,
                       static_cast<unsigned long long>(needed), static_cast<unsigned long long>(cv));
                auto fenceRes = waitForQueueFenceValue(needed, 5000000000ULL);
                if (!fenceRes) {
                    return mcdr::err("acquireSurface",
                                     "acquireSurface: " + fenceRes.error().message);
                }
            }
        }
    }
    // 同步 lastBlitIndex：游戏直接用 beginRenderPass 渲染到表面纹理时，readback 要靠 currentImageIndex
    // 才读得到正确 backBuffer（否则读到上一帧 blit 的旧数据 = 黑屏/错误内容）。
    s->lastBlitIndex = s->currentImageIndex;
    // 新的一帧已 acquire 但还没 blit：presentSurface 靠这个标志拒绝 present 未写入的 buffer。
    s->blittedThisFrame.store(false, std::memory_order_release);
    // 本 surface 真的持有"已 acquire 未 present"的 back buffer（原子量）：configureSurface 只据此
    // 决定能否用 Present(0,0) 释放它。
    s->bufferHeld.store(true, std::memory_order_release);
    return {};
}

// 返回当前 acquire 的 back buffer 原始 ID3D12Resource 指针。
uintptr_t getBackBufferHandle(Dx12Surface* s) {
    if (!s || s->currentImageIndex < 0 || s->currentImageIndex >= static_cast<int>(s->backBuffers.size())) {
        return 0ULL;
    }
    return reinterpret_cast<uintptr_t>(s->backBuffers[static_cast<size_t>(s->currentImageIndex)].Get());
}

// 返回当前渲染 pass 中第一个活跃颜色附件的纹理句柄（在 pass 内调用有效）。
uintptr_t getActiveColorTextureHandle(CommandContext* ctx) {
    if (!ctx || !ctx->inRenderPass || ctx->activeColorTargets.empty()) {
        return 0ULL;
    }
    return reinterpret_cast<uintptr_t>(ctx->activeColorTargets[0]->resource.Get());
}

mcdr::Result<void> blitSurface(CommandContext* ctx, Dx12Surface* s, Dx12Object* srcTex) {
    if (!ctx || !ctx->commandList) {
        return mcdr::err("blitSurface", "no command list");
    }
    if (!s || s->currentImageIndex < 0) {
        return mcdr::err("blitSurface", "no acquired back buffer");
    }
    if (!ctx->listOpen) {
        return mcdr::err("blitSurface", "command list not open (call dx12BeginCommandList first)");
    }
    // srcTex 为 null 时仅做纯红色 clear（无 copy），用于渲染循环自检。
    if (!srcTex || !srcTex->resource) {
        srcTex = nullptr;  // 标记为无源纹理，走纯 clear 路径
    }

    ID3D12GraphicsCommandList* cmd = ctx->commandList;
    ID3D12Resource* dst = s->backBuffers[static_cast<size_t>(s->currentImageIndex)].Get();
    UINT w = s->width;
    UINT h = s->height;
    dbgLogDebug("blitSurface: cmd=%p dst=%p idx=%d w=%u h=%u srcTex=%p", reinterpret_cast<void*>(cmd),
                reinterpret_cast<void*>(dst), s->currentImageIndex, w, h, reinterpret_cast<void*>(srcTex));

    // back buffer 必须**显式登记**进 ctx->resourceState，稳定态是 PRESENT（= 0，调试层显示 COMMON|PRESENT）。
    // 绝不依赖"未登记 → COMMON 兜底"（那样 stateOf/transitionTo 拿到的 Before 全是 COMMON，等于没查）。
    for (UINT i = 0; i < s->backBuffers.size(); ++i) {
        ID3D12Resource* bb = s->backBuffers[i].Get();
        if (bb && ctx->resourceState.find(bb) == ctx->resourceState.end()) {
            ctx->resourceState[bb] = D3D12_RESOURCE_STATE_PRESENT;
        }
    }
    if (ctx->inRenderPass) {
        // 诊断（限 3 次）：blit 被录在一条仍打开的 render pass 里 —— blit 会绑 back buffer 的 RTV，
        // 而 pass 自己的附件绑定已被覆盖，endRenderPass 只回切它自己的附件。
        static int p109InPassBlits = 0;
        if (p109InPassBlits < 3) {
            ++p109InPassBlits;
            dbgLogWarn("blit in open pass ctx=%p idx=%d "
                       "the pass' attachments were rebound to the back buffer",
                       reinterpret_cast<void*>(ctx), s->currentImageIndex);
        }
    }

    // 源纹理可能是本帧 render pass 的输出，或刚上传完的 COMMON：按跟踪状态过渡到 COPY_SOURCE。
    if (srcTex) transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = dst;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    // srcTex 为 null（首次/自检）：backbuffer 内容未定义，用纯黑 clear 兜底避免启动红屏，再回切 PRESENT。
    if (!srcTex) {
        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->rtvHandles[static_cast<size_t>(s->currentImageIndex)];
        float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        dbgLogDebug("blitSurface: srcTex=null — clear backbuffer to black");
        return {};
    }

    // Draw-based blit（全屏四边形）：绕过 CopyTextureRegion 的格式不兼容（源格式与 backbuffer 不同时
    // 它会静默失败）。步骤见本函数体：PRESENT→RT → 绑 PSO/SRV/IB → Draw → RT→PRESENT → 源回 COMMON。
    {
        // 1. PRESENT → RENDER_TARGET。用 transitionTo 而非裸 barrier（dst 纳入 ctx->resourceState）；
        // Before 锚点必须是 PRESENT，不能依赖 COMMON 兜底。
        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->rtvHandles[static_cast<size_t>(s->currentImageIndex)];
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        // viewport / scissor 是命令列表状态：上一次 render pass 留下的值会被整屏四边形继承，blit 只覆盖
        // 一小块（剩余区域保留 2-3 帧前的内容 = 叠帧 + OSD 重叠）。尺寸取 backbuffer 自身描述。
        const D3D12_RESOURCE_DESC bbDesc = dst->GetDesc();
        D3D12_VIEWPORT blitVp{0.0f, 0.0f, static_cast<FLOAT>(bbDesc.Width),
                              static_cast<FLOAT>(bbDesc.Height), 0.0f, 1.0f};
        cmd->RSSetViewports(1, &blitVp);
        D3D12_RECT blitSc{0, 0, static_cast<LONG>(bbDesc.Width), static_cast<LONG>(bbDesc.Height)};
        cmd->RSSetScissorRects(1, &blitSc);
        dbgLogDebug("blitSurface: viewport/scissor set %llux%llu", bbDesc.Width, bbDesc.Height);
        auto blitInit = initBlitPipeline();
        if (!blitInit) {
            dbgLogError("blitSurface: initBlitPipeline FAILED: %s", blitInit.error().message.c_str());
            return mcdr::err("blitSurface", "blitSurface: " + blitInit.error().message);
        }
        const BlitPipeline* bp = getBlitPipeline();
        if (bp == nullptr) {
            return mcdr::err("blitSurface", "blitSurface: blit pipeline unavailable after init");
        }
        // SMAA 已移到世界层（applySmaaToTexture）：最终 blit 必须是**纯线性升采样**，否则整帧
        // （世界 + GUI）一起被柔化，界面发虚。
        {
            cmd->SetGraphicsRootSignature(bp->rootSig.Get());
            // HDR 开启时用 RGBA16F 的 PSO，并把 sRGB→线性 + SDR 白系数交给根常量 b0；SDR 路径也必须显式
            // 设成 (1,0,0,0) —— 根常量未设置时读到的内容是未定义的。
            const bool hdr = s->hdrActive;
            ID3D12PipelineState* presentPso = (hdr && bp->psoHdr) ? bp->psoHdr.Get() : bp->pso.Get();
            cmd->SetPipelineState(presentPso);
            const float hdrParams[4] = {hdr ? s->hdrSdrWhiteScale : 1.0f, hdr ? 1.0f : 0.0f, 0.0f, 0.0f};
            cmd->SetGraphicsRoot32BitConstants(1, 4, hdrParams, 0);
            dbgLogDebug("blitSurface: set blit rootSig=%p pso=%p hdr=%d scale=%.3f",
                        reinterpret_cast<void*>(bp->rootSig.Get()),
                        reinterpret_cast<void*>(presentPso), hdr ? 1 : 0,
                        static_cast<double>(hdrParams[0]));

            auto blitBind = blitBindSourceTexture(ctx, srcTex, cmd);
            if (!blitBind) {
                dbgLogError("blitSurface: blitBindSourceTexture FAILED: %s",
                            blitBind.error().message.c_str());
                return blitBind;
            }

            cmd->IASetVertexBuffers(0, 1, &bp->vbView);
            cmd->IASetIndexBuffer(&bp->ibView);
            // IA topology 同样是命令列表状态（D3D12 初值 UNDEFINED）：必须显式设置，否则继承上一次 pass 的
            // 拓扑 → 整屏四边形退化/丢失，backbuffer 只被部分覆盖。
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            dbgLogDebug("blitSurface: drawIndexed inst=1 firstIdx=0 firstVert=0");
            cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);
            dbgLogDebug("blitSurface: drawIndexed done");
        }

        // 6. RENDER_TARGET → PRESENT：blit 是本帧对 back buffer 的最后一次 GPU 写入，之后必须回到 PRESENT
        // 才能 Present；Before 从跟踪表读（上一步刚写成 RT）。
        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_PRESENT);
        // 必须把 back buffer 的 RTV **解绑**：OMSetRenderTargets 会留到下一次设置，而 back buffer 已切回
        // PRESENT(0)；同一列表里 blit 之后若还有 draw，调试层会判 "#538：拿 COMMON|PRESENT 当 RT"。
        cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

        if (srcTex) transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COMMON);
    }
    // 源纹理实际尺寸：拷贝区是 min(源, backbuffer)，源比窗口小时留黑边。
    if (srcTex && srcTex->resource) {
        D3D12_RESOURCE_DESC srcDesc = srcTex->resource->GetDesc();
        bool srcWasWritten = ctx->colorTargetsWritten;
        dbgLogDebug("blitSurface: ctx=%p ctxW=%d src=%p -> backbuf=%ux%u", reinterpret_cast<void*>(ctx),
                    static_cast<int>(ctx->colorTargetsWritten), reinterpret_cast<void*>(srcTex), w, h);
        dbgLogDebug("blit %p %llux%llu fmt=%d written=%d -> %ux%u",
                    reinterpret_cast<void*>(srcTex), static_cast<unsigned long long>(srcDesc.Width),
                    static_cast<unsigned long long>(srcDesc.Height), static_cast<int>(srcTex->dxgiFormat),
                    static_cast<int>(srcWasWritten), w, h);
    } else {
        dbgLogDebug("blitSurface: srcTex=null (pure red clear), no src diagnostic");
    }
    // 记录本帧 blit 写入的 back buffer 下标（Present 后 currentImageIndex=-1，readback 必须用它）。
    s->lastBlitIndex = s->currentImageIndex;
    // 本帧已有内容：presentSurface 只在这种情况下才 Present。失败路径提前 return，此标志保持 false。
    s->blittedThisFrame.store(true, std::memory_order_release);
    return {};
}

// 纹理 → 纹理（可缩放）blit：GUI 的独立全尺寸目标靠它把世界层结果从主渲染目标搬过去
// （renderScale != 100 时两者尺寸不同，必须走带线性采样的整屏三角形，不能 CopyTextureRegion）。
mcdr::Result<void> blitTextureToTexture(CommandContext* ctx, Dx12Object* src, Dx12Object* dst) {
    if (!ctx || !ctx->commandList || !src || !src->resource || !dst || !dst->resource) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: null ctx/src/dst");
    }
    if (src->resource.Get() == dst->resource.Get()) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: src and dst are the same resource");
    }

    ID3D12GraphicsCommandList* cmd = ctx->commandList;
    const D3D12_RESOURCE_DESC srcDesc = src->resource->GetDesc();
    const D3D12_RESOURCE_DESC dstDesc = dst->resource->GetDesc();
    if (dstDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dstDesc.SampleDesc.Count != 1) {
        return mcdr::err("blitTextureToTexture",
                         "blitTextureToTexture: dst is not a single-sampled 2D texture");
    }

    // 目标当渲染目标用：view/scissor 取目标自身尺寸，避免继承上一次 pass 的状态而叠帧。
    auto rtv = acquireTextureRtv(ctx, dst);
    if (!rtv) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: " + rtv.error().message);
    }
    auto blitInit = initBlitPipeline();
    if (!blitInit) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: " + blitInit.error().message);
    }
    const BlitPipeline* bp = getBlitPipeline();
    if (bp == nullptr) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: blit pipeline unavailable");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dstRtv = rtv.value();
    cmd->OMSetRenderTargets(1, &dstRtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<FLOAT>(dstDesc.Width), static_cast<FLOAT>(dstDesc.Height), 0.0f, 1.0f};
    cmd->RSSetViewports(1, &vp);
    D3D12_RECT sc{0, 0, static_cast<LONG>(dstDesc.Width), static_cast<LONG>(dstDesc.Height)};
    cmd->RSSetScissorRects(1, &sc);
    transitionTextureTo(ctx, dst, D3D12_RESOURCE_STATE_RENDER_TARGET);

    cmd->SetGraphicsRootSignature(bp->rootSig.Get());
    cmd->SetPipelineState(bp->pso.Get());
    // 这条路径恒定是 SDR→SDR（内部纹理之间拷贝），显式关掉 HDR 变换。
    const float sdrHdrParams[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    cmd->SetGraphicsRoot32BitConstants(1, 4, sdrHdrParams, 0);
    auto bind = blitBindSourceTexture(ctx, src, cmd);
    if (!bind) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: " + bind.error().message);
    }

    cmd->IASetVertexBuffers(0, 1, &bp->vbView);
    cmd->IASetIndexBuffer(&bp->ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // 源回 RENDER_TARGET（世界层那一帧仍可能被 PostChain/清深度使用），目标留在 RENDER_TARGET 供 GUI pass 直接画。
    transitionTextureTo(ctx, src, D3D12_RESOURCE_STATE_RENDER_TARGET);
    dbgLogDebug("blitTextureToTexture: %llux%llu -> %llux%llu", srcDesc.Width, srcDesc.Height, dstDesc.Width,
                dstDesc.Height);
    return {};
}

mcdr::Result<void> sharpenTextureToTexture(CommandContext* ctx, Dx12Object* src, Dx12Object* dst,
                                          float amount) {
    if (!ctx || !ctx->commandList || !src || !src->resource || !dst || !dst->resource) {
        return mcdr::err("sharpenTextureToTexture", "sharpenTextureToTexture: null ctx/src/dst");
    }
    if (src->resource.Get() == dst->resource.Get()) {
        return mcdr::err("sharpenTextureToTexture",
                         "sharpenTextureToTexture: src and dst are the same resource");
    }

    ID3D12GraphicsCommandList* cmd = ctx->commandList;
    const D3D12_RESOURCE_DESC srcDesc = src->resource->GetDesc();
    const D3D12_RESOURCE_DESC dstDesc = dst->resource->GetDesc();
    if (dstDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dstDesc.SampleDesc.Count != 1) {
        return mcdr::err("sharpenTextureToTexture",
                         "sharpenTextureToTexture: dst is not a single-sampled 2D texture");
    }

    auto rtv = acquireTextureRtv(ctx, dst);
    if (!rtv) {
        return mcdr::err("sharpenTextureToTexture", "sharpenTextureToTexture: " + rtv.error().message);
    }
    auto init = initSharpenPipeline();
    if (!init) {
        return mcdr::err("sharpenTextureToTexture", "sharpenTextureToTexture: " + init.error().message);
    }
    const SharpenPipeline* sp = getSharpenPipeline();
    if (sp == nullptr) {
        return mcdr::err("sharpenTextureToTexture",
                         "sharpenTextureToTexture: sharpen pipeline unavailable");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dstRtv = rtv.value();
    cmd->OMSetRenderTargets(1, &dstRtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<FLOAT>(dstDesc.Width), static_cast<FLOAT>(dstDesc.Height),
                      0.0f, 1.0f};
    cmd->RSSetViewports(1, &vp);
    D3D12_RECT sc{0, 0, static_cast<LONG>(dstDesc.Width), static_cast<LONG>(dstDesc.Height)};
    cmd->RSSetScissorRects(1, &sc);
    transitionTextureTo(ctx, dst, D3D12_RESOURCE_STATE_RENDER_TARGET);

    cmd->SetGraphicsRootSignature(sp->rootSig.Get());
    cmd->SetPipelineState(sp->pso.Get());
    auto bind = blitBindSourceTexture(ctx, src, cmd);
    if (!bind) {
        return mcdr::err("sharpenTextureToTexture", "sharpenTextureToTexture: " + bind.error().message);
    }

    // b1 = {1/w, 1/h, amount, pad}：纹素步长按**源**尺寸算（采样源纹理）。
    const float params[4] = {
        srcDesc.Width == 0 ? 0.0f : 1.0f / static_cast<float>(srcDesc.Width),
        srcDesc.Height == 0 ? 0.0f : 1.0f / static_cast<float>(srcDesc.Height), amount, 0.0f};
    cmd->SetGraphicsRoot32BitConstants(1, 4, params, 0);

    cmd->IASetVertexBuffers(0, 1, &sp->vbView);
    cmd->IASetIndexBuffer(&sp->ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // 与 blitTextureToTexture 一致：源回到 RENDER_TARGET（世界层那一帧后续还要用）。
    transitionTextureTo(ctx, src, D3D12_RESOURCE_STATE_RENDER_TARGET);
    dbgLogDebug("sharpenTextureToTexture: %llux%llu -> %llux%llu amount=%.3f", srcDesc.Width,
                srcDesc.Height, dstDesc.Width, dstDesc.Height, amount);
    return {};
}

void presentSurface(Dx12Surface* s) {
    if (!s || !s->swapChain) {
        return;
    }
    // back buffer 的 RT→PRESENT 过渡不能在这里做：调用时该帧命令列表早已 Close+Execute，往已关闭的
    // 列表里记 barrier 非法，收口点在 blitSurface 尾部。
    // 只有本帧真的 blit 过才 Present：vanilla 的门只看包装层的 hasBlittedTexture/isAcquired，与我们是否
    // 真的 acquire+blit 成功脱钩。无条件 Present 会 flip 到本帧从未写过的 buffer（未初始化内存 / 2~3 帧前
    // 旧内容 = 叠帧闪烁，OSD 同屏画两遍）。没有内容就返回，让上一帧继续留在屏幕上。
    if (!s->blittedThisFrame || s->currentImageIndex < 0) {
        // 静默返回：这是"本帧没有内容可显示"的正常状态（启动、菜单、窗口被遮挡），计数+日志只会变成噪音。
        return;
    }
    // IMMEDIATE=0 → Present(0, ALLOW_TEARING)；FIFO_RELAXED=3 / FIFO=2 → Present(1, 0)。
    //
    // 全屏（独立翻转/独占）时 syncInterval=0 但不带 ALLOW_TEARING 的 Present 仍被 DXGI 同步到 vblank
    // （实测 1920x1081 每帧阻塞 ~5.6ms）；前提：建链时带 ALLOW_TEARING 且 syncInterval==0。真独占下
    // DXGI 禁止 ALLOW_TEARING（返回 S_OK 但画面停在进全屏前那一帧），全屏时一律 flags=0 且 syncInterval=1。
    const UINT syncInterval = (s->presentMode == 0 && !s->exclusive) ? 0 : 1;
    const UINT flags = (s->presentMode == 0 && s->allowTearing && !s->exclusive)
                           ? DXGI_PRESENT_ALLOW_TEARING
                           : 0;
    // Reflex 要求 Present 之前 sleep（ProgrammingGuideReflex.md §5）。FG 钩子套在这一点上：
    // fgBeforePresent → reflex_before_present → Present → reflex_after_present → fgAfterPresent
    // （slDLSSGSetOptions 的生效点是"下一次 Present"，故必须在本帧 Present 之前）。设备被移除/重置后
    // 不要再进 FG 钩子 —— provider 会对着已失效的设备解引用。
    const bool fgDeviceAlive = !deviceRemoved();
    if (fgDeviceAlive) {
        fgBeforePresent();
    } else {
        static std::atomic<bool> fgDeviceLostLogged{false};
        if (!fgDeviceLostLogged.exchange(true, std::memory_order_relaxed)) {
            dbgLogWarn("device removed, fg hooks skipped");
        }
    }
    mcdr_reflex_before_present();
    HRESULT hr = s->swapChain->Present(syncInterval, flags);
    // Only now that a real frame reached the screen do we show the child window that carries it --
    // this is what keeps the startup from flashing an unpainted face.
    if (s->host != nullptr && SUCCEEDED(hr) && !s->hostRevealed) {
        s->hostRevealed = true;
        mcdr_host_reveal(static_cast<MCDR_WindowHost*>(s->host));
    }
    mcdr_reflex_after_present();  // PCL 的 ePresentEnd 标记
    // 呈递之后的钩子（与 fgBeforePresent 成对）。Present 期间设备也可能刚被移除，这里再查一次。
    if (fgDeviceAlive && !deviceRemoved()) {
        fgAfterPresent();
    }
    // Present 后 backbuffer 所有权已释放：不重置的话 configureSurface 的 ResizeBuffers 会误判仍有
    // acquired backbuffer 而返回 NOT_CURRENTLY_AVAILABLE（画面冻结）。
    s->currentImageIndex = -1;
    // 本帧内容已交给显示器；下一次 blit 之前都不应再 Present。
    s->blittedThisFrame.store(false, std::memory_order_release);
    // back buffer 已随 Present 交还 DXGI；configureSurface 只在 bufferHeld 为真时才做释放用的 Present(0,0)。
    s->bufferHeld.store(false, std::memory_order_release);
    if (hr == DXGI_STATUS_OCCLUDED) {
        dbgLogInfo("window occluded, present suboptimal");
        s->suboptimal = true;
    } else if (hr == DXGI_STATUS_MODE_CHANGED) {
        dbgLogInfo("presentSurface: MODE_CHANGED -> suboptimal");
        s->suboptimal = true;
    } else if (FAILED(hr)) {
        dbgLogError("presentSurface: FAILED %s", hrText(hr).c_str());
        // 设备移除/重置：把真正的原因（GetDeviceRemovedReason + InfoQueue 最近消息）打进日志，否则只有
        // 0x887A0005，看不出是"写了非当前 back buffer"（ACCESS_DENIED）还是真正的 GPU hang/TDR。
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            logDeviceStatus("presentSurface: FAILED");
        }
        s->suboptimal = true;
    } else {
        // 每帧成功路径：默认级别下静默（stderr/文件写位于帧关键路径上）。
        dbgLogDebug("presentSurface: ok (syncInterval=%u)", syncInterval);
        s->suboptimal = false;  // 正常 present 清除 suboptimal 标记
    }
}

// HDR 模式（0=auto / 1=强制开 / 2=强制关）：只写进程级状态，下次建链/重建链才生效（换格式必须重建链）。
void setHdrMode(int32_t mode) {
    mcdr_hdr_state_set_mode(mode);
    dbgLogInfo("HDR mode set to %d (applies on next swapchain creation)", mode);
}

int32_t hdrActiveFlag() { return mcdr_hdr_state_active(); }

void destroySurface(Dx12Surface* s) {
    if (!s) return;
    dbgLogDebug("destroySurface: enter surface=%p", reinterpret_cast<void*>(s));
    dbgLogProgress("destroySurface stage: enter");
    // 独占全屏必须在释放链之前退出，否则显示模式停在独占状态（桌面黑屏 / 分辨率不回退）。
    if (s->swapChain && s->exclusive) {
        const HRESULT fsHr = s->swapChain->SetFullscreenState(FALSE, nullptr);
        s->exclusive = false;
        dbgLogInfo("destroySurface: left exclusive fullscreen hr=0x%08X", static_cast<unsigned>(fsHr));
    }
    // async render thread 可能仍在操作同一 swapchain：先等它完成当前帧（仅同步、不销毁线程），
    // 再等 GPU 空闲，避免 Signal 与渲染线程的 Present 并发访问 swapchain。
    waitForRenderThreadSubmit();
    dbgLogProgress("destroySurface stage: waited render thread");
    (void)deviceWaitIdle();
    dbgLogProgress("destroySurface stage: device idle");
    // 先立刻把父窗口刷黑：退出时窗口消失前会短暂露出客户区，刷黑只动 GDI、不依赖 fence 等待。
    HWND presentChild = s->presentWindow;
    MCDR_WindowHost* const surfaceHost = static_cast<MCDR_WindowHost*>(s->host);
    s->host = nullptr;
    if (surfaceHost == nullptr) paintParentBlackBeforeDestroy(presentChild);
    dbgLogProgress("destroySurface stage: parent painted");
    // 等 per-surface present fence，确保 Display Controller 已完成 flip 并释放 backbuffer
    // （deviceWaitIdle 不等 display controller）。先查 GetCompletedValue 走快路径；等待上限 250 ms。
    if (!s->surfacePresentFences.empty()) {
        UINT64 pv = 0;
        for (UINT64 v : s->surfacePresentFences)
            if (v > pv) pv = v;
        if (pv > 0) {
            DeviceContext& dc = deviceContext();
            if (dc.queueFence && dc.queueFence->GetCompletedValue() < pv) {
                HANDLE hEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (hEvt) {
                    HRESULT hr = dc.queueFence->SetEventOnCompletion(pv, hEvt);
                    if (SUCCEEDED(hr)) {
                        WaitForSingleObject(hEvt, 250);
                    }
                    CloseHandle(hEvt);
                }
            }
        }
    }
    // present fence 完成后 flush 延迟删除对象，避免 GPU 仍引用资源时释放（PreSwapInfoQueue CORRUPTION）。
    dbgLogProgress("destroySurface stage: fence wait done");
    flushPendingDeletes();
    dbgLogProgress("destroySurface stage: pending deletes flushed");
    if (getActiveSurface() == s) setActiveSurface(nullptr);
    // 先释放 swapchain 再销毁它的呈现宿主窗口，顺序不能反：窗口活着时 DWM 仍持有其呈现资源引用。
    s->presentWindow = nullptr;
    dbgLogProgress("destroySurface stage: deleting surface");
    delete s;
    dbgLogProgress("destroySurface stage: surface deleted");
    // 父窗口已在 fence 等待之前刷黑，这里只补日志与销毁子窗口。
    logWindowState("destroySurface child(before)", presentChild);
    logWindowState("destroySurface parent(before)", presentChild ? GetParent(presentChild) : nullptr);
    if (surfaceHost != nullptr) {
        mcdr_host_release(surfaceHost);  // 最后一个引用时恢复父窗口过程并销毁子窗口
    } else if (presentChild != nullptr) {
        DestroyWindow(presentChild);
    }
    logWindowState("destroySurface parent(after)", presentChild ? GetParent(presentChild) : nullptr);
    dbgLogDebug("destroySurface: done surface=%p", reinterpret_cast<void*>(s));
}

// 仅清理表面，不调用 deviceWaitIdle（供 C++ 内部使用：render thread 已自行退出后调用）
void destroySurfaceNoWaitIdle(Dx12Surface* s) {
    if (!s) return;
    if (getActiveSurface() == s) setActiveSurface(nullptr);
    HWND presentChild = s->presentWindow;
    s->presentWindow = nullptr;
    MCDR_WindowHost* const surfaceHost = static_cast<MCDR_WindowHost*>(s->host);
    s->host = nullptr;
    dbgLogProgress("destroySurface stage: deleting surface");
    delete s;
    dbgLogProgress("destroySurface stage: surface deleted");
    // 同 destroySurface：先刷黑父窗口再销毁子窗口。
    if (surfaceHost != nullptr) {
        mcdr_host_release(surfaceHost);
    } else {
        paintParentBlackBeforeDestroy(presentChild);
        if (presentChild != nullptr) DestroyWindow(presentChild);
    }
}

// 读回 back buffer 采样像素：先等 GPU 空闲，用一次性命令列表拷到 readback staging，Map 后打印
// 3x3 网格 RGBA（每 ~60 帧一次）。纯红/纯黑/纯灰 = 只有 clear 生效，多色带中心内容 = 渲染正常。
mcdr::Result<void> readbackSurfacePixels(Dx12Surface* s) {
    DeviceContext& ctx = deviceContext();
    if (!ctx.device || !ctx.queue) {
        return mcdr::err("readbackSurfacePixels", "device not initialized");
    }
    if (!s || s->backBuffers.empty()) {
        return mcdr::err("readbackSurfacePixels", "surface has no back buffers");
    }
    {
        auto idleRes = deviceWaitIdle();
        if (!idleRes) {
            return mcdr::err("readbackSurfacePixels",
                             "deviceWaitIdle failed: " + idleRes.error().message);
        }
    }

    // 选择要读回的 back buffer：优先 currentImageIndex（游戏可能直接渲染到表面纹理，lastBlitIndex 是
    // 旧帧残留）；== -1（Present 后尚未 acquire）时退回 lastBlitIndex，保证自检能读到真实画面。
    int idx = s->currentImageIndex;
    if (idx < 0 || idx >= static_cast<int>(s->backBuffers.size())) {
        if (s->lastBlitIndex >= 0 && s->lastBlitIndex < static_cast<int>(s->backBuffers.size())) {
            idx = s->lastBlitIndex;
        } else {
            idx = static_cast<int>(s->swapChain->GetCurrentBackBufferIndex());
        }
    }
    if (idx < 0 || idx >= static_cast<int>(s->backBuffers.size())) {
        return mcdr::err("readbackSurfacePixels",
                         "no valid back buffer for readback (idx=" + std::to_string(idx) + ")");
    }
    ID3D12Resource* bb = s->backBuffers[static_cast<size_t>(idx)].Get();
    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    UINT w = static_cast<UINT>(bd.Width), h = bd.Height;
    // 行宽按 w*4 硬编码、拷贝目标声明成 R8G8B8A8_UNORM：HDR 链（R16G16B16A16_FLOAT）会错位，只用于 SDR 自检。
    UINT64 rowBytes = static_cast<UINT64>(w) * 4;
    UINT64 pitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                   & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UINT64 total = pitch * h;

    static ComPtr<ID3D12Resource> staging;
    if (!staging || staging->GetDesc().Width < total) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = total;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        hp.CreationNodeMask = 0;
        hp.VisibleNodeMask = 0;
        if (FAILED(ctx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&staging)))) {
            return mcdr::err("readbackSurfacePixels", "CreateCommittedResource(readback staging) failed");
        }
    }

    static ComPtr<ID3D12CommandAllocator> alloc;
    static ComPtr<ID3D12GraphicsCommandList> cl;
    if (!alloc) {
        if (FAILED(
                ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) {
            return mcdr::err("readbackSurfacePixels", "CreateCommandAllocator failed");
        }
    }
    if (!cl) {
        if (FAILED(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                 IID_PPV_ARGS(&cl)))) {
            return mcdr::err("readbackSurfacePixels", "CreateCommandList failed");
        }
    } else {
        alloc->Reset();
        cl->Reset(alloc.Get(), nullptr);
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = staging.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, &b);
    cl->Close();

    ID3D12CommandList* lists[] = {cl.Get()};
    ctx.queue->ExecuteCommandLists(1, lists);
    UINT64 fv = ++ctx.queueFenceValue;
    if (FAILED(ctx.queue->Signal(ctx.queueFence.Get(), fv))) {
        return mcdr::err("readbackSurfacePixels", "Signal(readback) failed");
    }
    {
        auto fenceRes = waitForQueueFenceValue(fv, 5'000'000'000ULL);
        if (!fenceRes) {
            return mcdr::err("readbackSurfacePixels",
                             "readback wait timeout: " + fenceRes.error().message);
        }
    }

    void* ptr = nullptr;
    if (FAILED(staging->Map(0, nullptr, &ptr))) {
        return mcdr::err("readbackSurfacePixels", "staging Map failed");
    }
    const uint8_t* base = static_cast<const uint8_t*>(ptr);
    int xs[3] = {0, static_cast<int>(w) / 2, static_cast<int>(w) - 1};
    int ys[3] = {0, static_cast<int>(h) / 2, static_cast<int>(h) - 1};
    for (int yi = 0; yi < 3; ++yi) {
        for (int xi = 0; xi < 3; ++xi) {
            const uint8_t* p = base + static_cast<UINT64>(ys[yi]) * pitch + static_cast<UINT64>(xs[xi]) * 4;
            dbgLogDebug("readback[%ux%u] (%d,%d) = RGBA(%3d,%3d,%3d,%3d)", w, h, xs[xi], ys[yi], p[0], p[1], p[2],
                   p[3]);
        }
    }
    // 整帧 dump 成 BMP + ASCII 缩略图，直接看 backbuffer 实际画面。
    dbgDumpPixelsToFile(base, w, h, pitch, "backbuf");
    staging->Unmap(0, nullptr);
    return {};
}

}  // namespace d3d12_mc
