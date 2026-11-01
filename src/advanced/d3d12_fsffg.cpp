// d3d12_fsffg.cpp — AMD FidelityFX 帧生成（FSR-FG）provider 的实现单元（模块 mcdr.d3d12.device）
//
// 与 `d3d12_fsr.cpp` 分开：FSR-FG 是**包装交换链**（SDK 收应用的 `IDXGISwapChain4**`，还回来的那条才是之后
// 要 Present 的），生命周期跟着链走，运行库也不同（`amd_fidelityfx_framegeneration_dx12.dll`，不进发行包，
// caps 的 note 要能说清"是这个 DLL 不在"）。与 DLSS-G 互斥在**链**上（DLSS-G 要 SL 代理链，FSR-FG 只收
// 应用原生链），与 XeSS-FG 互斥在**所有权**上。见 docs/advanced/framegen.md

module;

#include "_d3d12_module_prelude.h"

#include "mcdr_abi.h"

// FidelityFX 的 API 面（原型/返回码、ffxFunctions 与 desc、包装交换链）。只有本单元看得到它们。
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <ffx_api_framegeneration_dx12.h>

module mcdr.d3d12.device;

namespace d3d12_mc {

namespace {

constexpr const char* kFsrFgDll = "amd_fidelityfx_framegeneration_dx12.dll";

// 产品门：打开 = 允许 `wrapSwapChain` 真的接管链并插帧；关掉 = 注册表不包链、不派发（available=0 + note）。
// `presentCallback` 必须留 nullptr：指向空实现会让交换链图像一帧都没被写过（整屏冻结 + 闪烁）。
constexpr bool kWrapWired = true;

// 设备级支持（= 至少成功建过一次链上下文 + 帧生成上下文）。`fillFsrFgCaps` 是 const 且有多处调用点，用原子量传。
std::atomic<bool> gDeviceSupported{false};

// 世界切换那一帧，Java 侧纹理可能已 `close()` 却还挂在字段上，而 `ffxApiGetResourceDX12` 第一件事是
// `pRes->GetDesc()` —— 对着野指针立刻 AV。用 SEH 探测"还能不能当资源用"，拿不到描述就当作本帧没有输入。
// 探测函数刻意不带需要析构的局部对象（/EHsc 下 C2712）；宁可少一帧，也不能让过期句柄带走进程。
__declspec(noinline) bool resourceIsUsable(ID3D12Resource* pRes, D3D12_RESOURCE_DESC& out) {
    __try {
        out = pRes->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void writeCapsNote(char* dst, size_t cap, const char* text) {
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

// 本模块所在目录（`GetModuleFileNameA(nullptr)` 拿到的是 java.exe）。
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

// 进程级只尝试一次。`ffxLoadFunctions` 把缺的入口留成 nullptr，所以"加载了但缺入口"与"没加载"分开记。
struct FsrFgRuntime {
    std::mutex lock;
    bool attempted = false;
    HMODULE module = nullptr;
    ffxFunctions fn{};
    std::string problem;  // 空 = 运行库与五个入口都在
};

FsrFgRuntime& runtime() {
    static FsrFgRuntime instance;
    return instance;
}

int32_t loadLocked(FsrFgRuntime& rt) {
    if (rt.attempted) {
        return rt.problem.empty() ? 1 : 0;
    }
    rt.attempted = true;

    std::wstring candidate = selfDirectory();
    HMODULE module = nullptr;
    if (!candidate.empty()) {
        candidate += L"\\";
        candidate += std::wstring(kFsrFgDll, kFsrFgDll + std::strlen(kFsrFgDll));
        module = ::LoadLibraryW(candidate.c_str());
    }
    if (module == nullptr) {
        module = ::LoadLibraryA(kFsrFgDll);
    }
    if (module == nullptr) {
        rt.problem = "framegeneration dll missing";
        return 0;
    }

    rt.module = module;
    ffxLoadFunctions(&rt.fn, module);
    if (rt.fn.CreateContext == nullptr || rt.fn.DestroyContext == nullptr ||
        rt.fn.Configure == nullptr || rt.fn.Query == nullptr || rt.fn.Dispatch == nullptr) {
        rt.problem = "framegeneration dll entry pts";
        return 0;
    }
    return 1;
}

// D3D12 状态 → FFX 的"交给你时它在哪"标志；认不出来就按 compute-read 交（最保守，SDK 自己插屏障）。
uint32_t toFfxState(D3D12_RESOURCE_STATES state) {
    switch (state) {
        case D3D12_RESOURCE_STATE_RENDER_TARGET:
            return FFX_API_RESOURCE_STATE_RENDER_TARGET;
        case D3D12_RESOURCE_STATE_DEPTH_WRITE:
        case D3D12_RESOURCE_STATE_DEPTH_READ:
            return FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT;
        case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
        case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
            return FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        case D3D12_RESOURCE_STATE_COPY_SOURCE:
            return FFX_API_RESOURCE_STATE_COPY_SRC;
        case D3D12_RESOURCE_STATE_PRESENT:
            return FFX_API_RESOURCE_STATE_PRESENT;
        default:
            return FFX_API_RESOURCE_STATE_COMPUTE_READ;
    }
}

FfxApiResource makeResource(void* resource, D3D12_RESOURCE_STATES state) {
    if (resource == nullptr) {
        return FfxApiResource{};
    }
    return ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(resource), toFfxState(state));
}

// FSR-FG 的 provider。`unwrapSwapChain` 按官方 Shutdown 顺序销毁：先停插帧 → WaitForPresents → 先 FG
// 上下文、后链上下文。
class FsrFgFrameGenProvider final : public IFrameGenProvider {
public:
    FgBackendId backend() const override { return FgBackendId::FsrFg; }

    void fillCaps(mcdr_fg_caps& out) const override { fillFsrFgCaps(out); }

    bool setMode(const mcdr_fg_dispatch& desc) override {
        // 与 DLSS-G / XeSS-FG 同一条约定：只记录，真正下发在 beforePresent / 录制期。
        std::lock_guard<std::mutex> lk(lock_);
        desiredMode_ = desc.mode;
        desiredFrames_ = desc.numFrames < 1 ? 1 : desc.numFrames;
        reset_ = desc.reset != 0;
        return true;
    }

    // swapchain 在 Present 内部回调这个函数：SDK 已填好命令列表与输出纹理，我们只把 header 转给
    // `ffxDispatch`。`userCtx` 是 provider 自己（不是 `&fgCtx_`）：只有 provider 拿得到"这一帧生成几张"。
    static ffxReturnCode_t onFrameGenerationDispatch(ffxDispatchDescFrameGeneration* params, void* userCtx) {
        auto* self = static_cast<FsrFgFrameGenProvider*>(userCtx);
        if (params == nullptr || self == nullptr) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        const ffxReturnCode_t rc = runtime().fn.Dispatch(&self->fgCtx_, &params->header);
        self->noteDispatch(params, rc);
        return rc;
    }

    // SDK 这一帧实际要求生成几张 —— F3 的 presented 只信这个数，不信我们自己推的"倍率"；失败回落 1。
    void noteDispatch(const ffxDispatchDescFrameGeneration* params, ffxReturnCode_t rc) {
        if (rc == FFX_API_RETURN_OK) {
            int32_t generated = static_cast<int32_t>(params->numGeneratedFrames);
            if (generated < 0) {
                generated = 0;
            }
            if (generated > 3) {
                generated = 3;
            }
            presented_.store(generated + 1);
            dispatchFailures_.store(0);
            // 硬证据：每成功派发 +1。`presented_` 只是瞬时值，`app_frames` 与它的比值才判读得出频率。
            dispatchCount_.fetch_add(1, std::memory_order_relaxed);
            generatedFrames_.fetch_add(static_cast<uint64_t>(generated), std::memory_order_relaxed);
            if (!loggedRunning_) {
                loggedRunning_ = true;
                // DEBUG：控制台默认只看到 WARN 以上。
                dbgLogDebug("fsr-fg: interpolation running (generated=%d display=%ux%u)", generated,
                            displayWidth_, displayHeight_);
            }
            return;
        }
        presented_.store(1);
        const int32_t failures = ++dispatchFailures_;
        if (failures == 1 || failures % 600 == 0) {
            dbgLogWarn("fsr-fg: dispatch failed 0x%X (frame=%llu, consecutive=%d)",
                       static_cast<unsigned>(rc), static_cast<unsigned long long>(params->frameID), failures);
        }
    }

    // 把"UI 资源注册"同步到 `swapCtx_`；调用方必须持有 `lock_`。
    //
    // 只有 hudless 模式下推过来的纹理才算 UI-only 层，其它情况一律不注册：注册"世界+GUI 合成目标"会让 SDK 在
    // 已含 HUD 的画面上再叠一遍 HUD。
    // **每帧都要重发一次**：SDK 的 `copyUiResource()` 拷贝完会把资源置空（一次 Present 用一次）；只注册一次的话
    // 第一帧之后 SDK 就当没有 UI —— 内部副本被释放、合成退化成纯拷贝，HUD 整层消失且无报错。
    // 撤销：模式关掉 / 纹理被撤销 / 链上下文重建时空注册一次（UI 合成与插帧无关，登记着就会叠）。
    void syncUiResourceLocked(FsrFgRuntime& rt) {
        const bool wantUi = hudlessUi_ && uiTexture_ != nullptr;
        if (!wantUi && registeredUi_ != nullptr) {
            needUiUnregister_ = true;
        }
        if (wantUi) {
            if (swapCtx_ == nullptr || rt.fn.Configure == nullptr) {
                return;
            }
            D3D12_RESOURCE_DESC uiDesc{};
            if (!resourceIsUsable(static_cast<ID3D12Resource*>(uiTexture_), uiDesc)) {
                // 句柄过期（换世界 / resize 那一帧）：撤销注册、退回"整帧 hudless 插值"，别把坏指针交给 SDK 的拷贝。
                dbgLogWarn("fsr-fg: ui texture %p is gone, unregistering", uiTexture_);
                uiTexture_ = nullptr;
                needUiUnregister_ = registeredUi_ != nullptr;
                return;
            }
            ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiCfg{};
            uiCfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
            // 状态声明成 COMMON：后端每帧收尾把已知资源推回 COMMON、下帧从 COMMON 起步，FFX 合成完也恢复成这个状态。
            uiCfg.uiResource = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(uiTexture_),
                                                     FFX_API_RESOURCE_STATE_COMMON);
            // USE_PREMUL_ALPHA：GUI 画进清成 (0,0,0,0) 的目标，RGB 天然已按自身 alpha 预乘。
            // DOUBLE_BUFFERING：UI 目标每帧重画，不能让 SDK 的异步 present 线程直接引用它。
            uiCfg.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
                          FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
            const ffxReturnCode_t rc = rt.fn.Configure(&swapCtx_, &uiCfg.header);
            if (rc == FFX_API_RETURN_OK) {
                needUiUnregister_ = false;
                uiFailLogged_ = false;
                if (registeredUi_ != uiTexture_) {
                    registeredUi_ = uiTexture_;
                    dbgLogWarn("fsr-fg: ui=%p premul+doublebuffer",
                               uiTexture_);
                }
            } else {
                // 注册失败就别再试撤销：这一帧 Present 走"没有 UI"的合成路径，UI 会缺一帧。
                needUiUnregister_ = false;
                logOnce(uiFailLogged_, "fsr-fg: register ui resource failed 0x%X", static_cast<unsigned>(rc));
            }
            return;
        }
        if (needUiUnregister_ && swapCtx_ != nullptr && rt.fn.Configure != nullptr) {
            ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiCfg{};
            uiCfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
            uiCfg.uiResource = {};  // 空资源 = 撤销
            uiCfg.flags = 0;
            if (rt.fn.Configure(&swapCtx_, &uiCfg.header) == FFX_API_RETURN_OK) {
                needUiUnregister_ = false;
                registeredUi_ = nullptr;
                dbgLogWarn("fsr-fg: ui resource unregistered (present = world + ui)");
            }
        }
    }

    // 逐帧的 FFX 录制全部在 `applyFrameLocked`（录制期、命令列表还开着）；这里只做不需要命令列表的两件事：
    // UI 资源注册/撤销、停用时的 Configure(false) 收尾。
    void beforePresent() override {
        // 每帧 Present 之前都会走到这里 —— 用它数"应用帧数"。dispatches_per_app≈1 说明 FFX 真在插帧，
        // ≈0 说明它一次都没回调我们。用 WARN 而不是 INFO：这是用户控制台里唯一能看见的通道。
        const uint64_t appPresents = appPresents_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (everEnabled_) {
            const uint64_t since = ++presentsAfterEnable_;
        if (since == 600) {
            const uint64_t dispatches = dispatchCount_.load(std::memory_order_relaxed);
            const uint64_t generatedTotal = generatedFrames_.load(std::memory_order_relaxed);
            // `dispatches_per_app` 才是"插帧有没有在跑"的判据（应≈1.0）；present 回调已交给 SDK 内部实现。
            dbgLogWarn("fsr-fg: app_frames=%llu dispatches=%llu "
                       "dispatches_per_app=%.2f generated_total=%llu generated=%d enabled=%d prepared=%d "
                       "hudless_ui=%d ui_registered=%d dispatch_failures=%d frame_id=%u",
                       static_cast<unsigned long long>(appPresents),
                       static_cast<unsigned long long>(dispatches),
                       static_cast<double>(dispatches) / static_cast<double>(since),
                       static_cast<unsigned long long>(generatedTotal),
                       presented_.load(std::memory_order_relaxed), enabledInFg_ ? 1 : 0, applied_ ? 1 : 0,
                       hudlessUi_ ? 1 : 0, registeredUi_ != nullptr ? 1 : 0,
                       dispatchFailures_.load(std::memory_order_relaxed), frameId_);
        }
        }
        const bool wantOn = desiredMode_ != 0;
        FsrFgRuntime& rt = runtime();
        std::lock_guard<std::mutex> lk(lock_);
        // UI 注册/撤销每帧同步一次，且必须在早退**之前** —— `wantOn && applied_` 的常态帧正是最需要它的时候。
        syncUiResourceLocked(rt);
        if (wantOn && applied_) {
            return;
        }
        if (!enabledInFg_ || swapCtx_ == nullptr || fgCtx_ == nullptr || rt.fn.Configure == nullptr) {
            return;
        }
        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = proxyChain_;
        cfg.frameGenerationEnabled = false;
        cfg.frameID = frameId_;
        if (rt.fn.Configure(&fgCtx_, &cfg.header) == FFX_API_RETURN_OK) {
            enabledInFg_ = false;
            applied_ = false;
            presented_.store(1);
        }
    }

    // 每帧的完整派发：Configure → PrepareV2（把 GPU job 录进**本帧自己的**命令列表），任何返回码不为 0 就放弃
    // 这一帧（不插帧是合法状态，宁可少一帧也不要让 SDK 吃半套数据）。
    // 两件事都必须在应用自己的命令列表**还开着**时做、随本帧提交；放进 `beforePresent` 是非法的（那时已 Close）。
    void applyFrameLocked(ID3D12GraphicsCommandList* cmd) {
        FsrFgRuntime& rt = runtime();
        if (cmd == nullptr || swapCtx_ == nullptr || fgCtx_ == nullptr || rt.fn.Configure == nullptr) {
            applied_ = false;
            return;
        }

        const bool wantOn = desiredMode_ != 0;
        // 停用时只把"关"下发一次，不必每帧 configure。
        if (!wantOn && !enabledInFg_) {
            applied_ = false;
            return;
        }

        // PrepareV2 里空资源会在 SDK 内部直接炸（无 hs_err）：第一帧（加载屏）没经过 tag，必须等输入齐了再下发。
        if (wantOn && (hudlessColor_ == nullptr || depth_ == nullptr || motionVectors_ == nullptr)) {
            logOnce(inputsMissingLogged_, "fsr-fg: waiting for frame inputs (color=%p depth=%p mv=%p)",
                    hudlessColor_, depth_, motionVectors_);
            applied_ = false;
            return;
        }

        // 句柄可能已过期（见 `resourceIsUsable`）：拿不到描述就先把自己关掉并跳过这一帧。
        if (wantOn) {
            bool stale = false;
            void* bad = nullptr;
            // UI 纹理是可选装饰：过期了就撤注册、退回"整帧 hudless 插值"，不该连累插帧本身。
            if (uiTexture_ != nullptr) {
                D3D12_RESOURCE_DESC uiDesc{};
                if (!resourceIsUsable(static_cast<ID3D12Resource*>(uiTexture_), uiDesc)) {
                    dbgLogWarn("fsr-fg: ui texture %p was released, dropping it", uiTexture_);
                    uiTexture_ = nullptr;
                    registeredUi_ = nullptr;
                }
            }
            for (void* const p : {hudlessColor_, depth_, motionVectors_}) {
                if (p == nullptr) {
                    continue;
                }
                D3D12_RESOURCE_DESC desc{};
                if (!resourceIsUsable(static_cast<ID3D12Resource*>(p), desc)) {
                    stale = true;
                    bad = p;
                    break;
                }
            }
            if (stale) {
                logOnce(staleInputsLogged_,
                        "fsr-fg: frame inputs were released under us (color=%p depth=%p mv=%p bad=%p)",
                        hudlessColor_, depth_, motionVectors_, bad);
                ffxConfigureDescFrameGeneration off{};
                off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
                off.swapChain = proxyChain_;
                off.frameGenerationEnabled = false;
                off.frameID = frameId_;
                if (rt.fn.Configure(&fgCtx_, &off.header) == FFX_API_RETURN_OK) {
                    enabledInFg_ = false;
                }
                applied_ = false;
                return;
            }
        }

        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        // `swapChain` 要的是 **IDXGISwapChain 指针**，不是链上下文句柄；传上下文会崩在 DLL 里。
        cfg.swapChain = proxyChain_;
        cfg.frameGenerationEnabled = wantOn;
        cfg.allowAsyncWorkloads = false;
        cfg.onlyPresentGenerated = false;
        cfg.generationRect.top = 0;
        cfg.generationRect.left = 0;
        cfg.generationRect.width = static_cast<int32_t>(displayWidth_);
        cfg.generationRect.height = static_cast<int32_t>(displayHeight_);
        // ---- 呈现形态：hudless 与复合，二选一（由 JVM 的 `mcdr_dx12FgSetUiComposition` 选择） ----
        // a) hudless（`hudlessUi_` = true）：应用 Present 的是**世界层**，UI 由 SDK 用 `RegisterUiResource` 合成 ——
        //    这里**不给** HUDLessColor，backbuffer 本就不含 UI。
        // b) 复合（`hudlessUi_` = false）：backbuffer = 世界+GUI。此时**绝不能注册 UI 纹理** —— 那张 `uiTexture`
        //    就是同一张合成目标，SDK 会在已含 HUD 的画面上再叠一遍（HUD 两份、随插帧闪）；插值源改用 HUDLessColor。
        const bool uiActive = hudlessUi_ && uiTexture_ != nullptr;
        if (uiActive) {
            cfg.HUDLessColor = {};
        } else {
            cfg.HUDLessColor = makeResource(hudlessColor_, hudlessColorState_);
        }
        cfg.frameID = frameId_;
        // present 回调留空，让 FFX 用内部的 UI 合成实现：`getPresentCallback` 在解析时把空指针换成
        // `ffxFrameInterpolationUiCompositionWrapper`（自己 copy + 按 flags 叠 UI）。`FFX_ASSERT` 检查的是解析后的函数。
        cfg.presentCallback = nullptr;
        cfg.presentCallbackUserContext = nullptr;
        // 插帧工作量不在这里构造 dispatch desc：交给 swapchain 在 Present 内部回调，让它写进正确的命令列表。
        cfg.frameGenerationCallback = &FsrFgFrameGenProvider::onFrameGenerationDispatch;
        // 用户上下文是 provider 本身：回调里要拿 `fgCtx_` + 记录"这一帧生成几张"。
        cfg.frameGenerationCallbackUserContext = this;
        const ffxReturnCode_t cfgRc = rt.fn.Configure(&fgCtx_, &cfg.header);
        if (cfgRc != FFX_API_RETURN_OK) {
            logOnce(configureFailLogged_, "fsr-fg: configure failed 0x%X (on=%d hudless=%p frame=%llu)",
                    static_cast<unsigned>(cfgRc), wantOn ? 1 : 0, hudlessColor_,
                    static_cast<unsigned long long>(frameId_));
            applied_ = false;
            return;
        }
        enabledInFg_ = wantOn;
        if (wantOn && !everEnabled_) {
            everEnabled_ = true;
            dbgLogWarn("fsr-fg: enabled (frame_id=%u render=%ux%u display=%ux%u)",
                       frameId_, renderWidth_, renderHeight_, displayWidth_, displayHeight_);
        }
        if (!wantOn) {
            applied_ = false;
            presented_.store(1);
            return;
        }

        // PrepareV2 录在**应用自己的**命令列表上，随本帧提交；SDK 那条插帧列表只用于手动派发 dispatch desc（文档
        // 明确不推荐），不参与 prepare。PrepareV2 是 3.1.5+ 的唯一非废弃入口。
        ffxDispatchDescFrameGenerationPrepareV2 prep{};
        prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prep.frameID = frameId_;
        prep.commandList = cmd;
        prep.renderSize.width = renderWidth_;
        prep.renderSize.height = renderHeight_;
        prep.jitterOffset.x = jitterX_;
        prep.jitterOffset.y = jitterY_;
        prep.motionVectorScale.x = mvScaleX_;
        prep.motionVectorScale.y = mvScaleY_;
        prep.frameTimeDelta = frameTimeDeltaMs_;
        prep.reset = reset_;
        prep.cameraNear = cameraNear_;
        prep.cameraFar = cameraFar_;
        prep.cameraFovAngleVertical = cameraFovVertical_;
        prep.viewSpaceToMetersFactor = viewSpaceToMeters_;
        prep.depth = makeResource(depth_, depthState_);
        // 深度资源常常是 **R32_TYPELESS**：typeless 不能建 SRV，FFX 的 DX12 后端会失败并留下空描述符，
        // 插帧 compute 一执行就崩（DLL 里 AV、无 SDK 消息）。按 R32_FLOAT 描述即可。
        if (prep.depth.description.format == FFX_API_SURFACE_FORMAT_R32_TYPELESS) {
            prep.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
        }
        prep.motionVectors = makeResource(motionVectors_, motionVectorState_);
        // `renderSize` 以深度/MV 的实际尺寸为准（SR 路径给的 `renderWidth` 可能是 0），否则 PrepareV2 返回 0x1。
        if (prep.depth.description.width > 0 && prep.depth.description.height > 0) {
            prep.renderSize.width = prep.depth.description.width;
            prep.renderSize.height = prep.depth.description.height;
        }
        const ffxReturnCode_t prepRc = rt.fn.Dispatch(&fgCtx_, &prep.header);
        if (prepRc != FFX_API_RETURN_OK) {
            logOnce(prepareFailLogged_, "fsr-fg: prepare failed 0x%X (render=%ux%u depth=%p mv=%p)",
                    static_cast<unsigned>(prepRc), renderWidth_, renderHeight_, depth_, motionVectors_);
            applied_ = false;
            return;
        }

        applied_ = true;
        reset_ = false;
        ++frameId_;
        // presented 由 `noteDispatch` 写：这里只代表"这一帧的 prepare 成功了"，不代表插了几帧。
    }

    void afterPresent() override {
        // FFX 的插帧发生在它接管的 Present 里，这一侧无事可做（与另外两家一致）。
    }

    int32_t presented() const override { return static_cast<int32_t>(presented_.load()); }

    void onResize(int32_t displayWidth, int32_t displayHeight) override {
        std::lock_guard<std::mutex> lk(lock_);
        displayWidth_ = displayWidth;
        displayHeight_ = displayHeight;
        // 代理链自己的 ResizeBuffers 由 surface 层调用；这里只更新 generationRect。
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lk(lock_);
        desiredMode_ = 0;
        applied_ = false;
        presented_.store(1);
    }

    // UI 合成：JVM 侧把 GUI 层纹理推过来时才启用。FSR-FG 的 UI 注册在**链上下文**上（`RegisterUiResource`），与 XeSS-FG 用 tag 是两条路径。
    void setUiTexture(void* object) override {
        std::lock_guard<std::mutex> lk(lock_);
        // 注册表已把 `Dx12Object*` 解包成 `ID3D12Resource*`：这里只记指针、下一帧注册；生命周期由 JVM 侧 GUI 目标持有。
        if (uiTexture_ != object && registeredUi_ != nullptr) {
            // 旧纹理可能已随 resize 释放：先让 SDK 松手（`beforePresent` 里发空注册），否则 double-buffer 的每次拷贝都会踩到它。
            needUiUnregister_ = true;
        }
        uiTexture_ = object;
        registeredUi_ = nullptr;
    }

    // hudless 呈现开关（JVM 在"当前后端是 FSR-FG + 本帧有 3D 世界层"时置 1）。切模式强制下一帧重下发 Configure。
    void setUiComposition(int32_t enabled) override {
        std::lock_guard<std::mutex> lk(lock_);
        const bool on = enabled != 0;
        if (hudlessUi_ == on) {
            return;
        }
        hudlessUi_ = on;
        if (!on && registeredUi_ != nullptr) {
            needUiUnregister_ = true;
        }
        applied_ = false;
        dbgLogWarn("fsr-fg: hudless ui composition %s",
                   on ? "on (present = world layer, ui composed by sdk)" : "off (present = world + ui)");
    }

    // "这次 Present 会由 SDK 合成 UI"吗？**不要求 UI 已注册**（注册就在同一个 Present 之前的 `beforePresent`
    // 里），也**不要求 hudlessUi_ 已打开**（JVM 必须先问"你能不能合成"才敢打开）。没包链 / 没 UI 纹理回 0。
    bool uiCompositionReady() const override {
        std::lock_guard<std::mutex> lk(lock_);
        // 必须**真的发生过插帧 dispatch** 才算：`proxyChain_` 只证明链被包过，不证明游戏现在 Present 的是代理链 ——
        // 建了上下文却一次没跑过时，UI 注册落在一条没人 Present 的链上，JVM 照样"武装" hudless ⇒ 界面整层消失。
        return uiTexture_ != nullptr && swapCtx_ != nullptr && proxyChain_ != nullptr &&
               dispatchCount_.load(std::memory_order_relaxed) > 0;
    }

    void setFrameConstants(const mcdr_fg_constants& constants) override {
        std::lock_guard<std::mutex> lk(lock_);
        jitterX_ = constants.jitterX;
        jitterY_ = constants.jitterY;
        mvScaleX_ = constants.mvScaleX;
        mvScaleY_ = constants.mvScaleY;
        hasConstants_ = true;
    }

    void tagFrameInputs(const FgFrameInputs& inputs) override {
        std::lock_guard<std::mutex> lk(lock_);
        if (!tagLogged_) {
            tagLogged_ = true;
            // 打的是**本帧输入**的尺寸，不是成员 `displayWidth_`（后者在 wrap 阶段才填，首帧还是 0）。
            dbgLogWarn("fsr-fg: tag color=%p depth=%p mv=%p %dx%d->%dx%d cmd=%p",
                       inputs.pColorOutput, inputs.pDepth, inputs.pMotionVectors, inputs.renderWidth,
                       inputs.renderHeight, inputs.displayWidth, inputs.displayHeight,
                       static_cast<void*>(inputs.cmdList));
        }
        cmdList_ = inputs.cmdList;
        hudlessColor_ = inputs.pColorOutput;
        hudlessColorState_ = inputs.colorState;
        depth_ = inputs.pDepth;
        depthState_ = inputs.depthState;
        motionVectors_ = inputs.pMotionVectors;
        motionVectorState_ = inputs.motionVectorState;
        renderWidth_ = inputs.renderWidth > 0 ? inputs.renderWidth : displayWidth_;
        renderHeight_ = inputs.renderHeight > 0 ? inputs.renderHeight : displayHeight_;
        // 相机与帧时：只接受有效值（0 表示上游没填），保留成员里的默认值。
        if (inputs.cameraNear > 0.0f && inputs.cameraFar > inputs.cameraNear) {
            cameraNear_ = inputs.cameraNear;
            cameraFar_ = inputs.cameraFar;
        }
        if (inputs.cameraVerticalFov > 0.0f) {
            cameraFovVertical_ = inputs.cameraVerticalFov;
        }
        if (inputs.frameTimeDeltaMs > 0.0f) {
            frameTimeDeltaMs_ = inputs.frameTimeDeltaMs;
        }
        if (inputs.viewSpaceToMeters > 0.0f) {
            viewSpaceToMeters_ = inputs.viewSpaceToMeters;
        }
        // 输入齐了就在本帧的命令列表上做 Configure + Prepare（见 applyFrameLocked 的时序说明）。
        applyFrameLocked(inputs.cmdList);
    }

    int32_t wrapSwapChain(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* appChain,
                          int32_t width, int32_t height, DXGI_FORMAT format,
                          IDXGISwapChain** outPresentChain) override {
        if (outPresentChain != nullptr) {
            *outPresentChain = nullptr;
        }
        if (device == nullptr || appChain == nullptr) {
            return 0;
        }
        FsrFgRuntime& rt = runtime();
        std::lock_guard<std::mutex> lk(lock_);
        if (loadLocked(rt) != 1) {
            dbgLogWarn("fsr-fg: %s", rt.problem.c_str());
            return 0;
        }
        if (swapCtx_ != nullptr) {
            destroyContextsLocked(rt);
        }

        // 交给 SDK 的链必须只有我们这一个引用。QueryInterface 会 +1，所以建 context 前把调用方那份放掉。
        IDXGISwapChain4* chain4 = nullptr;
        if (FAILED(appChain->QueryInterface(__uuidof(IDXGISwapChain4), reinterpret_cast<void**>(&chain4))) ||
            chain4 == nullptr) {
            dbgLogWarn("fsr-fg: app chain is not IDXGISwapChain4, skip wrap");
            return 0;
        }
        appChain->Release();

        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 swapVersion{};
        swapVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        swapVersion.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

        ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrapDesc{};
        wrapDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
        wrapDesc.header.pNext = &swapVersion.header;
        wrapDesc.swapchain = &chain4;
        wrapDesc.gameQueue = queue;

        ffxContext swapCtx = nullptr;
        const ffxReturnCode_t wrapRc = rt.fn.CreateContext(&swapCtx, &wrapDesc.header, nullptr);
        if (wrapRc != FFX_API_RETURN_OK || swapCtx == nullptr) {
            chain4->Release();
            dbgLogWarn("fsr-fg: swapchain wrap failed 0x%X", static_cast<unsigned>(wrapRc));
            return 0;
        }

        ffxCreateContextDescFrameGenerationVersion fgVersion{};
        fgVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        fgVersion.version = FFX_FRAMEGENERATION_VERSION;

        ffxCreateBackendDX12Desc backendDesc{};
        backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backendDesc.header.pNext = &fgVersion.header;
        backendDesc.device = device;

        // HUDless 格式必须声明给 FG 上下文：provider 用它设 `previousInterpolationSourceFormat`。
        const uint32_t bbFormat = ffxApiGetSurfaceFormatDX12(format);
        ffxCreateContextDescFrameGenerationHudless hudlessDesc{};
        hudlessDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hudlessDesc.hudlessBackBufferFormat = bbFormat;
        fgVersion.header.pNext = &hudlessDesc.header;

        ffxCreateContextDescFrameGeneration fgDesc{};
        fgDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        fgDesc.header.pNext = &backendDesc.header;
        // 深度约定必须和 SR 一致：本项目是 reverse-Z，Far 面自 26.3 起有限，所以只置 DEPTH_INVERTED、**不**置
        // DEPTH_INFINITE；不置 inverted 时 FFX 会把反向深度当普通深度，光流/深度测试全错。DEBUG_CHECKING 不带。
        fgDesc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        fgDesc.displaySize.width = static_cast<uint32_t>(width > 0 ? width : 1);
        fgDesc.displaySize.height = static_cast<uint32_t>(height > 0 ? height : 1);
        fgDesc.maxRenderSize = fgDesc.displaySize;
        fgDesc.backBufferFormat = bbFormat;

        ffxContext fgCtx = nullptr;
        const ffxReturnCode_t fgRc = rt.fn.CreateContext(&fgCtx, &fgDesc.header, nullptr);
        if (fgRc != FFX_API_RETURN_OK || fgCtx == nullptr) {
            rt.fn.DestroyContext(&swapCtx, nullptr);
            chain4->Release();
            dbgLogWarn("fsr-fg: frame generation context failed 0x%X", static_cast<unsigned>(fgRc));
            return 0;
        }

        swapCtx_ = swapCtx;
        fgCtx_ = fgCtx;
        // 新链上下文不认旧的 UI 注册；不清掉的话 `registeredUi_ == uiTexture_` 会让我们以为已注册过，其实没有 UI。
        registeredUi_ = nullptr;
        // SDK 在内部把 `chain4` 原地换成代理链，这里拿到的就是之后要 Present 的那条链；surface 持有引用。
        proxyChain_ = reinterpret_cast<IDXGISwapChain*>(chain4);
        proxyChain3_ = nullptr;
        if (chain4 != nullptr) {
            (void)chain4->QueryInterface(__uuidof(IDXGISwapChain3),
                                         reinterpret_cast<void**>(&proxyChain3_));
        }
        displayWidth_ = fgDesc.displaySize.width;
        displayHeight_ = fgDesc.displaySize.height;
        renderWidth_ = displayWidth_;
        renderHeight_ = displayHeight_;
        frameId_ = 1;
        enabledInFg_ = false;
        configureFailLogged_ = false;
        cmdFailLogged_ = false;
        prepareFailLogged_ = false;
        inputsMissingLogged_ = false;
        loggedRunning_ = false;
        gDeviceSupported.store(true);

        // proxy 链第一次 GetBuffer/Present 就进 FFX 的呈现路径，要读 generationRect 与帧号；包装成功后立刻下发一次"关"。
        {
            ffxConfigureDescFrameGeneration initial{};
            initial.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            initial.swapChain = proxyChain_;
            initial.frameGenerationEnabled = false;
            initial.allowAsyncWorkloads = false;
            initial.onlyPresentGenerated = false;
            initial.generationRect.top = 0;
            initial.generationRect.left = 0;
            initial.generationRect.width = static_cast<int32_t>(displayWidth_);
            initial.generationRect.height = static_cast<int32_t>(displayHeight_);
            initial.frameID = frameId_;
            const ffxReturnCode_t initRc = rt.fn.Configure(&fgCtx_, &initial.header);
            if (initRc != FFX_API_RETURN_OK) {
                dbgLogWarn("fsr-fg: initial configure failed 0x%X", static_cast<unsigned>(initRc));
            }
        }

        if (outPresentChain != nullptr) {
            // 所有权交给 surface（它会 Attach）；provider 不持有引用，否则 surface 的 Reset() 之后代理链析构不掉。
            *outPresentChain = reinterpret_cast<IDXGISwapChain*>(chain4);
        } else {
            chain4->Release();
        }

        dbgLogInfo("fsr-fg: wrap %ux%u fmt=%d proxy=%p swap=%p fg=%p",
                   displayWidth_, displayHeight_, static_cast<int>(format),
                   static_cast<void*>(chain4), swapCtx, fgCtx);
        return 1;
    }

    void unwrapSwapChain() override {
        FsrFgRuntime& rt = runtime();
        std::lock_guard<std::mutex> lk(lock_);
        destroyContextsLocked(rt);
    }

private:
    // 一次性日志：同一类失败只在第一次出现时落一行，避免每帧刷屏。
    static void logOnce(bool& flag, const char* fmt, ...) {
        if (flag) {
            return;
        }
        flag = true;
        char buffer[256] = {};
        va_list args;
        va_start(args, fmt);
        (void)std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        dbgLogWarn("%s", buffer);
    }

    void destroyContextsLocked(FsrFgRuntime& rt) {
        if (fgCtx_ != nullptr && rt.fn.Configure != nullptr) {
            ffxConfigureDescFrameGeneration off{};
            off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            off.swapChain = proxyChain_;
            off.frameGenerationEnabled = false;
            rt.fn.Configure(&fgCtx_, &off.header);
        }
        if (swapCtx_ != nullptr && rt.fn.Dispatch != nullptr) {
            ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
            wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
            rt.fn.Dispatch(&swapCtx_, &wait.header);
        }
        // 顺序按官方 Shutdown：先帧生成上下文，再链上下文。
        if (fgCtx_ != nullptr) {
            rt.fn.DestroyContext(&fgCtx_, nullptr);
            fgCtx_ = nullptr;
        }
        if (swapCtx_ != nullptr) {
            rt.fn.DestroyContext(&swapCtx_, nullptr);
            swapCtx_ = nullptr;
        }
        gDeviceSupported.store(false);
        enabledInFg_ = false;
        applied_ = false;
        loggedRunning_ = false;
        // 新链上下文要重新注册 UI 纹理（同一块纹理在新上下文里没登记过）。
        registeredUi_ = nullptr;
        needUiUnregister_ = false;
        if (proxyChain3_ != nullptr) {
            proxyChain3_->Release();
            proxyChain3_ = nullptr;
        }
        proxyChain_ = nullptr;
    }

    mutable std::mutex lock_;
    ffxContext swapCtx_ = nullptr;
    ffxContext fgCtx_ = nullptr;
    // proxy 链的裸指针（surface 持有它的引用；这里只借来填 Configure 的 swapChain 字段）。
    IDXGISwapChain* proxyChain_ = nullptr;
    // 同一个代理链的 3 代接口，只为 `GetCurrentBackBufferIndex()` / `GetBuffer()`。
    IDXGISwapChain3* proxyChain3_ = nullptr;

    void* hudlessColor_ = nullptr;
    void* uiTexture_ = nullptr;
    // 已成功注册到当前 `swapCtx_` 上的 UI 纹理（裸指针只用于同一性比较，不持有引用）。
    void* registeredUi_ = nullptr;
    // hudless 呈现模式（present 的世界层不含 UI、UI 由 SDK 合成）。
    bool hudlessUi_ = false;
    // 需要在下一个 `beforePresent` 里显式撤销 UI 注册（空资源）。
    bool needUiUnregister_ = false;
    void* depth_ = nullptr;
    void* motionVectors_ = nullptr;
    D3D12_RESOURCE_STATES hudlessColorState_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList* cmdList_ = nullptr;
    D3D12_RESOURCE_STATES depthState_ = D3D12_RESOURCE_STATE_DEPTH_READ;
    D3D12_RESOURCE_STATES motionVectorState_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    uint32_t displayWidth_ = 0;
    uint32_t displayHeight_ = 0;
    uint32_t renderWidth_ = 0;
    uint32_t renderHeight_ = 0;
    uint64_t frameId_ = 1;
    float jitterX_ = 0.0f;
    float jitterY_ = 0.0f;
    float mvScaleX_ = 1.0f;
    float mvScaleY_ = 1.0f;
    float frameTimeDeltaMs_ = 16.6f;
    // 相机参数：PrepareV2 要用。上游没给时保留这组保守默认值，至少不会因为 0 近平面 / 0 视场被判非法。
    float cameraNear_ = 0.05f;
    float cameraFar_ = 1024.0f;
    float cameraFovVertical_ = 1.0f;
    float viewSpaceToMeters_ = 1.0f;
    bool hasConstants_ = false;
    bool reset_ = false;

    int32_t desiredMode_ = 0;
    int32_t desiredFrames_ = 1;
    bool applied_ = false;
    bool enabledInFg_ = false;
    bool configureFailLogged_ = false;
    bool cmdFailLogged_ = false;
    bool prepareFailLogged_ = false;
    bool inputsMissingLogged_ = false;
    bool staleInputsLogged_ = false;
    bool uiFailLogged_ = false;
    bool loggedRunning_ = false;
    std::atomic<int32_t> presented_{1};
    // 连续 dispatch 失败次数（成功清零）：区分"回调没发生"（恒 0）与"回调发生了但每帧都失败"（持续增长）。
    std::atomic<int32_t> dispatchFailures_{0};

    std::atomic<uint64_t> appPresents_{0};
    // 插帧确实发生的硬计数：成功派发次数、累计生成帧数（`noteDispatch` 里累加）。
    std::atomic<uint64_t> dispatchCount_{0};
    std::atomic<uint64_t> generatedFrames_{0};
    // 只有真的下发过 enabled=true 之后才开始数统计帧：加载屏/标题页里 SR 根本不跑，在那里打 ratio 只会误导。
    bool everEnabled_ = false;
    bool tagLogged_ = false;
    uint64_t presentsAfterEnable_ = 0;
};

}  // namespace

bool fsrFgRuntimeAvailable() {
    FsrFgRuntime& rt = runtime();
    std::lock_guard<std::mutex> lk(rt.lock);
    return loadLocked(rt) == 1;
}

void fillFsrFgCaps(mcdr_fg_caps& out) {
    out.backend = MCDR_FG_BACKEND_FSRFG;

    FsrFgRuntime& rt = runtime();
    bool loaded = false;
    std::string problem;
    {
        std::lock_guard<std::mutex> lk(rt.lock);
        loaded = loadLocked(rt) == 1;
        problem = rt.problem;
    }

    out.runtimeLoaded = loaded ? 1 : 0;
    // 设备支持 = 至少成功建过一次链上下文 + 帧生成上下文（`wrapSwapChain` 里置位）。
    out.deviceSupported = gDeviceSupported.load() ? 1 : 0;
    // 链要求：FSR-FG 要一条**应用原生**链（SDK 自己包），而工厂是建链时按 `picked` 选链（`createDxgiFactory`
    // → `fgWantsAppNativeChain`）。所以"兼容"必须按**将要建的链**判断：FG 默认关着进场时链可能是 DLSS-G 的 SL
    // 代理链，这里返回 0 会让 `ensureProviderLocked` 建不出 provider，用户点开 FSR-FG 永远"没有反应"。
    // 真正的失败路径（独占全屏）由 `rebuildSwapChainForFg` 拒绝并通过 `lastReason` 报给 UI。
    out.chainCompatible = 1;
    // FSR-FG 不依赖 Reflex；它的低延迟方案是 Anti-Lag 2，仓库里只有头、没有 DLL，这里只是架构位。
    out.needsReflex = 0;
    out.needsHudless = 1;
    out.needsUiResource = 1;
    out.transitionsInputs = 1;
    // 4.0.1 的 swapchain 支持一帧插多张（最多 3 张），本仓 UI 只暴露 2x；不虚报上限。
    out.maxGeneratedFrames = 2;
    out.latencyProvider = MCDR_FG_LATENCY_ANTILAG2;
    out.reserved = 0;
    // 可用 = 运行库在 + 链兼容 + 产品门打开；`deviceSupported` 不参与（第一次打开开关时正是"还没建过"的状态）。
    out.available = (loaded && kWrapWired) ? 1 : 0;

    if (!loaded) {
        writeCapsNote(out.note, sizeof(out.note), problem.empty() ? "fsr-fg runtime missing" : problem.c_str());
    } else if (!kWrapWired) {
        // 只在产品门被改回 false 时走到：如实说明这条后端在本构建里被关了。
        writeCapsNote(out.note, sizeof(out.note), "FSR-FG gated off in this build (kWrapWired=false); see known_bugs.md P306ac");
    } else {
        writeCapsNote(out.note, sizeof(out.note), "FSR-FG ready");
    }

    static std::atomic<bool> loggedCaps{false};
    if (!loggedCaps.exchange(true)) {
        dbgLogInfo("fsr-fg: caps run=%d dev=%d chain=%d ok=%d lat=%d note=\"%s\"",
                   out.runtimeLoaded, out.deviceSupported, out.chainCompatible, out.available, out.latencyProvider,
                   out.note);
    }
}

std::unique_ptr<IFrameGenProvider> makeFsrFgProvider() {
    return std::make_unique<FsrFgFrameGenProvider>();
}

}  // namespace d3d12_mc
