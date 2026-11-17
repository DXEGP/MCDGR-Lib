// d3d12_framegen.cpp — P293：帧生成（FG）Provider 接口层的实现单元（模块 mcdr.d3d12.device）
//
// 三块内容，与 `d3d12_sr.cpp`（P272）一一对应：
//   1. 能力表（`fillDlssGCaps`）—— 「这家后端能不能用、为什么不能用」的唯一事实来源；
//   2. `DlssGFrameGenProvider` —— DLSS-G 的实现。**P293 是个薄包装**：真正的
//      `slDLSSGSetOptions` 仍然在 `d3d12_dlss.cpp` 的 streamlineEvaluate 里下发，这里只把
//      既有 C 入口（`mcdr_dlss_set_fg_mode` / `mcdr_dlss_fg_presented`）接到接口上。
//      为什么先包装而不是搬：P293/P295 只新增抽象，行为等价性优先 —— 搬实现要同时动
//      渲染线程的 tag 提交顺序，那属于 P296；
//   3. 注册表 —— 期望后端（picked）/ 生效后端（applied）/ 链归属（chainOwner）/ 一行状态文本。
//
// 为什么放在本模块而不是再切一个模块（D15）：provider 要在 Present 两侧插钩子、要能读
// 交换链状态、要解包 `Dx12Object`，这三件事只有 mcdr.d3d12.device 能做 —— 与 P272 把
// `IUpscaleProvider` 留在本模块是同一条理由（`mcdr_device.cppm:1105-1107`）。

module;

#include "_d3d12_module_prelude.h"

// mcdr_fg_caps / mcdr_fg_dispatch / MCDR_FG_* 都在这里。必须放在全局模块片段：模块
// purview 内文本包含头文件会触发 C5244，而本项目 /WX 直接失败。与 d3d12_sr.cpp:20-24
// 同理，**.cppm 全局模块片段里的声明不会传给实现单元**，每个实现单元要自己再 include。
#include "mcdr_abi.h"

// FG 的两个既有 C 入口（`mcdr_dlss_set_fg_mode` / `mcdr_dlss_fg_presented`）与 Reflex 的
// 可用性查询，都在这份头里。P293 不搬它们，只调它们。
#include "mcdr_dlss_impl.h"
#include <optional>

module mcdr.d3d12.device;

import mcdr.render.oit;

namespace d3d12_mc {

namespace {

// 把一句话写进固定长度的 note 缓冲（NUL 结尾、超长截断）。不用 snprintf：这里只需要
// 逐字节拷贝，少一个格式化依赖，也不会因为 /analyze 的格式检查多出警告。
void writeNote(char* dst, size_t cap, const char* text) {
    if (dst == nullptr || cap == 0) {
        return;
    }
    size_t i = 0;
    if (text != nullptr) {
        for (; i + 1 < cap && text[i] != '\0'; ++i) {
            dst[i] = text[i];
        }
    }
    dst[i] = '\0';
}

// 注册表状态。期望（picked）与生效（applied）分开，热切换在下一帧帧首完成（P280 同构）。
//
// P296 起这个结构体跨线程：`fgSetMode` 由 JVM 的设置线程调（改一下界面滑条就调），
// `fgBeforePresent`/`fgAfterPresent`/`fgChainRebuildPending` 由渲染线程/主线程每帧调。
// 里面既有 `std::unique_ptr` 又有 `std::string`，裸读裸写就是数据竞争（provider 可能在
// 渲染线程正调 `beforePresent()` 时被设置线程 reset 掉）。所以**每个入口都拿同一把锁**，
// 包括每帧那两条 —— 无竞争时 mutex 约 20ns，相对一次 Present（数百 µs）可忽略。
// 后端还没建成时 `fgBeforePresent` 的重试间隔（帧）。60 fps 下 30 帧约 0.5 s，
// 与 `refreshFgApi()` 对 SL 入口的 500 ms 重试节奏一致。
constexpr uint64_t ENSURE_RETRY_FRAMES = 30;

// 「期望开、可后端一直建不出来」时给一次 warn 的宽限期（帧）。300 帧在 60 fps 下约 5 s ——
// 启动顺序造成的短暂不可用会在这之前自愈，过了这个点还建不出来就是真的不行了。
constexpr uint64_t USABLE_GRACE_FRAMES = 300;

// P300：能力位从「可用」掉到「不可用」连续多少帧才回滚。60 帧在 60 fps 下约 1 s ——
// 一次瞬时抖动（运行库重新解析、SR 后端切换的中间态）不该把后端拆掉。
constexpr uint64_t ROLLBACK_FRAMES = 60;

struct FgRegistry {
    std::mutex mutex;
    std::unique_ptr<IFrameGenProvider> provider;
    int32_t picked = MCDR_FG_BACKEND_NONE;
    // P305ab：自测钩子显式指定过后端时为真，gSetMode 就不再让门面的推测覆盖 picked。
    bool forcedPick = false;
    int32_t applied = MCDR_FG_BACKEND_NONE;
    // P253 之后链的归属是「建链那一刻定的」，provider 改不动它 —— 所以单独存一份，
    // 由 `d3d12_surface.cpp:91` 在工厂升级成功后写进来。
    int32_t chainOwner = MCDR_FG_CHAIN_APP_NATIVE;
    int32_t lastResult = 0;
    std::string lastReason;
    mcdr_fg_constants constants{};
    bool hasConstants = false;
    enum class TicketState { Invalidated, Accepted, Queued, Presented };
    struct CameraReceipt {
        FgPresentTicket ticket;
        TicketState state = TicketState::Invalidated;
        mcdr_frame_identity identity{};
        uint64_t historyGeneration = 0;
        uint64_t providerGeneration = 0;
        double anchor[3]{};
        CommandContext* context = nullptr;
        std::weak_ptr<void> recording;
        ComPtr<ID3D12Fence> fence;
        UINT64 fenceValue = 0;
        bool valid = false;
        FgFrameInputs facts{};
    };
    CameraReceipt pendingCamera;
    CameraReceipt committedCamera;
    uint64_t cameraGeneration = 1;
    uint64_t frameSerial = 0;
    FgPresentTicket activeFrame;
    mcdr_frame_identity activeIdentity{};
    mcdr_frame_settings activeSettings{};
    uint64_t activeHistoryGeneration = 0;
    uint32_t activeColorEncoding = 0;
    bool frameStarted = false;
    bool providerStarted = false;
    bool guiReady = false;
    bool resetPending = false;
    // Keep both the pool-wrapper lease and the COM resource. Never replay a borrowed
    // Dx12Object*: a world reset/resize may have closed its JVM owner meanwhile.
    std::shared_ptr<void> uiLease;
    ComPtr<ID3D12Resource> uiTexture;
    struct RecordingUse {
        ComPtr<ID3D12Fence> fence;
        UINT64 value = 0;
        std::weak_ptr<void> recording;
        bool submitted = false;
    };
    std::vector<RecordingUse> recordings;
    // P306ac：hudless 呈现模式（1 = present 的世界层不含 UI，UI 由 provider 合成）。
    // 与 `uiTexture` 同一条纪律：期望值，provider 建成后回放。
    int32_t uiComposition = 0;
    // P296（D13）：期望的 mode 与「链是在哪个 mode 下建成的」分开记。两者跨过 0 边界
    // （开 → 关 / 关 → 开）时挂一次重建请求，由帧首的 acquireSurface 消费。
    int32_t desiredMode = 0;
    int32_t desiredFrames = 1;
    int32_t chainMode = 0;
    bool rebuildPending = false;
    int32_t rebuilds = 0;
    // P297：三轨帧号。**谁维护、什么时候复位**是契约的一部分，别随手清零 ——
    // FSR-FG / XeSS-FG 都要求交给它们的 present id **严格单调 +1**，一个回绕或跳号
    // 会让它们静默丢帧（或直接拒绝）。
    //   appFrameId     渲染线程每调一次 `fgBeforePresent` +1（= 每次尝试 Present 一帧，
    //                  Present 因为没 blit 被跳过时不会走到这里，也就不会 +1）；
    //   backendFrameId provider 真正向后端提交了一帧时由 provider 调 `fgNoteBackendFrame`
    //                  写进来（DLSS-G 不提交 —— SL 自己管帧 token，所以它恒为 0）；
    //   presentId      每次 `Present` 返回之后 +1，**从 0 开始、跨 Resize 与切后端都不复位**。
    uint64_t appFrameId = 0;
    uint64_t backendFrameId = 0;
    uint64_t presentId = 0;
    // 每 600 帧打一行，验收 A7 就靠它（600 帧严格 +1）。
    uint64_t lastTrackLog = 0;
    // P296 实测缺口（2026-10-27 13:06 冒烟）：启动早期 `mcdr_reflex_available()` 还是 0
    // （slInit / 设备创建晚于 `Options.load` 里的 push），当时建不出后端，而 JVM 只在设置
    // 变更时 push 一次 —— 用户存下来的"帧生成开"会静默失效一整局。所以这里记一个重试时刻，
    // 由 `fgBeforePresent` 每 ENSURE_RETRY_FRAMES 帧再试一次（30 帧 ≈ 60 fps 下 0.5 s）。
    uint64_t ensureTryAt = 0;
    // 「期望开但一直建不出来」只 warn 一次（见 fgAfterPresent）。后端建成后清零，
    // 这样将来热切换失败一次仍然能再报警。
    bool warnedNotUsable = false;
    // P300：后端已建成、但能力位后来掉成不可用的连续帧数（见 fgAfterPresent 的回滚段）。
    uint64_t unavailableFrames = 0;
    struct PreparedMode {
        uint64_t token = 0;
        mcdr_fg_dispatch description{};
        uint64_t cameraGeneration = 0;
        std::unique_ptr<IFrameGenProvider> provider;
    };
    std::optional<PreparedMode> preparedMode;
    uint64_t nextPreparationToken = 1;
};

FgRegistry& registry() {
    static FgRegistry s;
    return s;
}

bool invalidateCameraLocked(FgRegistry& state) {
    ++state.cameraGeneration;
    state.pendingCamera.state = FgRegistry::TicketState::Invalidated;
    state.pendingCamera = {};
    state.committedCamera = {};
    state.hasConstants = false;
    if (state.provider) state.provider->discardFrameInputs();
    return state.applied != MCDR_FG_BACKEND_DLSSG || mcdr_dlss_invalidate_inputs() == 1;
}

bool recordingComplete(const FgRegistry::RecordingUse& use) {
    if (use.recording.expired()) return true; // abandoned list, no GPU submission
    const UINT64 completed = use.fence ? use.fence->GetCompletedValue() : 0;
    // UINT64_MAX signals device removal, not successful GPU completion.
    return use.submitted && completed != UINT64_MAX && completed >= use.value;
}

bool recordingsCompleteLocked(FgRegistry& s) {
    std::erase_if(s.recordings, recordingComplete);
    if (s.recordings.empty()) return true;
    s.lastReason = "FG input recording still live";
    return false;
}

// P304：链归属的一个**无锁快照**。
//
// 为什么需要第二份：`fillCaps` 会被 `fgAfterPresent` 在**持有注册表锁**的情况下调用
// （见那里的 `s.provider->fillCaps(live)`），而包装型后端（FSR-FG / XeSS-FG）判
// `chainCompatible` 时必须知道链是哪一种。若在 `fillCaps` 里调 `fgChainOwner()`，
// `std::mutex` 不可重入 → 每帧死锁一次。所以写入口（`fgNoteChainOwner` /
// `fgNoteChainRebuildDone`）同时更新这个原子量，读入口（`fgChainOwnerFast`）只读它。
//
// 它不是"第二个真相"：注册表里的 `chainOwner` 才是权威，这个原子量是它的镜像，
// 且只在两个写点上更新（都在已持锁的临界区里写）。
std::atomic<int32_t> gChainOwnerSnapshot{MCDR_FG_CHAIN_APP_NATIVE};

bool validBackendId(int32_t backendId) {
    return backendId == MCDR_FG_BACKEND_NONE || backendId == MCDR_FG_BACKEND_DLSSG ||
           backendId == MCDR_FG_BACKEND_FSRFG || backendId == MCDR_FG_BACKEND_XESSFG;
}

// P300：状态**变化**打一行，周期性快照仍由 `[FG]` 那条负责。
//
// 为什么还要这一行：周期快照（600 帧一次）能说明"现在是什么"，说不清"什么时候变的、变了几次"。
// 验收 A6/A11 恰恰要看顺序（先 on 后 off、先回滚后重建），而两次快照之间发生的事在快照里只剩
// 差值。P296b 那个"每帧都在下发"就是靠数下发次数才发现的 —— 那一类问题需要有序事实。
//
// 只在变化时打，所以常态下一局只有几行（用户开关一次、回滚一次各一行），不占日志。
void logStateChange(const FgRegistry& s, const char* what, int32_t before, int32_t after) {
    if (before == after) {
        return;
    }
    dbgLogInfo("[FG] %s %d -> %d at present=%llu (picked=%d applied=%d chain=%d rebuilds=%d)",
               what, before, after, static_cast<unsigned long long>(s.presentId), s.picked, s.applied,
               s.chainMode, s.rebuilds);
}

// ---- DLSS-G 的 provider ----
//
// 行为与 P213 的直连路径**等价**，只是把「什么时候把值交下去」从 setMode 挪到了
// beforePresent：P296 起 setMode 只记录（它可能被 JVM 的设置线程调），真正的
// `mcdr_dlss_set_fg_mode` 在 Present 之前、且只在值变化时调一次。
class DlssGFrameGenProvider final : public IFrameGenProvider {
public:
    FgBackendId backend() const override { return FgBackendId::DlssG; }

    void fillCaps(mcdr_fg_caps& out) const override { fillDlssGCaps(out); }

    bool setMode(const mcdr_fg_dispatch& desc) override {
        // 只记录。P296：下发搬到 beforePresent —— 官方 ProgrammingGuideDLSS_G §6 明确
        // 「选项在**下一次 Present** 生效」，所以在 Present 之前写下去是**保守合法**的：
        // 这一帧的标签/选项已经由渲染线程的 `streamlineEvaluate()` 采集完毕，写下的值由
        // 下一帧的 evaluate 读走（P213 的两个原子量），生效点因此一定不早于「下一次
        // Present」。跨线程调 setMode 也不会落到不确定的那次 Present 上。
        const int32_t frames = desc.numFrames < 1 ? 1 : desc.numFrames;
        desiredMode_ = desc.mode;
        desiredFrames_ = frames;
        return true;
    }

    void beforePresent() override {
        // 注意：这里调的是 DLSS-G 实现单元的「状态写入」入口，不是 `slDLSSGSetOptions`
        // 本身 —— 那条调用留在 `streamlineEvaluate()` 里（P213 的既有顺序，tag 与选项都
        // 用同一个 frame token）。本函数只负责「把期望值交下去」这一步的时机。
        if (!applied_ || appliedMode_ != desiredMode_ || appliedFrames_ != desiredFrames_) {
            mcdr_dlss_set_fg_mode(desiredMode_, desiredFrames_);
            appliedMode_ = desiredMode_;
            appliedFrames_ = desiredFrames_;
            applied_ = true;
        }
        if (desiredMode_ == 0) (void)mcdr_dlss_invalidate_inputs();
    }

    void afterPresent() override {
        historyAccepted_ = mcdr_dlss_fg_after_present() == 1;
    }

    void discardFrameInputs() override { historyAccepted_ = false; }

    FgInputAcceptance tagFrameInputs(const FgFrameInputs& inputs) override {
        historyAccepted_ = false;
        mcdr_dlss_set_fg_mode(desiredMode_, desiredFrames_);
        mcdr_dlss_input input{};
        input.renderWidth = inputs.renderWidth;
        input.renderHeight = inputs.renderHeight;
        input.targetWidth = inputs.displayWidth;
        input.targetHeight = inputs.displayHeight;
        input.pInOutput = static_cast<mcdr_object*>(inputs.pColorOutput);
        input.pInDepth = static_cast<mcdr_object*>(inputs.pDepth);
        input.pInMotionVectors = static_cast<mcdr_object*>(inputs.pMotionVectors);
        mcdr_dlss_constants camera{};
        std::copy_n(inputs.currentViewProjection, 16, camera.viewProjection);
        std::copy_n(inputs.previousViewProjection, 16, camera.prevViewProjection);
        std::copy_n(inputs.projection, 16, camera.projection);
        camera.jitterOffsetX = inputs.jitterX;
        camera.jitterOffsetY = inputs.jitterY;
        camera.mvecScaleX = inputs.mvScaleX / inputs.renderWidth;
        camera.mvecScaleY = inputs.mvScaleY / inputs.renderHeight;
        camera.cameraPosX = inputs.cameraPosition[0];
        camera.cameraPosY = inputs.cameraPosition[1];
        camera.cameraPosZ = inputs.cameraPosition[2];
        camera.cameraRightX = inputs.cameraRight[0];
        camera.cameraRightY = inputs.cameraRight[1];
        camera.cameraRightZ = inputs.cameraRight[2];
        camera.cameraUpX = inputs.cameraUp[0];
        camera.cameraUpY = inputs.cameraUp[1];
        camera.cameraUpZ = inputs.cameraUp[2];
        camera.cameraFwdX = inputs.cameraForward[0];
        camera.cameraFwdY = inputs.cameraForward[1];
        camera.cameraFwdZ = inputs.cameraForward[2];
        camera.cameraNear = inputs.cameraNear;
        camera.cameraFar = inputs.cameraFar;
        camera.cameraFovRadians = inputs.cameraVerticalFov;
        camera.cameraAspectRatio = static_cast<float>(inputs.displayWidth) / inputs.displayHeight;
        camera.depthInverted = 1;
        camera.cameraMotionIncluded = 1;
        camera.reset = constants_.resetHistory != 0 ? 1u : 0u;
        auto* context = recordingContextForCommandList(inputs.cmdList);
        const auto transition = [](void* rawContext, void* rawResource, int32_t target) {
            transitionTo(static_cast<CommandContext*>(rawContext),
                static_cast<ID3D12Resource*>(rawResource), D3D12_RESOURCE_STATE_COMMON,
                static_cast<D3D12_RESOURCE_STATES>(target));
        };
        const int result = mcdr_dlss_submit_display_frame(inputs.cmdList, &input, &camera,
            static_cast<int32_t>(inputs.colorState), static_cast<int32_t>(inputs.depthState),
            static_cast<int32_t>(inputs.motionVectorState), inputs.pUi,
            static_cast<int32_t>(inputs.uiState), inputs.realFrameId,
            inputs.motionVectorsDilated ? 1u : 0u, inputs.frameTimeDeltaMs, transition, context);
        return result == 1 ? FgInputAcceptance::Accepted :
            result == 0 ? FgInputAcceptance::Skipped : FgInputAcceptance::Failed;
    }

    void setFrameConstants(const mcdr_fg_constants& constants) override { constants_ = constants; }

    bool historyAcceptedAfterPresent() const override { return historyAccepted_; }

    int32_t presented() const override { return mcdr_dlss_fg_presented(); }

    void onResize(int32_t displayWidth, int32_t displayHeight) override {
        // SL 的呈现路径自己处理尺寸（它拿得到 swapchain 的 desc），这里只记下来备诊断。
        static_cast<void>(displayWidth);
        static_cast<void>(displayHeight);
    }

    void shutdown() override {
        // P213e：**必须显式下发 eOff**，否则 SL 还在插帧，界面却已经说关了。
        // P213c：`mcdr_dlss_set_fg_mode(0, …)` 内部会把 presented 复位成 1。
        mcdr_dlss_set_fg_mode(0, desiredFrames_ < 1 ? 1 : desiredFrames_);
        (void)mcdr_dlss_invalidate_inputs();
        desiredMode_ = 0;
        appliedMode_ = 0;
        applied_ = false;
    }

private:
    // 期望值（setMode 写）与已下发值（beforePresent 写）分开，差一帧也看得出来。
    int32_t desiredMode_ = 0;
    int32_t desiredFrames_ = 1;
    int32_t appliedMode_ = 0;
    int32_t appliedFrames_ = 1;
    bool applied_ = false;
    bool historyAccepted_ = false;
    mcdr_fg_constants constants_{};
};

}  // namespace

// ---------------------------------------------------------------------------
// 能力表
// ---------------------------------------------------------------------------

void fillDlssGCaps(mcdr_fg_caps& out) {
    out.backend = MCDR_FG_BACKEND_DLSSG;

    const bool dlssReady = mcdr_dlss_available() != 0;
    const bool streamline = mcdr_dlss_streamline_requested() != 0;
    const bool reflexReady = mcdr_reflex_available() != 0;

    out.runtimeLoaded = dlssReady ? 1 : 0;
    out.deviceSupported = streamline ? 1 : 0;
    // P253 之后「实现是 STREAMLINE ⇒ 建链时一律升级代理工厂」，所以链兼容性不依赖 FG 开关。
    out.chainCompatible = streamline ? 1 : 0;
    // 帧生成依赖 Reflex：SDK 会因为 Reflex 不是 On 而静默禁用插帧（P213）。
    out.needsReflex = 1;
    out.needsUiResource = 1;
    out.needsHudless = 1;
    // SL 自己管被 tag 的资源状态（官方 ProgrammingGuideDLSS_G §5.2），我们不替它转。
    out.transitionsInputs = 0;
    // P225：自己算帧数并 clamp 到 1..5（slDLSSGGetState 的 numFramesToGenerateMax）。
    out.maxGeneratedFrames = 5;
    // chainOwner 由 `fgFillCaps` 在外面填（这里读注册表会和它的锁互相嵌套）。
    out.reserved = 0;
    // P298：DLSS-G 的低延迟方案就是 Reflex（而且不是"可选"—— FG 依赖它，见 needsReflex）。
    out.latencyProvider = MCDR_FG_LATENCY_REFLEX;

    const bool available = dlssReady && streamline && reflexReady;
    out.available = available ? 1 : 0;

    if (!dlssReady) {
        writeNote(out.note, sizeof(out.note), "DLSS runtime not loaded");
    } else if (!streamline) {
        writeNote(out.note, sizeof(out.note), "not on the Streamline path");
    } else if (!reflexReady) {
        // P298：必须说清"是 Reflex 挡住了"，不能只说 unavailable —— 否则人只会去查 FG 本身。
        // （note 只有 28 B，这句话必须压到 27 字符以内。）
        writeNote(out.note, sizeof(out.note), "needs Reflex, unavailable");
    } else if (!available) {
        writeNote(out.note, sizeof(out.note), "unavailable");
    } else {
        writeNote(out.note, sizeof(out.note), "DLSS-G ready");
    }
}

std::unique_ptr<IFrameGenProvider> makeDlssGProvider() {
    return std::make_unique<DlssGFrameGenProvider>();
}

// ---------------------------------------------------------------------------
// 注册表
// ---------------------------------------------------------------------------

int32_t fgPickBackend(int32_t backendId) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.preparedMode.has_value()) {
        s.lastReason = "FG backend selection is blocked by a pending preparation";
        return 0;
    }
    if (!validBackendId(backendId)) {
        s.lastReason = "unknown frame generation backend id";
        return 0;
    }
    // 纯记录：不建、不销毁。真要换后端时，下一次 fgEnsureProvider 会按 picked 重建。
    // P306w：链已经被某一家的逻辑接管时不允许改 `picked` —— 代理链的上下文绑在旧 provider
    // 身上，`picked` 一改，后面所有"按 picked 判断"的地方（重建策略、caps、状态文本）与真实
    // 链归属就对不上了；自测里"启动时是 FSR、tick 20 又 pick XeSS"正是这种状态，实测会在
    // 几秒后崩在 SDK 的线程里。产品语义：一次会话只用一个接管链的后端，换后端要重启。
    if (s.provider != nullptr && s.applied != backendId &&
        (s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP || s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP)) {
        s.lastReason = "restart to switch FG";
        return 0;
    }
    s.picked = backendId;
    s.forcedPick = true;
    return 1;
}

void fgFillCaps(int32_t backendId, mcdr_fg_caps& out) {
    // 清零 + 填 backend 由这里负责，实现只写自己的字段（与 srFillCaps 的分工一致）。
    out = mcdr_fg_caps{};
    out.backend = backendId;
    switch (backendId) {
        case MCDR_FG_BACKEND_DLSSG:
            fillDlssGCaps(out);
            break;
        case MCDR_FG_BACKEND_FSRFG:
            // P304：实现挪进 `src\advanced\d3d12_fsffg.cpp`（FFX 的类型只在那一个单元里）。
            // P298 那条「架构上有 Anti-Lag 2、运行时没有 DLL」的结论保留在那份实现里。
            fillFsrFgCaps(out);
            break;
        case MCDR_FG_BACKEND_XESSFG:
            // P304：实现挪进 `src\advanced\d3d12_xessfg.cpp`（XeSS-FG 的类型只在那一个单元里）。
            fillXessFgCaps(out);
            break;
        default:
            out.latencyProvider = MCDR_FG_LATENCY_NONE;
            writeNote(out.note, sizeof(out.note), "no frame generation backend");
            break;
    }
    // 链的归属是全局状态、不是某个后端的能力，所以统一在这里补。
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    out.chainOwner = s.chainOwner;
    // 期望 ≠ 生效时用 lastReason **替换** note：否则用户在视频设置里点了开关、实际什么都没发生，
    // 而 UI 只显示一句静态的 "ready"。note 只有 28 字节，所以理由串都写成短句。
    //
    // P306p：包装型后端还要多看一眼链 —— provider 建成（applied==picked）不等于链已经包好。
    // 重建失败（典型是独占全屏）时 provider 在、链没换，`picked==applied` 会让这里把失败理由
    // 咽掉，UI 继续显示 "ready"，用户点了开关却没有任何插帧。所以把"链家族是否已满足"也算进来。
    const bool wrapPicked = s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG;
    const bool chainSatisfied =
        !wrapPicked ||
        (s.picked == MCDR_FG_BACKEND_FSRFG && s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP) ||
        (s.picked == MCDR_FG_BACKEND_XESSFG && s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP);
    if (s.picked != MCDR_FG_BACKEND_NONE && !s.lastReason.empty() &&
        (!chainSatisfied || s.applied != s.picked)) {
        writeNote(out.note, sizeof(out.note), s.lastReason.c_str());
    }
}

// 调用方必须已持锁（`fgEnsureProvider` 与 `fgSetMode` 都会走到这里，别在这里再锁一次，
// std::mutex 不可重入）。
// Incoming wrappers are unwrapped only while their owner/lease is live.
ID3D12Resource* unwrapDx12Resource(void* object);
int32_t ensureProviderLocked(FgRegistry& s) {
    const int32_t target = s.picked;
    if (target == MCDR_FG_BACKEND_NONE) {
        s.lastReason = "no frame generation backend picked";
        return 0;
    }
    if (s.provider != nullptr && s.applied == target) {
        return 1;
    }
    if (s.frameStarted) {
        s.lastReason = "frame generation backend change is waiting for display frame completion";
        return 0;
    }
    // 链已经被另一家接管时**不允许热切换**。代理链的引用在 surface 手里：旧 provider 一销毁，
    // 到下一帧 acquire 之前那一次 Present 就会走一条已经没有上下文的链（实测 FFX 侧返回
    // `FFX_API_RETURN_ERROR_RUNTIME_ERROR` 0x3，或直接崩在 SDK 里）。
    // 产品语义因此定为：一次会话只用一个"接管交换链"的后端，换后端要重启游戏。
    // 下面那段先 unwrap 再 shutdown 的代码仍然保留 —— 它服务于"同一后端重建链"（resize）路径。
    if (s.provider != nullptr && s.applied != target &&
        (s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP || s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP)) {
        s.lastReason = "restart to switch FG";
        return 0;
    }
    // 换后端：先让旧 provider 自己收尾（它会显式下发 eOff）。**不等 GPU**：DLSS-G 的
    // shutdown 只写 atomic 与 SL 选项，不销毁设备资源；真要等的时候由
    // fgReleaseProviderWithWait 负责（P296 接 Present 之后才有意义）。
    if (s.provider != nullptr) {
        // P306：换一家"接管交换链"的后端之前必须先解包。旧 provider 的 SDK context
        // 绑在代理链上，不解包就 shutdown，代理链会留在 `IDXGISwapChain` 里；
        // 新后端再包一次 = 双重包装，实测 FFX 直接返回
        // `FFX_API_RETURN_ERROR_RUNTIME_ERROR` (0x3)。
        invalidateCameraLocked(s);
        if (!recordingsCompleteLocked(s) || !s.provider->waitForPreviousFrame() ||
            !s.provider->tryUnwrapSwapChain()) {
            s.rebuildPending = true;
            return 0;
        }
        s.provider->shutdown();
        s.provider.reset();
        s.applied = MCDR_FG_BACKEND_NONE;
        // 链的归属已经变了：让 surface 在下一帧首重建回应用原生链，新后端在那里再包。
        s.rebuildPending = true;
        s.chainMode = 0;
    }

    std::unique_ptr<IFrameGenProvider> made;
    switch (target) {
        case MCDR_FG_BACKEND_DLSSG:
            made = makeDlssGProvider();
            break;
        case MCDR_FG_BACKEND_FSRFG:
            made = makeFsrFgProvider();
            break;
        case MCDR_FG_BACKEND_XESSFG:
            made = makeXessFgProvider();
            break;
        default:
            s.lastReason = "unknown frame generation backend";
            return 0;
    }
    if (made == nullptr) {
        s.lastReason = "frame generation backend factory returned null";
        return 0;
    }

    // 先问一次能力：不可用就别把它当成生效后端（否则 UI 会以为已经开着）。
    mcdr_fg_caps caps = mcdr_fg_caps{};
    made->fillCaps(caps);
    if (caps.available == 0) {
        // P306：自测/诊断用 `fg-backend-*` 强制选定时允许越过 available —— 否则实验性
        // 后端连 provider 都建不出来，wrap 永远不触发。产品路径（非强制）仍然拒绝，
        // 这样配置/UI 里选不到未完成的后端，也就不会把玩家带进一个会 abort 的路径。
        //
        // P306r：拒绝必须留痕。以前这里只写 `lastReason`，用户报"开了没生效"时日志里
        // 一行都没有，只能靠猜。同一条原因只打一次（重试是每 30 帧一次）。
        if (!s.forcedPick) {
            if (s.lastReason != caps.note) {
                dbgLogWarn("provider %d unavailable: %s", target, caps.note);
            }
            s.lastReason = caps.note;
            return 0;
        }
        s.lastReason = caps.note;
    }

    s.applied = target;
    invalidateCameraLocked(s);
    s.provider = std::move(made);
    // P305al：JVM 在启动早期就把 UI 纹理与帧常量推过来了（那时还没有 provider，注册表
    // 只把它们存进 `uiTexture` / `constants`）。新建的 provider 是白纸，必须回放一次，
    // 否则 UI 合成永远拿不到纹理（实测 breadcrumb 里 `ui=null`），tag 出去的 HUDless
    // 帧也就没人合成 HUD。
    if (s.uiTexture != nullptr) {
        s.provider->setUiTexture(s.uiTexture.Get(), s.uiLease);
    }
    // P306ac：hudless 呈现模式也是"期望状态"，新建的 provider 默认是关的，必须回放。
    // 顺序在 `setUiTexture` 之后：先有纹理、再开模式，provider 才可能在同一帧里注册 UI。
    if (s.uiComposition != 0) {
        s.provider->setUiComposition(s.uiComposition);
    }
    // P306u：provider 可能是**事后**才建出来的 —— 开机那一帧帧生成运行库还没装载、或链重建
    // 那一帧才第一次真的需要它。注册表里记着用户的期望状态，这里必须补一次 `setMode`：
    // provider 是白纸，默认就是"关"，不补的话会一直停在关着的状态。
    //
    // 实测症状（2026-10-28 14:08，用户实机日志）：`app_frames=600 swapchain_presents=0
    // ratio=0.00 enabled=0 prepared=0 frame_id=1` —— 帧号停在 wrap 时那一次 Configure(off)
    // 的 1，说明 Configure(enabled) 从未下发；FFX 的 `interpolationEnabled` 也就一直是
    // false，`Present` 里根本不会走插帧分支（`FrameInterpolationSwapchainDX12.cpp:2169`）。
    if (s.desiredMode != 0) {
        mcdr_fg_dispatch replay{};
        replay.backend = target;
        replay.mode = s.desiredMode;
        replay.numFrames = s.desiredFrames;
        replay.reset = 1;
        s.provider->setMode(replay);
    }
    // 建成之后「建不出来」那条 warn 的资格要还回去，否则一次迟到的建成会让后续真故障静默。
    s.warnedNotUsable = false;
    return 1;
}

int32_t fgEnsureProvider(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return ensureProviderLocked(s);
}

int32_t fgSetMode(const mcdr_fg_dispatch& desc) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.preparedMode.has_value()) {
        s.lastReason = "FG configuration preparation is pending";
        return 0;
    }
    if (desc.backend != MCDR_FG_BACKEND_NONE) {
        if (!validBackendId(desc.backend)) {
            return -1;
        }
        // P304：picked 真的换了后端时，旧 provider 必须换掉 —— DLSS-G 走 SL 代理链、
        // FSR-FG / XeSS-FG 走应用原生链并被 SDK 包装，两家的资源契约与链要求都不同。
        // 这里只拆旧的、不建新的：新的由下面 `ensureProviderLocked` 建，链重建统一在帧首做。
        if (s.provider != nullptr && s.applied != desc.backend) {
            // A settings-thread call cannot destroy a context used by an open
            // recording or by the surface's SDK-owned swap chain.
            s.lastReason = "restart to switch FG";
            return 0;
        }
        // P305ab：显式指定优先。门面在 XeSS-FG/FSR-FG 的 available=0 时会把 backend 算成 DLSS-G，
        // 若让它无条件覆盖 picked，自测钩子强制选中的后端在 FG 打开的瞬间就被抹掉（P305aa）。
        if (!s.forcedPick) {
            s.picked = desc.backend;
        } else if (s.picked != desc.backend) {
            s.lastReason = "picked backend is forced; ignoring the facade's suggestion";
        }
    }
    if (desc.mode < 0 || desc.mode > 2) {
        return -1;
    }
    // 先把「用户要什么」记下来，再去试图满足它。P296 实测（2026-10-27 13:06）：启动早期
    // Reflex 还没解析出来、能力位是 0，如果这里直接失败并把期望值丢掉，整局都不会有人重试，
    // 用户存的「帧生成开」就等于没生效（日志里只有一行 warn，界面还把 FG 显示成开）。
    const int32_t beforeMode = s.desiredMode;
    const int32_t beforeChainMode = s.chainMode;
    s.desiredMode = desc.mode;
    s.resetPending = s.resetPending || desc.mode == 0 || desc.reset != 0;
    if (!s.frameStarted && s.resetPending) {
        invalidateCameraLocked(s);
        s.resetPending = false;
    }
    s.desiredFrames = desc.numFrames < 1 ? 1 : desc.numFrames;
    logStateChange(s, "desiredMode", beforeMode, s.desiredMode);
    // mode=0 是"关"：即使现在没有生效后端也必须成功，否则界面关不掉一个半建成的后端。
    if (desc.mode == 0 && s.provider == nullptr) {
        s.lastResult = 0;
        // 从没建过后端就等于"链一直按关的状态建"，不存在要重建的差异。
        s.chainMode = 0;
        s.rebuildPending = false;
        logStateChange(s, "chainMode", beforeChainMode, s.chainMode);
        return 1;
    }
    if (!s.frameStarted && s.provider == nullptr && ensureProviderLocked(s) == 0) {
        s.lastResult = 0;
        // 不挂重建请求：后端都还没建成，没有链要改。`fgBeforePresent` 会按 ensureTryAt 重试，
        // 成功的那一刻再挂重建（那时才真的要从"关的链"换成"开的链"）。
        s.ensureTryAt = s.appFrameId + ENSURE_RETRY_FRAMES;
        return 0;
    }
    const bool ok = s.frameStarted || (s.provider && s.provider->setMode(desc));
    s.lastResult = ok ? 1 : 0;
    if (!ok) {
        return s.lastResult;
    }
    // P305af：链**只在家族不符时**重建，开关本身不再是重建理由。
    //
    // 旧规则是「期望开/关 != 链建成时的开/关 ⇒ 重建」，对 DLSS-G（走 SL 代理工厂、链本身
    // 不换）只是多一次开销，对 XeSS-FG / FSR-FG 却是每次切开关都 destroy + create 一条
    // HWND 链。P305ae 实测（2026-10-27 21:12）：连续 4 次重建都成功，第 5 次
    // `CreateSwapChainForHwnd` 直接 0x887A0001（DXGI_ERROR_INVALID_CALL）——同一条 HWND
    // 上的旧链销毁是异步的，短时间内反复易主就踩到；用户真实操作里"关掉再开"就会撞上。
    //
    // 包装链在关掉帧生成之后**仍然是合法链**：SDK 支持整局保持包装，只把插帧关掉
    // （`SetEnabled(0)`，见 `xessfg` 的 provider），ResizeBuffers 也由代理链自己转给
    // 底层链。所以唯一必须重建的情形是：需要一个包装链而现在不是（首次打开），或者
    // 现在包着 A 家而后端换成了 B 家 / 不再需要包装（切 DLSS-G、切回应用原生链）。
    const bool wantOn = s.desiredMode != 0;
    const bool wantsWrap = s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG;
    const bool chainIsWrap = s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP ||
                             s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP;
    const bool chainFamilyMatches =
        (s.picked == MCDR_FG_BACKEND_FSRFG && s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP) ||
        (s.picked == MCDR_FG_BACKEND_XESSFG && s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP);
    const bool needWrap = wantOn && wantsWrap && (!chainIsWrap || !chainFamilyMatches);
    // 关掉帧生成时**不**解包（见上）；只有后端已经不是包装家族了才解包，否则包着的链会
    // 挡住 DLSS-G 的 SL 代理链 / 用户想要的"关到最原始状态"。
    const bool needUnwrap = chainIsWrap && !wantsWrap;
    if (needWrap || needUnwrap) {
        s.rebuildPending = true;
        s.lastReason.clear();
    }
    return s.lastResult;
}

int32_t fgPrepareMode(const mcdr_fg_dispatch& description, uint64_t* token) {
    if (token == nullptr) return MCDR_FG_PREPARE_INVALID_ARGUMENT;
    *token = 0;
    if (!waitForPendingPresents(2'000)) return MCDR_FG_PREPARE_NOT_READY;
    FgRegistry& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!validBackendId(description.backend) || description.mode < 0 ||
        description.mode > 2 || description.numFrames < 1 || description.numFrames > 5 ||
        (description.mode != 0 && (description.displayWidth <= 0 || description.displayHeight <= 0)))
        return MCDR_FG_PREPARE_INVALID_ARGUMENT;
    if (state.preparedMode.has_value() || state.frameStarted || !recordingsCompleteLocked(state)) {
        state.lastReason = "FG preparation requires a completed display frame";
        return MCDR_FG_PREPARE_NOT_READY;
    }
    if (state.provider != nullptr && !state.provider->waitForPreviousFrame()) {
        state.lastReason = "FG previous frame has not retired";
        return MCDR_FG_PREPARE_NOT_READY;
    }
    const bool wantsWrapper = description.backend == MCDR_FG_BACKEND_FSRFG ||
        description.backend == MCDR_FG_BACKEND_XESSFG;
    const bool matchingWrapper =
        (description.backend == MCDR_FG_BACKEND_FSRFG && state.chainOwner == MCDR_FG_CHAIN_FSR_WRAP) ||
        (description.backend == MCDR_FG_BACKEND_XESSFG && state.chainOwner == MCDR_FG_CHAIN_XESS_WRAP);
    if (description.mode != 0 &&
        ((wantsWrapper && !matchingWrapper) ||
            (state.provider != nullptr && state.applied != description.backend))) {
        state.lastReason = "restart to switch FG";
        return MCDR_FG_PREPARE_RESTART_REQUIRED;
    }
    FgRegistry::PreparedMode candidate{};
    if (description.mode != 0) {
        IFrameGenProvider* provider = state.provider.get();
        if (provider == nullptr) {
            std::unique_ptr<IFrameGenProvider> made;
            switch (description.backend) {
                case MCDR_FG_BACKEND_DLSSG: made = makeDlssGProvider(); break;
                case MCDR_FG_BACKEND_FSRFG: made = makeFsrFgProvider(); break;
                case MCDR_FG_BACKEND_XESSFG: made = makeXessFgProvider(); break;
                default: break;
            }
            if (made == nullptr) {
                state.lastReason = "FG SDK provider factory failed";
                return MCDR_FG_PREPARE_REJECTED;
            }
            mcdr_fg_caps available{};
            made->fillCaps(available);
            if (available.available == 0) {
                state.lastReason = available.note;
                return MCDR_FG_PREPARE_REJECTED;
            }
            candidate.provider = std::move(made);
            provider = candidate.provider.get();
        }
        mcdr_fg_caps capabilities{};
        provider->fillCaps(capabilities);
        if (capabilities.available == 0 ||
            (description.mode == 1 && description.numFrames > capabilities.maxGeneratedFrames)) {
            state.lastReason = "FG SDK resources or requested frame count are unavailable";
            return MCDR_FG_PREPARE_REJECTED;
        }
        if (description.backend == MCDR_FG_BACKEND_DLSSG) {
            uint32_t maxFrames = 0;
            const int32_t prepared = mcdr_dlss_prepare_fg(
                description.mode, description.numFrames,
                static_cast<uint32_t>(description.displayWidth),
                static_cast<uint32_t>(description.displayHeight),
                static_cast<uint32_t>(description.displayWidth),
                static_cast<uint32_t>(description.displayHeight), &maxFrames);
            if (prepared != MCDR_FG_PREPARE_READY) {
                state.lastReason = prepared == MCDR_FG_PREPARE_NOT_READY
                    ? "DLSS-G previous Present inputs are still in use"
                    : "DLSS-G SDK rejected the requested frame count, dynamic mode, or display extent";
                return prepared;
            }
        }
    }
    candidate.description = description;
    candidate.cameraGeneration = state.cameraGeneration;
    candidate.token = state.nextPreparationToken++;
    if (candidate.token == 0) candidate.token = state.nextPreparationToken++;
    *token = candidate.token;
    state.preparedMode = std::move(candidate);
    return MCDR_FG_PREPARE_READY;
}

int32_t fgCommitPreparedConfiguration(uint64_t token, uint64_t srToken, int32_t antiAliasing) {
    FgRegistry& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (antiAliasing < 0 || antiAliasing > 3 || !state.preparedMode.has_value() ||
        state.preparedMode->token != token) return -1;
    if (state.frameStarted || state.cameraGeneration != state.preparedMode->cameraGeneration ||
        !recordingsCompleteLocked(state)) {
        state.lastReason = "FG prepared configuration became stale";
        return 0;
    }
    const int32_t committed = srCommitPreparedProvider(srToken);
    if (committed != 1) return committed;
    const auto description = state.preparedMode->description;
    if (state.preparedMode->provider != nullptr) {
        state.provider = std::move(state.preparedMode->provider);
        state.applied = description.backend;
        state.chainOwner = MCDR_FG_CHAIN_APP_NATIVE;
    }
    state.preparedMode.reset();
    state.picked = description.mode == 0
        ? (state.provider != nullptr ? state.applied : MCDR_FG_BACKEND_NONE)
        : description.backend;
    state.desiredMode = description.mode;
    state.desiredFrames = description.numFrames;
    state.resetPending = state.resetPending || description.mode == 0 || description.reset != 0;
    (void)setAntiAliasing(antiAliasing == 3 ? 0 : antiAliasing);
    (void)setDlssEnabled(srActiveBackend() == MCDR_SR_BACKEND_DLSS ? 1 : 0);
    state.lastReason.clear();
    return 1;
}

int32_t fgAbortPreparedMode(uint64_t token) {
    FgRegistry& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.preparedMode.has_value() || state.preparedMode->token != token) return -1;
    state.preparedMode.reset();
    return 1;
}

bool fgChainRebuildPending(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.rebuildPending;
}

void fgNoteChainRebuildDone(int32_t ownerAfterRebuild, int32_t ok, const char* reason) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    invalidateCameraLocked(s);
    // 无论成功失败都撤销请求：失败时重挂请求会在每帧 acquire 里反复尝试同一次注定失败
    // 的重建（例如独占全屏），那是死循环；等下次用户改开关自然会再挂一次。
    s.rebuildPending = false;
    if (ok == 0) {
        if (reason != nullptr && reason[0] != '\0') {
            s.lastReason = reason;
        }
        return;
    }
    const int32_t beforeChainMode = s.chainMode;
    s.chainMode = s.desiredMode;
    ++s.rebuilds;
    if (ownerAfterRebuild >= MCDR_FG_CHAIN_APP_NATIVE && ownerAfterRebuild <= MCDR_FG_CHAIN_XESS_WRAP) {
        s.chainOwner = ownerAfterRebuild;
        gChainOwnerSnapshot.store(ownerAfterRebuild, std::memory_order_relaxed);
    }
    logStateChange(s, "chainMode", beforeChainMode, s.chainMode);
}

bool fgAbortWrapAfterFailure(const char* reason) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    invalidateCameraLocked(s);
    // P306af 实测（2026-10-28 19:51）：从 FSR 包链热切到 XeSS 时，`InitFromSwapChain` 会在
    // 同一个 HWND 上建它自己的链，而旧链刚被放开、DWM 还没交还 HWND，于是
    // `sdk[3] Failed to create swap chain: 0x887a0001`。wrap 失败之后应用链已经没了
    // （`wrapChainForFrameGen` 里 Detach 过），如果上层只是 return 错误，surface 就再也
    // present 不出去 —— 用户看到"游戏还在跑、画面不动"。
    //
    // 这里把状态退回"没有帧生成"：期望清零、链归属记回原生、provider 拆掉，再把原因写进
    // `lastReason`（界面读它）。调用方随后按"不需要包装"重建一条纯应用链。
    if (s.provider != nullptr) {
        if (!recordingsCompleteLocked(s) || !s.provider->waitForPreviousFrame() ||
            !s.provider->tryUnwrapSwapChain()) {
            s.lastReason = "FG teardown pending after wrap failure";
            s.rebuildPending = true;
            return false;
        }
        s.provider->shutdown();
        s.provider.reset();
    }
    s.applied = MCDR_FG_BACKEND_NONE;
    s.picked = MCDR_FG_BACKEND_NONE;
    s.chainOwner = MCDR_FG_CHAIN_APP_NATIVE;
    gChainOwnerSnapshot.store(MCDR_FG_CHAIN_APP_NATIVE, std::memory_order_relaxed);
    const int32_t beforeMode = s.desiredMode;
    s.desiredMode = 0;
    s.chainMode = 0;
    s.rebuildPending = false;
    s.lastResult = 0;
    if (reason != nullptr && reason[0] != '\0') {
        s.lastReason = reason;
    }
    logStateChange(s, "desiredMode", beforeMode, s.desiredMode);
    dbgLogWarn("%s", reason == nullptr ? "frame generation could not take the swap chain" : reason);
    return true;
}

int beginDisplayFrameRecording(void* context) {
    auto* recording = static_cast<CommandContext*>(context);
    if (recording && (!recording->listOpen || !recording->commandList || recording->submissionFailed.load()))
        return -1;
    if (!waitForPendingPresents(2000)) return -1;
    auto& s = registry();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        std::erase_if(s.recordings, recordingComplete);
        for (const auto& use : s.recordings) {
            if (!use.submitted && !use.recording.expired()) {
                s.lastReason = "previous display input recording has not been submitted or discarded";
                return -1;
            }
        }
        if (s.provider && !s.provider->waitForPreviousFrame()) return -1;
        if (s.frameStarted || s.pendingCamera.valid || s.resetPending) invalidateCameraLocked(s);
        oit::discardResolved(s.activeFrame.context, s.activeFrame.realFrameId, s.activeFrame.fenceValue);
        s.resetPending = false;
        s.frameStarted = false;
        s.providerStarted = false;
        s.activeFrame = {};
        s.activeIdentity = {};
        s.guiReady = false;
        if (s.provider == nullptr && s.desiredMode != 0 && s.appFrameId >= s.ensureTryAt) {
            s.ensureTryAt = s.appFrameId + ENSURE_RETRY_FRAMES;
            if (ensureProviderLocked(s) == 1) {
                const bool wantsWrap = s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG;
                const bool chainIsWrap = s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP ||
                                         s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP;
                if (wantsWrap && !chainIsWrap) s.rebuildPending = true;
                dbgLogInfo("frame generation became usable at display frame begin: backend=%d mode=%d frames=%d",
                    s.picked, s.desiredMode, s.desiredFrames);
            }
        }
    }
    if (auto* surface = getActiveSurface()) {
        if (auto prepared = prepareDisplayPresentChain(surface, context); !prepared) {
            dbgLogError("fg: display frame begin failed: %s", prepared.error().message.c_str());
            return -1;
        }
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    s.activeFrame = {++s.frameSerial, 0, s.cameraGeneration, context, 0};
    if (auto receipt = beginDisplayPresentReceipt(s.activeFrame); !receipt) {
        s.activeFrame = {};
        return -1;
    }
    s.frameStarted = true;
    if (s.provider) {
        mcdr_fg_dispatch mode{};
        mode.backend = s.applied;
        mode.mode = s.desiredMode;
        mode.numFrames = s.desiredFrames;
        if (!s.provider->setMode(mode)) {
            invalidateCameraLocked(s);
            s.frameStarted = false;
            return -1;
        }
        s.provider->frameStart();
        s.providerStarted = true;
    }
    return 1;
}

bool fgDisplayFrameStarted() {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.frameStarted;
}

int fgAttachWorldFrame(void* context, const mcdr_vanilla_frame_input& frame) {
    if (!fgDisplayFrameStarted() && beginDisplayFrameRecording(context) != 1) return -1;
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    if ((state.activeFrame.context && state.activeFrame.context != context) ||
        state.activeIdentity.realFrameId != 0) return -1;
    state.activeFrame.context = context;
    state.activeFrame.realFrameId = frame.identity.realFrameId;
    state.activeIdentity = frame.identity;
    state.activeSettings = frame.settings;
    state.activeHistoryGeneration = frame.historyGeneration;
    attachDisplayPresentIdentity(context, frame.identity, frame.historyGeneration);
    state.activeColorEncoding = frame.colorEncoding;
    return 1;
}

int fgMarkGuiReady(void* context, const mcdr_frame_identity& identity) {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.frameStarted || state.activeFrame.context != context || state.guiReady ||
        state.activeIdentity.worldEpoch != identity.worldEpoch ||
        state.activeIdentity.realFrameId != identity.realFrameId ||
        state.activeIdentity.configVersion != identity.configVersion ||
        state.activeIdentity.resourceVersion != identity.resourceVersion) return -1;
    state.guiReady = true;
    return 1;
}

bool fgDisplayFrameQueued(void* context) {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.frameStarted && state.activeFrame.context == context && state.activeFrame.fenceValue != 0;
}

FgPresentTicket fgCurrentDisplayFrameTicket() {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.frameStarted ? state.activeFrame : FgPresentTicket{};
}

FgPresentTicket fgDisplayFrameTicket(void* context) {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.frameStarted && state.activeFrame.context == context ? state.activeFrame : FgPresentTicket{};
}

bool fgDisplayFrameCurrent(const FgPresentTicket& ticket) {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    return ticket.serial != 0 && state.frameStarted && state.activeFrame == ticket;
}

FgPresentTicket fgDisplayGuideTicket(void* context) {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.frameStarted || !state.providerStarted || !state.provider || !state.guiReady ||
        state.activeFrame.context != context || state.activeFrame.fenceValue != 0 ||
        state.activeFrame.realFrameId == 0 || state.pendingCamera.valid ||
        state.activeFrame.providerGeneration != state.cameraGeneration) return {};
    return state.activeFrame;
}

void fgNoteRecordingSubmitted(CommandContext* context, UINT64 fenceValue) {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (auto& use : state.recordings)
        if (use.fence.Get() == context->fence.Get() && use.value == fenceValue) use.submitted = true;
}

mcdr::Result<FgPresentTicket> fgQueueDisplayFrame(CommandContext* context, UINT64 fenceValue) {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.frameStarted || (state.activeFrame.context && state.activeFrame.context != context))
        return FgPresentTicket{};
    auto ticket = state.activeFrame;
    ticket.context = context;
    ticket.fenceValue = fenceValue;
    if (state.pendingCamera.valid) {
        auto& receipt = state.pendingCamera;
        if (receipt.state != FgRegistry::TicketState::Accepted || receipt.context != context ||
            receipt.fenceValue != fenceValue || receipt.recording.expired() ||
            receipt.ticket.serial != ticket.serial || receipt.providerGeneration != state.cameraGeneration) {
            invalidateCameraLocked(state);
        } else {
            ticket = receipt.ticket;
            receipt.state = FgRegistry::TicketState::Queued;
        }
    }
    if (ticket.realFrameId != 0 && oit::queuePresented(context, ticket.realFrameId, ticket.fenceValue) < 0)
        return mcdr::err("display.history", "OIT history does not match the submitted display frame");
    state.activeFrame = ticket;
    queueDisplayPresentReceipt(ticket, MCDR_SUBMISSION_SUBMITTED);
    return ticket;
}

void fgAbortDisplayFrame(void* context) {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    if ((!state.frameStarted || state.activeFrame.context != context) &&
        (!state.pendingCamera.valid || state.pendingCamera.context != context)) return;
    if (state.activeFrame.context == context)
        oit::discardResolved(context, state.activeFrame.realFrameId, state.activeFrame.fenceValue);
    invalidateCameraLocked(state);
    state.frameStarted = false;
    state.providerStarted = false;
    state.activeFrame = {};
    state.activeIdentity = {};
    state.guiReady = false;
}

void fgAbortDisplayFrame(const FgPresentTicket& ticket) {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (ticket.serial == 0 || !state.frameStarted || state.activeFrame != ticket) return;
    oit::discardResolved(ticket.context, ticket.realFrameId, ticket.fenceValue);
    invalidateCameraLocked(state);
    state.frameStarted = false;
    state.providerStarted = false;
    state.activeFrame = {};
    state.activeIdentity = {};
    state.guiReady = false;
}

void fgBeforePresent(const FgPresentTicket& ticket) {
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.appFrameId;
    if (!state.frameStarted || ticket != state.activeFrame) return;
    if (state.provider && state.providerStarted) state.provider->beforePresent();
}

DisplayHistoryPublication fgAfterPresent(const FgPresentTicket& ticket, int32_t presentSucceeded) {
    DisplayHistoryPublication publication;
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // P297：只有真的 Present 过才进这里（`presentSurface` 在没 blit 时提前 return），
    // 所以 presentId 天然等于「真正提交的帧数」，FSR/XeSS-FG 要的就是这个语义。
    ++s.presentId;
    if (!s.frameStarted || ticket != s.activeFrame) {
        oit::discardResolved(ticket.context, ticket.realFrameId, ticket.fenceValue);
        return publication;
    }
    if (presentSucceeded != 0 && ticket.realFrameId != 0) {
        const int committed = oit::commitPresented(ticket.context, ticket.realFrameId, ticket.fenceValue);
        if (committed == 1) publication.flags |= MCDR_PRESENT_OIT_HISTORY_PUBLISHED;
        if (committed < 0) {
            oit::discardResolved(ticket.context, ticket.realFrameId, ticket.fenceValue);
            dbgLogError("OIT history: Present publication failed (%d)", committed);
        }
    } else {
        oit::discardResolved(ticket.context, ticket.realFrameId, ticket.fenceValue);
    }
    if (s.provider != nullptr && s.providerStarted) {
        s.provider->afterPresent();
    }
    if (presentSucceeded != 0)
        publication.outputFrameCount = s.provider ? static_cast<uint32_t>(std::max(1, s.provider->presented())) : 1u;
    if (presentSucceeded != 0 && s.provider && s.pendingCamera.valid &&
        s.pendingCamera.state == FgRegistry::TicketState::Queued &&
        s.pendingCamera.ticket == ticket &&
        s.pendingCamera.fence && s.pendingCamera.fenceValue == ticket.fenceValue &&
        (!s.pendingCamera.recording.expired() ||
         (s.pendingCamera.fence && s.pendingCamera.fence->GetCompletedValue() != UINT64_MAX &&
          s.pendingCamera.fence->GetCompletedValue() >= s.pendingCamera.fenceValue)) &&
        s.pendingCamera.providerGeneration == s.cameraGeneration &&
        s.provider->historyAcceptedAfterPresent()) {
        s.pendingCamera.state = FgRegistry::TicketState::Presented;
        s.committedCamera = s.pendingCamera;
        s.committedCamera.facts = {};
        s.pendingCamera = {};
        publication.flags |= MCDR_PRESENT_FG_CAMERA_PUBLISHED;
    } else {
        invalidateCameraLocked(s);
    }
    s.frameStarted = false;
    s.providerStarted = false;
    s.activeFrame = {};
    s.activeIdentity = {};
    s.guiReady = false;
    // 连续 ROLLBACK_FRAMES 帧都不行才动手；`fillCaps` 每帧问一次是可以的（这一层只读几个原子量
    // 与运行库句柄），将来某家的 caps 查询变贵就该在它自己的 `fillCaps` 里缓存，而不是在这里省。
    if (s.provider != nullptr && s.desiredMode != 0) {
        mcdr_fg_caps live{};
        s.provider->fillCaps(live);
        if (live.available != 0) {
            s.unavailableFrames = 0;
        } else if (++s.unavailableFrames >= ROLLBACK_FRAMES) {
            const std::string why = live.note[0] != '\0' ? std::string(live.note)
                                                         : std::string("backend reports unavailable");
            // Keep the provider and SDK context until surface teardown drains
            // recordings and SDK queues. Present return is not that boundary.
            s.provider->shutdown();
            invalidateCameraLocked(s);
            s.desiredMode = 0;
            s.desiredFrames = 1;
            s.lastReason = why;
            s.unavailableFrames = 0;
            // 这条 warn 已经把原因说清楚了，别让 300 帧后那条「建不出来」再重复报一次。
            s.warnedNotUsable = true;
            if (s.chainMode != 0) {
                s.rebuildPending = true;
            }
            dbgLogWarn("frame generation rolled back after %llu frames unavailable (%s); "
                       "chain rebuild queued so the swapchain returns to the native path",
                       static_cast<unsigned long long>(ROLLBACK_FRAMES), why.c_str());
        }
    }
    if (s.presentId - s.lastTrackLog >= 600) {
        s.lastTrackLog = s.presentId;
        dbgLogInfo("fg tracks: app=%llu backend=%llu present=%llu chainOwner=%d mode=%d/%d",
                   static_cast<unsigned long long>(s.appFrameId),
                   static_cast<unsigned long long>(s.backendFrameId),
                   static_cast<unsigned long long>(s.presentId), s.chainOwner, s.desiredMode, s.chainMode);
        // §3.8 的一半：能力位与生效状态一行打全，验收时只看这行就够判断"为什么没插帧"。
        // 为什么没有 cost（CPU 耗时）：这两点在 Present 两侧，中间的 `Present()` 会等 vsync，
        // 测出来的差值是被呈现节流主导的，不是插帧开销。要测真开销得走 GPU timestamp（未做）。
        mcdr_fg_caps caps{};
        if (s.provider != nullptr) {
            s.provider->fillCaps(caps);
        } else if (s.picked == MCDR_FG_BACKEND_FSRFG) {
            fillFsrFgCaps(caps);
        } else if (s.picked == MCDR_FG_BACKEND_XESSFG) {
            fillXessFgCaps(caps);
        } else {
            // P296 冒烟实测发现的缺口（2026-10-27 13:00）：帧生成关着的会话里 provider 从来没建过，
            // 于是这一行把 caps 全打成 0（`backend=0 available=0 maxGen=0`），恰好是最需要它说人话的时候
            // 什么都看不出来。改成"没 provider 也回答如果现在开会是什么结果" —— 这几个 fillXxxCaps 都是
            // 自由函数、不碰注册表、不持锁（本函数已持 s.mutex，调 `fgFillCaps` 会死锁）。
            fillDlssGCaps(caps);
        }
        caps.chainOwner = s.chainOwner;
        dbgLogInfo("[FG] backend=%d available=%d runtime=%d device=%d chainCompat=%d needsReflex=%d "
                   "maxGen=%d latency=%d transitions=%d chain=%d rebuilds=%d active=%d note=\"%s\"",
                   caps.backend, caps.available, caps.runtimeLoaded, caps.deviceSupported,
                   caps.chainCompatible, caps.needsReflex, caps.maxGeneratedFrames,
                   caps.latencyProvider, caps.transitionsInputs, s.chainOwner, s.rebuilds,
                   s.provider != nullptr ? 1 : 0, caps.note);
    }
    // P296 实测缺口（2026-10-27 13:06）：期望是"开"、可后端一直建不出来时，用户看到的只是
    // "没有插帧" —— 给它一次说人话的机会。等 300 帧（60 fps 下约 5 s）再说，是因为启动顺序
    // 造成的短暂不可用（Reflex 还没解析、运行库晚于插件加载）是**正常现象**，`fgBeforePresent`
    // 会每 ENSURE_RETRY_FRAMES 重试一次，这时候报警只会制造假故障（JVM 侧原来那条 warn 就是
    // 这么来的）。只在第一次给一个 warn，之后仍由 `[FG]` 行的 available/active/note 持续暴露。
    if (s.desiredMode != 0 && s.provider == nullptr && !s.warnedNotUsable && s.presentId > USABLE_GRACE_FRAMES) {
        s.warnedNotUsable = true;
        mcdr_fg_caps caps{};
        if (s.picked == MCDR_FG_BACKEND_FSRFG) {
            fillFsrFgCaps(caps);
        } else if (s.picked == MCDR_FG_BACKEND_XESSFG) {
            fillXessFgCaps(caps);
        } else {
            fillDlssGCaps(caps);
        }
        dbgLogWarn("frame generation requested (mode=%d frames=%d) but no provider after %llu frames: %s",
                   s.desiredMode, s.desiredFrames, static_cast<unsigned long long>(s.presentId),
                   s.lastReason.empty() ? caps.note : s.lastReason.c_str());
    }
    return publication;
}

void fgNoteBackendFrame(uint64_t backendFrameId) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.backendFrameId = backendFrameId;
}

int32_t fgPresented(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.provider != nullptr ? s.provider->presented() : 1;
}

void fgOnResize(int32_t displayWidth, int32_t displayHeight) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    invalidateCameraLocked(s);
    if (s.provider != nullptr) {
        s.provider->onResize(displayWidth, displayHeight);
    }
}

bool fgReleaseProviderWithWait(CommandContext* ctx) {
    invalidateFrameGenerationHistory(1);
    if (ctx != nullptr) {
        if (ctx->listOpen || ctx->submissionFailed.load()) return false;
        if (ctx->fenceValue != 0) {
            auto waited = waitForFenceValue(ctx, ctx->fenceValue, 5'000'000'000ULL);
            if (!waited) return false;
        }
    }
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.provider != nullptr) {
        // P306y：必须先让 provider 自己解包（包装型后端在 unwrap 里销毁自己的 SDK context），
        // 再 shutdown。只调 shutdown 会把 xefg/ffx 的 context 泄漏在进程里 —— 下一个 provider
        // 再 `CreateContext` 时 SDK 侧还留着上一条链的 latency-reduction/swapchain 状态，
        // 表现为 `SetEnabled(1)` 返回成功、`GetLastPresentStatus` 却一直报 disabled（实测 1:1 出帧）。
        if (!recordingsCompleteLocked(s) || !s.provider->waitForPreviousFrame() ||
            !s.provider->tryUnwrapSwapChain()) return false;
        s.provider->shutdown();
        s.provider.reset();
    }
    s.applied = MCDR_FG_BACKEND_NONE;
    s.uiTexture.Reset();
    s.uiLease.reset();
    s.hasConstants = false;
    // P306x：**不要清 `picked` / `desiredMode`**。这个入口的语义是"surface/设备要重建了"，
    // 不是"用户把帧生成关掉了"。以前在这里把 picked 清成 NONE，后果是：窗口重建（启动早期
    // 常见）之后 FG 永久消失，而界面上还显示着开 —— 实测日志里启动时 XeSS 已经包上了链，
    // 第二次 `configureSurface` 之后 provider 被 release，picked 变 NONE，新链再也没被包过，
    // 而 `chainOwner` 还停在 XESS_WRAP，于是 `[FG] ... chainOwner=3` 看着像好的、实际 1:1 出帧。
    // 保留意图 + 把链归属复位成"应用原生"，下一帧 `configureSurface` 会按 picked 重新建、重新包。
    s.chainMode = 0;
    s.chainOwner = MCDR_FG_CHAIN_APP_NATIVE;
    gChainOwnerSnapshot.store(MCDR_FG_CHAIN_APP_NATIVE, std::memory_order_relaxed);
    s.rebuildPending = s.desiredMode != 0 &&
                       (s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG);
    return true;
}

void fgNoteChainOwner(int32_t owner) {
    if (owner < MCDR_FG_CHAIN_APP_NATIVE || owner > MCDR_FG_CHAIN_XESS_WRAP) {
        return;
    }
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.chainOwner = owner;
    gChainOwnerSnapshot.store(owner, std::memory_order_relaxed);
}

int32_t fgChainOwner(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.chainOwner;
}

int32_t fgChainOwnerFast(void) {
    // 只读原子镜像：调用点可能在已持锁的临界区里（见 gChainOwnerSnapshot 的注释）。
    return gChainOwnerSnapshot.load(std::memory_order_relaxed);
}

int32_t fgPickedBackend(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.picked;
}

bool fgWantsAppNativeChain(void) {
    // 包装型后端（FSR-FG / XeSS-FG）只能吃应用原生链：SL 的代理工厂把链的 `Present` 槽
    // 留给自己，SDK 再包装一层就会与 SL 的钩子互相顶掉（两种集成方式互斥，见
    // `d3d12_surface.cpp:72-78`）。所以 pick 了这两家就要求 factory 不升级。
    const int32_t picked = fgPickedBackend();
    return picked == MCDR_FG_BACKEND_FSRFG || picked == MCDR_FG_BACKEND_XESSFG;
}

std::string fgStatusText(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    const char* picked = "none";
    switch (s.picked) {
        case MCDR_FG_BACKEND_DLSSG:
            picked = "dlssg";
            break;
        case MCDR_FG_BACKEND_FSRFG:
            picked = "fsr-fg";
            break;
        case MCDR_FG_BACKEND_XESSFG:
            picked = "xess-fg";
            break;
        default:
            break;
    }
    const char* applied = "none";
    switch (s.applied) {
        case MCDR_FG_BACKEND_DLSSG:
            applied = "dlssg";
            break;
        case MCDR_FG_BACKEND_FSRFG:
            applied = "fsr-fg";
            break;
        case MCDR_FG_BACKEND_XESSFG:
            applied = "xess-fg";
            break;
        default:
            break;
    }
    // P304：链的归属现在有四种，"sl-proxy / app-native" 二分会把包装型后端说成 app-native
    // 而掩盖"谁在包装这条链"这个事实（重建时要先 unwrap）。
    const char* chain = "app-native";
    switch (s.chainOwner) {
        case MCDR_FG_CHAIN_SL_PROXY:
            chain = "sl-proxy";
            break;
        case MCDR_FG_CHAIN_FSR_WRAP:
            chain = "fsr-wrap";
            break;
        case MCDR_FG_CHAIN_XESS_WRAP:
            chain = "xess-wrap";
            break;
        default:
            break;
    }

    std::string text = "[fg] picked=";
    text += picked;
    text += " applied=";
    text += applied;
    text += " chain=";
    text += chain;
    // P296：期望 / 链建成时的 mode / 重建次数 —— 「开关到底生效了没有」只能看这三个数。
    text += " mode=";
    text += std::to_string(s.desiredMode);
    text += "/";
    text += std::to_string(s.chainMode);
    text += " rebuilds=";
    text += std::to_string(s.rebuilds);
    if (s.rebuildPending) {
        text += " rebuildPending=1";
    }
    // P297：三轨帧号。验收时看的就是这三个数是否严格 +1。
    text += " tracks=";
    text += std::to_string(s.appFrameId);
    text += "/";
    text += std::to_string(s.backendFrameId);
    text += "/";
    text += std::to_string(s.presentId);
    // P306s：provider 自报的“每次 Present 实际呈现了几帧”。1 = 没插帧（链路可能接上了但
    // 生成没发生），2 = 2x。这是唯一能区分“接上了”与“真在插帧”的数字，设置页直接可见，
    // 比 F3 的 `presented`（历史上只看 DLSS-G 计数器）可靠。
    if (s.provider != nullptr) {
        text += " presented=";
        text += std::to_string(s.provider->presented());
    }
    // P298：低延迟方案。括号里那句是**现实**，别删 —— 免得有人看到 antilag2/xell 就以为已经支持。
    text += " latency=";
    switch (s.picked) {
        case MCDR_FG_BACKEND_DLSSG:
            text += "reflex";
            break;
        case MCDR_FG_BACKEND_FSRFG:
            text += "antilag2(no-dll)";
            break;
        case MCDR_FG_BACKEND_XESSFG:
            // P305ah 起 XeLL 的 marker/sleep 已接（SetLatencyReduction + 帧号对齐 presentId），
            // 不再是“没接”。这句曾写 xell(not-wired)，是旧状态，别改回去。
            text += "xell";
            break;
        default:
            text += "none";
            break;
    }
    if (!s.lastReason.empty()) {
        text += " reason=";
        text += s.lastReason;
    }
    return text;
}

int32_t fgStatePacked(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // 位布局是契约，改它必须同步 `include\mcdr_abi.h` 的声明注释与 `MCDR_ABI.kt` 的读取方式。
    const int32_t desired = s.desiredMode & 0xFF;
    const int32_t chain = s.chainMode & 0xFF;
    const int32_t active = s.provider != nullptr ? 1 : 0;
    const int32_t rebuilds = s.rebuilds > 255 ? 255 : (s.rebuilds < 0 ? 0 : s.rebuilds);
    return desired | (chain << 8) | (active << 16) | (rebuilds << 24);
}

// P305：把新建好的链交给包装型 provider。**只在 picked 是包装型后端时**动手 —— 否则返回 0，
// 调用方继续用原链（这就是"零行为变化"的保证：DLSS-G 路径永远走不到这里）。
int32_t fgWrapPresentChain(void* device, void* queue, void* appChain, int32_t width, int32_t height, int32_t format,
                           void** outPresentChain) {
    if (outPresentChain != nullptr) {
        *outPresentChain = nullptr;
    }
    if (device == nullptr || queue == nullptr || appChain == nullptr || outPresentChain == nullptr) {
        return 0;
    }
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // P306：FSR-FG 与 XeSS-FG 都是"接管应用原生链"的后端，两家都走这里；注册表的
    // "同一时刻只有一个 provider"约束保证它们不会同时抢同一条链。
    if (s.picked != MCDR_FG_BACKEND_XESSFG && s.picked != MCDR_FG_BACKEND_FSRFG) {
        return 0;
    }
    // P306：FG 关闭时**不要**接管交换链。包装链在关闭态没有任何收益，但会让"换一家
    // 后端"永远失败 —— 旧 provider 的代理链还挂在 `s->swapChain` 上，新后端再包一次
    // 就是双重包装（实测 `CreateContext(swap) rc=0x3`，随后整条 FG 静默失效）。
    // 打开 FG 的那一帧 `fgSetMode` 会置 `rebuildPending`，链在那里重建并包装。
    if (s.desiredMode == 0) {
        return 0;
    }
    if (ensureProviderLocked(s) != 1 || s.provider == nullptr) {
        return 0;
    }
    return s.provider->wrapSwapChain(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue),
                                     static_cast<IDXGISwapChain*>(appChain), width, height,
                                     static_cast<DXGI_FORMAT>(format),
                                     reinterpret_cast<IDXGISwapChain**>(outPresentChain));
}

// 链要销毁/重建之前必须先解包：provider 的 context 绑在那条链上（`InitFromSwapChain` 的行头）。
bool fgUnwrapPresentChain(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    invalidateCameraLocked(s);
    if (s.provider != nullptr) {
        if (!recordingsCompleteLocked(s) || !s.provider->waitForPreviousFrame() ||
            !s.provider->tryUnwrapSwapChain()) return false;
    }
    return true;
}
ID3D12Resource* unwrapDx12Resource(void* object) {
    // P306m：JVM 交过来的是 `Dx12Object*`（`D3D12GpuTexture.handle()` 的值），不是
    // `ID3D12Resource*`。之前把包装对象直接当资源传给 `ffxApiGetResourceDX12`，它读
    // `GetDesc()` 时崩在 0x50（hs_err 指向 `ffxApiGetResourceDX12+0x4f` → `applyFrameLocked`）。
    // Caller must already have acquired a wrapper lease while its owner is live.
    if (object == nullptr) {
        return nullptr;
    }
    auto* wrapper = static_cast<Dx12Object*>(object);
    return wrapper != nullptr ? wrapper->resource.Get() : nullptr;
}

void fgSetUiTexture(void* object) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // Acquire while the caller still owns the handle. No stored raw wrapper,
    // no SEH-based attempt to resurrect an already-freed object.
    auto lease = retainObjectLease(object);
    ComPtr<ID3D12Resource> resource = unwrapDx12Resource(object);
    if (s.provider != nullptr) {
        s.provider->setUiTexture(resource.Get(), lease);
    }
    s.uiTexture = std::move(resource);
    s.uiLease = std::move(lease);
}

// P306ac：hudless 呈现开关。与 `uiTexture` 一样属于"期望状态"：provider 可能还没建出来
// （启动早期 / 链重建那一帧），所以先记在注册表里，建出来时由 `ensureProviderLocked` 回放。
void fgSetUiComposition(int32_t enabled) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    const int32_t value = enabled != 0 ? 1 : 0;
    if (s.uiComposition == value) {
        return;
    }
    s.uiComposition = value;
    if (s.provider != nullptr) {
        s.provider->setUiComposition(value);
    }
    dbgLogWarn("fg: hudless ui composition %s (backend=%d)", value != 0 ? "on" : "off", s.applied);
}

int32_t fgUiCompositionReady(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    const auto& receipt = s.pendingCamera;
    const bool receiptReady = s.frameStarted && s.providerStarted && s.guiReady && s.provider &&
        s.uiComposition != 0 && s.activeFrame.context != nullptr && s.activeFrame.serial != 0 &&
        s.activeFrame.providerGeneration == s.cameraGeneration && receipt.valid &&
        (receipt.state == FgRegistry::TicketState::Accepted || receipt.state == FgRegistry::TicketState::Queued) &&
        receipt.ticket.serial == s.activeFrame.serial && receipt.context == s.activeFrame.context &&
        receipt.ticket.context == s.activeFrame.context && receipt.providerGeneration == s.cameraGeneration &&
        receipt.ticket.providerGeneration == s.cameraGeneration &&
        receipt.identity.worldEpoch == s.activeIdentity.worldEpoch &&
        receipt.identity.realFrameId == s.activeIdentity.realFrameId &&
        receipt.identity.configVersion == s.activeIdentity.configVersion &&
        receipt.identity.resourceVersion == s.activeIdentity.resourceVersion &&
        receipt.ticket.realFrameId == s.activeIdentity.realFrameId &&
        receipt.historyGeneration == s.activeHistoryGeneration;
    if (receiptReady && s.provider->uiCompositionReady()) return 1;
    s.uiComposition = 0;
    if (s.provider) s.provider->setUiComposition(0);
    return 0;
}

void invalidateFrameGenerationHistory(uint32_t reason) {
    static_cast<void>(reason);
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    invalidateCameraLocked(state);
}

bool displayFrameMatchesLocked(const FgRegistry& state, void* context,
    const mcdr_display_frame_input& frame) {
    return state.frameStarted && state.guiReady && state.activeFrame.context == context &&
        state.activeIdentity.worldEpoch == frame.identity.worldEpoch &&
        state.activeIdentity.realFrameId == frame.identity.realFrameId &&
        state.activeIdentity.configVersion == frame.identity.configVersion &&
        state.activeIdentity.resourceVersion == frame.identity.resourceVersion &&
        state.activeSettings.frameGenerationBackend == frame.frameGenerationBackend &&
        state.activeSettings.frameGenerationMode == frame.frameGenerationMode &&
        state.activeSettings.frameGenerationFrames == frame.frameGenerationFrames &&
        state.activeHistoryGeneration == frame.historyGeneration &&
        state.activeColorEncoding == frame.colorEncoding;
}

int checkDisplayFrameGenerationLocked(const FgRegistry& state, void* context,
    const mcdr_display_frame_input& frame) {
    if (frame.postCount > 64 || !std::isfinite(frame.frameDeltaMs) || frame.frameDeltaMs < 0 ||
        (frame.postCount != 0 && (!frame.posts || frame.postStride != sizeof(mcdr_display_post_input)))) {
        return -1;
    }
    if (!displayFrameMatchesLocked(state, context, frame) || state.pendingCamera.valid) return -1;
    if (frame.frameGenerationMode == 0 || !state.providerStarted) {
        return 0;
    }
    if (!state.provider || frame.frameGenerationBackend != static_cast<uint32_t>(state.applied)) {
        return 0;
    }
    if ((frame.flags & 3u) != 1u || frame.frameDeltaMs == 0) {
        return 0;
    }
    for (uint32_t index = 0; index < frame.postCount; ++index) {
        if (frame.posts[index].kind == 2) {
            return 0;
        }
    }
    return 1;
}

int checkDisplayFramePreparation(void* context, const void* frameCopy, uint32_t frameBytes) {
    if (!context || !frameCopy || frameBytes != sizeof(mcdr_display_frame_input)) return -1;
    mcdr_display_frame_input frame{};
    std::memcpy(&frame, frameCopy, sizeof(frame));
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    return checkDisplayFrameGenerationLocked(state, context, frame);
}

int skipDisplayFrameGeneration(void* context, const void* frameCopy, uint32_t frameBytes) {
    if (!context || !frameCopy || frameBytes != sizeof(mcdr_display_frame_input)) return -1;
    mcdr_display_frame_input frame{};
    std::memcpy(&frame, frameCopy, sizeof(frame));
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!displayFrameMatchesLocked(state, context, frame) || state.pendingCamera.valid) return -1;
    return invalidateCameraLocked(state) ? 0 : -1;
}

FgInputAcceptance fgTagFrameInputs(FgRegistry& state, FgRegistry::CameraReceipt& receipt,
    const FgFrameInputs& inputs) {
    if (!inputs.cmdList) return FgInputAcceptance::Failed;
    auto* ctx = static_cast<CommandContext*>(recordingContextForCommandList(inputs.cmdList));
    if (!ctx || !ctx->listOpen || ctx->submissionFailed.load()) {
        invalidateCameraLocked(state);
        return FgInputAcceptance::Failed;
    }
    struct RestoreHeap {
        CommandContext* context;
        ~RestoreHeap() { restoreDrawHeap(context); }
    } restore{ctx};
    if (!state.provider || !receipt.valid || receipt.context != ctx ||
        receipt.providerGeneration != state.cameraGeneration ||
        receipt.identity.realFrameId != inputs.realFrameId) {
        invalidateCameraLocked(state);
        return FgInputAcceptance::Failed;
    }
    struct InputLease {
        std::shared_ptr<void> wrappers;
        ComPtr<ID3D12Resource> color, depth, motion, ui;
    };
    auto lease = std::make_shared<InputLease>();
    lease->wrappers = inputs.resourceLease;
    if (!lease->wrappers) {
        invalidateCameraLocked(state);
        return FgInputAcceptance::Failed;
    }
    lease->color = static_cast<ID3D12Resource*>(inputs.pColorOutput);
    lease->depth = static_cast<ID3D12Resource*>(inputs.pDepth);
    lease->motion = static_cast<ID3D12Resource*>(inputs.pMotionVectors);
    lease->ui = static_cast<ID3D12Resource*>(inputs.pUi);
    ctx->recordingLeases[ctx->currentListSlot].push_back(lease);
    auto recording = std::make_shared<int>(0);
    ctx->recordingLeases[ctx->currentListSlot].push_back(recording);
    std::erase_if(state.recordings, recordingComplete);
    state.recordings.push_back({ctx->fence, ctx->fenceValue + 1, recording});
    FgFrameInputs retained = inputs;
    retained.resourceLease = lease;
    const auto normalize = [ctx](void* raw, D3D12_RESOURCE_STATES initial) {
        const auto current = static_cast<D3D12_RESOURCE_STATES>(
            resourceSubresourceState(ctx, raw, 0, initial));
        transitionTo(ctx, static_cast<ID3D12Resource*>(raw), initial, current);
        return current;
    };
    retained.colorState = normalize(retained.pColorOutput, retained.colorState);
    retained.depthState = normalize(retained.pDepth, retained.depthState);
    retained.motionVectorState = normalize(retained.pMotionVectors, retained.motionVectorState);
    retained.uiState = normalize(retained.pUi, retained.uiState);
    if (ctx->submissionFailed.load()) {
        invalidateCameraLocked(state);
        return FgInputAcceptance::Failed;
    }
    state.provider->setUiTexture(retained.pUi, lease);
    state.uiTexture = lease->ui;
    state.uiLease = lease;
    state.provider->setFrameConstants(state.constants);
    const auto acceptance = state.provider->tagFrameInputs(retained);
    if (acceptance != FgInputAcceptance::Accepted) {
        invalidateCameraLocked(state);
        return acceptance;
    }
    receipt.state = FgRegistry::TicketState::Accepted;
    receipt.recording = recording;
    receipt.fence = ctx->fence;
    receipt.fenceValue = ctx->fenceValue + 1;
    receipt.facts = retained;
    return FgInputAcceptance::Accepted;
}

int submitDisplayFrameGeneration(void* context, const void* frameCopy, uint32_t frameBytes,
    const FgFrameInputs& guides) {
    if (!context || !frameCopy || frameBytes != sizeof(mcdr_display_frame_input)) return -1;
    mcdr_display_frame_input frame{};
    std::memcpy(&frame, frameCopy, sizeof(frame));
    auto* ctx = static_cast<CommandContext*>(context);
    FgFrameInputs inputs = guides;
    {
        auto& state = registry();
        std::lock_guard<std::mutex> lock(state.mutex);
        const int readiness = checkDisplayFrameGenerationLocked(state, context, frame);
        if (readiness != 1) {
            invalidateCameraLocked(state);
            return readiness;
        }
        if (!ctx->listOpen || ctx->submissionFailed.load() || guides.cmdList != ctx->commandList ||
            !guides.resourceLease || !guides.pColorOutput || !guides.pDepth ||
            !guides.pMotionVectors || !guides.pUi || frame.width == 0 || frame.height == 0 ||
            (frame.current.flags & 1u) == 0 || !std::isfinite(frame.current.nearPlane) ||
            !std::isfinite(frame.current.farPlane) || !std::isfinite(frame.current.verticalFovRadians) ||
            !(frame.current.nearPlane > 0) || !(frame.current.farPlane > frame.current.nearPlane) ||
            !(frame.current.verticalFovRadians > 0) || !(frame.current.verticalFovRadians < 3.141593f) ||
            !std::isfinite(frame.frameDeltaMs) || !(frame.frameDeltaMs > 0) ||
            !std::isfinite(frame.viewSpaceToMeters) || !(frame.viewSpaceToMeters > 0) ||
            guides.renderWidth != static_cast<int32_t>(frame.width) ||
            guides.renderHeight != static_cast<int32_t>(frame.height)) {
            invalidateCameraLocked(state);
            return -1;
        }
        const auto& committed = state.committedCamera;
        const bool compatible = frame.resetReasons == 0 && (frame.flags & 1u) != 0 &&
            committed.valid && committed.providerGeneration == state.cameraGeneration &&
            committed.identity.worldEpoch == frame.identity.worldEpoch &&
            committed.identity.configVersion == frame.identity.configVersion &&
            committed.identity.resourceVersion == frame.identity.resourceVersion &&
            committed.identity.realFrameId == frame.previousRealFrameId &&
            committed.historyGeneration == frame.historyGeneration;
        FgRegistry::CameraReceipt candidate{};
        candidate.ticket = {state.activeFrame.serial, frame.identity.realFrameId, state.cameraGeneration,
            ctx, ctx->fenceValue + 1};
        candidate.identity = frame.identity;
        candidate.historyGeneration = frame.historyGeneration;
        candidate.providerGeneration = state.cameraGeneration;
        candidate.context = ctx;
        candidate.valid = true;
        float currentView[16], previousView[16];
        std::copy_n(frame.current.viewRotation, 16, currentView);
        std::copy_n(compatible ? frame.previous.viewRotation : frame.current.viewRotation, 16, previousView);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            candidate.anchor[axis] = compatible ? committed.anchor[axis] : frame.current.origin[axis];
            inputs.cameraPosition[axis] = static_cast<float>(frame.current.origin[axis] - candidate.anchor[axis]);
            inputs.cameraRight[axis] = frame.current.viewRotation[axis * 4];
            inputs.cameraUp[axis] = frame.current.viewRotation[axis * 4 + 1];
            inputs.cameraForward[axis] = -frame.current.viewRotation[axis * 4 + 2];
        }
        const auto& previous = compatible ? frame.previous : frame.current;
        for (uint32_t row = 0; row < 3; ++row) {
            double currentTranslation = 0, previousTranslation = 0;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                currentTranslation -= frame.current.viewRotation[axis * 4 + row] *
                    (frame.current.origin[axis] - candidate.anchor[axis]);
                previousTranslation -= previous.viewRotation[axis * 4 + row] *
                    (previous.origin[axis] - candidate.anchor[axis]);
            }
            currentView[12 + row] = static_cast<float>(currentTranslation);
            previousView[12 + row] = static_cast<float>(previousTranslation);
        }
        const auto project = [](const float* projection, const float* view, float* output) {
            for (uint32_t column = 0; column < 4; ++column)
                for (uint32_t row = 0; row < 4; ++row) {
                    float value = 0;
                    for (uint32_t inner = 0; inner < 4; ++inner)
                        value += projection[inner * 4 + row] * view[column * 4 + inner];
                    output[column * 4 + row] = value;
                }
        };
        project(frame.current.projection, currentView, inputs.currentViewProjection);
        project(previous.projection, previousView, inputs.previousViewProjection);
        std::copy_n(frame.current.projection, 16, inputs.projection);
        inputs.displayWidth = static_cast<int32_t>(frame.width);
        inputs.displayHeight = static_cast<int32_t>(frame.height);
        inputs.cameraNear = frame.current.nearPlane;
        inputs.cameraFar = frame.current.farPlane;
        inputs.cameraVerticalFov = frame.current.verticalFovRadians;
        inputs.frameTimeDeltaMs = frame.frameDeltaMs;
        inputs.viewSpaceToMeters = frame.viewSpaceToMeters;
        inputs.jitterX = 0;
        inputs.jitterY = 0;
        inputs.mvScaleX = 1;
        inputs.mvScaleY = 1;
        inputs.realFrameId = frame.identity.realFrameId;
        mcdr_fg_constants constants{};
        std::copy_n(currentView, 16, constants.view);
        std::copy_n(frame.current.projection, 16, constants.projection);
        constants.mvScaleX = 1;
        constants.mvScaleY = 1;
        constants.frameRenderTime = frame.frameDeltaMs;
        constants.resetHistory = compatible ? 0 : 1;
        mcdr_fg_dispatch mode{};
        mode.backend = static_cast<int32_t>(frame.frameGenerationBackend);
        mode.mode = static_cast<int32_t>(frame.frameGenerationMode);
        mode.numFrames = static_cast<int32_t>(frame.frameGenerationFrames);
        mode.reset = constants.resetHistory;
        if (!state.provider->setMode(mode)) {
            invalidateCameraLocked(state);
            return -1;
        }
        state.constants = constants;
        state.hasConstants = true;
        const auto acceptance = fgTagFrameInputs(state, candidate, inputs);
        if (acceptance == FgInputAcceptance::Accepted) state.pendingCamera = std::move(candidate);
        return acceptance == FgInputAcceptance::Accepted ? 1 :
            acceptance == FgInputAcceptance::Skipped ? 0 : -1;
    }
}
}  // namespace d3d12_mc
