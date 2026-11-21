#include <windows.h>

#include <atomic>
#include <cstdint>

#include "mcdr_window_host.h"

namespace {

constexpr wchar_t kHostClass[] = L"MCDR_D3D12_SeamlessHost";
constexpr wchar_t kHostProp[] = L"MCDR_D3D12_SeamlessHostState";
constexpr UINT kMsgReveal = WM_APP + 0x4D1;

}

struct MCDR_WindowHost {
    HWND parent = nullptr;
    HWND child = nullptr;
    WNDPROC parentProc = nullptr;
    std::atomic<int32_t> minimized{0};
    std::atomic<int32_t> revealed{0};
    std::atomic<int32_t> refs{0};
    int width = 1;
    int height = 1;
};

namespace {

MCDR_WindowHost* gHost = nullptr;
HDC gSavedDc = nullptr;
HGLRC gSavedGlrc = nullptr;

struct GlApi {
    BOOL(WINAPI* makeCurrent)(HDC, HGLRC) = nullptr;
    HDC(WINAPI* getCurrentDC)(void) = nullptr;
    HGLRC(WINAPI* getCurrentContext)(void) = nullptr;
};

const GlApi& glApi() {
    static const GlApi api = []() {
        GlApi a{};
        HMODULE mod = ::GetModuleHandleW(L"opengl32.dll");
        if (mod == nullptr) mod = ::LoadLibraryW(L"opengl32.dll");
        if (mod != nullptr) {
            a.makeCurrent =
                reinterpret_cast<BOOL(WINAPI*)(HDC, HGLRC)>(::GetProcAddress(mod, "wglMakeCurrent"));
            a.getCurrentDC =
                reinterpret_cast<HDC(WINAPI*)(void)>(::GetProcAddress(mod, "wglGetCurrentDC"));
            a.getCurrentContext = reinterpret_cast<HGLRC(WINAPI*)(void)>(
                ::GetProcAddress(mod, "wglGetCurrentContext"));
        }
        return a;
    }();
    return api;
}

void resizeChild(MCDR_WindowHost* host, int width, int height) {
    if (host == nullptr || host->child == nullptr) return;
    if (width <= 0 || height <= 0) return;
    host->width = width;
    host->height = height;
    ::SetWindowPos(host->child, nullptr, 0, 0, width, height,
                   SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_DEFERERASE);
}

void paintParentBlack(HWND child) {
    if (child == nullptr) return;
    HWND parent = ::GetParent(child);
    if (parent == nullptr || ::IsWindow(parent) == 0) return;
    HDC dc = ::GetDC(parent);
    if (dc == nullptr) return;
    RECT client{};
    ::GetClientRect(parent, &client);
    ::FillRect(dc, &client, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
    ::ReleaseDC(parent, dc);
    ::GdiFlush();
}

LRESULT CALLBACK hostChildProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        ::BeginPaint(hwnd, &ps);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case kMsgReveal: {
        MCDR_WindowHost* host = gHost;
        if (host != nullptr && host->child == hwnd && host->revealed.exchange(1) == 0) {
            ::ShowWindow(hwnd, SW_SHOWNA);
            ::SetWindowPos(hwnd, HWND_TOP, 0, 0, host->width, host->height,
                           SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        return 0;
    }
    default:
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

LRESULT CALLBACK hostParentProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MCDR_WindowHost* host = static_cast<MCDR_WindowHost*>(::GetPropW(hwnd, kHostProp));
    if (host == nullptr) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            host->minimized.store(1, std::memory_order_relaxed);
        } else {
            host->minimized.store(0, std::memory_order_relaxed);
            resizeChild(host, static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
        }
        break;
    case WM_SHOWWINDOW:
        host->minimized.store(wParam != 0 ? 0 : 1, std::memory_order_relaxed);
        break;
    case WM_NCDESTROY: {
        WNDPROC prev = host->parentProc;
        ::RemovePropW(hwnd, kHostProp);
        host->parentProc = nullptr;
        host->parent = nullptr;
        if (prev != nullptr) return ::CallWindowProcW(prev, hwnd, msg, wParam, lParam);
        break;
    }
    default:
        break;
    }
    if (host->parentProc == nullptr) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    return ::CallWindowProcW(host->parentProc, hwnd, msg, wParam, lParam);
}

bool registerHostClass() {
    static const bool registered = []() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = hostChildProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kHostClass;
        if (::RegisterClassExW(&wc) != 0) return true;
        return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

void teardownHost(MCDR_WindowHost* host) {
    if (host == nullptr) return;
    if (host->parent != nullptr && host->parentProc != nullptr &&
        ::IsWindow(host->parent) != 0) {
        ::SetWindowLongPtrW(host->parent, GWLP_WNDPROC,
                            reinterpret_cast<LONG_PTR>(host->parentProc));
        ::RemovePropW(host->parent, kHostProp);
    }
    host->parentProc = nullptr;
    if (host->child != nullptr && ::IsWindow(host->child) != 0) {
        ::DestroyWindow(host->child);
    }
    host->child = nullptr;
}

}

extern "C" {

MCDR_WindowHost* mcdr_host_create(void* parentHwnd, int32_t width, int32_t height) {
    HWND parent = reinterpret_cast<HWND>(parentHwnd);
    if (parent == nullptr || ::IsWindow(parent) == 0) return nullptr;
    if (gHost != nullptr) {
        if (gHost->parent == parent && gHost->child != nullptr) {
            gHost->refs.fetch_add(1, std::memory_order_relaxed);
            resizeChild(gHost, static_cast<int>(width), static_cast<int>(height));
            return gHost;
        }
        return nullptr;
    }
    if (!registerHostClass()) return nullptr;

    MCDR_WindowHost* host = new MCDR_WindowHost();
    host->parent = parent;
    host->width = width > 0 ? static_cast<int>(width) : 1;
    host->height = height > 0 ? static_cast<int>(height) : 1;
    host->child = ::CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY, kHostClass,
                                    L"MCDR D3D12 seamless host", WS_CHILD | WS_CLIPSIBLINGS, 0, 0,
                                    host->width, host->height, parent, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
    if (host->child == nullptr) {
        delete host;
        return nullptr;
    }
    ::SetPropW(parent, kHostProp, reinterpret_cast<HANDLE>(host));
    host->parentProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        parent, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hostParentProc)));
    if (host->parentProc == nullptr) {
        ::RemovePropW(parent, kHostProp);
        ::DestroyWindow(host->child);
        delete host;
        return nullptr;
    }
    host->refs.store(1, std::memory_order_relaxed);
    gHost = host;
    return host;
}

void* mcdr_host_child(MCDR_WindowHost* host) {
    return host != nullptr ? reinterpret_cast<void*>(host->child) : nullptr;
}

void mcdr_host_resize(MCDR_WindowHost* host, int32_t width, int32_t height) {
    resizeChild(host, static_cast<int>(width), static_cast<int>(height));
}

void mcdr_host_reveal(MCDR_WindowHost* host) {
    if (host == nullptr || host->child == nullptr) return;
    ::PostMessageW(host->child, kMsgReveal, 0, 0);
}

int32_t mcdr_host_prepare_release(MCDR_WindowHost* host) {
    if (host == nullptr || host->refs.load(std::memory_order_acquire) != 1) return 0;
    if (host->child != nullptr && ::IsWindow(host->child) != 0) {
        paintParentBlack(host->child);
        HWND parent = ::GetParent(host->child);
        if (parent != nullptr && ::IsWindow(parent) != 0) {
            ::ShowWindow(parent, SW_HIDE);
            ::SetWindowPos(parent, HWND_BOTTOM, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING |
                               SWP_HIDEWINDOW);
        }
        ::ShowWindow(host->child, SW_HIDE);
        ::SetWindowPos(host->child, HWND_BOTTOM, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING |
                           SWP_HIDEWINDOW);
    }
    return 1;
}

int32_t mcdr_host_minimized(MCDR_WindowHost* host) {
    if (host == nullptr) return 0;
    return host->minimized.load(std::memory_order_relaxed);
}

int32_t mcdr_host_suspend_gl(void) {
    const GlApi& api = glApi();
    if (api.makeCurrent == nullptr || api.getCurrentContext == nullptr ||
        api.getCurrentDC == nullptr) {
        return 0;
    }
    HGLRC rc = api.getCurrentContext();
    if (rc == nullptr) return 0;
    HDC dc = api.getCurrentDC();
    if (dc == nullptr) return 0;
    if (api.makeCurrent(nullptr, nullptr) == FALSE) return 0;
    gSavedDc = dc;
    gSavedGlrc = rc;
    return 1;
}

void mcdr_host_restore_gl(void) {
    if (gSavedGlrc == nullptr) return;
    const GlApi& api = glApi();
    if (api.makeCurrent != nullptr) api.makeCurrent(gSavedDc, gSavedGlrc);
    gSavedDc = nullptr;
    gSavedGlrc = nullptr;
}

void mcdr_host_release(MCDR_WindowHost* host) {
    if (host == nullptr) return;
    if (host->refs.fetch_sub(1, std::memory_order_acq_rel) > 1) return;
    if (gHost == host) gHost = nullptr;
    teardownHost(host);
    delete host;
}

}
