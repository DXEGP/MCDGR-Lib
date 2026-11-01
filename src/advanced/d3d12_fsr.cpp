// d3d12_fsr.cpp — FSR 超分后端（FidelityFX 2.3 ffx_api，amd_fidelityfx_upscaler_dx12.dll）。
// **FFX 不替我们转屏障**：输入自己 transition 到 NON_PIXEL_SHADER_RESOURCE、输出到 UNORDERED_ACCESS，
// 返回前必须把输出切回 RENDER_TARGET（接口契约，GUI 还要当渲染目标用）。
// 上下文按最大尺寸建、按实际尺寸 dispatch，越界要等 GPU 追平后重建（见 ensureContext）。
// 深度 reverse-Z → DEPTH_INVERTED（不置 DEPTH_INFINITE，26.3 远平面有限）；MV 是渲染分辨率像素位移
// → motionVectorScale=1 并置 MOTION_VECTORS_JITTER_CANCELLATION，否则 FSR 会把 jitter 当物体运动。
// 失败策略：每条失败路径最多一行日志并钉住状态，不每帧重试；失败不让帧失败（调用方退回无超分）。
module;
#include "_d3d12_module_prelude.h"

// FFX 的 C API（external\fidelityfx\include，vcxproj 已登记）。放全局模块片段，理由同下。
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
// 上限取 8K：再大就不是窗口而是拼接墙了，超过直接拒绝并报原因，比悄悄建错尺寸的上下文好。
constexpr int kMaxSide = 8192;

// FFX 的分配回调（官方只要求 malloc 语义）。
void* ffxAllocCb(void* /*userData*/, uint64_t size) { return std::malloc(static_cast<size_t>(size)); }
void ffxDeallocCb(void* /*userData*/, void* mem) { std::free(mem); }

//
// FFX 的堆分配回调让我们替它建堆：给了 PfnFfxHeapAllocatorFunc 后 CreateHeapDX12 完全让位（返回什么用什么），
// 销毁走 PfnFfxHeapDeallocatorFunc，堆所有权全在我们手里，重建上下文时同一块 DEFAULT 堆可复用。只接管 DEFAULT
// 且非 aliasing：UPLOAD/READBACK 必须 CPU 可见，aliasable 的资源可叠在同一段内存，交给 bump 分配器只会白占内存。
//
struct FsrHeapBroker {
    std::mutex lock;
    ID3D12Device* device = nullptr;
    ComPtr<ID3D12Heap> shared;      // 复用的 DEFAULT 堆（一次给足，不再增长）
    uint64_t cursor = 0;
    uint64_t capacity = 0;
    std::vector<ID3D12Heap*> loose; // 放不进 shared 的临时堆，由 deallocator 回收
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
    heap->AddRef();   // 裸指针进 loose，等 deallocator 负责 Release
    ++b.fallbacks;
    *out = heap.Get();
    *offset = 0;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t fsrHeapAlloc(uint32_t /*effectId*/, const D3D12_HEAP_DESC* desc, bool aliasable,
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
        // 一次给足：按需求的 4 倍且不少于 96 MB 建，之后同一后端反复重建上下文都不再建堆。
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
    *out = b.shared.Get();   // FFX 只借用，不加引用；销毁时走 deallocator，我们认得这个指针
    *offset = b.cursor;
    b.cursor += need;
    ++b.sharedHits;
    if (!b.logged) {
        b.logged = true;
        dbgLogInfo("FSR heap: %.1f MB reserved (first %.1f MB, offset %llu)",
                   static_cast<double>(b.capacity) / (1024.0 * 1024.0),
                   static_cast<double>(need) / (1024.0 * 1024.0),
                   static_cast<unsigned long long>(*offset));
    }
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t fsrHeapFree(uint32_t /*effectId*/, ID3D12Heap* heap, uint64_t /*offset*/,
                            uint64_t /*size*/) {
    if (heap == nullptr) { return FFX_API_RETURN_OK; }
    auto& b = broker();
    std::lock_guard<std::mutex> guard(b.lock);
    if (heap == b.shared.Get()) { return FFX_API_RETURN_OK; }   // 共享堆由 broker 持有，不在这里放
    for (auto it = b.loose.begin(); it != b.loose.end(); ++it) {
        if (*it == heap) {
            (*it)->Release();
            b.loose.erase(it);
            return FFX_API_RETURN_OK;
        }
    }
    heap->Release();   // 不是我们发的，也不在 loose 里：按引用计数还回去
    return FFX_API_RETURN_OK;
}

// FFX 的诊断消息是宽字符，转窄字符后交给统一日志（截断到 512 字节够用）。
void fsrMessage(uint32_t type, const wchar_t* message) {
    if (message == nullptr) { return; }
    char narrow[512] = {};
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, message, -1, narrow,
                                              static_cast<int>(sizeof(narrow)) - 1, nullptr, nullptr);
    if (written <= 0) { return; }
    dbgLogWarn("ffx message type=0x%08x: %s", type, narrow);
}

// 本模块所在目录：GetModuleFileNameA(nullptr) 拿到的宿主是 java.exe，必须用 GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 问自己。
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

// 从 DLL 里取到的五个入口。缺任何一个都算不可用（FFX 的 C API 只有这几个是必需的）。
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

// 进程级只尝试加载一次；失败原因留在 problem 里，caps 的 note[] 会把它透给 JVM。
struct FfxRuntime {
    std::mutex lock;
    bool attempted = false;
    FfxApi api{};
    std::string problem;   // 空 = 可用
    std::wstring loadedFrom;
};

FfxRuntime& runtime() {
    static FfxRuntime instance;
    return instance;
}

// 载入运行库：本模块目录 → 裸 DLL 名。模块目录即 <user.dir>\dxrmc\，不需要额外的 JVM 开关。
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

// 运行库是否可用（进程内只尝试加载一次），why 带回失败原因。
bool isAvailable(std::string& why) {
    FfxRuntime& rt = runtime();
    std::lock_guard<std::mutex> guard(rt.lock);
    const bool ok = loadLocked(rt);
    why = rt.problem;
    return ok;
}

// 把 ABI 的 void* 句柄解成模块自己的对象。**只在模块内做**：别的 TU 去 reinterpret 会得到空 vtable。
Dx12Object* asObject(void* handle) {
    return handle == nullptr ? nullptr : static_cast<Dx12Object*>(handle);
}

// 上下文创建标志。**每帧不变**，所以只在建/重建上下文时算一次。
uint32_t contextFlagsFor(const mcdr_sr_dispatch& seed) {
    uint32_t flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (seed.hdrInput != 0) { flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE; }
    // 不置 DISPLAY_RESOLUTION_MOTION_VECTORS（MV 是渲染分辨率的像素位移）、DEPTH_INFINITE（26.3 远平面有限）、
    // AUTO_EXPOSURE（曝光由 JVM 给 preExposure）。
    return flags;
}

// 把一句话写进固定长度的 note（C 侧是 char[28]，必须留结尾 0）。
void writeNote(char* dst, size_t cap, const std::string& text) {
    if (dst == nullptr || cap == 0) { return; }
    const size_t keep = text.size() < (cap - 1) ? text.size() : (cap - 1);
    std::memcpy(dst, text.data(), keep);
    dst[keep] = '\0';
}

void fillCapsFields(mcdr_sr_caps& out, bool available, const std::string& note) {
    out.backend = static_cast<int32_t>(SrBackendId::Fsr);
    out.available = available ? 1 : 0;
    out.coupledDenoise = 0;  // FSR 不带降噪器（SHARC/NRD 由我们自己的分支处理）
    out.needsExposure = 0;   // preExposure 标量就够
    // 收 reactive mask（有人给就用）；responsive mask 是 XeSS 的概念，这里不声明。
    out.takesMasks = MCDR_SR_MASK_REACTIVE;
    out.externalHeap = 0;    // 堆由我们的 broker 建（见 fsrHeapAlloc），这里不声明外部堆
    out.jitterPhaseCount = 8;
    out.minRenderWidth = 320;
    out.minRenderHeight = 180;
    out.maxRenderWidth = 7680;
    out.maxRenderHeight = 4320;
    out.nativeAA = 1;        // FFX_UPSCALE_QUALITY_MODE_NATIVEAA = 1.0x
    out.reserved = 0;
    writeNote(out.note, sizeof(out.note), note);
}

}  // namespace

class FsrSrProvider final : public IUpscaleProvider {
public:
    SrBackendId backend() const override { return SrBackendId::Fsr; }

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
            // broker 只有一个实例（FFX 的回调是进程级静态函数指针），把设备交给它。
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
        // 只记账：真正重建留到下一帧 evaluate（那里有命令上下文，可以先等 GPU 追平）。
        adoptSizes(renderWidth, renderHeight, displayWidth, displayHeight);
    }

    bool evaluate(CommandContext* ctx, const mcdr_sr_dispatch& dispatch) override {
        if (ctx == nullptr || ctx->commandList == nullptr) {
            problem_ = "no command list";
            return false;
        }
        // 输入输出。缺任何一个都不猜：让调用方退回没有超分的老路径。
        Dx12Object* color = asObject(dispatch.pColorInput);
        Dx12Object* depth = asObject(dispatch.pDepth);
        Dx12Object* motion = asObject(dispatch.pMotionVectors);
        Dx12Object* output = asObject(dispatch.pColorOutput);
        if (color == nullptr) { problem_ = "missing color input"; return false; }
        if (depth == nullptr) { problem_ = "missing depth"; return false; }
        if (motion == nullptr) { problem_ = "missing motion vectors"; return false; }
        if (output == nullptr) { problem_ = "missing color output"; return false; }
        if (color->resource == nullptr || depth->resource == nullptr ||
            motion->resource == nullptr || output->resource == nullptr) {
            problem_ = "resource handle without d3d12 resource";
            return false;
        }
        // 尺寸低于下限时 FFX 只给出无效结果（世界全黑、GUI 正常）：明确拒绝并只记一次日志，让调用方抬渲染比例。
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
        if (flags_ == 0) { flags_ = contextFlagsFor(dispatch); }
        adoptSizes(dispatch.renderWidth, dispatch.renderHeight, dispatch.displayWidth,
                   dispatch.displayHeight);
        if (!ensureContext(ctx)) { return false; }

        // FFX 不替我们转屏障（见文件头第 1 条）：走模块的状态跟踪过渡，不猜 Before 状态。
        transitionTextureTo(ctx, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transitionTextureTo(ctx, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transitionTextureTo(ctx, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // reactive mask 是可选的第五个输入：句柄在但资源为空时当作没有 —— 把半个对象交给 FFX 会发屏障到空指针上。
        Dx12Object* reactive = asObject(dispatch.pReactiveMask);
        if (reactive != nullptr && reactive->resource != nullptr) {
            transitionTextureTo(ctx, reactive, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            reactive = nullptr;
        }
        transitionTextureTo(ctx, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ffxDispatchDescUpscale desc{};
        desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        desc.header.pNext = nullptr;
        // 后端 DLL 的 FfxCommandList 就是 ID3D12GraphicsCommandList*，直接传，不依赖额外导出符号。
        desc.commandList = static_cast<void*>(ctx->commandList);
        desc.color = ffxApiGetResourceDX12(color->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.depth = ffxApiGetResourceDX12(depth->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.motionVectors = ffxApiGetResourceDX12(motion->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.exposure = ffxApiGetResourceDX12(nullptr);                  // v1 不接曝光纹理
        // FFX 把 state 当作进入 dispatch 时该资源所处的状态（它那道 barrier 的 Before），必须与 transitionTextureTo
        // 的结果一致：NON_PIXEL_SHADER_RESOURCE => COMPUTE_READ。
        desc.reactive = reactive != nullptr
            ? ffxApiGetResourceDX12(reactive->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ)
            : ffxApiGetResourceDX12(nullptr);
        desc.transparencyAndComposition = ffxApiGetResourceDX12(nullptr); // 同上
        desc.output = ffxApiGetResourceDX12(output->resource.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        desc.jitterOffset.x = dispatch.jitterOffsetX;
        desc.jitterOffset.y = dispatch.jitterOffsetY;
        // MV 已是像素位移 → 倍率 1（0 = 未填按 1 处理，与载荷契约一致）。
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
        // MC 的世界单位就是米，所以换算是 1:1（0 = 未填时也按 1）。
        desc.viewSpaceToMetersFactor =
            dispatch.viewSpaceToMetersFactor > 0.0f ? dispatch.viewSpaceToMetersFactor : 1.0f;
        desc.flags = 0;

        const ffxReturnCode_t rc = runtime().api.dispatch(&context_, &desc.header);
        // 无论成功与否都得把输出交回 RENDER_TARGET（接口契约，GUI 还要当渲染目标用）。
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
        if (context_ != nullptr && renderW_ <= maxRenderW_ && renderH_ <= maxRenderH_ &&
            displayW_ <= maxDisplayW_ && displayH_ <= maxDisplayH_) {
            return true;
        }
        if (context_ != nullptr) {
            // 旧上下文的内部资源可能还在飞：先等 GPU 追平最后一次提交再销毁。只在首次建 / 尺寸变大时发生。
            if (ctx != nullptr && ctx->fenceValue > 0) {
                (void)waitForFenceValue(ctx, ctx->fenceValue, 2000000000ull);
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

        // 把"建堆"接过来：只给 heap 那一对回调，资源仍由 FFX 自己建。
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
            context_ = nullptr;
            problem_ = "ffxCreateContext failed rc=";
            problem_ += std::to_string(static_cast<int>(rc));
            if (!logged_) {
                dbgLogWarn("FSR ctx failed: %s render=%dx%d display=%dx%d",
                           problem_.c_str(), maxRenderW_, maxRenderH_, maxDisplayW_, maxDisplayH_);
                logged_ = true;
            }
            return false;
        }
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
        // 上下文没了，FFX 已把借出去的堆段全部还回来（走 fsrHeapFree），游标归零，但 shared 堆本身留着复用。
        {
            auto& b = broker();
            std::lock_guard<std::mutex> guard(b.lock);
            b.cursor = 0;
        }
        context_ = nullptr;
        maxRenderW_ = 0;
        maxRenderH_ = 0;
        maxDisplayW_ = 0;
        maxDisplayH_ = 0;
    }

    ID3D12Device* device_ = nullptr;
    ffxContext context_ = nullptr;
    uint32_t flags_ = 0;
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
    bool tooSmallLogged_ = false;
};

// caps 的 FSR 分支（srFillCaps 调它）。**不建上下文**：建上下文要设备、要尺寸，caps 只回答"能不能用"。
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

// 运行库是否载入成功（给 srJitterPhaseCount 之类"没有上下文也要回答"的地方用）。
bool fsrRuntimeAvailable() {
    std::string why;
    return isAvailable(why);
}

}  // namespace d3d12_mc
