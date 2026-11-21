module;
#include "_d3d12_module_prelude.h"

#include "ffx_api.h"
#include "ffx_api_types.h"
#include "ffx_upscale.h"
#include "ffx_api_dx12.h"
#include "mcdr_abi.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

module mcdr.d3d12.device;

namespace d3d12_mc {
namespace {

constexpr const char* kFfxUpscalerDll = "amd_fidelityfx_upscaler_dx12.dll";
constexpr int kMaxSide = 8192;

void* ffxAllocCb(void*, uint64_t size) { return std::malloc(static_cast<size_t>(size)); }
void ffxDeallocCb(void*, void* mem) { std::free(mem); }

struct FsrHeapBroker {
    std::mutex lock;
    ID3D12Device* device = nullptr;
    ComPtr<ID3D12Heap> shared;
    uint64_t cursor = 0;
    uint64_t capacity = 0;
    uint64_t sharedAllocations = 0;
    std::vector<ID3D12Heap*> loose;
    uint32_t sharedHits = 0;
    uint32_t fallbacks = 0;
    bool logged = false;
};

FsrHeapBroker& broker() {
    static FsrHeapBroker instance;
    return instance;
}

ffxReturnCode_t createLooseHeap(const D3D12_HEAP_DESC* desc, ID3D12Heap** out, uint64_t* offset) {
    auto& b = broker();
    if (b.device == nullptr) { return FFX_API_RETURN_ERROR; }
    ComPtr<ID3D12Heap> heap;
    if (FAILED(b.device->CreateHeap(desc, IID_PPV_ARGS(&heap)))) { return FFX_API_RETURN_ERROR; }
    b.loose.push_back(heap.Get());
    heap->AddRef();
    ++b.fallbacks;
    *out = heap.Get();
    *offset = 0;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t fsrHeapAlloc(uint32_t, const D3D12_HEAP_DESC* desc, bool aliasable,
                             ID3D12Heap** out, uint64_t* offset) {
    if (desc == nullptr || out == nullptr || offset == nullptr) { return FFX_API_RETURN_ERROR; }
    auto& b = broker();
    std::lock_guard<std::mutex> guard(b.lock);
    const bool shareable = !aliasable && desc->Properties.Type == D3D12_HEAP_TYPE_DEFAULT &&
                           b.device != nullptr;
    if (!shareable) { return createLooseHeap(desc, out, offset); }

    const uint64_t kAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    auto alignUp = [kAlign](uint64_t v) { return (v + kAlign - 1) & ~(kAlign - 1); };
    const uint64_t need = alignUp(desc->SizeInBytes);
    if (b.shared == nullptr) {
        const uint64_t cap = alignUp(std::max<uint64_t>(need * 4, 96ull << 20));
        D3D12_HEAP_DESC heapDesc = *desc;
        heapDesc.SizeInBytes = cap;
        heapDesc.Alignment = kAlign;
        heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
        ComPtr<ID3D12Heap> heap;
        if (FAILED(b.device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)))) {
            return createLooseHeap(desc, out, offset);
        }
        b.shared = heap;
        b.capacity = cap;
        b.cursor = 0;
    }
    if (b.cursor + need > b.capacity) { return createLooseHeap(desc, out, offset); }
    *out = b.shared.Get();
    *offset = b.cursor;
    b.cursor += need;
    ++b.sharedAllocations;
    ++b.sharedHits;
    if (!b.logged) {
        b.logged = true;
        dbgLogInfo("FSR external heap: %.1f MB reserved (first request %.1f MB, offset %llu)",
                   static_cast<double>(b.capacity) / (1024.0 * 1024.0),
                   static_cast<double>(need) / (1024.0 * 1024.0),
                   static_cast<unsigned long long>(*offset));
    }
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t fsrHeapFree(uint32_t, ID3D12Heap* heap, uint64_t,
                            uint64_t) {
    if (heap == nullptr) { return FFX_API_RETURN_OK; }
    auto& b = broker();
    std::lock_guard<std::mutex> guard(b.lock);
    if (heap == b.shared.Get()) {
        if (b.sharedAllocations == 0) { return FFX_API_RETURN_ERROR; }
        --b.sharedAllocations;
        if (b.sharedAllocations == 0) { b.cursor = 0; }
        return FFX_API_RETURN_OK;
    }
    for (auto it = b.loose.begin(); it != b.loose.end(); ++it) {
        if (*it == heap) {
            (*it)->Release();
            b.loose.erase(it);
            return FFX_API_RETURN_OK;
        }
    }
    heap->Release();
    return FFX_API_RETURN_OK;
}

void fsrMessage(uint32_t type, const wchar_t* message) {
    if (message == nullptr) { return; }
    char narrow[512] = {};
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, message, -1, narrow,
                                              static_cast<int>(sizeof(narrow)) - 1, nullptr, nullptr);
    if (written <= 0) { return; }
    dbgLogWarn("ffx message type=0x%08x: %s", type, narrow);
}

std::wstring selfDirectory() {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&selfDirectory), &self) == FALSE ||
        self == nullptr || ::GetModuleFileNameW(self, path, MAX_PATH) == 0) {
        return {};
    }
    std::wstring dir(path);
    const std::wstring::size_type slash = dir.find_last_of(L"\\/");
    dir.resize(slash == std::wstring::npos ? 0 : slash);
    return dir;
}

struct FfxApi {
    HMODULE module = nullptr;
    PfnFfxCreateContext createContext = nullptr;
    PfnFfxDestroyContext destroyContext = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;

    bool ready() const {
        return createContext != nullptr && destroyContext != nullptr && query != nullptr &&
               dispatch != nullptr;
    }
};

struct FfxRuntime {
    std::mutex lock;
    bool attempted = false;
    FfxApi api{};
    std::string problem;
    std::wstring loadedFrom;
};

FfxRuntime& runtime() {
    static FfxRuntime instance;
    return instance;
}

bool loadLocked(FfxRuntime& rt) {
    if (rt.attempted) { return rt.api.ready(); }
    rt.attempted = true;

    std::wstring wideName;
    wideName.assign(kFfxUpscalerDll, kFfxUpscalerDll + std::strlen(kFfxUpscalerDll));

    const std::wstring dir = selfDirectory();
    std::wstring candidates[2];
    int count = 0;
    if (!dir.empty()) { candidates[count++] = dir + L"\\" + wideName; }
    candidates[count++] = wideName;

    DWORD lastError = 0;
    for (int i = 0; i < count; ++i) {
        ::SetLastError(ERROR_SUCCESS);
        HMODULE mod = ::LoadLibraryW(candidates[i].c_str());
        if (mod == nullptr) {
            lastError = ::GetLastError();
            continue;
        }
        rt.api.module = mod;
        rt.loadedFrom = candidates[i];
        break;
    }
    if (rt.api.module == nullptr) {
        rt.problem = "cannot load ";
        rt.problem += kFfxUpscalerDll;
        rt.problem += " (win32 error ";
        rt.problem += std::to_string(lastError);
        rt.problem += ")";
        return false;
    }

    rt.api.createContext = reinterpret_cast<PfnFfxCreateContext>(
        ::GetProcAddress(rt.api.module, "ffxCreateContext"));
    rt.api.destroyContext = reinterpret_cast<PfnFfxDestroyContext>(
        ::GetProcAddress(rt.api.module, "ffxDestroyContext"));
    rt.api.query = reinterpret_cast<PfnFfxQuery>(::GetProcAddress(rt.api.module, "ffxQuery"));
    rt.api.dispatch = reinterpret_cast<PfnFfxDispatch>(::GetProcAddress(rt.api.module, "ffxDispatch"));
    if (!rt.api.ready()) {
        rt.problem = "ffx entry points missing in ";
        rt.problem += kFfxUpscalerDll;
        return false;
    }
    rt.problem.clear();
    return true;
}

bool isAvailable(std::string& why) {
    FfxRuntime& rt = runtime();
    std::lock_guard<std::mutex> guard(rt.lock);
    const bool ok = loadLocked(rt);
    why = rt.problem;
    return ok;
}

Dx12Object* asObject(void* handle) {
    return handle == nullptr ? nullptr : static_cast<Dx12Object*>(handle);
}

uint32_t contextFlagsFor(const mcdr_sr_dispatch& seed) {
    uint32_t flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (seed.hdrInput != 0) { flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE; }
    return flags;
}

void writeNote(char* dst, size_t cap, const std::string& text) {
    if (dst == nullptr || cap == 0) { return; }
    const size_t keep = text.size() < (cap - 1) ? text.size() : (cap - 1);
    std::memcpy(dst, text.data(), keep);
    dst[keep] = '\0';
}

void fillCapsFields(mcdr_sr_caps& out, bool available, const std::string& note) {
    out.backend = static_cast<int32_t>(SrBackendId::Fsr);
    out.available = available ? 1 : 0;
    out.coupledDenoise = 0;
    out.needsExposure = 0;
    out.takesMasks = MCDR_SR_MASK_REACTIVE | MCDR_SR_MASK_TRANSPARENCY_AND_COMPOSITION;
    out.externalHeap = 0;
    out.jitterPhaseCount = 8;
    out.minRenderWidth = 320;
    out.minRenderHeight = 180;
    out.maxRenderWidth = 7680;
    out.maxRenderHeight = 4320;
    out.nativeAA = 1;
    out.reserved = 0;
    writeNote(out.note, sizeof(out.note), note);
}

}

class FsrSrProvider final : public IUpscaleProvider {
public:
    SrBackendId backend() const override { return SrBackendId::Fsr; }

    std::string failureText() const override { return problem_; }

    void fillCaps(mcdr_sr_caps& out) const override {
        std::string why;
        const bool ok = isAvailable(why);
        fillCapsFields(out, ok, ok ? std::string("fidelityfx runtime ok") : why);
    }

    bool initialize(CommandContext* ctx, const mcdr_sr_dispatch& seed) override {
        if (ctx == nullptr) {
            problem_ = "initialize without command context";
            return false;
        }
        device_ = deviceContext().device.Get();
        {
            auto& b = broker();
            std::lock_guard<std::mutex> guard(b.lock);
            b.device = device_;
        }
        if (device_ == nullptr) {
            problem_ = "no d3d12 device";
            return false;
        }
        adoptSizes(seed.renderWidth, seed.renderHeight, seed.displayWidth, seed.displayHeight);
        flags_ = contextFlagsFor(seed);
        return ensureContext(ctx);
    }

    void onResize(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                  int32_t displayHeight) override {
        adoptSizes(renderWidth, renderHeight, displayWidth, displayHeight);
    }

    bool evaluate(CommandContext* ctx, const mcdr_sr_dispatch& dispatch) override {
        if (ctx == nullptr || ctx->commandList == nullptr) {
            problem_ = "no command list";
            return false;
        }
        Dx12Object* color = asObject(dispatch.pColorInput);
        Dx12Object* depth = asObject(dispatch.pDepth);
        Dx12Object* motion = asObject(dispatch.pMotionVectors);
        Dx12Object* output = asObject(dispatch.pColorOutput);
        Dx12Object* exposure = asObject(dispatch.pExposure);
        if (color == nullptr) { problem_ = "missing color input"; return false; }
        if (depth == nullptr) { problem_ = "missing depth"; return false; }
        if (motion == nullptr) { problem_ = "missing motion vectors"; return false; }
        if (output == nullptr) { problem_ = "missing color output"; return false; }
        if (exposure != nullptr && (!exposure->resource || exposure->dxgiFormat != DXGI_FORMAT_R32_FLOAT ||
            exposure->resource->GetDesc().Width != 1 || exposure->resource->GetDesc().Height != 1)) {
            problem_ = "incompatible exposure texture";
            return false;
        }
        if (color->resource == nullptr || depth->resource == nullptr ||
            motion->resource == nullptr || output->resource == nullptr) {
            problem_ = "resource handle without d3d12 resource";
            return false;
        }
        if (dispatch.renderWidth < 320 || dispatch.renderHeight < 180) {
            if (!tooSmallLogged_) {
                tooSmallLogged_ = true;
                dbgLogError("render %dx%d is below the %s minimum of 320x180; skipping "
                         "the evaluate (raise the render scale)",
                         dispatch.renderWidth, dispatch.renderHeight, "AMD FidelityFX");
            }
            problem_ = "render size below the 320x180 minimum";
            return false;
        }
        const uint32_t requestedFlags = contextFlagsFor(dispatch);
        if (flags_ != requestedFlags) {
            flags_ = requestedFlags;
            needsRecreate_ = true;
        }
        adoptSizes(dispatch.renderWidth, dispatch.renderHeight, dispatch.displayWidth,
                   dispatch.displayHeight);
        if (!ensureContext(ctx)) { return false; }

        transitionTextureTo(ctx, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transitionTextureTo(ctx, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transitionTextureTo(ctx, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (exposure) transitionTextureTo(ctx, exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Dx12Object* reactive = asObject(dispatch.pReactiveMask);
        if (reactive != nullptr && reactive->resource != nullptr) {
            transitionTextureTo(ctx, reactive, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            reactive = nullptr;
        }
        Dx12Object* transparencyAndComposition = asObject(dispatch.pTransparencyAndComposition);
        if (transparencyAndComposition != nullptr && transparencyAndComposition->resource != nullptr) {
            transitionTextureTo(ctx, transparencyAndComposition, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            transparencyAndComposition = nullptr;
        }
        transitionTextureTo(ctx, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ffxDispatchDescUpscale desc{};
        desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        desc.header.pNext = nullptr;
        desc.commandList = static_cast<void*>(ctx->commandList);
        desc.color = ffxApiGetResourceDX12(color->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.depth = ffxApiGetResourceDX12(depth->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.motionVectors = ffxApiGetResourceDX12(motion->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.exposure = exposure
            ? ffxApiGetResourceDX12(exposure->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ)
            : ffxApiGetResourceDX12(nullptr);
        desc.reactive = reactive != nullptr
            ? ffxApiGetResourceDX12(reactive->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ)
            : ffxApiGetResourceDX12(nullptr);
        desc.transparencyAndComposition = transparencyAndComposition != nullptr
            ? ffxApiGetResourceDX12(transparencyAndComposition->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ)
            : ffxApiGetResourceDX12(nullptr);
        desc.output = ffxApiGetResourceDX12(output->resource.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        desc.jitterOffset.x = dispatch.jitterOffsetX;
        desc.jitterOffset.y = dispatch.jitterOffsetY;
        desc.motionVectorScale.x = dispatch.motionVectorScaleX != 0.0f ? dispatch.motionVectorScaleX : 1.0f;
        desc.motionVectorScale.y = dispatch.motionVectorScaleY != 0.0f ? dispatch.motionVectorScaleY : 1.0f;
        desc.renderSize = { static_cast<uint32_t>(renderW_), static_cast<uint32_t>(renderH_) };
        desc.upscaleSize = { static_cast<uint32_t>(displayW_), static_cast<uint32_t>(displayH_) };
        desc.enableSharpening = dispatch.sharpness > 0.0f;
        desc.sharpness = std::clamp(dispatch.sharpness, 0.0f, 1.0f);
        desc.frameTimeDelta = dispatch.frameTimeDeltaMs > 0.0f ? dispatch.frameTimeDeltaMs : 16.7f;
        desc.preExposure = dispatch.preExposure > 0.0f ? dispatch.preExposure : 1.0f;
        desc.reset = dispatch.reset != 0;
        desc.cameraNear = dispatch.cameraNear;
        desc.cameraFar = dispatch.cameraFar;
        desc.cameraFovAngleVertical =
            dispatch.cameraVerticalFov > 0.0f ? dispatch.cameraVerticalFov : 1.2217f;
        desc.viewSpaceToMetersFactor =
            dispatch.viewSpaceToMetersFactor > 0.0f ? dispatch.viewSpaceToMetersFactor : 1.0f;
        desc.flags = 0;

        const ffxReturnCode_t rc = runtime().api.dispatch(&context_, &desc.header);
        transitionTextureTo(ctx, output, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (rc != FFX_API_RETURN_OK) {
            problem_ = "ffxDispatch failed rc=";
            problem_ += std::to_string(static_cast<int>(rc));
            if (!logged_) {
                dbgLogWarn("FSR dispatch failed: %s", problem_.c_str());
                logged_ = true;
            }
            return false;
        }
        problem_.clear();
        return true;
    }

    void shutdown() override {
        destroyContext();
        flags_ = 0;
        problem_.clear();
        logged_ = false;
    }

private:
    void adoptSizes(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                    int32_t displayHeight) {
        if (renderWidth > 0) { renderW_ = renderWidth; }
        if (renderHeight > 0) { renderH_ = renderHeight; }
        if (displayWidth > 0) { displayW_ = displayWidth; }
        if (displayHeight > 0) { displayH_ = displayHeight; }
    }

    bool ensureContext(CommandContext* ctx) {
        std::string why;
        if (!isAvailable(why)) {
            if (!logged_) {
                dbgLogWarn("FSR unavailable: %s", why.c_str());
                logged_ = true;
            }
            problem_ = why.empty() ? std::string("fidelityfx runtime unavailable") : why;
            return false;
        }
        if (renderW_ <= 0 || renderH_ <= 0 || displayW_ <= 0 || displayH_ <= 0) {
            problem_ = "sizes not set";
            return false;
        }
        if (renderW_ > kMaxSide || renderH_ > kMaxSide || displayW_ > kMaxSide || displayH_ > kMaxSide) {
            problem_ = "size beyond 8k";
            return false;
        }
        if (context_ != nullptr && !needsRecreate_ && renderW_ <= maxRenderW_ && renderH_ <= maxRenderH_ &&
            displayW_ <= maxDisplayW_ && displayH_ <= maxDisplayH_) {
            return true;
        }
        if (context_ != nullptr) {
            if (ctx != nullptr && ctx->fenceValue > 0) {
                if (!waitForFenceValue(ctx, ctx->fenceValue, 2000000000ull)) {
                    problem_ = "context resize fence timed out";
                    return false;
                }
            }
            destroyContext();
        }

        maxRenderW_ = renderW_;
        maxRenderH_ = renderH_;
        maxDisplayW_ = displayW_;
        maxDisplayH_ = displayH_;

        ffxCreateBackendDX12Desc backendDesc{};
        backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backendDesc.header.pNext = nullptr;
        backendDesc.device = device_;

        ffxCreateBackendDX12AllocationCallbacksDesc allocDesc{};
        allocDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12_ALLOCATION_CALLBACKS;
        allocDesc.header.pNext = &backendDesc.header;
        allocDesc.pfnFfxResourceAllocator = nullptr;
        allocDesc.pfnFfxResourceDeallocator = nullptr;
        allocDesc.pfnFfxHeapAllocator = &fsrHeapAlloc;
        allocDesc.pfnFfxHeapDeallocator = &fsrHeapFree;
        allocDesc.pfnFfxConstantBufferAllocator = nullptr;

        ffxCreateContextDescUpscale upscaleDesc{};
        upscaleDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        upscaleDesc.header.pNext = &allocDesc.header;
        upscaleDesc.flags = flags_ != 0 ? flags_ : FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        upscaleDesc.maxRenderSize = { static_cast<uint32_t>(maxRenderW_),
                                     static_cast<uint32_t>(maxRenderH_) };
        upscaleDesc.maxUpscaleSize = { static_cast<uint32_t>(maxDisplayW_),
                                      static_cast<uint32_t>(maxDisplayH_) };
        upscaleDesc.fpMessage = &fsrMessage;

        ffxAllocationCallbacks callbacks{};
        callbacks.pUserData = nullptr;
        callbacks.alloc = &ffxAllocCb;
        callbacks.dealloc = &ffxDeallocCb;

        context_ = nullptr;
        const ffxReturnCode_t rc =
            runtime().api.createContext(&context_, &upscaleDesc.header, &callbacks);
        if (rc != FFX_API_RETURN_OK || context_ == nullptr) {
            destroyContext();
            problem_ = "ffxCreateContext failed rc=";
            problem_ += std::to_string(static_cast<int>(rc));
            if (!logged_) {
                dbgLogWarn("FSR context creation failed: %s (render=%dx%d display=%dx%d)",
                           problem_.c_str(), maxRenderW_, maxRenderH_, maxDisplayW_, maxDisplayH_);
                logged_ = true;
            }
            return false;
        }
        needsRecreate_ = false;
        problem_.clear();
        return true;
    }

    void destroyContext() {
        if (context_ == nullptr) { return; }
        ffxAllocationCallbacks callbacks{};
        callbacks.pUserData = nullptr;
        callbacks.alloc = &ffxAllocCb;
        callbacks.dealloc = &ffxDeallocCb;
        (void)runtime().api.destroyContext(&context_, &callbacks);
        context_ = nullptr;
        maxRenderW_ = 0;
        maxRenderH_ = 0;
        maxDisplayW_ = 0;
        maxDisplayH_ = 0;
    }

    ID3D12Device* device_ = nullptr;
    ffxContext context_ = nullptr;
    uint32_t flags_ = 0;
    bool needsRecreate_ = false;
    int32_t renderW_ = 0;
    int32_t renderH_ = 0;
    int32_t displayW_ = 0;
    int32_t displayH_ = 0;
    int32_t maxRenderW_ = 0;
    int32_t maxRenderH_ = 0;
    int32_t maxDisplayW_ = 0;
    int32_t maxDisplayH_ = 0;
    std::string problem_;
    bool logged_ = false;
    /** P291i：尺寸过小只记一次日志，避免每帧刷屏。 */
    bool tooSmallLogged_ = false;
};

void fillFsrCaps(mcdr_sr_caps& out) {
    std::string why;
    const bool ok = isAvailable(why);
    fillCapsFields(out, ok, ok ? std::string("fidelityfx runtime ok") : why);
}

std::unique_ptr<IUpscaleProvider> makeFsrProvider() {
    std::string why;
    if (!isAvailable(why)) {
        dbgLogInfo("FSR provider not created: %s", why.c_str());
        return nullptr;
    }
    return std::make_unique<FsrSrProvider>();
}

bool fsrRuntimeAvailable() {
    std::string why;
    return isAvailable(why);
}

}
