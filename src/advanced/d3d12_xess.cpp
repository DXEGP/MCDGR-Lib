// d3d12_xess.cpp — XeSS 超分后端（Intel XeSS SDK 3.0，libxess.dll）。
// **XeSS 自己转自己的屏障，但不替我们转输入输出**：颜色 / 速度 / 深度 / 掩码要求
// NON_PIXEL_SHADER_RESOURCE、输出要求 UNORDERED_ACCESS，必须由我们发（transitionTextureTo 状态跟踪），返回前交回 RENDER_TARGET。
// 输入分辨率每帧告诉它（Dynamic Input Resolution），init 只定 [min, max]；qualitySetting 按 doc 表格（1.3 起默认映射）
// 从 display/render 比值取最近一档，对不上不报错。
// 深度 reverse-Z → XESS_INIT_FLAG_INVERTED_DEPTH；MV 是渲染分辨率像素位移 → 不置 USE_NDC_VELOCITY / HIGH_RES_MV，
// 只有 MV 含 jitter 时才置 XESS_INIT_FLAG_JITTERED_MV；曝光倍率直传 preExposure。
// 失败策略：每条失败路径最多一行日志并钉住状态，失败不让帧失败（调用方退回无超分）。
module;
#include "_d3d12_module_prelude.h"

// XeSS 的 C API（external\xess\inc\xess，vcxproj 已登记）。
#include "xess.h"
#include "xess_d3d12.h"
#include "mcdr_abi.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

module mcdr.d3d12.device;

namespace d3d12_mc {
namespace {

constexpr const char* kXessDll = "libxess.dll";
// 上限取 8K，与 FSR 一致：再大直接拒绝并报原因，比悄悄建错尺寸的上下文好。
constexpr int kMaxSide = 8192;

// 本模块所在目录：GetModuleFileNameA(nullptr) 拿到的是宿主 exe（java.exe），必须用
// GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 问本模块自己。libxess.dll 由 extractVendorResources() 解到 dxrmc\，同目录。
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

// 从 DLL 里取到的入口：前六条必需，后三条有就用（BuildPipelines 只为省 init 停顿，IsOptimalDriver 只进日志）。
struct XessApi {
    HMODULE module = nullptr;
    decltype(&xessD3D12CreateContext) createContext = nullptr;
    decltype(&xessD3D12Init) init = nullptr;
    decltype(&xessD3D12Execute) execute = nullptr;
    decltype(&xessDestroyContext) destroy = nullptr;
    decltype(&xessGetVersion) getVersion = nullptr;
    decltype(&xessGetOptimalInputResolution) getOptimalInputResolution = nullptr;
    decltype(&xessD3D12BuildPipelines) buildPipelines = nullptr;
    decltype(&xessIsOptimalDriver) isOptimalDriver = nullptr;
    decltype(&xessGetProperties) getProperties = nullptr;

    bool ready() const {
        return createContext != nullptr && init != nullptr && execute != nullptr &&
               destroy != nullptr && getVersion != nullptr && getOptimalInputResolution != nullptr;
    }
};

// 进程级只尝试加载一次；失败原因留在 problem 里，caps 的 note[] 会透给 JVM。
struct XessRuntime {
    std::mutex lock;
    bool attempted = false;
    XessApi api{};
    std::string problem;    // 空 = 可用
    std::string version;    // 例如 "3.0.2"
};

XessRuntime& runtime() {
    static XessRuntime instance;
    return instance;
}

bool loadLocked(XessRuntime& rt) {
    if (rt.attempted) { return rt.problem.empty(); }
    rt.attempted = true;

    std::wstring candidate = selfDirectory();
    if (!candidate.empty()) {
        candidate += L"\\";
        candidate += std::wstring(kXessDll, kXessDll + std::strlen(kXessDll));
    }
    HMODULE module = candidate.empty() ? nullptr : ::LoadLibraryW(candidate.c_str());
    if (module == nullptr) {
        module = ::LoadLibraryA(kXessDll);
    }
    if (module == nullptr) {
        rt.problem = "cannot load ";
        rt.problem += kXessDll;
        rt.problem += " (win32 error ";
        rt.problem += std::to_string(static_cast<int>(::GetLastError()));
        rt.problem += ")";
        return false;
    }

    XessApi api{};
    api.module = module;
    api.createContext = reinterpret_cast<decltype(api.createContext)>(
        ::GetProcAddress(module, "xessD3D12CreateContext"));
    api.init = reinterpret_cast<decltype(api.init)>(::GetProcAddress(module, "xessD3D12Init"));
    api.execute = reinterpret_cast<decltype(api.execute)>(
        ::GetProcAddress(module, "xessD3D12Execute"));
    api.destroy = reinterpret_cast<decltype(api.destroy)>(
        ::GetProcAddress(module, "xessDestroyContext"));
    api.getVersion = reinterpret_cast<decltype(api.getVersion)>(
        ::GetProcAddress(module, "xessGetVersion"));
    api.getOptimalInputResolution = reinterpret_cast<decltype(api.getOptimalInputResolution)>(
        ::GetProcAddress(module, "xessGetOptimalInputResolution"));
    api.buildPipelines = reinterpret_cast<decltype(api.buildPipelines)>(
        ::GetProcAddress(module, "xessD3D12BuildPipelines"));
    api.isOptimalDriver = reinterpret_cast<decltype(api.isOptimalDriver)>(
        ::GetProcAddress(module, "xessIsOptimalDriver"));
    api.getProperties = reinterpret_cast<decltype(api.getProperties)>(
        ::GetProcAddress(module, "xessGetProperties"));

    if (!api.ready()) {
        rt.problem = "xess entry points missing in ";
        rt.problem += kXessDll;
        return false;
    }

    xess_version_t version{};
    if (api.getVersion(&version) == XESS_RESULT_SUCCESS) {
        rt.version = std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
                     std::to_string(version.patch);
    }
    rt.api = api;
    rt.problem.clear();
    return true;
}

bool isAvailable(std::string& why) {
    XessRuntime& rt = runtime();
    std::lock_guard<std::mutex> guard(rt.lock);
    const bool ok = loadLocked(rt);
    why = rt.problem;
    return ok;
}

// 质量档：doc 表格里 XeSS 1.3 起的默认（非 legacy）映射；比值 = display / render 取最近一档，对不上不报错。
xess_quality_settings_t qualityForRatio(double ratio) {
    struct Entry {
        double ratio;
        xess_quality_settings_t setting;
    };
    static const Entry kTable[] = {
        {1.00, XESS_QUALITY_SETTING_AA},
        {1.30, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS},
        {1.50, XESS_QUALITY_SETTING_ULTRA_QUALITY},
        {1.70, XESS_QUALITY_SETTING_QUALITY},
        {2.00, XESS_QUALITY_SETTING_BALANCED},
        {2.30, XESS_QUALITY_SETTING_PERFORMANCE},
        {3.00, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE},
    };
    if (!(ratio > 0.0) || !std::isfinite(ratio)) { return XESS_QUALITY_SETTING_QUALITY; }
    double bestDistance = 1.0e9;
    xess_quality_settings_t best = XESS_QUALITY_SETTING_QUALITY;
    for (const Entry& entry : kTable) {
        const double distance = std::fabs(ratio - entry.ratio);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = entry.setting;
        }
    }
    return best;
}

// 把 ABI 的 void* 句柄解成模块自己的对象。**只在模块内做**：别的 TU 去 reinterpret 会得到空 vtable。
Dx12Object* asObject(void* handle) {
    return handle == nullptr ? nullptr : static_cast<Dx12Object*>(handle);
}

// 把 std::string 安全地抄进 caps 的 note[28]（截断 + 保证 0 结尾）。
void writeNote(char* dst, size_t cap, const std::string& text) {
    if (dst == nullptr || cap == 0) { return; }
    const size_t limit = cap - 1;
    const size_t length = text.size() < limit ? text.size() : limit;
    std::memcpy(dst, text.data(), length);
    dst[length] = '\0';
}

void fillCapsFields(mcdr_sr_caps& out, bool available, const std::string& note) {
    out.backend = MCDR_SR_BACKEND_XESS;
    out.available = available ? 1 : 0;
    out.coupledDenoise = 0;                 // XeSS-SR 不含降噪
    out.needsExposure = 0;                  // v1 不接曝光纹理，只传 exposureScale
    out.takesMasks = MCDR_SR_MASK_RESPONSIVE; // pResponsivePixelMaskTexture
    out.externalHeap = 1;                   // SDK 原生支持 pTempBufferHeap/pTempTextureHeap
    out.jitterPhaseCount = 8;
    out.minRenderWidth = 320;
    out.minRenderHeight = 180;
    out.maxRenderWidth = 7680;
    out.maxRenderHeight = 4320;
    out.nativeAA = 1;                       // XESS_QUALITY_SETTING_AA = 1.0x
    out.reserved = 0;
    writeNote(out.note, sizeof(out.note), note);
}

}  // namespace

class XessSrProvider final : public IUpscaleProvider {
public:
    SrBackendId backend() const override { return SrBackendId::XeSS; }
    bool motionVectorsJittered() const override {
        return (initFlags_ & XESS_INIT_FLAG_JITTERED_MV) != 0;
    }

    void fillCaps(mcdr_sr_caps& out) const override {
        std::string why;
        const bool ok = isAvailable(why);
        if (ok) {
            std::string note = "libxess";
            {
                std::lock_guard<std::mutex> guard(runtime().lock);
                if (!runtime().version.empty()) {
                    note += " ";
                    note += runtime().version;
                }
            }
            fillCapsFields(out, true, note);
        } else {
            fillCapsFields(out, false, why);
        }
    }

    bool initialize(CommandContext* ctx, const mcdr_sr_dispatch& seed) override {
        device_ = deviceContext().device.Get();
        if (device_ == nullptr) {
            problem_ = "no d3d12 device";
            return false;
        }
        adoptSizes(seed.renderWidth, seed.renderHeight, seed.displayWidth, seed.displayHeight);
        maskExpected_ = seed.pReactiveMask != nullptr;
        motionUnjittered_ = (seed.flags & MCDR_SR_FLAG_UNJITTERED_MV) != 0;
        return ensureContext(ctx);
    }

    void onResize(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                  int32_t displayHeight) override {
        // 只记账：XeSS 的存储与层特化在 init 时就定了，重建要等 GPU 追平，推迟到下一帧 evaluate。
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
        Dx12Object* mask = asObject(dispatch.pReactiveMask);
        if (color == nullptr) { problem_ = "missing color input"; return false; }
        if (depth == nullptr) { problem_ = "missing depth"; return false; }
        if (motion == nullptr) { problem_ = "missing motion vectors"; return false; }
        if (output == nullptr) { problem_ = "missing color output"; return false; }
        if (color->resource == nullptr || depth->resource == nullptr ||
            motion->resource == nullptr || output->resource == nullptr) {
            problem_ = "resource handle without d3d12 resource";
            return false;
        }
        if (mask != nullptr && mask->resource == nullptr) { mask = nullptr; }
        // 低于能力下限时 XeSS 同样给不出有效结果：明确拒绝并只记一次日志。
        if (dispatch.renderWidth < 320 || dispatch.renderHeight < 180) {
            if (!tooSmallLogged_) {
                tooSmallLogged_ = true;
                dbgLogError("render %dx%d is below the %s minimum of 320x180; skipping "
                            "the evaluate (raise the render scale)",
                            dispatch.renderWidth, dispatch.renderHeight, "Intel XeSS");
            }
            problem_ = "render size below the 320x180 minimum";
            return false;
        }
        const bool unjittered = (dispatch.flags & MCDR_SR_FLAG_UNJITTERED_MV) != 0;
        if (!completion_.prepare(ctx, resetPending_, needsRecreate_)) {
            problem_ = "previous evaluation pending";
            return false;
        }
        if (motionUnjittered_ != unjittered) {
            if (handle_ != nullptr && !completion_.ready()) {
                problem_ = "previous evaluation pending";
                return false;
            }
            motionUnjittered_ = unjittered;
            needsRecreate_ = true;
        }
        if (maskExpected_ != (mask != nullptr)) {
            if (handle_ != nullptr && !completion_.ready()) {
                problem_ = "previous evaluation pending";
                return false;
            }
            maskExpected_ = mask != nullptr;
            needsRecreate_ = true;
        }
        adoptSizes(dispatch.renderWidth, dispatch.renderHeight, dispatch.displayWidth,
                   dispatch.displayHeight);
        if (!ensureContext(ctx)) { return false; }

        std::vector<SdkResourceAccess> accesses = {
            {color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON},
            {depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON},
            {motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON},
            {output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET}};
        if (mask != nullptr) {
            accesses.push_back({mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_COMMON});
        }
        SdkResourceScope resources(ctx, SdkResourceOwner::XeSS, std::move(accesses));
        if (!resources) return false;

        xess_d3d12_execute_params_t params{};
        params.pColorTexture = color->resource.Get();
        params.pVelocityTexture = motion->resource.Get();
        params.pDepthTexture = depth->resource.Get();
        params.pExposureScaleTexture = nullptr;      // 没置 EXPOSURE_SCALE_TEXTURE
        params.pResponsivePixelMaskTexture =
            mask != nullptr ? mask->resource.Get() : nullptr;
        params.pOutputTexture = output->resource.Get();
        params.jitterOffsetX = dispatch.jitterOffsetX;
        params.jitterOffsetY = dispatch.jitterOffsetY;
        params.exposureScale = dispatch.preExposure > 0.0f ? dispatch.preExposure : 1.0f;
        params.resetHistory = dispatch.reset != 0 || resetPending_ ? 1u : 0u;
        params.inputWidth = static_cast<uint32_t>(renderW_);
        params.inputHeight = static_cast<uint32_t>(renderH_);
        params.pDescriptorHeap = nullptr;            // 堆通过 init 的 pTempBufferHeap/pTempTextureHeap 给

        resetPending_ = true;
        completion_.record(ctx);
        const xess_result_t rc = runtime().api.execute(handle_, ctx->commandList, &params);
        resources.reclaim();
        if (rc != XESS_RESULT_SUCCESS) {
            problem_ = "xessD3D12Execute failed rc=";
            problem_ += std::to_string(static_cast<int>(rc));
            if (!logged_) {
                dbgLogWarn("XeSS execute failed: %s", problem_.c_str());
                logged_ = true;
            }
            return false;
        }
        problem_.clear();
        resetPending_ = false;
        return true;
    }

    void shutdown() override {
        destroyContext();
        problem_.clear();
        logged_ = false;
    }

    bool readyToRelease() const override { return completion_.ready(); }

private:
    void adoptSizes(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                    int32_t displayHeight) {
        if (renderWidth > 0) { renderW_ = renderWidth; }
        if (renderHeight > 0) { renderH_ = renderHeight; }
        if (displayWidth > 0) { displayW_ = displayWidth; }
        if (displayHeight > 0) { displayH_ = displayHeight; }
    }

    /// 挑一个能容纳 renderW_ x renderH_ 的质量档（全装不下取比例最接近的），把它的 [min,max] 写进 inputMin_ /
    /// inputMax_，让 evaluate 的越界检查与 init 用同一套边界。
    xess_quality_settings_t pickQualityForSize(const xess_2d_t& outputResolution,
                                               xess_2d_t& optimalOut) {
        static constexpr xess_quality_settings_t kCandidates[] = {
            XESS_QUALITY_SETTING_QUALITY,
            XESS_QUALITY_SETTING_BALANCED,
            XESS_QUALITY_SETTING_ULTRA_QUALITY,
            XESS_QUALITY_SETTING_PERFORMANCE,
            XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
            XESS_QUALITY_SETTING_ULTRA_PERFORMANCE,
            XESS_QUALITY_SETTING_AA,
        };
        const double wanted = static_cast<double>(displayW_) / static_cast<double>(renderW_);
        xess_quality_settings_t best = XESS_QUALITY_SETTING_QUALITY;
        double bestScore = 1.0e30;
        for (const xess_quality_settings_t candidate : kCandidates) {
            xess_2d_t optimal{};
            xess_2d_t minSize{};
            xess_2d_t maxSize{};
            if (runtime().api.getOptimalInputResolution(handle_, &outputResolution, candidate,
                                                        &optimal, &minSize, &maxSize) !=
                XESS_RESULT_SUCCESS) {
                continue;
            }
            const double got = static_cast<double>(displayW_) /
                               static_cast<double>(optimal.x == 0 ? 1u : optimal.x);
            const double score = got > wanted ? got - wanted : wanted - got;
            if (minSize.x <= static_cast<uint32_t>(renderW_) &&
                minSize.y <= static_cast<uint32_t>(renderH_) &&
                maxSize.x >= static_cast<uint32_t>(renderW_) &&
                maxSize.y >= static_cast<uint32_t>(renderH_)) {
                inputMin_ = minSize;
                inputMax_ = maxSize;
                optimalOut = optimal;
                if (candidate != XESS_QUALITY_SETTING_QUALITY) {
                    dbgLogInfo("XeSS q=%d fits %dx%d, range %ux%u..%ux%u",
                               static_cast<int>(candidate), renderW_, renderH_, minSize.x,
                               minSize.y, maxSize.x, maxSize.y);
                }
                return candidate;
            }
            if (score < bestScore) {
                bestScore = score;
                best = candidate;
                inputMin_ = minSize;
                inputMax_ = maxSize;
                optimalOut = optimal;
            }
        }
        return best;
    }
    bool ensureContext(CommandContext* ctx) {
        std::string why;
        if (!isAvailable(why)) {
            if (!logged_) {
                dbgLogWarn("XeSS unavailable: %s", why.c_str());
                logged_ = true;
            }
            problem_ = why.empty() ? std::string("libxess unavailable") : why;
            return false;
        }
        if (renderW_ <= 0 || renderH_ <= 0 || displayW_ <= 0 || displayH_ <= 0) {
            problem_ = "sizes not set";
            return false;
        }
        if (renderW_ > kMaxSide || renderH_ > kMaxSide || displayW_ > kMaxSide ||
            displayH_ > kMaxSide) {
            problem_ = "size beyond 8k";
            return false;
        }
        if (handle_ != nullptr && !needsRecreate_ && displayW_ == builtDisplayW_ &&
            displayH_ == builtDisplayH_ && renderW_ >= static_cast<int32_t>(inputMin_.x) &&
            renderW_ <= static_cast<int32_t>(inputMax_.x) &&
            renderH_ >= static_cast<int32_t>(inputMin_.y) &&
            renderH_ <= static_cast<int32_t>(inputMax_.y)) {
            return true;
        }
        if (handle_ != nullptr) {
            if (!completion_.ready()) { problem_ = "previous evaluation pending"; return false; }
            destroyContext();
        }

        const xess_result_t createRc =
            runtime().api.createContext(device_, &handle_);
        if (createRc != XESS_RESULT_SUCCESS || handle_ == nullptr) {
            handle_ = nullptr;
            problem_ = "xessD3D12CreateContext failed rc=";
            problem_ += std::to_string(static_cast<int>(createRc));
            if (!logged_) {
                dbgLogWarn("XeSS context creation failed: %s", problem_.c_str());
                logged_ = true;
            }
            return false;
        }
        if (runtime().api.isOptimalDriver != nullptr &&
            runtime().api.isOptimalDriver(handle_) == XESS_RESULT_WARNING_OLD_DRIVER) {
            dbgLogWarn("XeSS: old driver (XESS_RESULT_WARNING_OLD_DRIVER)");
        }

        // 质量档决定"允许的输入区间"，每帧输入尺寸必须落在其中：只按比例取最近档会每帧判越界重建（1.7x 档下界
        // 503x283，75% 渲染比例只给 495x278）→ 必须按尺寸挑档。
        xess_2d_t outputResolution{static_cast<uint32_t>(displayW_),
                                   static_cast<uint32_t>(displayH_)};
        xess_2d_t optimal{};
        inputMin_ = xess_2d_t{};
        inputMax_ = xess_2d_t{};
        const xess_quality_settings_t quality = pickQualityForSize(outputResolution, optimal);

        uint32_t flags = XESS_INIT_FLAG_INVERTED_DEPTH;
        if (!motionUnjittered_) { flags |= XESS_INIT_FLAG_JITTERED_MV; }
        if (maskExpected_) { flags |= XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK; }

        // 非阻塞的管线预建：init 会等它完成，先发出去能省一点排队时间。
        if (runtime().api.buildPipelines != nullptr) {            (void)runtime().api.buildPipelines(handle_, nullptr, false, flags);
        }

        // 临时堆的尺寸只与输出分辨率有关（xessGetProperties 的第三参），重建条件已包含它，建一次可复用。
        ensureTempHeaps(outputResolution);

        xess_d3d12_init_params_t initParams{};
        initParams.outputResolution = outputResolution;
        initParams.qualitySetting = quality;
        initParams.initFlags = flags;
        initParams.creationNodeMask = 0;
        initParams.visibleNodeMask = 0;
        initParams.pTempBufferHeap = tempBufferHeap_.Get();
        initParams.pTempTextureHeap = tempTextureHeap_.Get();
        initParams.bufferHeapOffset = 0;
        initParams.textureHeapOffset = 0;
        initParams.pPipelineLibrary = nullptr;

        const xess_result_t initRc = runtime().api.init(handle_, &initParams);
        if (initRc != XESS_RESULT_SUCCESS) {
            destroyContext();
            problem_ = "xessD3D12Init failed rc=";
            problem_ += std::to_string(static_cast<int>(initRc));
            if (!logged_) {
                dbgLogWarn("XeSS init failed: %s render=%dx%d display=%dx%d q=%d",
                           problem_.c_str(), renderW_, renderH_, displayW_,
                           static_cast<int>(quality));
                logged_ = true;
            }
            return false;
        }

        builtDisplayW_ = displayW_;
        builtDisplayH_ = displayH_;
        initFlags_ = flags;
        resetPending_ = true;
        needsRecreate_ = false;
        std::string version;
        {
            std::lock_guard<std::mutex> guard(runtime().lock);
            version = runtime().version;
        }
        dbgLogInfo("XeSS ready: libxess %s %dx%d->%dx%d q=%d flags=0x%x in %ux%u..%ux%u",
                   version.empty() ? "?" : version.c_str(), renderW_, renderH_, displayW_,
                   displayH_, static_cast<int>(quality), flags, inputMin_.x, inputMin_.y,
                   inputMax_.x, inputMax_.y);
        problem_.clear();
        return true;
    }

    void destroyContext() {
        if (handle_ != nullptr) {
            (void)runtime().api.destroy(handle_);
            handle_ = nullptr;
        }
        builtDisplayW_ = 0;
        builtDisplayH_ = 0;
        completion_.clear();
        initFlags_ = 0;
        // 顺序要紧：上下文先没了，堆才能放（SDK 在上下文生命周期内持有这些堆上的资源）。
        tempBufferHeap_.Reset();
        tempTextureHeap_.Reset();
    }

    /// 按 xessGetProperties 报的字节数建两个 DEFAULT 堆；失败就退回内部分配。
    void ensureTempHeaps(const xess_2d_t& outputResolution) {
        if (tempBufferHeap_ != nullptr && tempTextureHeap_ != nullptr) { return; }
        if (device_ == nullptr || runtime().api.getProperties == nullptr) { return; }
        xess_properties_t props{};
        if (runtime().api.getProperties(handle_, &outputResolution, &props) != XESS_RESULT_SUCCESS) {
            return;
        }
        // 大小与对齐都按 64 KB 走；堆本身不限制资源类型，因为 XeSS 会在里面放哪一类我们不该假设。
        const uint64_t kAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        auto alignUp = [kAlign](uint64_t v) { return (v + kAlign - 1) & ~(kAlign - 1); };
        D3D12_HEAP_DESC desc{};
        desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        desc.Properties.CreationNodeMask = 1;
        desc.Properties.VisibleNodeMask = 1;
        desc.Alignment = kAlign;
        desc.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;

        ComPtr<ID3D12Heap> bufferHeap;
        desc.SizeInBytes = alignUp(props.tempBufferHeapSize);
        HRESULT hr = desc.SizeInBytes > 0 ? device_->CreateHeap(&desc, IID_PPV_ARGS(&bufferHeap)) : E_INVALIDARG;
        if (FAILED(hr)) {
            warnHeapOnce("temp buffer", hr, props.tempBufferHeapSize);
            return;
        }

        ComPtr<ID3D12Heap> textureHeap;
        desc.SizeInBytes = alignUp(props.tempTextureHeapSize);
        hr = desc.SizeInBytes > 0 ? device_->CreateHeap(&desc, IID_PPV_ARGS(&textureHeap)) : E_INVALIDARG;
        if (FAILED(hr)) {
            warnHeapOnce("temp texture", hr, props.tempTextureHeapSize);
            return;
        }

        tempBufferHeap_ = bufferHeap;
        tempTextureHeap_ = textureHeap;
        dbgLogInfo("XeSS heaps: buffer=%.1f MB texture=%.1f MB desc=%u",
                   static_cast<double>(props.tempBufferHeapSize) / (1024.0 * 1024.0),
                   static_cast<double>(props.tempTextureHeapSize) / (1024.0 * 1024.0),
                   props.requiredDescriptorCount);
    }

    void warnHeapOnce(const char* what, HRESULT hr, uint64_t bytes) {
        if (heapWarned_) { return; }
        heapWarned_ = true;
        dbgLogWarn("XeSS: could not create the %s heap (%llu B, hr=0x%08lx); "
                   "falling back to XeSS-allocated memory",
                   what, static_cast<unsigned long long>(bytes), static_cast<unsigned long>(hr));
    }

    //
    // 临时缓冲 / 临时纹理堆：给了外部堆 SDK 就不再自己建，重建时复用同一对；失败退回 nullptr 让 SDK 自建并只警告一次。
    ComPtr<ID3D12Heap> tempBufferHeap_;
    ComPtr<ID3D12Heap> tempTextureHeap_;
    bool heapWarned_ = false;

    ID3D12Device* device_ = nullptr;
    xess_context_handle_t handle_ = nullptr;
    xess_2d_t inputMin_{};
    xess_2d_t inputMax_{};
    int32_t builtDisplayW_ = 0;
    int32_t builtDisplayH_ = 0;
    int32_t renderW_ = 0;
    int32_t renderH_ = 0;
    int32_t displayW_ = 0;
    int32_t displayH_ = 0;
    bool maskExpected_ = false;
    bool motionUnjittered_ = false;
    uint32_t initFlags_ = 0;
    SrContextCompletion completion_;
    bool resetPending_ = true;
    bool needsRecreate_ = false;
    std::string problem_;
    bool logged_ = false;
    bool tooSmallLogged_ = false;
};

// caps 的 XeSS 分支（srFillCaps 调它）。**不建上下文**：caps 只回答"这个后端能不能用"。
void fillXessCaps(mcdr_sr_caps& out) {
    std::string why;
    const bool ok = isAvailable(why);
    if (ok) {
        std::string note = "libxess";
        {
            std::lock_guard<std::mutex> guard(runtime().lock);
            if (!runtime().version.empty()) {
                note += " ";
                note += runtime().version;
            }
        }
        fillCapsFields(out, true, note);
    } else {
        fillCapsFields(out, false, why);
    }
}

std::unique_ptr<IUpscaleProvider> makeXessProvider() {
    std::string why;
    if (!isAvailable(why)) {
        dbgLogInfo("XeSS provider not created: %s", why.c_str());
        return nullptr;
    }
    return std::make_unique<XessSrProvider>();
}

// 运行库是否载入成功（给"没有上下文也要回答"的地方用）。
bool xessRuntimeAvailable() {
    std::string why;
    return isAvailable(why);
}

}  // namespace d3d12_mc
