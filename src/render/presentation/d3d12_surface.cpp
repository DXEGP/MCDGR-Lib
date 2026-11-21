module;

#include "_d3d12_module_prelude.h"
#include "mcdr_dlss_impl.h"
#include "mcdr_hdr_state.h"
#include "mcdr_surface_opts_state.h"
#include "mcdr_window_host.h"
#include <algorithm>
#include <cwctype>
#include <tlhelp32.h>

module mcdr.d3d12.device;

import mcdr.error;

namespace d3d12_mc {

namespace {

std::string hrText(HRESULT hr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

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

struct SwapChainBundle {
    ComPtr<IDXGISwapChain1> swapChain;
    bool composition = false;
    bool allowTearing = false;
    ComPtr<IDCompositionDevice> dcompDevice;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;
};

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

template <typename T>
HRESULT createDxgiFactory(ComPtr<T>& factory) {
    const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    if (fgWantsAppNativeChain()) {
        fgNoteChainOwner(MCDR_FG_CHAIN_APP_NATIVE);
        return hr;
    }
    void* raw = factory.Get();
    if (mcdr_sl_upgrade_interface(&raw) != 0 && raw != nullptr) {
        factory.Attach(static_cast<T*>(raw));
        fgNoteChainOwner(MCDR_FG_CHAIN_SL_PROXY);
    }
    return hr;
}

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
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Flags = 0;
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

constexpr wchar_t kPresentWindowClass[] = L"MCDR_D3D12_PresentChild";

static void paintWindowBlack(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) return;
    HDC dc = GetDC(hwnd);
    if (dc == nullptr) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    ReleaseDC(hwnd, dc);
    GdiFlush();
}

static void paintParentBlackBeforeDestroy(HWND child) {
    if (child == nullptr) return;
    paintWindowBlack(GetParent(child));
}

static bool prepareSurfaceWindowForDestroy(uintptr_t parentHandle, HWND child,
                                           MCDR_WindowHost* host) {
    if (host != nullptr) return mcdr_host_prepare_release(host) != 0;
    if (child != nullptr && IsWindow(child)) {
        HWND parent = GetParent(child);
        paintWindowBlack(parent);
        if (parent != nullptr && IsWindow(parent)) {
            ShowWindow(parent, SW_HIDE);
            SetWindowPos(parent, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING |
                             SWP_HIDEWINDOW);
        }
        ShowWindow(child, SW_HIDE);
        SetWindowPos(child, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING |
                         SWP_HIDEWINDOW);
        return true;
    }
    HWND parent = reinterpret_cast<HWND>(parentHandle);
    paintWindowBlack(parent);
    if (parent != nullptr && IsWindow(parent)) {
        ShowWindow(parent, SW_HIDE);
        SetWindowPos(parent, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING |
                         SWP_HIDEWINDOW);
    }
    return true;
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
    dbgLogInfo("%s: hwnd=%p cls=%ls rect=%ld,%ld %ldx%ld visible=%d iconic=%d classBrush=%p", tag,
               reinterpret_cast<void*>(hwnd), cls, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
               static_cast<int>(IsWindowVisible(hwnd)), static_cast<int>(IsIconic(hwnd)), brush);
}

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
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = presentationWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kPresentWindowClass;
        if (RegisterClassExW(&wc)) return true;
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) return nullptr;
    const int w = width > 0 ? static_cast<int>(width) : 1;
    const int h = height > 0 ? static_cast<int>(height) : 1;
    HWND child = CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY, kPresentWindowClass,
                                 L"MCDR D3D12 present", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, w,
                                 h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!child) return nullptr;
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
    dbgLogDebug("resizePresentationWindow: child=%p -> %ux%u (visible=%d rect=%dx%d)",
           reinterpret_cast<void*>(child), width, height, IsWindowVisible(child) ? 1 : 0,
           static_cast<int>(rc.right - rc.left), static_cast<int>(rc.bottom - rc.top));
}

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
    dbgLog("swapchain Present slot=%p owner=%ls bytes=%02X %02X %02X %02X",
           present, sv.empty() ? L"(unknown)" : std::wstring(sv).c_str(),
           b[0], b[1], b[2], b[3]);

    const HMODULE rtssHooks = ::GetModuleHandleW(L"RTSSHooks64.dll");
    const HMODULE rtssLoader = ::GetModuleHandleW(L"RTSSHooksLoader64.dll");
    DXGI_SWAP_CHAIN_DESC desc{};
    const bool haveDesc = SUCCEEDED(sc->GetDesc(&desc));
    dbgLog("overlay probe: RTSSHooks64=%p RTSSHooksLoader64=%p desc.ok=%d OutputWindow=%p "
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

struct HdrProbe {
    bool hdrOn = false;
    float sdrWhiteNits = 200.0f;
    float maxLuminanceNits = 0.0f;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool found = false;
};

HdrProbe probeHdrForWindow(IDXGIFactory4* factory, HWND win) {
    HdrProbe probe{};
    if (factory == nullptr || win == nullptr) return probe;

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

    const HWND chainWin = presentWin != nullptr ? presentWin : win;

    if (!reuse || !reuse->composition) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
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
                dbgLogError("createSwapChainForWindow: all HWND tiers failed (last hr=0x%08X), "
                       "retry pass=%d (pump %dms)",
                       static_cast<unsigned>(lastHr), pass, pass * 60);
                pumpWindowMessages(pass * 60);
            }
            for (const auto& tier : kTiers) {
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
                    dbgLogInfo("swapchain OK (tier=%s pass=%d hwnd=%s) — OSD/overlay-capable",
                               tier.label, pass,
                               presentWin != nullptr ? "present-child" : "game-window");
                    const HRESULT assocHr = factory->MakeWindowAssociation(chainWin, DXGI_MWA_NO_ALT_ENTER);
                    dbgLog("MakeWindowAssociation(hwnd, NO_ALT_ENTER) hr=%08X",
                           static_cast<unsigned>(assocHr));
                    logSwapChainPresentHook(out.swapChain.Get());
                    break;
                }
                lastHr = hr;
                dbgLog("swapchain tier %s failed hr=%08X (pass=%d hwnd=%s)", tier.label,
                       static_cast<unsigned>(hr), pass,
                       presentWin != nullptr ? "present-child" : "game-window");
            }
        }
    } else {
        dbgLogInfo("createSwapChainForWindow: composition surface -> recreate composition directly");
    }

    if (out.swapChain) return {};

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

}

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

    UINT initW = 1, initH = 1;
    if (win != nullptr) {
        RECT crc{};
        if (GetClientRect(win, &crc) && crc.right > crc.left && crc.bottom > crc.top) {
            initW = static_cast<UINT>(crc.right - crc.left);
            initH = static_cast<UINT>(crc.bottom - crc.top);
        }
    }

    const int hdrMode = mcdr_hdr_state_mode();
    const HdrProbe probe = probeHdrForWindow(factory.Get(), win);
    const bool wantHdr = hdrMode == 1 || (hdrMode == 0 && probe.hdrOn);
    mcdr_hdr_state_set_applied_mode(hdrMode);
    const DXGI_FORMAT chainFormat =
        wantHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    dbgLogInfo("HDR probe: mode=%d found=%d hdrOn=%d colorSpace=%d sdrWhite=%.1f nits "
               "max=%.1f nits -> wantHdr=%d chainFormat=%d",
               hdrMode, probe.found ? 1 : 0, probe.hdrOn ? 1 : 0, static_cast<int>(probe.colorSpace),
               probe.sdrWhiteNits, probe.maxLuminanceNits, wantHdr ? 1 : 0,
               static_cast<int>(chainFormat));

    MCDR_WindowHost* host = nullptr;
    HWND presentChild = nullptr;
    if (win != nullptr && mcdr_surface_opts_force_present_child() != 0) {
        host = mcdr_host_create(win, static_cast<int32_t>(initW), static_cast<int32_t>(initH));
        if (host != nullptr) {
            presentChild = reinterpret_cast<HWND>(mcdr_host_child(host));
            dbgLogInfo("seamless host child=%p (%ux%u) parent=%p",
                       reinterpret_cast<void*>(presentChild), initW, initH,
                       reinterpret_cast<void*>(win));
            paintWindowBlack(win);
        } else {
            dbgLogWarn("seamless host creation failed (err=%lu); using top-level window",
                       static_cast<unsigned long>(GetLastError()));
        }
    }

    const bool glWasSuspended = mcdr_host_suspend_gl() != 0;
    SwapChainBundle bundle;
    auto swapChainRes = createSwapChainForWindow(factory.Get(), ctx.queue.Get(), win, presentChild,
                                                initW, initH, nullptr, bundle, chainFormat);
    if (!swapChainRes && presentChild != nullptr) {
        dbgLog("createSurface: chain on seamless host failed (%s); retrying on game window",
               swapChainRes.error().message.c_str());
        mcdr_host_release(host);
        host = nullptr;
        presentChild = nullptr;
        swapChainRes = createSwapChainForWindow(factory.Get(), ctx.queue.Get(), win, nullptr, initW,
                                                initH, nullptr, bundle, chainFormat);
    }
    if (glWasSuspended) mcdr_host_restore_gl();
    if (bundle.composition && presentChild != nullptr) {
        dbgLogInfo("createSurface: composition fallback -> releasing seamless host");
        mcdr_host_release(host);
        host = nullptr;
        presentChild = nullptr;
    }
    ComPtr<IDXGISwapChain1> swapChain1 = bundle.swapChain;

    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain1.As(&swapChain3))) {
        return mcdr::err("createSurface", "swapchain does not support IDXGISwapChain3");
    }

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
            dbgLogWarn("scRGB color space rejected (check=0x%08X support=0x%X set=0x%08X) — "
                       "presenting SDR through the float chain",
                       static_cast<unsigned>(csHr), support, static_cast<unsigned>(setHr));
        }
    }

    Dx12Surface* s = new Dx12Surface();
    s->hwnd = hwnd;
    s->presentWindow = presentChild;
    s->host = host;
    s->swapChain = swapChain3;
    s->compositionMode = bundle.composition;
    s->dcompDevice = bundle.dcompDevice;
    s->dcompTarget = bundle.dcompTarget;
    s->dcompVisual = bundle.dcompVisual;
    s->allowTearing = bundle.allowTearing;
    s->format = chainFormat;
    s->swapChainFormat = chainFormat;
    s->hdrActive = hdrActive;
    s->hdrMaxLuminance = probe.maxLuminanceNits;
    s->hdrSdrWhiteScale = hdrActive ? std::clamp(probe.sdrWhiteNits / 80.0f, 1.0f, 10.0f) : 1.0f;
    mcdr_hdr_state_set_active(hdrActive ? 1 : 0);
    if (hdrActive) {
        dbgLogInfo("HDR active: scRGB (R16G16B16A16_FLOAT + G10), SDR white %.1f nits "
                   "(scale %.3f), max luminance %.1f nits, presentation window %s",
                   probe.sdrWhiteNits, s->hdrSdrWhiteScale, probe.maxLuminanceNits,
                   presentChild ? "present-child" : "game-window");
    } else {
        dbgLogInfo("HDR inactive: SDR presentation (format=%d)",
                   static_cast<int>(chainFormat));
    }
    return static_cast<void*>(s);
}

mcdr::Result<void> setExclusiveFullscreen(Dx12Surface* s, bool on) {
    if (s == nullptr) return mcdr::err("setExclusiveFullscreen", "surface is null");
    if (!s->swapChain) return mcdr::err("setExclusiveFullscreen", "surface has no swap chain");
    if (s->compositionMode) {
        return mcdr::err("setExclusiveFullscreen",
                         "composition swap chain cannot enter exclusive fullscreen");
    }
    if (s->host != nullptr) {
        return mcdr::err("setExclusiveFullscreen",
                         "exclusive fullscreen needs a top-level swap chain (seamless host active)");
    }
    const BOOL want = on ? TRUE : FALSE;
    const HRESULT hr = s->swapChain->SetFullscreenState(want, nullptr);
    BOOL actual = FALSE;
    s->swapChain->GetFullscreenState(&actual, nullptr);
    if (FAILED(hr)) {
        dbgLogWarn("setExclusiveFullscreen(%d): SetFullscreenState hr=0x%08X "
                   "(fullscreenState=%d) — staying windowed",
                   on ? 1 : 0, static_cast<unsigned>(hr), actual ? 1 : 0);
        return mcdr::err("setExclusiveFullscreen", "SetFullscreenState failed: " + hrText(hr));
    }
    s->exclusive = on;
    if (on && s->hwnd != 0) {
        HWND hwnd = reinterpret_cast<HWND>(s->hwnd);
        ShowWindow(hwnd, SW_SHOW);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
    }
    if (s->width > 0 && s->height > 0) {
        auto resized = configureSurface(s, static_cast<int>(s->width), static_cast<int>(s->height),
                                        s->presentMode,true);
        if (!resized) {
            dbgLogWarn("setExclusiveFullscreen(%d): forced ResizeBuffers failed: %s", on ? 1 : 0,
                       resized.error().message.c_str());
        }
    }
    dbgLogInfo("setExclusiveFullscreen(%d): ok, fullscreenState=%d presentWindow=%p",
               on ? 1 : 0, actual ? 1 : 0, reinterpret_cast<void*>(s->presentWindow));
    return {};
}

std::vector<int> surfacePresentModes() {
    return {0, 2, 3};
}

int32_t wrapChainForFrameGen(Dx12Surface* s, DeviceContext& ctx) {

    if (s == nullptr || s->swapChain == nullptr || !ctx.device || !ctx.queue) {
        return 0;
    }
    const int32_t picked = fgPickedBackend();
    if (picked != MCDR_FG_BACKEND_XESSFG && picked != MCDR_FG_BACKEND_FSRFG) {
        return 0;
    }
    auto* appChain = s->swapChain.Detach();
    void* wrapped = nullptr;
    const int32_t ok = fgWrapPresentChain(
        ctx.device.Get(), ctx.queue.Get(), appChain, static_cast<int32_t>(s->width),
        static_cast<int32_t>(s->height), static_cast<int32_t>(s->swapChainFormat), &wrapped);
    if (ok != 1 || wrapped == nullptr) {
        dbgLogWarn("frame generation did not take the app chain; the swap chain is gone and must be rebuilt");
        s->backBuffers.clear();
        s->currentImageIndex = -1;
        s->lastBlitIndex = -1;
        s->suboptimal = true;
        return -1;
    }
    auto* rawProxy = static_cast<IDXGISwapChain4*>(wrapped);
    s->swapChain.Attach(rawProxy);
    DXGI_SWAP_CHAIN_DESC1 proxyDesc{};
    if (SUCCEEDED(rawProxy->GetDesc1(&proxyDesc))) {
        const bool proxyTearing = (proxyDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
        if (proxyTearing != s->allowTearing) {
            dbgLogInfo("proxy chain allowTearing=%d (app chain had %d), following the proxy",
                       static_cast<int>(proxyTearing), static_cast<int>(s->allowTearing));
        }
        s->allowTearing = proxyTearing;
        dbgLogInfo("proxy chain %ux%u buffers=%u format=%d effect=%d", proxyDesc.Width,
                   proxyDesc.Height, proxyDesc.BufferCount, static_cast<int>(proxyDesc.Format),
                   static_cast<int>(proxyDesc.SwapEffect));
    }
    dbgLogInfo("presenting through the wrapped chain (%ux%u)", s->width, s->height);
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
    if (width <= 0 || height <= 0) {
        s->presentMode = presentMode;
        return {};
    }
    const int curW = static_cast<int>(s->width);
    const int curH = static_cast<int>(s->height);
    if (curW > 0 && curH > 0 && width == curW && height > curH * 2) {
        dbgLog("configureSurface: rejecting invalid height %d (current %d), keeping old", height, s->height);
        s->presentMode = presentMode;
        return {};
    }
    s->presentMode = presentMode;
    if (s->host != nullptr) {
        mcdr_host_resize(static_cast<MCDR_WindowHost*>(s->host), width, height);
    } else {
        resizePresentationWindow(s->presentWindow, static_cast<UINT>(width), static_cast<UINT>(height));
    }

    if (!force && static_cast<UINT>(width) == s->width && static_cast<UINT>(height) == s->height) {
        dbgLogInfo("configureSurface: size unchanged %dx%d, skip ResizeBuffers (total=%.1f ms)", width, height,
               nowMs() - p98t0);
        return {};
    }

    const double p98tWaitRender0 = nowMs();
    if (!waitForRenderThreadSubmit()) return mcdr::err("configureSurface", "Render submission did not drain");
    p98waitRenderMs = nowMs() - p98tWaitRender0;
    if (!waitForPendingPresents(2000)) {
        return mcdr::err("configureSurface", "Pending presents did not drain before ResizeBuffers");
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
    if (mcdr_dlss_invalidate_inputs() != 1) return mcdr::err("configureSurface", "Streamline input invalidation failed");
    dbgLogDebug("configureSurface: idle ok, ResizeBuffers %dx%d mode=%d", width, height, presentMode);
    if (s->bufferHeld.exchange(false)) {
        dbgLogDebug("configureSurface: releasing acquired backbuffer idx=%d before ResizeBuffers",
               s->currentImageIndex);
        s->swapChain->Present(0, 0);
        s->currentImageIndex = -1;
    }
    if (s->compositionMode) {
        auto unbind = setCompositionContent(s->dcompVisual.Get(), s->dcompDevice.Get(), nullptr);
        if (!unbind) {
            dbgLogError("configureSurface: unbind composition content failed: %s",
                        unbind.error().message.c_str());
        }
    }
    for (auto& bb : s->backBuffers) {
        if (bb) bb.Reset();
    }
    s->backBuffers.clear();
    const DXGI_FORMAT scFmt = s->swapChainFormat;
    const UINT scFlags =
        s->compositionMode ? 0 : (s->allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    HRESULT hr = S_OK;
    dbgLogInfo("configureSurface: target=%ux%u presentWindow=%p", static_cast<unsigned>(width),
               static_cast<unsigned>(height), reinterpret_cast<void*>(s->presentWindow));
    logWindowState("configureSurface child", s->presentWindow);
    logWindowState("configureSurface parent", s->presentWindow ? GetParent(s->presentWindow) : nullptr);
    for (int retry = 0; retry < 10; ++retry) {
        if (retry > 0) {
            int waitMs = (1 << retry);
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
        if (retry == 0) {
            dbgLog("configureSurface: ResizeBuffers failed %s (flags=0x%X tearing=%d) — retrying",
                   hrText(hr).c_str(), scFlags, static_cast<int>(s->allowTearing));
        } else {
            dbgLogDebug("configureSurface: ResizeBuffers failed %s (retry=%d)", hrText(hr).c_str(),
                        retry);
        }
        if (hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE && hr != DXGI_ERROR_INVALID_CALL) break;
    }
    if (FAILED(hr)) {
        dbgLogError("configureSurface: ResizeBuffers FAILED after 10 retries, falling back to recreate");
        if (s->bufferHeld.exchange(false)) {
            s->swapChain->Present(0, 0);
            s->currentImageIndex = -1;
        }
        if (auto idle = deviceWaitIdle(); !idle) return std::unexpected<mcdr::Error>(idle.error());
        s->backBuffers.clear();
        s->swapChain.Reset();
        if (!fgUnwrapPresentChain()) return mcdr::err("configureSurface", "Frame generation teardown is still active");
        s->currentImageIndex = -1;
        s->lastBlitIndex = -1;

        ComPtr<IDXGIFactory4> factory;
        hr = createDxgiFactory(factory);
        if (FAILED(hr)) {
            return mcdr::err("configureSurface", "CreateDXGIFactory1 failed " + hrText(hr));
        }
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
            paintParentBlackBeforeDestroy(s->presentWindow);
            DestroyWindow(s->presentWindow);
            s->presentWindow = nullptr;
        }
        if (FAILED(bundle.swapChain.As(&s->swapChain))) {
            return mcdr::err("configureSurface", "swapchain does not support IDXGISwapChain3");
        }
        bundle.swapChain.Reset();
        s->compositionMode = bundle.composition;
        s->dcompDevice = bundle.dcompDevice;
        s->dcompTarget = bundle.dcompTarget;
        s->dcompVisual = bundle.dcompVisual;
        s->allowTearing = bundle.allowTearing;
        dbgLogInfo("configureSurface: recreated swapchain %dx%d (composition=%d)", width, height,
               static_cast<int>(s->compositionMode));
    }

    if (s->compositionMode) {
        auto rebind = setCompositionContent(s->dcompVisual.Get(), s->dcompDevice.Get(), s->swapChain.Get());
        if (!rebind) {
            return mcdr::err("configureSurface",
                             "configureSurface: rebind composition content failed: " +
                                 rebind.error().message);
        }
    }

    s->width = static_cast<UINT>(width);
    s->height = static_cast<UINT>(height);

    if (wrapChainForFrameGen(s, ctx) < 0) {
        if (!fgAbortWrapAfterFailure(
            "frame generation could not take the swap chain and was turned off; "
            "the swap chain was rebuilt without it")) {
            return mcdr::err("configureSurface", "Frame generation cleanup failed after swap chain wrapping");
        }
        auto plain = rebuildSwapChainForFg(s, "wrap-failed");
        if (!plain) {
            return mcdr::err("configureSurface",
                             "recreating a plain swap chain after a failed frame generation wrap "
                             "failed: " + plain.error().message);
        }
        return {};
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
        hr = s->swapChain->GetBuffer(i, IID_PPV_ARGS(&s->backBuffers[i]));
        if (FAILED(hr)) {
            return mcdr::err("configureSurface", "GetBuffer failed " + hrText(hr));
        }
        ctx.device->CreateRenderTargetView(s->backBuffers[i].Get(), nullptr, s->rtvHandles[i]);
    }
    if (!s->backBuffers.empty() && s->backBuffers[0]) {
        const D3D12_RESOURCE_DESC bd0 = s->backBuffers[0]->GetDesc();
        dbgLogDebug("configureSurface: swapchain buffer0 = %llux%u (requested %dx%d)",
               static_cast<unsigned long long>(bd0.Width), static_cast<unsigned>(bd0.Height),
               static_cast<int>(width), static_cast<int>(height));
    }
    setActiveSurface(s);
    fgOnResize(static_cast<int32_t>(width), static_cast<int32_t>(height));
    dbgLogInfo("configureSurface: registered active surface=%p bbCount=%u", reinterpret_cast<void*>(s),
               static_cast<UINT>(s->backBuffers.size()));
    {
        const double p98total = nowMs() - p98t0;
        const double p98other = p98total - (p98waitRenderMs < 0.0 ? 0.0 : p98waitRenderMs) -
                                (p98waitPresentsMs < 0.0 ? 0.0 : p98waitPresentsMs) -
                                (p98idleMs < 0.0 ? 0.0 : p98idleMs);
        dbgLogInfo("configureSurface: %dx%d total=%.1f ms (waitRender=%.1f waitPresents=%.1f idle=%.1f resize+rest=%.1f)",
               width, height, p98total, p98waitRenderMs, p98waitPresentsMs, p98idleMs, p98other);
    }
    return {};
}

mcdr::Result<void> rebuildSwapChainForFg(Dx12Surface* s, const char* why, void* recording) {
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
    if ((!recording || gAsyncRenderCtx.load(std::memory_order_acquire) != recording) &&
        !waitForRenderThreadSubmit())
        return mcdr::err("rebuildSwapChainForFg", "Render submission did not drain");
    if (!waitForPendingPresents(2000)) return mcdr::err("rebuildSwapChainForFg", "Pending presents did not drain");
    const bool held = s->bufferHeld.exchange(false);
    if (held && s->swapChain != nullptr) {
        s->swapChain->Present(0, 0);
    }
    s->currentImageIndex = -1;
    if (auto idle = deviceWaitIdle(); !idle) return std::unexpected<mcdr::Error>(idle.error());
    if (mcdr_dlss_invalidate_inputs() != 1) return mcdr::err("rebuildSwapChainForFg", "Streamline input invalidation failed");
    s->backBuffers.clear();
    s->swapChain.Reset();
    if (!fgUnwrapPresentChain()) return mcdr::err("rebuildSwapChainForFg", "Frame generation teardown is still active");
    s->lastBlitIndex = -1;

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = createDxgiFactory(factory);
    if (FAILED(hr)) {
        return mcdr::err("rebuildSwapChainForFg", "CreateDXGIFactory1 failed " + hrText(hr));
    }
    const UINT w = s->width == 0 ? 1u : s->width;
    const UINT h = s->height == 0 ? 1u : s->height;
    for (int attempt = 0;; ++attempt) {
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
            paintParentBlackBeforeDestroy(s->presentWindow);
            DestroyWindow(s->presentWindow);
            s->presentWindow = nullptr;
        }
        if (FAILED(bundle.swapChain.As(&s->swapChain))) {
            return mcdr::err("rebuildSwapChainForFg", "swapchain does not support IDXGISwapChain3");
        }
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
        if (wrapChainForFrameGen(s, ctx) >= 0) {
            break;
        }
        if (attempt >= 2) {
            return mcdr::err("rebuildSwapChainForFg",
                             "frame generation kept failing to take the swap chain");
        }
        if (!fgAbortWrapAfterFailure(
            "frame generation could not take the swap chain and was turned off; "
            "the swap chain was rebuilt without it")) {
            return mcdr::err("rebuildSwapChainForFg", "Frame generation cleanup failed after swap chain wrapping");
        }
        s->backBuffers.clear();
        s->swapChain.Reset();
        if (auto idle = deviceWaitIdle(); !idle) return std::unexpected<mcdr::Error>(idle.error());
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
    fgOnResize(static_cast<int32_t>(w), static_cast<int32_t>(h));
    dbgLogInfo("rebuildSwapChainForFg: %ux%u done (why=%s composition=%d tearing=%d chainOwner=%d)",
               static_cast<unsigned>(w), static_cast<unsigned>(h), why == nullptr ? "?" : why,
               static_cast<int>(s->compositionMode), static_cast<int>(s->allowTearing),
               static_cast<int>(fgChainOwner()));
    return {};
}

mcdr::Result<void> prepareDisplayPresentChain(Dx12Surface* s, void* recording) {
    if (!s || !s->swapChain) return mcdr::err("prepareDisplayPresentChain", "surface not created");
    if (!fgChainRebuildPending()) return {};
    if (auto rebuilt = rebuildSwapChainForFg(s, "fg-toggle", recording); !rebuilt) {
        s->suboptimal = true;
        const auto& reason = rebuilt.error().message;
        fgNoteChainRebuildDone(fgChainOwner(), 0, reason.c_str());
        return mcdr::err("prepareDisplayPresentChain", reason);
    }
    fgNoteChainRebuildDone(fgChainOwner(), 1, nullptr);
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
    if (!waitForPendingPresents(2000)) {
        s->currentImageIndex = -1;
        return mcdr::err("acquireSurface",
                         "acquireSurface: present sync timeout (render thread stalled) — frame skipped to "
                         "avoid writing a non-current back buffer");
    }
    if (!fgDisplayFrameStarted() && beginDisplayFrameRecording(nullptr) != 1)
        return mcdr::err("acquireSurface", "display frame could not start");
    dbgLogDebug("acquireSurface: enter surface=%p", reinterpret_cast<void*>(s));
    UINT rawIdx = s->swapChain->GetCurrentBackBufferIndex();
    s->currentImageIndex = static_cast<int>(rawIdx);
    UINT bbCount = static_cast<UINT>(s->backBuffers.size());
    dbgLogDebug("acquireSurface: rawIdx=%u bbCount=%u currentImageIndex=%d", rawIdx, bbCount,
                s->currentImageIndex);
    if (s->currentImageIndex < 0 || s->currentImageIndex >= static_cast<int>(bbCount)) {
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
    s->lastBlitIndex = s->currentImageIndex;
    s->blittedThisFrame.store(false, std::memory_order_release);
    s->bufferHeld.store(true, std::memory_order_release);
    return {};
}

uintptr_t getBackBufferHandle(Dx12Surface* s) {
    if (!s || s->currentImageIndex < 0 || s->currentImageIndex >= static_cast<int>(s->backBuffers.size())) {
        return 0ULL;
    }
    return reinterpret_cast<uintptr_t>(s->backBuffers[static_cast<size_t>(s->currentImageIndex)].Get());
}

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
    if (!srcTex || !srcTex->resource) {
        srcTex = nullptr;
    }

    ID3D12GraphicsCommandList* cmd = ctx->commandList;
    ID3D12Resource* dst = s->backBuffers[static_cast<size_t>(s->currentImageIndex)].Get();
    UINT w = s->width;
    UINT h = s->height;
    dbgLogDebug("blitSurface: cmd=%p dst=%p idx=%d w=%u h=%u srcTex=%p", reinterpret_cast<void*>(cmd),
                reinterpret_cast<void*>(dst), s->currentImageIndex, w, h, reinterpret_cast<void*>(srcTex));

    for (UINT i = 0; i < s->backBuffers.size(); ++i) {
        ID3D12Resource* bb = s->backBuffers[i].Get();
        if (bb && ctx->resourceState.find(bb) == ctx->resourceState.end()) {
            ctx->resourceState[bb] = D3D12_RESOURCE_STATE_PRESENT;
        }
    }
    if (ctx->inRenderPass) {
        static int p109InPassBlits = 0;
        if (p109InPassBlits < 3) {
            ++p109InPassBlits;
            dbgLogWarn("blitSurface recorded INSIDE an open render pass (ctx=%p idx=%d): "
                       "the pass' attachments were rebound to the back buffer",
                       reinterpret_cast<void*>(ctx), s->currentImageIndex);
        }
    }

    if (srcTex) transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = dst;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (!srcTex) {
        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->rtvHandles[static_cast<size_t>(s->currentImageIndex)];
        float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        dbgLogDebug("blitSurface: srcTex=null — clear backbuffer to black");
        return {};
    }

    {
        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->rtvHandles[static_cast<size_t>(s->currentImageIndex)];
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_RESOURCE_DESC bbDesc = dst->GetDesc();
        D3D12_VIEWPORT blitVp{0.0f, 0.0f, static_cast<FLOAT>(bbDesc.Width),
                              static_cast<FLOAT>(bbDesc.Height), 0.0f, 1.0f};
        invalidateGraphicsInputBindings(ctx, false);
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
        {
            cmd->SetGraphicsRootSignature(bp->rootSig.Get());
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
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            dbgLogDebug("blitSurface: drawIndexed inst=1 firstIdx=0 firstVert=0");
            cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);
            dbgLogDebug("blitSurface: drawIndexed done");
        }

        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_PRESENT);
        cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

        if (srcTex) transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COMMON);
    }
    if (srcTex && srcTex->resource) {
        D3D12_RESOURCE_DESC srcDesc = srcTex->resource->GetDesc();
        bool srcWasWritten = ctx->colorTargetsWritten;
        dbgLogDebug("blitSurface: ctx=%p ctxW=%d src=%p -> backbuf=%ux%u", reinterpret_cast<void*>(ctx),
                    static_cast<int>(ctx->colorTargetsWritten), reinterpret_cast<void*>(srcTex), w, h);
        dbgLogDebug("blitSurface: src=%p srcW=%llu srcH=%llu fmt=%d wasWritten=%d -> backbuf=%ux%u",
                    reinterpret_cast<void*>(srcTex), static_cast<unsigned long long>(srcDesc.Width),
                    static_cast<unsigned long long>(srcDesc.Height), static_cast<int>(srcTex->dxgiFormat),
                    static_cast<int>(srcWasWritten), w, h);
    } else {
        dbgLogDebug("blitSurface: srcTex=null (pure red clear), no src diagnostic");
    }
    s->lastBlitIndex = s->currentImageIndex;
    s->blittedThisFrame.store(true, std::memory_order_release);
    return {};
}

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

    if (prepareSampledTextureWrite(ctx, dst, 0, 1) != SampledTextureContentStatus::Ready)
        return mcdr::err("blitTextureToTexture", "Sampled texture contents could not be preserved before writing");
    D3D12_CPU_DESCRIPTOR_HANDLE dstRtv = rtv.value();
    invalidateGraphicsInputBindings(ctx, false);
    cmd->OMSetRenderTargets(1, &dstRtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<FLOAT>(dstDesc.Width), static_cast<FLOAT>(dstDesc.Height), 0.0f, 1.0f};
    cmd->RSSetViewports(1, &vp);
    D3D12_RECT sc{0, 0, static_cast<LONG>(dstDesc.Width), static_cast<LONG>(dstDesc.Height)};
    cmd->RSSetScissorRects(1, &sc);
    transitionTextureTo(ctx, dst, D3D12_RESOURCE_STATE_RENDER_TARGET);

    cmd->SetGraphicsRootSignature(bp->rootSig.Get());
    cmd->SetPipelineState(bp->pso.Get());
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

    if (prepareSampledTextureWrite(ctx, dst, 0, 1) != SampledTextureContentStatus::Ready)
        return mcdr::err("sharpenTextureToTexture", "Sampled texture contents could not be preserved before writing");
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
    invalidateGraphicsInputBindings(ctx, false);
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

    const float params[4] = {
        srcDesc.Width == 0 ? 0.0f : 1.0f / static_cast<float>(srcDesc.Width),
        srcDesc.Height == 0 ? 0.0f : 1.0f / static_cast<float>(srcDesc.Height), amount, 0.0f};
    cmd->SetGraphicsRoot32BitConstants(1, 4, params, 0);

    cmd->IASetVertexBuffers(0, 1, &sp->vbView);
    cmd->IASetIndexBuffer(&sp->ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    transitionTextureTo(ctx, src, D3D12_RESOURCE_STATE_RENDER_TARGET);
    dbgLogDebug("sharpenTextureToTexture: %llux%llu -> %llux%llu amount=%.3f", srcDesc.Width,
                srcDesc.Height, dstDesc.Width, dstDesc.Height, amount);
    return {};
}

SurfacePresentResult presentSurface(Dx12Surface* s, FgPresentTicket ticket) {
    SurfacePresentResult result;
    if (!s) {
        fgAbortDisplayFrame(ticket);
        return result;
    }
    std::lock_guard<std::mutex> presentLock(s->presentMutex);
    if (ticket.serial == 0) ticket = s->frameGenerationTicket;
    if (ticket.serial != 0 && !fgDisplayFrameCurrent(ticket)) {
        result.outcome = MCDR_PRESENT_CANCELLED;
        return result;
    }
    struct CompleteFrame {
        FgPresentTicket ticket;
        ~CompleteFrame() {
            fgAbortDisplayFrame(ticket);
        }
    } completion{ticket};
    if (!s || !s->swapChain) {
        return result;
    }
    const auto checkPresentInputs = [] {
        if (mcdr_dlss_inputs_ready_for_present() == 1) return true;
        dbgLogError("presentSurface: Streamline input invalidation is still pending");
        return false;
    };
    if (!checkPresentInputs()) return result;
    if (!s->blittedThisFrame || s->currentImageIndex < 0) {
        return result;
    }
    const UINT syncInterval = (s->presentMode == 0 && !s->exclusive) ? 0 : 1;
    const UINT flags = (s->presentMode == 0 && s->allowTearing && !s->exclusive)
                           ? DXGI_PRESENT_ALLOW_TEARING
                           : 0;
    const bool fgDeviceAlive = !deviceRemoved();
    if (fgDeviceAlive) {
        fgBeforePresent(ticket);
    } else {
        static std::atomic<bool> fgDeviceLostLogged{false};
        if (!fgDeviceLostLogged.exchange(true, std::memory_order_relaxed)) {
            dbgLogWarn("presentSurface: device removed -> frame generation hooks skipped");
        }
    }
    if (!checkPresentInputs()) return result;
    mcdr_reflex_before_present();
    HRESULT hr = s->swapChain->Present(syncInterval, flags);
    result.dxgiStatus = static_cast<int32_t>(hr);
    result.flags = MCDR_PRESENT_DXGI_STATUS_VALID;
    result.outcome = hr == S_OK ? MCDR_PRESENT_SUCCEEDED :
        hr == DXGI_STATUS_OCCLUDED ? MCDR_PRESENT_OCCLUDED :
        hr == DXGI_STATUS_MODE_CHANGED ? MCDR_PRESENT_MODE_CHANGED :
        FAILED(hr) ? MCDR_PRESENT_FAILED : MCDR_PRESENT_UNKNOWN;
    if (s->host != nullptr && SUCCEEDED(hr) && !s->hostRevealed) {
        s->hostRevealed = true;
        mcdr_host_reveal(static_cast<MCDR_WindowHost*>(s->host));
    }
    mcdr_reflex_after_present();
    if (fgDeviceAlive && !deviceRemoved()) {
        const auto publication = fgAfterPresent(ticket, hr == S_OK ? 1 : 0);
        result.flags |= publication.flags;
        result.outputFrameCount = publication.outputFrameCount;
    } else {
        invalidateFrameGenerationHistory(1);
    }
    s->currentImageIndex = -1;
    s->frameGenerationTicket = {};
    s->blittedThisFrame.store(false, std::memory_order_release);
    s->bufferHeld.store(false, std::memory_order_release);
    if (hr == DXGI_STATUS_OCCLUDED) {
        dbgLogInfo("presentSurface: OCCLUDED (window fully occluded) -> suboptimal");
        s->suboptimal = true;
    } else if (hr == DXGI_STATUS_MODE_CHANGED) {
        dbgLogInfo("presentSurface: MODE_CHANGED -> suboptimal");
        s->suboptimal = true;
    } else if (FAILED(hr)) {
        dbgLogError("presentSurface: FAILED %s", hrText(hr).c_str());
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            logDeviceStatus("presentSurface: FAILED");
        }
        s->suboptimal = true;
    } else {
        dbgLogDebug("presentSurface: ok (syncInterval=%u)", syncInterval);
        s->suboptimal = false;
    }
    return result;
}

void setHdrMode(int32_t mode) {
    mcdr_hdr_state_set_mode(mode);
    dbgLogInfo("HDR mode set to %d (applies on next swapchain creation)", mode);
}

int32_t hdrActiveFlag() { return mcdr_hdr_state_active(); }

mcdr::Result<void> destroySurface(Dx12Surface* s) {
    if (!s) return {};
    dbgLogDebug("destroySurface: enter surface=%p", reinterpret_cast<void*>(s));
    dbgLogProgress("destroySurface stage: enter");
    HWND presentChild = s->presentWindow;
    MCDR_WindowHost* const surfaceHost = static_cast<MCDR_WindowHost*>(s->host);
    const bool preparedWindow = prepareSurfaceWindowForDestroy(s->hwnd, presentChild, surfaceHost);
    if (preparedWindow) dbgLogProgress("destroySurface stage: presentation hidden and parent painted");
    if (s->swapChain && s->exclusive) {
        const HRESULT fsHr = s->swapChain->SetFullscreenState(FALSE, nullptr);
        s->exclusive = false;
        dbgLogInfo("destroySurface: left exclusive fullscreen hr=0x%08X", static_cast<unsigned>(fsHr));
    }
    if (!waitForRenderThreadSubmit()) return mcdr::err("destroySurface", "Render submission did not drain");
    dbgLogProgress("destroySurface stage: waited render thread");
    if (auto idle = deviceWaitIdle(); !idle) return std::unexpected<mcdr::Error>(idle.error());
    dbgLogProgress("destroySurface stage: device idle");
    if (mcdr_dlss_invalidate_inputs() != 1) return mcdr::err("destroySurface", "Streamline input invalidation failed");
    s->backBuffers.clear();
    s->swapChain.Reset();
    if (!fgReleaseProviderWithWait(nullptr)) return mcdr::err("destroySurface", "Frame generation teardown is still active");
    s->host = nullptr;
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
    dbgLogProgress("destroySurface stage: fence wait done");
    flushPendingDeletes();
    dbgLogProgress("destroySurface stage: pending deletes flushed");
    if (getActiveSurface() == s) setActiveSurface(nullptr);
    s->presentWindow = nullptr;
    dbgLogProgress("destroySurface stage: deleting surface");
    delete s;
    dbgLogProgress("destroySurface stage: surface deleted");
    logWindowState("destroySurface child(before)", presentChild);
    logWindowState("destroySurface parent(before)", presentChild ? GetParent(presentChild) : nullptr);
    if (surfaceHost != nullptr) {
        mcdr_host_release(surfaceHost);
    } else if (presentChild != nullptr) {
        DestroyWindow(presentChild);
    }
    logWindowState("destroySurface parent(after)", presentChild ? GetParent(presentChild) : nullptr);
    dbgLogDebug("destroySurface: done surface=%p", reinterpret_cast<void*>(s));
    return {};
}

void destroySurfaceNoWaitIdle(Dx12Surface* s) {
    if (!s) return;
    if (getActiveSurface() == s) setActiveSurface(nullptr);
    HWND presentChild = s->presentWindow;
    MCDR_WindowHost* const surfaceHost = static_cast<MCDR_WindowHost*>(s->host);
    const bool preparedWindow = prepareSurfaceWindowForDestroy(s->hwnd, presentChild, surfaceHost);
    if (preparedWindow) dbgLogProgress("destroySurface stage: presentation hidden and parent painted");
    s->presentWindow = nullptr;
    s->host = nullptr;
    dbgLogProgress("destroySurface stage: deleting surface");
    delete s;
    dbgLogProgress("destroySurface stage: surface deleted");
    if (surfaceHost != nullptr) {
        mcdr_host_release(surfaceHost);
    } else if (presentChild != nullptr) {
        DestroyWindow(presentChild);
    }
}

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
    dbgDumpPixelsToFile(base, w, h, pitch, "backbuf");
    staging->Unmap(0, nullptr);
    return {};
}

}
