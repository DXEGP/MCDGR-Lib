module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"
#include "mcdr_dlss_impl.h"
#include "mcdr_hdr_state.h"
#include "mcdr_rt_state.h"
#include "mcdr_surface_opts_state.h"

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;
import mcdr.error;

using namespace d3d12_mc;

namespace {

int32_t bindErr(const mcdr::Result<void>& r, std::string& err) {
    if (!r) {
        err = r.error().message;
        return 0;
    }
    return 1;
}

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
        dbgLogError("readbackSamples36: Map failed fmt=%d %ux%u removedReason=0x%08X", static_cast<int>(fbFmt),
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

}

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
        d3d12_mc::dbgLogError("dx12ConfigureSurface FAILED %dx%d mode=%d: %s", width, height, presentMode,
                         r.error().message.c_str());
        return mcdr::abi::failWith("mcdr_dx12ConfigureSurface", r.error().message);
    }
    d3d12_mc::dbgLogInfo("dx12ConfigureSurface: ok %dx%d mode=%d", width, height, presentMode);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetExclusiveFullscreen(mcdr_surface* surface, int32_t on) {
    if (const auto r = setExclusiveFullscreen(surface, on != 0); !r) {
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
    d3d12_mc::dbgLogDebug("acquireSurface: ok surface=%p", static_cast<void*>(surface));
    return 1;
}

void MCDR_CALL mcdr_dx12BlitSurface(mcdr_command_ctx* ctx, mcdr_surface* surface, mcdr_object* texture) {
    if (const auto r = blitSurface(ctx, surface, texture); !r) {
        static unsigned long long blitFailures = 0;
        ++blitFailures;
        mcdr::abi::failVoid("mcdr_dx12BlitSurface", r.error().message);
        if (blitFailures <= 5 || blitFailures % 60 == 0) {
            d3d12_mc::dbgLogWarn(
                "dx12BlitSurface: failure #%llu — present will be skipped for this frame",
                blitFailures);
        }
        return;
    }
    d3d12_mc::dbgLogDebug("blitSurface: ok ctx=%p surface=%p texture=%p", static_cast<void*>(ctx),
                         static_cast<void*>(surface), static_cast<void*>(texture));
}

void MCDR_CALL mcdr_dx12PresentSurface(mcdr_surface* surface) {
    if (gAsyncOwnsPresent || gAsyncRenderCtx != nullptr) {
        gAsyncOwnsPresent = false;
        d3d12_mc::dbgLogDebug("dx12PresentSurface: skipped (async render thread owns present) surface=%p",
                              static_cast<void*>(surface));
        return;
    }
    auto ticket = surface ? surface->frameGenerationTicket : FgPresentTicket{};
    if (ticket.serial == 0) ticket = fgCurrentDisplayFrameTicket();
    const auto result = presentSurface(surface, ticket);
    completeDisplayPresentReceipt(ticket, result, 0, MCDR_PRESENT_RETIREMENT_NOT_REQUIRED);
}

uint64_t MCDR_CALL mcdr_dx12GetBackBufferHandle(mcdr_surface* surface) {
    return static_cast<uint64_t>(getBackBufferHandle(surface));
}

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

int32_t MCDR_CALL mcdr_dx12ReadbackTexturePixelsIn(mcdr_object* texture, int32_t fromState,
                                                   int32_t* out) {
    return readbackTexturePixelsCore(texture, out, fromState, "mcdr_dx12ReadbackTexturePixelsIn");
}

int32_t MCDR_CALL mcdr_dx12DlssOutputState(void) { return mcdr_dlss_output_state(); }

void MCDR_CALL mcdr_dx12DumpTextureToFile(mcdr_object* texture, const char* tag) {
    dbgReadbackTexturePixels(texture, (tag && *tag) ? tag : "tex");
}

int32_t MCDR_CALL mcdr_dx12IsSurfaceSuboptimal(mcdr_surface* surface) {
    return (surface && surface->suboptimal) ? 1 : 0;
}

void MCDR_CALL mcdr_dx12SetHdrMode(int32_t mode) { setHdrMode(mode); }

void MCDR_CALL mcdr_dx12SetRaytracingMode(int32_t mode) {
    mcdr_rt_state_set_mode(mode);
    dbgLog("raytracing mode=%d (0=off 1=ao 2=ao_shadows 3=full)", static_cast<int>(mode));
}

void MCDR_CALL mcdr_dx12SetSwapchainOptions(int32_t allowTearing, int32_t forcePresentChild) {
    mcdr_surface_opts_set_allow_tearing(allowTearing);
    mcdr_surface_opts_set_force_present_child(forcePresentChild);
    dbgLogInfo("swapchain options: allowTearing=%d presentChild=%d (applies on next swapchain creation)",
               static_cast<int>(allowTearing), static_cast<int>(forcePresentChild));
}

int32_t MCDR_CALL mcdr_dx12GetHdrAppliedMode(void) { return mcdr_hdr_state_applied_mode(); }

int32_t MCDR_CALL mcdr_dx12DestroySurfaceChecked(mcdr_surface* surface) {
    auto result = destroySurface(surface);
    if (!result) return mcdr::abi::failWith("mcdr_dx12DestroySurfaceChecked", result.error().message);
    return 1;
}

void MCDR_CALL mcdr_dx12DestroySurface(mcdr_surface* surface) {
    (void)mcdr_dx12DestroySurfaceChecked(surface);
}

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
    if (!readbackSamples36(bb, w, h, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PRESENT,
                           D3D12_RESOURCE_STATE_PRESENT, out, err)) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackSurfacePixels", err);
    }
    dbgLogInfo("readback %ux%u center=(%d,%d)=(%d,%d,%d,%d) corners=%d black=%d", w,
               h, static_cast<int>(w) / 2, static_cast<int>(h) / 2, out[(4 * 3 + 4) + 0],
               out[(4 * 3 + 4) + 1], out[(4 * 3 + 4) + 2], out[(4 * 3 + 4) + 3],
               (out[0] != 0 || out[1] != 0 || out[2] != 0) ? 1 : 0,
               (out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0) ? 1 : 0);
    return 1;
}

}
