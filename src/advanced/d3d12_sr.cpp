// d3d12_sr.cpp — 超分（SR）Provider 接口层的实现单元（模块 mcdr.d3d12.device）：
// 能力表 srFillCaps、DLSS 实现 DlssSrProvider、注册表（picked / active / 状态文本）。
// 厂商细节只留在 provider 里，src\c_abi\abi_sr.cpp 不得出现任何厂商名。见 docs\advanced\upscalers.md

module;

#include "_d3d12_module_prelude.h"

// 必须放在全局模块片段：模块 purview 内文本包含头文件会触发 C5244，/WX 下直接失败。
// .cppm 全局模块片段里的声明不会传给实现单元，各实现单元要自己再 include 一次（guard 幂等）。
#include "mcdr_abi.h"

#include <algorithm>
#include <cstdio>
#include <string_view>

module mcdr.d3d12.device;

namespace d3d12_mc {
bool releaseProviderWithWait();

bool SrContextCompletion::prepare(CommandContext* ctx, bool& resetPending, bool& recreatePending) {
    if (ctx == nullptr || !ctx->listOpen) return false;
    const auto current = currentCommandRecording(ctx);
    for (auto use = uses_.begin(); use != uses_.end();) {
        const auto& recording = *use->recording;
        if (recording.cancelled && !recording.submitted) {
            resetPending = true;
            recreatePending = true;
            use = uses_.erase(use);
            continue;
        }
        if (!recording.submitted) {
            if (use->recording != current) return false;
            ++use;
            continue;
        }
        if (recording.cancelled && !use->submissionFailureObserved) {
            resetPending = true;
            use->submissionFailureObserved = true;
        }
        const auto completed = recording.fence->GetCompletedValue();
        if (completed == UINT64_MAX) return false;
        if (completed >= recording.fenceValue) use = uses_.erase(use);
        else ++use;
    }
    return true;
}

bool SrContextCompletion::ready() const {
    if (std::ranges::any_of(uses_, [](const auto& use) {
        return !use.recording->submitted && !use.recording->cancelled;
    })) return false;
    const auto started = GetTickCount64();
    for (const auto& use : uses_) {
        const auto& recording = *use.recording;
        if (!recording.submitted) continue;
        const auto completed = recording.fence->GetCompletedValue();
        if (completed == UINT64_MAX) return false;
        if (completed >= recording.fenceValue) continue;
        const auto elapsed = GetTickCount64() - started;
        if (elapsed >= 5000) return false;
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) return false;
        const HRESULT result = recording.fence->SetEventOnCompletion(recording.fenceValue, event);
        const bool completedInTime = SUCCEEDED(result) &&
            WaitForSingleObject(event, static_cast<DWORD>(5000 - elapsed)) == WAIT_OBJECT_0;
        CloseHandle(event);
        if (!completedInTime) return false;
    }
    return true;
}

void SrContextCompletion::record(CommandContext* ctx) {
    const auto recording = currentCommandRecording(ctx);
    if (std::ranges::none_of(uses_, [&](const auto& use) { return use.recording == recording; })) {
        uses_.push_back({recording});
    }
}

void SrContextCompletion::clear() {
    uses_.clear();
}

namespace {

// MCDR_SR_BACKEND_* → SrBackendId 的**唯一**转换点。加后端只改这里。
SrBackendId toId(int32_t backendId) {
    switch (backendId) {
        case MCDR_SR_BACKEND_DLSS:
            return SrBackendId::Dlss;
        case MCDR_SR_BACKEND_FSR:
            return SrBackendId::Fsr;
        case MCDR_SR_BACKEND_XESS:
            return SrBackendId::XeSS;
        default:
            return SrBackendId::None;
    }
}

bool validBackend(int32_t backendId) {
    return backendId == MCDR_SR_BACKEND_NONE || backendId == MCDR_SR_BACKEND_DLSS ||
           backendId == MCDR_SR_BACKEND_FSR || backendId == MCDR_SR_BACKEND_XESS;
}

const char* nameOf(int32_t backendId) {
    switch (backendId) {
        case MCDR_SR_BACKEND_DLSS:
            return "dlss";
        case MCDR_SR_BACKEND_FSR:
            return "fsr";
        case MCDR_SR_BACKEND_XESS:
            return "xess";
        default:
            return "none";
    }
}

// 定长字段的 NUL 结尾拷贝：note 是 char[28]，不能假设调用方清零或源串更短。
void copyNote(char* dst, size_t cap, const char* src) {
    if (cap == 0) {
        return;
    }
    size_t n = 0;
    while (n + 1 < cap && src[n] != '\0') {
        ++n;
    }
    for (size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
}

// 能力表数值改动要同步 docs\advanced\upscalers.md 的 caps 表。
void fillDlssCaps(mcdr_sr_caps& out) {
    char buf[192] = {};
    const bool up = queryDlss(buf, static_cast<int>(sizeof(buf))) == 1;
    out.available = up ? 1 : 0;
    out.coupledDenoise = 0;    // DLSS-SR 不含降噪；DLSS-D 是另一个后端（v1 不做）
    out.needsExposure = 0;
    out.takesMasks = MCDR_SR_MASK_REACTIVE;
    out.externalHeap = 1;      // Streamline 可通过 slAllocateResources 用外部堆
    out.jitterPhaseCount = 8;  // Streamline 的 8 相位（与 D3D12Temporal 的 Halton(2,3) 对齐）
    out.minRenderWidth = 320;
    out.minRenderHeight = 180;
    out.maxRenderWidth = 7680;
    out.maxRenderHeight = 4320;
    out.nativeAA = 1;          // DLAA：1.0x
    copyNote(out.note, sizeof(out.note), up ? buf : "dlss unavailable");
}

// 把与厂商无关的载荷翻译成 DLSS 的老表。**0 = 未填**：motionVectorScale / preExposure 传 0 会让
// DLSS 把运动矢量当零位移（一运动就拖影），而 0 正是 JVM 侧零初始化的自然结果 —— 这里补 1.0，别原样传。
mcdr_dlss_input toDlssInput(const mcdr_sr_dispatch& d) {
    mcdr_dlss_input in{};
    in.renderWidth = d.renderWidth;
    in.renderHeight = d.renderHeight;
    in.targetWidth = d.displayWidth;
    in.targetHeight = d.displayHeight;
    in.jitterOffsetX = d.jitterOffsetX;
    in.jitterOffsetY = d.jitterOffsetY;
    // D3D12Velocity 写出的 MV 已经是像素位移（(uv - prevUv) * screenSize），倍率就该是 1.0。
    in.mvScaleX = d.motionVectorScaleX != 0.0f ? d.motionVectorScaleX : 1.0f;
    in.mvScaleY = d.motionVectorScaleY != 0.0f ? d.motionVectorScaleY : 1.0f;
    in.preExposure = d.preExposure != 0.0f ? d.preExposure : 1.0f;
    in.exposureScale = 1.0f;
    in.exposureMode = MCDR_DLSS_EXPOSURE_NONE;
    // 世界层颜色不含 UI alpha（GUI 有自己的目标），所以不做 alpha 升采样。
    in.alphaUpscaling = 0;
    in.nearZ = d.cameraNear;
    in.farZ = d.cameraFar;
    // 26.3 是 reverse-Z（清深度 0.0 + GREATER_THAN_OR_EQUAL）：深度倒置是硬要求，SR 载荷不单独暴露。
    in.depthInverted = 1;
    in.reset = d.reset;
    in.depthFormatOrdinal = d.depthFormatOrdinal;
    in.pInColor = static_cast<mcdr_object*>(d.pColorInput);
    in.pInDepth = static_cast<mcdr_object*>(d.pDepth);
    in.pInMotionVectors = static_cast<mcdr_object*>(d.pMotionVectors);
    in.pInOutput = static_cast<mcdr_object*>(d.pColorOutput);
    return in;
}

// ---- 注册表状态（全部由渲染线程写；热切换由 ensureProvider 懒执行）----
int32_t gSrPicked = MCDR_SR_BACKEND_NONE;
int32_t gSrActive = MCDR_SR_BACKEND_NONE;
std::string gSrNote;
std::unique_ptr<IUpscaleProvider> gSrProvider;

// 诊断计数：只给 mcdr_dx12SrQuery 的文本用 —— provider ready 只在初始化时打一次，这里记逐帧结果。
uint64_t gSrEvalOk = 0;
uint64_t gSrEvalFail = 0;
int32_t gSrLastRenderWidth = 0;
int32_t gSrLastRenderHeight = 0;
int32_t gSrLastDisplayWidth = 0;
int32_t gSrLastDisplayHeight = 0;

// ---------------------------------------------------------------------------
// 刻意**不持有**任何 Streamline / NGX 状态：那一切仍在 src\advanced\d3d12_dlss.cpp 的单例里，
// applyDlssToTexture 是那条路径的唯一入口（含 Dx12Object 解包、render pass 收尾、barrier）。
// ---------------------------------------------------------------------------
class DlssSrProvider final : public IUpscaleProvider {
public:
    SrBackendId backend() const override { return SrBackendId::Dlss; }

    void fillCaps(mcdr_sr_caps& out) const override { fillDlssCaps(out); }

    bool initialize(CommandContext* ctx, const mcdr_sr_dispatch& seed) override {
        // ctx 目前用不到：NGX 的 CreateFeature 会录制自己的命令，必须等命令列表打开，所以建特征在 evaluate 里。
        static_cast<void>(ctx);
        mMaxRenderWidth = seed.renderWidth;
        mMaxRenderHeight = seed.renderHeight;
        mDisplayWidth = seed.displayWidth;
        mDisplayHeight = seed.displayHeight;
        char buf[192] = {};
        if (queryDlss(buf, static_cast<int>(sizeof(buf))) != 1) {
            gSrNote = buf[0] != '\0' ? buf : "dlss unavailable";
            return false;
        }
        return true;
    }

    void onResize(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                  int32_t displayHeight) override {
        // DLSS 的尺寸来自每帧载荷，不需要单独通知；这里只记账，重建动作留给真正持有临时资源的后端。
        mMaxRenderWidth = renderWidth;
        mMaxRenderHeight = renderHeight;
        mDisplayWidth = displayWidth;
        mDisplayHeight = displayHeight;
    }

    bool evaluate(CommandContext* ctx, const mcdr_sr_dispatch& desc) override {
        if (ctx == nullptr || desc.pColorInput == nullptr || desc.pColorOutput == nullptr) {
            return false;
        }
        return applyDlssToTexture(ctx, toDlssInput(desc));
    }

    bool readyToRelease() const override { return true; }

    void shutdown() override {
        // **空操作是刻意的**：mcdr_dlss_shutdown 会清掉实现单元的全局状态，连带把还没切到
        // SR 路径的老 DLSS 调用一起关掉（hand-off 会丢输出纹理）。真正的释放在设备销毁时做。
    }

private:
    int32_t mMaxRenderWidth = 0;
    int32_t mMaxRenderHeight = 0;
    int32_t mDisplayWidth = 0;
    int32_t mDisplayHeight = 0;
};

// 工厂：不加 case 的后端 srCreateProvider 返回 0，并把原因写进 gSrNote。
std::unique_ptr<IUpscaleProvider> makeProvider(SrBackendId id) {
    switch (id) {
        case SrBackendId::Dlss:
            return std::make_unique<DlssSrProvider>();
        case SrBackendId::Fsr:
            // 运行库不在时返回 nullptr，原因由 makeFsrProvider 打日志。
            return makeFsrProvider();
        case SrBackendId::XeSS:
            return makeXessProvider();
        default:
            return nullptr;
    }
}

// 懒建 / 换后端：srCreateProvider 与 srEvaluateFrame（只 Pick + Evaluate 时也要能工作）都走这里。
bool ensureProvider(CommandContext* ctx, int32_t backendId, const mcdr_sr_dispatch& seed) {
    if (gSrProvider != nullptr && gSrActive == backendId) {
        return true;
    }
    if (gSrProvider != nullptr) {
        const int32_t previous = gSrActive;
        if (!releaseProviderWithWait()) return false;
        dbgLogInfo("SR provider switch %s -> %s", nameOf(previous), nameOf(backendId));
    }
    std::unique_ptr<IUpscaleProvider> provider = makeProvider(toId(backendId));
    if (provider == nullptr) {
        gSrNote = "backend not implemented";
        return false;
    }
    if (!provider->initialize(ctx, seed)) {
        return false;  // initialize 已经写过 gSrNote
    }
    gSrActive = backendId;
    gSrProvider = std::move(provider);
    dbgLogInfo("SR ready: %s render %dx%d display %dx%d", nameOf(backendId),
               seed.renderWidth, seed.renderHeight, seed.displayWidth, seed.displayHeight);
    return true;
}

// 种子载荷：只填尺寸、资源指针留空；要在建上下文时就知道 HDR / 深度格式就往这里加字段。
mcdr_sr_dispatch seedFromSizes(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                               int32_t displayHeight) {
    mcdr_sr_dispatch seed{};
    seed.renderWidth = renderWidth;
    seed.renderHeight = renderHeight;
    seed.displayWidth = displayWidth;
    seed.displayHeight = displayHeight;
    return seed;
}

}  // namespace

int32_t srPickBackend(int32_t backendId) {
    if (!validBackend(backendId)) {
        gSrNote = "unknown backend id";
        return 0;
    }
    gSrPicked = backendId;
    return 1;
}

// 帧生成门控用 srActiveBackend 而不是这里：SR=FSR/XeSS 时 DLSS-G 拿不到 Streamline 的
// ScalingOutputColor tag 会静默不插帧，而 JVM 主要走 SrCreate、gSrPicked 可能一直是 NONE。
//
// 用户选的那一家（mcdr_dx12SrPick 写入）。
int32_t srSelectedBackend(void) { return gSrPicked; }

int32_t srActiveBackend(void) { return gSrActive; }

void srFillCaps(int32_t backendId, mcdr_sr_caps& out) {
    out = mcdr_sr_caps{};
    out.backend = backendId;
    switch (static_cast<int>(backendId)) {
        case MCDR_SR_BACKEND_NONE:
            // "不超分"也是一个合法选择：UI 关掉 SR 走这条，不是错误。
            out.available = 1;
            out.nativeAA = 1;
            copyNote(out.note, sizeof(out.note), "off");
            return;
        case MCDR_SR_BACKEND_DLSS:
            fillDlssCaps(out);
            return;
        case MCDR_SR_BACKEND_FSR:
            // caps 只由运行库能否加载决定，不建上下文（见 fillFsrCaps）。
            fillFsrCaps(out);
            return;
        case MCDR_SR_BACKEND_XESS:
            fillXessCaps(out);
            return;
        default:
            copyNote(out.note, sizeof(out.note), "unknown backend id");
            return;
    }
}

int32_t srCreateProvider(CommandContext* ctx, int32_t backendId, int32_t maxRenderWidth,
                         int32_t maxRenderHeight, int32_t displayWidth, int32_t displayHeight) {
    if (ctx == nullptr || !validBackend(backendId) || maxRenderWidth <= 0 || maxRenderHeight <= 0 ||
        displayWidth <= 0 || displayHeight <= 0) {
        return -1;
    }
    if (backendId == MCDR_SR_BACKEND_NONE) {
        return releaseProviderWithWait() ? 1 : 0;
    }
    const mcdr_sr_dispatch seed =
        seedFromSizes(maxRenderWidth, maxRenderHeight, displayWidth, displayHeight);
    // 幂等：同一后端重复 create 只更新尺寸，不重建（热切换靠这个语义）。
    if (gSrProvider != nullptr && gSrActive == backendId) {
        gSrProvider->onResize(maxRenderWidth, maxRenderHeight, displayWidth, displayHeight);
        return 1;
    }
    return ensureProvider(ctx, backendId, seed) ? 1 : 0;
}

int32_t srResizeProvider(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                         int32_t displayHeight) {
    if (renderWidth <= 0 || renderHeight <= 0 || displayWidth <= 0 || displayHeight <= 0) {
        return -1;
    }
    if (gSrProvider == nullptr) {
        return 0;
    }
    gSrProvider->onResize(renderWidth, renderHeight, displayWidth, displayHeight);
    return 1;
}

int32_t srEvaluateFrame(CommandContext* ctx, const mcdr_sr_dispatch& desc) {
    if (ctx == nullptr || ctx->commandList == nullptr || !ctx->listOpen || ctx->inRenderPass) {
        return -1;
    }
    int32_t backendId = static_cast<int32_t>(desc.backend);
    if (backendId == MCDR_SR_BACKEND_NONE) {
        backendId = gSrPicked;
    }
    if (!validBackend(backendId)) {
        return -1;
    }
    if (backendId == MCDR_SR_BACKEND_NONE) {
        return 0;
    }
    if ((desc.flags & ~MCDR_SR_FLAG_UNJITTERED_MV) != 0 ||
        (backendId == MCDR_SR_BACKEND_DLSS && desc.flags != 0)) return -1;
    if (!ensureProvider(ctx, backendId, desc)) {
        return 0;  // 不可用 / 未实现：由 SrQuery / SrQueryCaps 报原因
    }
    const bool ok = gSrProvider->evaluate(ctx, desc);
    gSrLastRenderWidth = desc.renderWidth;
    gSrLastRenderHeight = desc.renderHeight;
    gSrLastDisplayWidth = desc.displayWidth;
    gSrLastDisplayHeight = desc.displayHeight;
    if (ok) {
        ++gSrEvalOk;
        // "last" 是最近一次失败的原因：成功就清掉，否则诊断行会长期挂着早已恢复的旧错误。
        gSrNote.clear();
    } else {
        ++gSrEvalFail;
        gSrNote.assign(nameOf(backendId)).append(" evaluate failed");
    }
    // 无论成败都要把主描述符堆绑回来：各厂商 Evaluate 内部会 SetDescriptorHeaps，而堆绑定是命令列表级
    // 状态、不随根签名 / PSO 复位，不绑回来后续走瞬时 SRV 的绘制会撞"descriptor heap … different"。
    restoreDrawHeap(ctx);
    if (!ok) {
        return 0;
    }
    gSrActive = backendId;
    // 包装型帧后端的输入 tag 必须落在**这一帧的命令列表**上（XeSS 的 TagFrameResource、FSR 的
    // InterpolationCommandListDX12 都是这个时机），而不是 Present 前后；DLSS-G 不覆写这个钩子。
    fgTagFrameInputs(fgMakeFrameInputs(ctx, desc));
    return 1;
}

// 相位长度由后端回答，不要让上层写死常数：DLSS(SL) 8（= caps.jitterPhaseCount）；XeSS 8（SDK 自己按
// xessGetJitterScale 缩放，序列长度不变）；None 0（不适用）。FSR 用公式 8*(dw/rw)^2 夹到 [1,256] ——
// 那是无后端近似，接了真后端应改问 ffxQueryDescUpscaleGetJitterPhaseCount。
int32_t srJitterPhaseCount(int32_t backendId, int32_t renderWidth, int32_t displayWidth) {
    if (!validBackend(backendId)) {
        return 0;
    }
    switch (toId(backendId)) {
        case SrBackendId::None:
            return 0;
        case SrBackendId::Dlss:
            return 8;
        case SrBackendId::Fsr: {
            if (renderWidth <= 0 || displayWidth <= 0) {
                return 8;
            }
            const double ratio = static_cast<double>(displayWidth) / static_cast<double>(renderWidth);
            long long count = static_cast<long long>(8.0 * ratio * ratio + 0.5);
            if (count < 1) count = 1;
            if (count > 256) count = 256;
            return static_cast<int32_t>(count);
        }
        case SrBackendId::XeSS:
            return 8;
    }
    return 0;
}

void srReleaseProvider(void) {
    (void)releaseProviderWithWait();
}

bool releaseProviderWithWait() {
    if (gSrProvider == nullptr) return true;
    if (!gSrProvider->readyToRelease()) {
        gSrNote = "previous evaluation pending";
        return false;
    }
    gSrProvider->shutdown();
    gSrProvider.reset();
    gSrActive = MCDR_SR_BACKEND_NONE;
    return true;
}

// 一行诊断文本：picked/active 是否一致、逐帧 ok/fail 计数、生效后端的 caps、最近一次降级原因。
// srFillCaps 会去问运行库（LoadLibrary 尝试），只该在诊断 / 界面刷新时调，不要挂到每帧路径上。
std::string srStatusText(void) {
    std::string text = "sr active=";
    text += nameOf(gSrActive);
    text += " picked=";
    text += nameOf(gSrPicked);
    text += " eval=";
    text += std::to_string(gSrEvalOk);
    text += "/";
    text += std::to_string(gSrEvalFail);
    if (gSrLastDisplayWidth > 0) {
        text += " size=";
        text += std::to_string(gSrLastRenderWidth);
        text += "x";
        text += std::to_string(gSrLastRenderHeight);
        text += "->";
        text += std::to_string(gSrLastDisplayWidth);
        text += "x";
        text += std::to_string(gSrLastDisplayHeight);
    }
    if (gSrProvider != nullptr) {
        text += " mvJitter=";
        text += gSrProvider->motionVectorsJittered() ? "1" : "0";
    }
    mcdr_sr_caps caps{};
    srFillCaps(gSrActive, caps);
    text += " caps[";
    text += caps.available != 0 ? "ok" : "unavailable";
    text += " masks=";
    text += std::to_string(caps.takesMasks);
    text += " jitter=";
    text += std::to_string(caps.jitterPhaseCount);
    text += " max=";
    text += std::to_string(caps.maxRenderWidth);
    text += "x";
    text += std::to_string(caps.maxRenderHeight);
    text += "]";
    // NONE 档的 note 固定是 "off"，那是正常状态，别塞进诊断行（会让人以为出错）。
    if (caps.available == 0 || std::string_view(caps.note) != "off") {
        if (caps.note[0] != 0) {
            text += " why=";
            text += caps.note;
        }
    }
    if (!gSrNote.empty()) {
        text += " last=";
        text += gSrNote;
    }
    return text;
}

}  // namespace d3d12_mc
