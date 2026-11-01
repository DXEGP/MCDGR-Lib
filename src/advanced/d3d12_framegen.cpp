// 帧生成（FG）Provider 抽象层与注册表（模块 mcdr.d3d12.device）。DlssGFrameGenProvider 只是薄包装：
// 真正的 slDLSSGSetOptions 在 d3d12_dlss.cpp 的 streamlineEvaluate 里下发。见 docs\advanced\framegen.md。

module;

#include "_d3d12_module_prelude.h"

// ABI 声明必须放在全局模块片段：模块 purview 内 include 会触发 C5244，本项目 /WX 直接失败。
#include "mcdr_abi.h"

#include "mcdr_dlss_impl.h"

module mcdr.d3d12.device;

namespace d3d12_mc {

namespace {

// 把一句话写进固定长度的 note 缓冲（NUL 结尾、超长截断）。不用 snprintf：少一个格式化依赖。
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

// 注册表状态。期望（picked）与生效（applied）分开，热切换在下一帧帧首完成。跨线程：每个入口
// 都必须拿同一把锁，裸读裸写 provider/lastReason 就是数据竞争。
constexpr uint64_t ENSURE_RETRY_FRAMES = 30;

// 「期望开、可后端一直建不出来」时给一次 warn 的宽限期（帧，300 ≈ 5 s @60fps）。
constexpr uint64_t USABLE_GRACE_FRAMES = 300;

// 能力位从「可用」掉到「不可用」连续多少帧才回滚（60 帧约 1 s）：瞬时抖动不该把后端拆掉。
constexpr uint64_t ROLLBACK_FRAMES = 60;

struct FgRegistry {
    std::mutex mutex;
    std::unique_ptr<IFrameGenProvider> provider;
    int32_t picked = MCDR_FG_BACKEND_NONE;
    // 自测钩子显式指定过后端时为真：此时 fgSetMode 不再让门面的推测覆盖 picked。
    bool forcedPick = false;
    int32_t applied = MCDR_FG_BACKEND_NONE;
    // 链的归属在「建链那一刻」定，provider 改不动，所以单独存一份；由 d3d12_surface.cpp 写。
    int32_t chainOwner = MCDR_FG_CHAIN_APP_NATIVE;
    int32_t lastResult = 0;
    std::string lastReason;
    mcdr_fg_constants constants{};
    bool hasConstants = false;
    // UI 纹理（GUI 合成目标）的 Dx12Object*，nullptr = 没有。
    void* uiTexture = nullptr;
    // hudless 呈现模式（1 = 世界层不含 UI，UI 由 provider 合成）。期望值，provider 建成后回放。
    int32_t uiComposition = 0;
    // 期望 mode 与「链建在哪个 mode」分开记；跨过 0 边界挂重建请求，由帧首 acquireSurface 消费。
    int32_t desiredMode = 0;
    int32_t desiredFrames = 1;
    int32_t chainMode = 0;
    bool rebuildPending = false;
    int32_t rebuilds = 0;
    // 三轨帧号。FSR-FG / XeSS-FG 要求交给它们的 present id **严格单调 +1**，回绕或跳号会静默丢帧。
    //   appFrameId     渲染线程每调一次 fgBeforePresent +1；backendFrameId 由 provider 提交时写（DLSS-G 恒 0）；
    //   presentId      每次 Present 后 +1，**从 0 开始、跨 Resize 与切后端都不复位**。
    uint64_t appFrameId = 0;
    uint64_t backendFrameId = 0;
    uint64_t presentId = 0;
    uint64_t lastTrackLog = 0;
    // 启动早期 Reflex 未解析时后端建不出来，而 JVM 只在设置变更时 push 一次：记下重试时刻，由
    // fgBeforePresent 每 ENSURE_RETRY_FRAMES 帧再试一次。
    uint64_t ensureTryAt = 0;
    bool warnedNotUsable = false;
    // 后端已建成、但能力位后来掉成不可用的连续帧数（见 fgAfterPresent 的回滚段）。
    uint64_t unavailableFrames = 0;
};

FgRegistry& registry() {
    static FgRegistry s;
    return s;
}

// 链归属的**无锁快照**：fillCaps 在**持锁**时被调用，在它里面调 fgChainOwner() 会因 std::mutex
// 不可重入而每帧死锁。两个写入口同时更新这个原子量，权威仍是注册表里的 chainOwner。
std::atomic<int32_t> gChainOwnerSnapshot{MCDR_FG_CHAIN_APP_NATIVE};

bool validBackendId(int32_t backendId) {
    return backendId == MCDR_FG_BACKEND_NONE || backendId == MCDR_FG_BACKEND_DLSSG ||
           backendId == MCDR_FG_BACKEND_FSRFG || backendId == MCDR_FG_BACKEND_XESSFG;
}

// 只在状态**变化**时打一行（一局常态只有几行）：周期快照说不清「什么时候变的、变了几次」。
void logStateChange(const FgRegistry& s, const char* what, int32_t before, int32_t after) {
    if (before == after) {
        return;
    }
    dbgLogInfo("fg %s %d -> %d at=%llu pick=%d applied=%d chain=%d rb=%d",
               what, before, after, static_cast<unsigned long long>(s.presentId), s.picked, s.applied,
               s.chainMode, s.rebuilds);
}

// ---- DLSS-G 的 provider ----（行为与直连路径等价，只是把下发时机从 setMode 挪到 beforePresent）
class DlssGFrameGenProvider final : public IFrameGenProvider {
public:
    FgBackendId backend() const override { return FgBackendId::DlssG; }

    void fillCaps(mcdr_fg_caps& out) const override { fillDlssGCaps(out); }

    bool setMode(const mcdr_fg_dispatch& desc) override {
        // 只记录，下发搬到 beforePresent：官方 ProgrammingGuideDLSS_G §6 要求「选项在**下一次 Present**
        // 生效」，本帧标签已由 streamlineEvaluate() 采集完毕，跨线程调 setMode 也不会落到不确定的那次上。
        const int32_t frames = desc.numFrames < 1 ? 1 : desc.numFrames;
        desiredMode_ = desc.mode;
        desiredFrames_ = frames;
        return true;
    }

    void beforePresent() override {
        // 调到的是状态写入入口，不是 slDLSSGSetOptions（那条在 streamlineEvaluate() 里与 tag 共用 frame token）。
        if (!applied_ || appliedMode_ != desiredMode_ || appliedFrames_ != desiredFrames_) {
            mcdr_dlss_set_fg_mode(desiredMode_, desiredFrames_);
            appliedMode_ = desiredMode_;
            appliedFrames_ = desiredFrames_;
            applied_ = true;
        }
    }

    void afterPresent() override {
        // 有意留空：插帧发生在 SL 的 Present 内部，我们只需在下一帧读 presented()。
    }

    int32_t presented() const override { return mcdr_dlss_fg_presented(); }

    void onResize(int32_t displayWidth, int32_t displayHeight) override {
        static_cast<void>(displayWidth);
        static_cast<void>(displayHeight);
    }

    void shutdown() override {
        // **必须显式下发 eOff**，否则 SL 还在插帧、界面却已经说关了；内部会把 presented 复位成 1。
        mcdr_dlss_set_fg_mode(0, desiredFrames_ < 1 ? 1 : desiredFrames_);
        desiredMode_ = 0;
        appliedMode_ = 0;
        applied_ = false;
    }

private:
    int32_t desiredMode_ = 0;
    int32_t desiredFrames_ = 1;
    int32_t appliedMode_ = 0;
    int32_t appliedFrames_ = 1;
    bool applied_ = false;
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
    // DLSS-G 的 HUDless 与 ScalingOutputColor 都来自 SL 超分输出：SR 被 pick 成 FSR/XeSS 时没有输入、
    // SL 静默不插。判据用「用户选的那家」优先、active 只兜底（100% 比例时 FSR/XeSS 不跑）。
    const int32_t srSelected = srSelectedBackend();
    const int32_t srActive = srActiveBackend();
    const int32_t srBackend = srSelected != MCDR_SR_BACKEND_NONE ? srSelected : srActive;
    const bool srIsDlss = srBackend == MCDR_SR_BACKEND_NONE || srBackend == MCDR_SR_BACKEND_DLSS;

    out.runtimeLoaded = dlssReady ? 1 : 0;
    out.deviceSupported = streamline ? 1 : 0;
    // 实现是 STREAMLINE ⇒ 建链时一律升级代理工厂，所以链兼容性不依赖 FG 开关。
    out.chainCompatible = streamline ? 1 : 0;
    // 帧生成依赖 Reflex：Reflex 不是 On 时 SDK 会静默禁用插帧。
    out.needsReflex = 1;
    // HUDless 复用 SR 的输出，不需要调用方再喂一份；UI 也由 SL 在 Present 路径上合成。
    out.needsUiResource = 0;
    out.needsHudless = 0;
    // SL 自己管被 tag 的资源状态（官方 ProgrammingGuideDLSS_G §5.2），我们不替它转。
    out.transitionsInputs = 0;
    // 自己算帧数并 clamp 到 1..5（slDLSSGGetState 的 numFramesToGenerateMax）。
    out.maxGeneratedFrames = 5;
    // chainOwner 由 fgFillCaps 在外面填（这里读注册表会和它的锁互相嵌套）。
    out.reserved = 0;
    // DLSS-G 的低延迟方案就是 Reflex，且不是可选的（见 needsReflex）。
    out.latencyProvider = MCDR_FG_LATENCY_REFLEX;

    const bool available = dlssReady && streamline && reflexReady && srIsDlss;
    out.available = available ? 1 : 0;

    if (!dlssReady) {
        writeNote(out.note, sizeof(out.note), "DLSS runtime not loaded");
    } else if (!streamline) {
        writeNote(out.note, sizeof(out.note), "not on the Streamline path");
    } else if (!reflexReady) {
        // 必须说清「是 Reflex 挡住了」，否则人只会去查 FG 本身。note 只有 28 B，原因串须不超过 27 字符。
        writeNote(out.note, sizeof(out.note), "needs Reflex, unavailable");
    } else if (!srIsDlss) {
        // 同理：说清「是超分后端不对」。
        writeNote(out.note, sizeof(out.note), "needs SR=DLSS");
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
    if (!validBackendId(backendId)) {
        s.lastReason = "unknown frame generation backend id";
        return 0;
    }
    // 纯记录：不建、不销毁。链已被某一家的逻辑接管时**不允许改 picked** —— 代理链的上下文绑在旧
    // provider 身上，一改就会在 SDK 线程里崩；一次会话只用一个接管链的后端，换后端要重启。
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
            fillFsrFgCaps(out);
            break;
        case MCDR_FG_BACKEND_XESSFG:
            fillXessFgCaps(out);
            break;
        default:
            out.latencyProvider = MCDR_FG_LATENCY_NONE;
            writeNote(out.note, sizeof(out.note), "no frame generation backend");
            break;
    }
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    out.chainOwner = s.chainOwner;
    // 期望≠生效时用 lastReason **替换** note，否则 UI 只显示静态 "ready"（note 只有 28 字节，理由须短）。
    // 包装型后端还要看链：provider 建成不等于链已包好，重建失败（典型是独占全屏）时理由会被咽掉。
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

// 调用方必须已持锁（std::mutex 不可重入）；uiTexture 存 Dx12Object*，解包函数定义在后部，先声明。
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
    // 链已被另一家接管时**不允许热切换**：旧 provider 一销毁，下一次 Present 会走一条没有上下文的链
    // （FFX 返回 0x3 或直接崩在 SDK 里）。下面 unwrap → shutdown 服务「同一后端重建链」路径。
    if (s.provider != nullptr && s.applied != target &&
        (s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP || s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP)) {
        s.lastReason = "restart to switch FG";
        return 0;
    }
    // 换后端：先让旧 provider 自己收尾（它显式下发 eOff）。**不等 GPU** —— 要等时走 fgReleaseProviderWithWait。
    if (s.provider != nullptr) {
        // 换一家「接管交换链」的后端前必须先解包：否则代理链留在 IDXGISwapChain 里，新后端再包一次 =
        // 双重包装，FFX 直接返回 FFX_API_RETURN_ERROR_RUNTIME_ERROR (0x3)。
        s.provider->unwrapSwapChain();
        s.provider->shutdown();
        s.provider.reset();
        s.applied = MCDR_FG_BACKEND_NONE;
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
        // 自测/诊断用 fg-backend-* 强制选定时允许越过 available，否则实验性后端连 provider 都
        // 建不出来、wrap 永远不触发；产品路径（非强制）仍然拒绝。拒绝必须留痕，同一原因只打一次。
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
    s.provider = std::move(made);
    // JVM 在启动早期就推来了 UI 纹理与帧常量（那时没 provider），新建 provider 必须回放，否则 ui=null。
    if (s.hasConstants) {
        s.provider->setFrameConstants(s.constants);
    }
    if (s.uiTexture != nullptr) {
        s.provider->setUiTexture(unwrapDx12Resource(s.uiTexture));
    }
    // hudless 呈现模式同样是期望状态，必须回放；顺序在 setUiTexture 之后：先有纹理、再开模式。
    if (s.uiComposition != 0) {
        s.provider->setUiComposition(s.uiComposition);
    }
    // provider 可能是事后才建出来的（运行库晚装载 / 链重建）。它是白纸、默认「关」，不补这次 setMode
    // 就一直关着：Configure(enabled) 从未下发，FFX 的 interpolationEnabled 恒 false，Present 不走插帧分支。
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
    if (desc.backend != MCDR_FG_BACKEND_NONE) {
        if (!validBackendId(desc.backend)) {
            return -1;
        }
        // picked 真换了后端时旧 provider 必须换掉：两家的链要求与资源契约都不同。这里只拆旧的、不建新的。
        if (s.provider != nullptr && s.applied != desc.backend) {
            s.provider->shutdown();
            s.provider.reset();
            s.applied = MCDR_FG_BACKEND_NONE;
            s.ensureTryAt = 0;
            s.rebuildPending = true;
            s.lastReason.clear();
        }
        // 显式指定优先：门面在 available=0 时会把 backend 算成 DLSS-G，无条件覆盖会抹掉自测钩子的选择。
        if (!s.forcedPick) {
            s.picked = desc.backend;
        } else if (s.picked != desc.backend) {
            s.lastReason = "picked backend is forced; ignoring the facade's suggestion";
        }
    }
    if (desc.mode < 0 || desc.mode > 2) {
        return -1;
    }
    // 先记下「用户要什么」再试图满足：启动早期能力位是 0，直接失败并丢掉期望值会让整局都不再重试。
    const int32_t beforeMode = s.desiredMode;
    const int32_t beforeChainMode = s.chainMode;
    s.desiredMode = desc.mode;
    s.desiredFrames = desc.numFrames < 1 ? 1 : desc.numFrames;
    logStateChange(s, "desiredMode", beforeMode, s.desiredMode);
    // mode=0 是「关」：即使现在没有生效后端也必须成功，否则界面关不掉一个半建成的后端。
    if (desc.mode == 0 && s.provider == nullptr) {
        s.lastResult = 0;
        s.chainMode = 0;
        s.rebuildPending = false;
        logStateChange(s, "chainMode", beforeChainMode, s.chainMode);
        return 1;
    }
    if (s.provider == nullptr && ensureProviderLocked(s) == 0) {
        s.lastResult = 0;
        s.ensureTryAt = s.appFrameId + ENSURE_RETRY_FRAMES;
        return 0;
    }
    const bool ok = s.provider->setMode(desc);
    s.lastResult = ok ? 1 : 0;
    if (!ok) {
        return s.lastResult;
    }
    // 链**只在家族不符时**重建，开关本身不是重建理由：包装链在关闭态仍然合法（SDK 支持只关插帧，
    // ResizeBuffers 由代理链转发）。每次开关都 destroy + create 一个 HWND 链则相反 —— 旧链销毁是
    // 异步的，连做 5 次 CreateSwapChainForHwnd 就 0x887A0001。
    const bool wantOn = s.desiredMode != 0;
    const bool wantsWrap = s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG;
    const bool chainIsWrap = s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP ||
                             s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP;
    const bool chainFamilyMatches =
        (s.picked == MCDR_FG_BACKEND_FSRFG && s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP) ||
        (s.picked == MCDR_FG_BACKEND_XESSFG && s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP);
    const bool needWrap = wantOn && wantsWrap && (!chainIsWrap || !chainFamilyMatches);
    // 关掉帧生成时**不**解包；只有后端已不是包装家族才解包，否则包着的链会挡住 SL 代理链 / 原生态。
    const bool needUnwrap = chainIsWrap && !wantsWrap;
    if (needWrap || needUnwrap) {
        s.rebuildPending = true;
        s.lastReason.clear();
    }
    return s.lastResult;
}

bool fgChainRebuildPending(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.rebuildPending;
}

void fgNoteChainRebuildDone(int32_t ownerAfterRebuild, int32_t ok, const char* reason) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // 无论成功失败都撤销请求：重挂会在每帧 acquire 里反复尝试同一次注定失败的重建（例如独占全屏），即死循环。
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

void fgAbortWrapAfterFailure(const char* reason) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // wrap 失败后应用链已经没了（wrapChainForFrameGen 里 Detach 过），只 return 错误会让 surface 再也
    // present 不出去（画面不动）。所以退回「没有帧生成」：清期望、链归属记回原生、拆 provider、写 lastReason。
    if (s.provider != nullptr) {
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
}

void fgFrameStart(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // 帧首钩子（XeSS-FG 的 XeLL marker 需要）：只在 acquireSurface 里调一次，只推进帧号 + 发一条 marker。
    if (s.provider != nullptr) {
        s.provider->frameStart();
    }
}

void fgBeforePresent(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // appFrameId 必须在**进 provider 之前**加，provider 在 beforePresent 里看到的才是本帧的号（反了差一帧）。
    ++s.appFrameId;
    // 期望「开」而后端未建成时按 ENSURE_RETRY_FRAMES 重试；成功后立刻下发期望的 mode 并挂一次链重建。
    if (s.provider == nullptr && s.desiredMode != 0 && s.appFrameId >= s.ensureTryAt) {
        s.ensureTryAt = s.appFrameId + ENSURE_RETRY_FRAMES;
        if (ensureProviderLocked(s) == 1) {
            mcdr_fg_dispatch d{};
            d.backend = s.picked;
            d.mode = s.desiredMode;
            d.numFrames = s.desiredFrames;
            (void)s.provider->setMode(d);
            const bool wantsWrap = s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG;
            const bool chainIsWrap = s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP ||
                                     s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP;
            if (wantsWrap && !chainIsWrap) {
                s.rebuildPending = true;
            }
            dbgLogInfo("fg usable lazily: backend=%d mode=%d frames=%d",
                       s.picked, s.desiredMode, s.desiredFrames);
        }
    }
    if (s.provider != nullptr) {
        s.provider->beforePresent();
    }
}

void fgAfterPresent(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // 只有真 Present 过才进这里，所以 presentId 天然等于「真正提交的帧数」—— FSR/XeSS-FG 要的就是这个语义。
    ++s.presentId;
    if (s.provider != nullptr) {
        s.provider->afterPresent();
    }
    // 能力位从 1 掉到 0 时回滚（典型：SR 从 DLSS 换成 FSR/XeSS，ScalingOutputColor 不再来自 SL）。
    // 回滚 = 关掉它、清期望、挂一次链重建。连续 ROLLBACK_FRAMES 帧才动手；这一层只读几个原子量与句柄，
    // 某家 caps 变贵要在它自己的 fillCaps 里缓存。
    if (s.provider != nullptr && s.desiredMode != 0) {
        mcdr_fg_caps live{};
        s.provider->fillCaps(live);
        if (live.available != 0) {
            s.unavailableFrames = 0;
        } else if (++s.unavailableFrames >= ROLLBACK_FRAMES) {
            const std::string why = live.note[0] != '\0' ? std::string(live.note)
                                                         : std::string("backend reports unavailable");
            s.provider->shutdown();
            s.provider.reset();
            s.applied = MCDR_FG_BACKEND_NONE;
            s.desiredMode = 0;
            s.desiredFrames = 1;
            s.lastReason = why;
            s.unavailableFrames = 0;
            s.warnedNotUsable = true;
            if (s.chainMode != 0) {
                s.rebuildPending = true;
            }
            dbgLogWarn("fg rolled back: %llu frames unavailable (%s)",
                       static_cast<unsigned long long>(ROLLBACK_FRAMES), why.c_str());
        }
    }
    if (s.presentId - s.lastTrackLog >= 600) {
        s.lastTrackLog = s.presentId;
        dbgLogInfo("fg tracks: app=%llu gen=%llu present=%llu ch=%d mode=%d/%d",
                   static_cast<unsigned long long>(s.appFrameId),
                   static_cast<unsigned long long>(s.backendFrameId),
                   static_cast<unsigned long long>(s.presentId), s.chainOwner, s.desiredMode, s.chainMode);
        // 能力位与生效状态一行打全。这里没有 CPU 耗时：两点之间会等 vsync，差值被呈现节流主导。
        mcdr_fg_caps caps{};
        if (s.provider != nullptr) {
            s.provider->fillCaps(caps);
        } else if (s.picked == MCDR_FG_BACKEND_FSRFG) {
            fillFsrFgCaps(caps);
        } else if (s.picked == MCDR_FG_BACKEND_XESSFG) {
            fillXessFgCaps(caps);
        } else {
            // 帧生成关着时 provider 从没建过，caps 会全 0 —— 这里改为回答「如果现在开会是什么结果」。
            // fillXxxCaps 是自由函数、不碰注册表（本函数已持锁，调 fgFillCaps 会死锁）。
            fillDlssGCaps(caps);
        }
        caps.chainOwner = s.chainOwner;
        dbgLogInfo("fg: backend=%d available=%d runtime=%d device=%d chain=%d reflex=%d "
                   "maxGen=%d latency=%d transitions=%d chain=%d rebuilds=%d active=%d note=\"%s\"",
                   caps.backend, caps.available, caps.runtimeLoaded, caps.deviceSupported,
                   caps.chainCompatible, caps.needsReflex, caps.maxGeneratedFrames,
                   caps.latencyProvider, caps.transitionsInputs, s.chainOwner, s.rebuilds,
                   s.provider != nullptr ? 1 : 0, caps.note);
    }
    // 只报一次；等 USABLE_GRACE_FRAMES 是因为启动顺序造成的短暂不可用是正常现象，提前报警只会制造假故障。
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
        dbgLogWarn("fg: mode=%d frames=%d, no provider after %llu frames: %s",
                   s.desiredMode, s.desiredFrames, static_cast<unsigned long long>(s.presentId),
                   s.lastReason.empty() ? caps.note : s.lastReason.c_str());
    }
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
    if (s.provider != nullptr) {
        s.provider->onResize(displayWidth, displayHeight);
    }
}

void fgReleaseProviderWithWait(CommandContext* ctx) {
    // DLSS-G 不持有设备资源，这里没有等 GPU 的必要；将来等时**只等已提交的 fence 值，不等 +1**。
    static_cast<void>(ctx);
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.provider != nullptr) {
        // 必须先解包再 shutdown：只调 shutdown 会把 xefg/ffx 的 context 泄漏在进程里 —— 下一个 provider
        // 的 SetEnabled(1) 返回成功、GetLastPresentStatus 却一直报 disabled。
        s.provider->unwrapSwapChain();
        s.provider->shutdown();
        s.provider.reset();
    }
    s.applied = MCDR_FG_BACKEND_NONE;
    // **不要清 picked / desiredMode**：语义是「surface/设备要重建了」，不是「用户把帧生成关掉了」。
    // 保留意图 + 链归属复位成应用原生，下一帧 configureSurface 会按 picked 重新建、重新包。
    s.chainMode = 0;
    s.chainOwner = MCDR_FG_CHAIN_APP_NATIVE;
    gChainOwnerSnapshot.store(MCDR_FG_CHAIN_APP_NATIVE, std::memory_order_relaxed);
    s.rebuildPending = s.desiredMode != 0 &&
                       (s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG);
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
    // 包装型后端（FSR-FG / XeSS-FG）只能吃应用原生链：SL 的代理工厂把链的 Present 槽留给自己，
    // SDK 再包装一层就会与 SL 的钩子互相顶掉（两种集成方式互斥，见 d3d12_surface.cpp）。
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
    // 链归属有四种："sl-proxy / app-native" 二分会把包装型后端说成 app-native，掩盖「谁在包装
    // 这条链」这个事实（重建时要先 unwrap）。
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
    // 期望 / 链建成时的 mode / 重建次数 —— 「开关到底生效了没有」只能看这三个数。
    text += " mode=";
    text += std::to_string(s.desiredMode);
    text += "/";
    text += std::to_string(s.chainMode);
    text += " rebuilds=";
    text += std::to_string(s.rebuilds);
    if (s.rebuildPending) {
        text += " rebuildPending=1";
    }
    text += " tracks=";
    text += std::to_string(s.appFrameId);
    text += "/";
    text += std::to_string(s.backendFrameId);
    text += "/";
    text += std::to_string(s.presentId);
    // provider 自报的「每次 Present 实际呈现几帧」：1 = 没插帧，2 = 2x。区分「接上了」与「真在插帧」的数字。
    if (s.provider != nullptr) {
        text += " presented=";
        text += std::to_string(s.provider->presented());
    }
    // 低延迟方案。「antilag2(no-dll)」是实际状态：仓库里只有头、没有 DLL，别简化成 antilag2。
    text += " latency=";
    switch (s.picked) {
        case MCDR_FG_BACKEND_DLSSG:
            text += "reflex";
            break;
        case MCDR_FG_BACKEND_FSRFG:
            text += "antilag2(no-dll)";
            break;
        case MCDR_FG_BACKEND_XESSFG:
            // 低延迟走 XeLL（SetLatencyReduction + 帧号对齐 presentId）。
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
    // 位布局是契约，改它必须同步 include\mcdr_abi.h 的声明与 MCDR_ABI.kt 的读取方式。
    const int32_t desired = s.desiredMode & 0xFF;
    const int32_t chain = s.chainMode & 0xFF;
    const int32_t active = s.provider != nullptr ? 1 : 0;
    const int32_t rebuilds = s.rebuilds > 255 ? 255 : (s.rebuilds < 0 ? 0 : s.rebuilds);
    return desired | (chain << 8) | (active << 16) | (rebuilds << 24);
}

// 只在 picked 是包装型后端时把新链交给 provider，否则返回 0、调用方继续用原链（DLSS-G 走不到这里）。
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
    if (s.picked != MCDR_FG_BACKEND_XESSFG && s.picked != MCDR_FG_BACKEND_FSRFG) {
        return 0;
    }
    // FG 关闭时**不要**接管交换链：关闭态没有收益，且会让「换一家后端」永远失败（旧代理链还挂着 →
    // 双重包装）。打开 FG 的那一帧 fgSetMode 置 rebuildPending，链在那里重建并包装。
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

// 链要销毁/重建之前必须先解包：provider 的 context 绑在那条链上（InitFromSwapChain 的行头）。
void fgUnwrapPresentChain(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.provider != nullptr) {
        s.provider->unwrapSwapChain();
    }
}
// 帧常量（jitter / MV 缩放 / 相机矩阵）：注册表持锁保存并同步给 provider —— provider 在 tag
// 时读它，不回头调注册表。
void fgSetFrameConstants(const mcdr_fg_constants* constants)
{
    if (constants == nullptr)
    {
        return;
    }
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.constants = *constants;
    s.hasConstants = true;
    if (s.provider != nullptr)
    {
        s.provider->setFrameConstants(*constants);
    }
}

ID3D12Resource* unwrapDx12Resource(void* object) {
    // JVM 交的 Dx12Object* 不是 ID3D12Resource*，当资源用会让 SDK 读 GetDesc() 时 AV。句柄按 void*
    // 存/回放：包装对象的生命周期由 JVM 持有，我们只借用。
    if (object == nullptr) {
        return nullptr;
    }
    auto* wrapper = static_cast<Dx12Object*>(object);
    return wrapper != nullptr ? wrapper->resource.Get() : nullptr;
}

void fgSetUiTexture(void* object) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.uiTexture = object;
    if (s.provider != nullptr) {
        s.provider->setUiTexture(unwrapDx12Resource(object));
    }
}

// hudless 呈现开关属于期望状态：provider 可能还没建出来，先记在注册表，建出时由 ensureProviderLocked 回放。
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
    if (s.provider == nullptr) {
        return 0;
    }
    // provider 自己回答「链已包装 + UI 纹理已交过来」。**不看** s.uiComposition：JVM 正是先用这个查询
    // 决定要不要开 hudless，要求「已打开」会让它永远打不开。拿不准就是 0。
    return s.provider->uiCompositionReady() ? 1 : 0;
}

FgFrameInputs fgMakeFrameInputs(CommandContext* ctx, const mcdr_sr_dispatch& sr) {
    FgFrameInputs in{};
    if (ctx != nullptr) {
        in.cmdList = ctx->commandList;
    }
    // SR 通用路径给的是 Dx12Object*，三个指针必须先解包（否则 provider 里 GetDesc() 会 AV）；DLSS 路径给裸指针。
    in.pColorOutput = unwrapDx12Resource(sr.pColorOutput);
    in.pDepth = unwrapDx12Resource(sr.pDepth);
    in.pMotionVectors = unwrapDx12Resource(sr.pMotionVectors);
    in.renderWidth = sr.renderWidth;
    in.renderHeight = sr.renderHeight;
    in.displayWidth = sr.displayWidth;
    in.displayHeight = sr.displayHeight;
    in.cameraNear = sr.cameraNear;
    in.cameraFar = sr.cameraFar;
    in.cameraVerticalFov = sr.cameraVerticalFov;
    in.frameTimeDeltaMs = sr.frameTimeDeltaMs;
    in.viewSpaceToMeters = sr.viewSpaceToMetersFactor;
    // SR 通用路径的契约：返回时输出已回到 RENDER_TARGET，深度是只读输入，MV 是 SRV。
    in.colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    in.depthState = D3D12_RESOURCE_STATE_DEPTH_READ;
    in.motionVectorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return in;
}

void fgTagFrameInputs(const FgFrameInputs& inputs) {
    if (inputs.cmdList == nullptr) {
        return;
    }
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.provider == nullptr) {
        return;
    }
    s.provider->tagFrameInputs(inputs);
}
}  // namespace d3d12_mc