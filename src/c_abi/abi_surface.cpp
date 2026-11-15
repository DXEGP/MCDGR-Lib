// abi_surface.cpp - DXGI swapchain surface: create/configure/acquire/blit/present, back-buffer handles, pixel readback

module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件
#include "mcdr_dlss_impl.h"  // mcdr_dlss_output_state()：DLSS 输出纹理的真实状态
#include "mcdr_hdr_state.h"  // 建链时实际用掉的 HDR 模式（普通 TU 的 C 状态）
#include "mcdr_rt_state.h"  // 光追可见性分层总开关（普通 TU 的 C 状态）
#include "mcdr_surface_opts_state.h"  // 交换链建链选项（普通 TU 的 C 状态）

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;
// mcdr.error 是叶子模块，不 import 就取不到 mcdr::Error（import 不传递）。
import mcdr.error;

using namespace d3d12_mc;

namespace {

// 把 Result 失败原因折进纯文本诊断串（不带 "where: " 前缀，failWith 自己补），成功返回 1。
int32_t bindErr(const mcdr::Result<void>& r, std::string& err) {
    if (!r) {
        err = r.error().message;
        return 0;
    }
    return 1;
}

// Reads a 3x3 sample grid out of `res`; writes 9 pixels x RGBA as 36 ints, raw bytes 0-255.
// `fromState` 必须是这张纹理**当前真实所处的状态**：写死 COMMON 时，被 GPU 写过的纹理（停在
// COPY_SOURCE / UNORDERED_ACCESS）会得到非法 barrier，读回内容未定义（恒为 0）。
// pitch 按 D3D12_TEXTURE_DATA_PITCH_ALIGNMENT（256B）上对齐；Map 前必须等到 queue fence，
// 否则读到的是上一帧内容。
int32_t readbackSamples36(ID3D12Resource* res, UINT w, UINT h, UINT bpp, DXGI_FORMAT fbFmt,
                          D3D12_RESOURCE_STATES fromState, D3D12_RESOURCE_STATES finalState,
                          int32_t* out, std::string& err) {
    DeviceContext& ctx = deviceContext();
    if (!ctx.device || !ctx.queue || !res) {
        err = "device not initialized";
        return 0;
    }
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        err = "bad dimensions";
        return 0;
    }
    if (const auto r = deviceWaitIdle(); !bindErr(r, err)) {
        return 0;
    }

    const UINT64 rowBytes = static_cast<UINT64>(w) * bpp;
    const UINT64 pitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                         & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 total = pitch * h;

    static ComPtr<ID3D12Resource> staging;
    static ComPtr<ID3D12CommandAllocator> alloc;
    static ComPtr<ID3D12GraphicsCommandList> cl;
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
        if (FAILED(ctx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&staging)))) {
            err = "readback staging create failed";
            return 0;
        }
    }
    if (!alloc) {
        ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    }
    if (!cl) {
        ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                      IID_PPV_ARGS(&cl));
    } else {
        alloc->Reset();
        cl->Reset(alloc.Get(), nullptr);
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = fromState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = staging.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = fbFmt;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = finalState;
    cl->ResourceBarrier(1, &b);
    const HRESULT closeHr = cl->Close();
    if (FAILED(closeHr)) {
        dbgLogError("readbackSamples36: Close hr=0x%08X fmt=%d %ux%u", static_cast<unsigned>(closeHr),
                    static_cast<int>(fbFmt), w, h);
    }

    ID3D12CommandList* lists[] = {cl.Get()};
    ctx.queue->ExecuteCommandLists(1, lists);
    const UINT64 fv = ++ctx.queueFenceValue;
    ctx.queue->Signal(ctx.queueFence.Get(), fv);
    if (const auto r = waitForQueueFenceValue(fv, 5'000'000'000ULL); !bindErr(r, err)) {
        return 0;
    }

    void* ptr = nullptr;
    if (FAILED(staging->Map(0, nullptr, &ptr))) {
        const HRESULT rr2 = ctx.device ? ctx.device->GetDeviceRemovedReason() : E_FAIL;
        dbgLogError("readback: map failed fmt=%d %ux%u reason=0x%08X", static_cast<int>(fbFmt),
                    w, h, static_cast<unsigned>(rr2));
        err = "staging Map failed";
        return 0;
    }
    const uint8_t* base = static_cast<const uint8_t*>(ptr);
    const int xs[3] = {0, static_cast<int>(w) / 2, static_cast<int>(w) - 1};
    const int ys[3] = {0, static_cast<int>(h) / 2, static_cast<int>(h) - 1};
    for (int yi = 0; yi < 3; ++yi) {
        for (int xi = 0; xi < 3; ++xi) {
            const uint8_t* p = base + static_cast<UINT64>(ys[yi]) * pitch + static_cast<UINT64>(xs[xi]) * bpp;
            const int o = (yi * 3 + xi) * 4;
            out[o + 0] = p[0];
            out[o + 1] = p[1];
            out[o + 2] = p[2];
            out[o + 3] = p[3];
        }
    }
    staging->Unmap(0, nullptr);
    return 1;
}

}  // namespace

extern "C" {

mcdr_surface* MCDR_CALL mcdr_dx12CreateSurface(uint64_t hwnd) {
    dbgLogDebug("dx12CreateSurface: hwnd=0x%llx (long=%lld)", static_cast<unsigned long long>(hwnd),
                static_cast<long long>(hwnd));
    const auto r = createSurface(static_cast<uintptr_t>(hwnd));
    if (!r) {
        return mcdr::abi::failWithPtr<Dx12Surface>("mcdr_dx12CreateSurface", r.error().message);
    }
    return static_cast<Dx12Surface*>(*r);
}

int32_t MCDR_CALL mcdr_dx12SurfacePresentModes(int32_t* out, int32_t cap) {
// Returns the TOTAL mode count so a caller can size-then-retry; only min(cap, total) are written.
    const std::vector<int> modes = surfacePresentModes();
    const int32_t total = static_cast<int32_t>(modes.size());
    if (out) {
        const int32_t n = cap < total ? cap : total;
        for (int32_t i = 0; i < n; ++i) {
            out[i] = modes[static_cast<size_t>(i)];
        }
    }
    return total;
}

int32_t MCDR_CALL mcdr_dx12ConfigureSurface(mcdr_surface* surface, int32_t width, int32_t height,
                                            int32_t presentMode) {
    if (const auto r = configureSurface(surface, width, height, presentMode); !r) {
// A failed configure freezes the picture, so it must reach dx12-native.log rather than only stderr.
        d3d12_mc::dbgLogError("dx12ConfigureSurface FAILED %dx%d mode=%d: %s", width, height, presentMode,
                         r.error().message.c_str());
        return mcdr::abi::failWith("mcdr_dx12ConfigureSurface", r.error().message);
    }
    d3d12_mc::dbgLogInfo("dx12ConfigureSurface: ok %dx%d mode=%d", width, height, presentMode);
    return 1;
}

// 真独占全屏：SDL 独占只改显示模式，DXGI 侧必须显式 SetFullscreenState。
int32_t MCDR_CALL mcdr_dx12SetExclusiveFullscreen(mcdr_surface* surface, int32_t on) {
    if (const auto r = setExclusiveFullscreen(surface, on != 0); !r) {
        // 非致命：驱动/覆盖层拒绝时画面仍可走窗口化，记 WARN 即可，不要升级成 ERROR。
        d3d12_mc::dbgLogWarn("dx12SetExclusiveFullscreen(%d) failed: %s", on ? 1 : 0,
                             r.error().message.c_str());
        return 0;
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12AcquireSurface(mcdr_surface* surface) {
    if (const auto r = acquireSurface(surface); !r) {
        return mcdr::abi::failWith("mcdr_dx12AcquireSurface", r.error().message);
    }
// Once per frame: DEBUG level only (needs DX12_LOG_VERBOSE=1); an stderr+file flush here is costly.
    d3d12_mc::dbgLogDebug("acquireSurface: ok surface=%p", static_cast<void*>(surface));
    return 1;
}

void MCDR_CALL mcdr_dx12BlitSurface(mcdr_command_ctx* ctx, mcdr_surface* surface, mcdr_object* texture) {
    if (const auto r = blitSurface(ctx, surface, texture); !r) {
// blit 失败时 surface->blittedThisFrame 仍为 false，presentSurface() 会跳过这一帧的 Present（不会
// flip 到未写入的 back buffer）；这里额外在 [dx12] sink 打点，别只 grep 一个 sink。
        static unsigned long long blitFailures = 0;
        ++blitFailures;
        mcdr::abi::failVoid("mcdr_dx12BlitSurface", r.error().message);
        if (blitFailures <= 5 || blitFailures % 60 == 0) {
            d3d12_mc::dbgLogWarn(
                "blit failure #%llu, present skipped",
                blitFailures);
        }
        return;
    }
// 每帧成功路径：必须 debug 级，INFO 会刷爆日志。
    d3d12_mc::dbgLogDebug("blitSurface: ok ctx=%p surface=%p texture=%p", static_cast<void*>(ctx),
                         static_cast<void*>(surface), static_cast<void*>(texture));
}

void MCDR_CALL mcdr_dx12PresentSurface(mcdr_surface* surface) {
// present 只能发生一次：异步路径下渲染线程已 Present 过（gAsyncOwnsPresent），且 gAsyncRenderCtx
// 非空证明本帧的 Present 归渲染线程所有 —— 主线程再 Present 会双 flip 并和渲染线程的
// ExecuteCommandLists/Present 竞争同一交换链 → DEVICE_REMOVED 0x887A0005。
    if (gAsyncOwnsPresent || gAsyncRenderCtx != nullptr) {
        gAsyncOwnsPresent = false;
        d3d12_mc::dbgLogDebug("present skipped: render thread owns surface=%p",
                              static_cast<void*>(surface));
        return;
    }
    presentSurface(surface);
// 真实结果（ok / OCCLUDED / MODE_CHANGED / FAILED）由 presentSurface() 自己记录。
}

uint64_t MCDR_CALL mcdr_dx12GetBackBufferHandle(mcdr_surface* surface) {
    return static_cast<uint64_t>(getBackBufferHandle(surface));
}

// 两个公开导出共用：`fromState` 由调用方给出这张纹理当前的真实状态，读回后恢复成同一状态，
// 对 native 内部的状态跟踪没有净影响。
static int32_t readbackTexturePixelsCore(mcdr_object* texture, int32_t* out, int32_t fromState,
                                         const char* fn) {
    if (!out) {
        return mcdr::abi::failWith(fn, "out buffer is null");
    }
    DeviceContext& ctx = deviceContext();
    Dx12Object* tex = texture;
    if (!ctx.device || !ctx.queue || !tex || tex->kind != Dx12Object::Kind::Texture || !tex->resource) {
        return mcdr::abi::failWith(
            fn,
            "invalid handle (kind=" + std::to_string(tex ? static_cast<int>(tex->kind) : -1) + ")");
    }
    ID3D12Resource* r = tex->resource.Get();
    const D3D12_RESOURCE_DESC td = r->GetDesc();
    const UINT w = static_cast<UINT>(td.Width);
    const UINT h = static_cast<UINT>(td.Height);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        return mcdr::abi::failWith(fn,
                                   "bad dims " + std::to_string(w) + "x" + std::to_string(h));
    }
    UINT bpp = 4;
    DXGI_FORMAT fbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (td.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            bpp = 4;
            fbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            bpp = 8;
            fbFmt = DXGI_FORMAT_R16G16B16A16_UNORM;
            break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            bpp = 16;
            fbFmt = DXGI_FORMAT_R32G32_FLOAT;
            break;
        default:
            bpp = 4;
            fbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
    }
// 只给 R8 家族开专用分支：bpp=1、足迹格式 = 源格式。R8 落进 default（bpp=4 + RGBA8 足迹）会因
// 拷贝足迹与源格式不兼容被 CopyTextureRegion 判非法 → 设备移除。其余格式保持原映射不动 ——
// 改成"一律用源格式"会让 compute 自检的 RGBA8 读回变成全 0。
    if (td.Format == DXGI_FORMAT_R8_UNORM || td.Format == DXGI_FORMAT_R8_UINT
        || td.Format == DXGI_FORMAT_R8_SNORM || td.Format == DXGI_FORMAT_R8_SINT) {
        bpp = 1;
        fbFmt = td.Format;
    }
    std::string err;
    const auto from = static_cast<D3D12_RESOURCE_STATES>(fromState);
    if (!readbackSamples36(r, w, h, bpp, fbFmt, from, from, out, err)) {
        return mcdr::abi::failWith(fn, err);
    }
    // Grid index of the centre sample is (yi=1, xi=1) -> element offset (1*3+1)*4 = 16.
    constexpr int kCenter = (1 * 3 + 1) * 4;
    dbgLogInfo("rbTex %ux%u center=(%d,%d) RGBA=(%d,%d,%d,%d) black=%d", w, h,
               static_cast<int>(w) / 2, static_cast<int>(h) / 2, out[kCenter + 0], out[kCenter + 1],
               out[kCenter + 2], out[kCenter + 3],
               (out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0) ? 1 : 0);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12ReadbackTexturePixels(mcdr_object* texture, int32_t* out) {
    return readbackTexturePixelsCore(texture, out, static_cast<int32_t>(D3D12_RESOURCE_STATE_COMMON),
                                     "mcdr_dx12ReadbackTexturePixels");
}

// 已知起始状态版本：给"被 GPU 写过、当前不在 COMMON"的纹理用（典型是 DLSS 输出）。状态传错
// 读回的就是未定义内容，调用方先用 mcdr_dx12DlssOutputState 问清楚，不要凭猜。
int32_t MCDR_CALL mcdr_dx12ReadbackTexturePixelsIn(mcdr_object* texture, int32_t fromState,
                                                   int32_t* out) {
    return readbackTexturePixelsCore(texture, out, fromState, "mcdr_dx12ReadbackTexturePixelsIn");
}

// DLSS 输出纹理当前所处的状态（D3D12_RESOURCE_STATES 数值）。
// 0 (COMMON) = 这一帧没有输出纹理 / 未知，调用方不要据此采样。
int32_t MCDR_CALL mcdr_dx12DlssOutputState(void) { return mcdr_dlss_output_state(); }

void MCDR_CALL mcdr_dx12DumpTextureToFile(mcdr_object* texture, const char* tag) {
// 走 dbgReadbackTexturePixels（waitIdle + 自己的命令列表，写 dx12_dump_<tag>.bmp 与 ASCII 缩略图）。
// 纹理必须在 COMMON 状态（render pass 之外），否则 barrier 校验失败。
    dbgReadbackTexturePixels(texture, (tag && *tag) ? tag : "tex");
}

int32_t MCDR_CALL mcdr_dx12IsSurfaceSuboptimal(mcdr_surface* surface) {
    return (surface && surface->suboptimal) ? 1 : 0;
}

void MCDR_CALL mcdr_dx12SetHdrMode(int32_t mode) { setHdrMode(mode); }

// 光追可见性分层总开关：此刻只做状态存储与日志，G-Buffer/采样的创建在 device 侧。
void MCDR_CALL mcdr_dx12SetRaytracingMode(int32_t mode) {
    mcdr_rt_state_set_mode(mode);
    dbgLog("raytracing mode=%d (0=off 1=ao 2=ao_shadows 3=full)", static_cast<int>(mode));
}

// Swapchain options (allow tearing / present child window): state only, applied when the chain is
// created again.
void MCDR_CALL mcdr_dx12SetSwapchainOptions(int32_t allowTearing, int32_t forcePresentChild) {
    mcdr_surface_opts_set_allow_tearing(allowTearing);
    mcdr_surface_opts_set_force_present_child(forcePresentChild);
    dbgLogInfo("swapchain: allowTearing=%d presentChild=%d",
               static_cast<int>(allowTearing), static_cast<int>(forcePresentChild));
}

// 建链时实际用掉的 HDR 模式（-1 = 还没建过链），JVM 侧据此提示需要重启。
int32_t MCDR_CALL mcdr_dx12GetHdrAppliedMode(void) { return mcdr_hdr_state_applied_mode(); }

void MCDR_CALL mcdr_dx12DestroySurface(mcdr_surface* surface) { destroySurface(surface); }

mcdr_surface* MCDR_CALL mcdr_dx12GetActiveSurfaceHandle(void) { return getActiveSurface(); }

int32_t MCDR_CALL mcdr_dx12ReadbackSurfacePixels(mcdr_surface* surface, int32_t* out) {
    if (!out) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackSurfacePixels", "out buffer is null");
    }
    DeviceContext& ctx = deviceContext();
    Dx12Surface* s = surface;
    if (!ctx.device || !ctx.queue || !s || s->backBuffers.empty()) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackSurfacePixels", (!ctx.device || !ctx.queue)
                                                                         ? "device not initialized"
                                                                         : "surface has no back buffers");
    }
    int idx = s->currentImageIndex;
    if (idx < 0 || idx >= static_cast<int>(s->backBuffers.size())) {
// present() 会把 currentImageIndex 复位成 -1，此时 GetCurrentBackBufferIndex() 报的是下一帧
// 的 buffer（还没写入，读回全 0 的假黑屏）—— 故回退到 lastBlitIndex。
        if (s->lastBlitIndex >= 0 && s->lastBlitIndex < static_cast<int>(s->backBuffers.size())) {
            idx = s->lastBlitIndex;
        } else {
            idx = static_cast<int>(s->swapChain->GetCurrentBackBufferIndex());
        }
    }
    if (idx < 0 || idx >= static_cast<int>(s->backBuffers.size())) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackSurfacePixels",
                                   "no valid back buffer (idx=" + std::to_string(idx) + ")");
    }
    ID3D12Resource* bb = s->backBuffers[static_cast<size_t>(idx)].Get();
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    const UINT w = static_cast<UINT>(bd.Width);
    const UINT h = bd.Height;
    dbgLogDebug("readback surf=0x%p idx=%d w=%u h=%u", static_cast<void*>(s), idx, w, h);
    std::string err;
// back buffer 恒为 R8G8B8A8_UNORM；拷贝后必须回 PRESENT（不是 COMMON），否则下次 Present 失败。
    if (!readbackSamples36(bb, w, h, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PRESENT,
                           D3D12_RESOURCE_STATE_PRESENT, out, err)) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackSurfacePixels", err);
    }
    dbgLogInfo("readback %ux%u center(%d,%d)=%d,%d,%d,%d corners=%d black=%d", w,
               h, static_cast<int>(w) / 2, static_cast<int>(h) / 2, out[(4 * 3 + 4) + 0],
               out[(4 * 3 + 4) + 1], out[(4 * 3 + 4) + 2], out[(4 * 3 + 4) + 3],
               (out[0] != 0 || out[1] != 0 || out[2] != 0) ? 1 : 0,
               (out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0) ? 1 : 0);
    return 1;
}

}  // extern "C"
