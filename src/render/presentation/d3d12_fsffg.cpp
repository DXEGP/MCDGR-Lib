module;

#include "_d3d12_module_prelude.h"

#include "mcdr_abi.h"

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <ffx_api_framegeneration_dx12.h>

module mcdr.d3d12.device;

namespace d3d12_mc {

namespace {

constexpr const char* kFsrFgDll = "amd_fidelityfx_framegeneration_dx12.dll";

constexpr bool kWrapWired = true;

std::atomic<bool> gDeviceSupported{false};

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

struct FsrFgRuntime {
    std::mutex lock;
    bool attempted = false;
    HMODULE module = nullptr;
    ffxFunctions fn{};
    std::string problem;
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

class FsrFgFrameGenProvider final : public IFrameGenProvider {
public:
    FgBackendId backend() const override { return FgBackendId::FsrFg; }

    void fillCaps(mcdr_fg_caps& out) const override { fillFsrFgCaps(out); }

    bool setMode(const mcdr_fg_dispatch& desc) override {
        std::lock_guard<std::mutex> lk(lock_);
        desiredMode_ = desc.mode;
        desiredFrames_ = desc.numFrames < 1 ? 1 : desc.numFrames;
        reset_ = reset_ || desc.reset != 0;
        return true;
    }

    bool waitForPreviousFrame() override {
        std::lock_guard<std::mutex> lock(lock_);
        if (retirementFailed_) return false;
        if (!inputFence_ || inputFenceValue_ == 0) return true;
        const UINT64 completed = inputFence_->GetCompletedValue();
        if (completed == UINT64_MAX) return false;
        if (completed < inputFenceValue_) {
            if (!inputFenceEvent_) inputFenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!inputFenceEvent_) return false;
            const HRESULT registration = inputFence_->SetEventOnCompletion(inputFenceValue_, inputFenceEvent_);
            const DWORD waited = SUCCEEDED(registration) ? WaitForSingleObject(inputFenceEvent_, 5000) : WAIT_FAILED;
            if (waited != WAIT_OBJECT_0) {
                dbgLogError("fsr-fg: previous source reads did not finish; frame recording rejected");
                return false;
            }
            const UINT64 retired = inputFence_->GetCompletedValue();
            if (retired == UINT64_MAX || retired < inputFenceValue_) return false;
        }
        collectInputReadsLocked();
        return true;
    }

    void discardFrameInputs() override {
        std::lock_guard<std::mutex> lock(lock_);
        applied_ = false;
        historyAccepted_ = false;
        reset_ = true;
        hasConstants_ = false;
        cmdList_ = nullptr;
        hudlessColor_ = depth_ = motionVectors_ = nullptr;
    }

    static ffxReturnCode_t onFrameGenerationDispatch(ffxDispatchDescFrameGeneration* params, void* userCtx) {
        auto* self = static_cast<FsrFgFrameGenProvider*>(userCtx);
        if (params == nullptr || self == nullptr) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        const ffxReturnCode_t rc = runtime().fn.Dispatch(&self->fgCtx_, &params->header);
        self->noteDispatch(params, rc);
        return rc;
    }

    void noteDispatch(const ffxDispatchDescFrameGeneration* params, ffxReturnCode_t rc) {
        completedDispatchSucceeded_.store(rc == FFX_API_RETURN_OK, std::memory_order_relaxed);
        completedDispatchFrame_.store(params->frameID, std::memory_order_release);
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
            dispatchCount_.fetch_add(1, std::memory_order_relaxed);
            generatedFrames_.fetch_add(static_cast<uint64_t>(generated), std::memory_order_relaxed);
            if (!loggedRunning_) {
                loggedRunning_ = true;
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

    void syncUiResourceLocked(FsrFgRuntime& rt) {
        const bool wantUi = hudlessUi_ && uiTexture_ != nullptr;
        if (!wantUi && registeredUi_ != nullptr) {
            needUiUnregister_ = true;
        }
        if (wantUi) {
            if (uiRegisteredForPresent_ && registeredUi_ == uiTexture_ && !needUiUnregister_) return;
            if (swapCtx_ == nullptr || rt.fn.Configure == nullptr) {
                return;
            }
            ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiCfg{};
            uiCfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
            uiCfg.uiResource = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(uiTexture_),
                                                     FFX_API_RESOURCE_STATE_COMMON);
            uiCfg.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
                          FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
            const ffxReturnCode_t rc = rt.fn.Configure(&swapCtx_, &uiCfg.header);
            if (rc == FFX_API_RETURN_OK) {
                registeredUiHold_ = uiTextureHold_;
                registeredUiLease_ = uiLease_;
                needUiUnregister_ = false;
                uiRegisteredForPresent_ = true;
                uiFailLogged_ = false;
                if (registeredUi_ != uiTexture_) {
                    registeredUi_ = uiTexture_;
                    dbgLogWarn("fsr-fg: ui resource registered (ui=%p premul+doublebuffer, per present)",
                               uiTexture_);
                }
            } else {
                uiRegisteredForPresent_ = false;
                needUiUnregister_ = registeredUi_ != nullptr;
                logOnce(uiFailLogged_, "fsr-fg: register ui resource failed 0x%X", static_cast<unsigned>(rc));
            }
            return;
        }
        if (needUiUnregister_ && swapCtx_ != nullptr && rt.fn.Configure != nullptr) {
            ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiCfg{};
            uiCfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
            uiCfg.uiResource = {};
            uiCfg.flags = 0;
            if (rt.fn.Configure(&swapCtx_, &uiCfg.header) == FFX_API_RETURN_OK) {
                needUiUnregister_ = false;
                uiRegisteredForPresent_ = false;
                registeredUi_ = nullptr;
                registeredUiHold_.Reset();
                registeredUiLease_.reset();
                dbgLogWarn("fsr-fg: ui resource unregistered (present = world + ui)");
            }
        }
    }

    void beforePresent() override {
        const uint64_t appPresents = appPresents_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (everEnabled_) {
            const uint64_t since = ++presentsAfterEnable_;
        if (since == 600) {
            const uint64_t dispatches = dispatchCount_.load(std::memory_order_relaxed);
            const uint64_t generatedTotal = generatedFrames_.load(std::memory_order_relaxed);
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
        FsrFgRuntime& rt = runtime();
        std::lock_guard<std::mutex> lk(lock_);
        collectInputReadsLocked();
        const bool wantOn = desiredMode_ != 0 && !retirementFailed_;
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

    void applyFrameLocked(ID3D12GraphicsCommandList* cmd) {
        FsrFgRuntime& rt = runtime();
        if (retirementFailed_ || cmd == nullptr || swapCtx_ == nullptr || fgCtx_ == nullptr || rt.fn.Configure == nullptr) {
            applied_ = false;
            return;
        }

        const bool wantOn = desiredMode_ != 0;
        if (!wantOn && !enabledInFg_) {
            applied_ = false;
            return;
        }

        if (wantOn && (hudlessColor_ == nullptr || depth_ == nullptr || motionVectors_ == nullptr)) {
            logOnce(inputsMissingLogged_, "fsr-fg: waiting for frame inputs (color=%p depth=%p mv=%p)",
                    hudlessColor_, depth_, motionVectors_);
            applied_ = false;
            return;
        }

        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = proxyChain_;
        cfg.frameGenerationEnabled = wantOn;
        cfg.allowAsyncWorkloads = false;
        cfg.onlyPresentGenerated = false;
        cfg.generationRect.top = 0;
        cfg.generationRect.left = 0;
        cfg.generationRect.width = static_cast<int32_t>(displayWidth_);
        cfg.generationRect.height = static_cast<int32_t>(displayHeight_);
        const bool uiActive = hudlessUi_ && uiTexture_ != nullptr;
        if (uiActive) {
            cfg.HUDLessColor = {};
        } else {
            cfg.HUDLessColor = makeResource(hudlessColor_, D3D12_RESOURCE_STATE_COMMON);
        }
        cfg.frameID = frameId_;
        cfg.presentCallback = nullptr;
        cfg.presentCallbackUserContext = nullptr;
        cfg.frameGenerationCallback = &FsrFgFrameGenProvider::onFrameGenerationDispatch;
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
        std::copy_n(cameraPosition_, 3, prep.cameraPosition);
        std::copy_n(cameraUp_, 3, prep.cameraUp);
        std::copy_n(cameraRight_, 3, prep.cameraRight);
        std::copy_n(cameraForward_, 3, prep.cameraForward);
        prep.depth = makeResource(depth_, depthState_);
        if (prep.depth.description.format == FFX_API_SURFACE_FORMAT_R32_TYPELESS) {
            prep.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
        }
        prep.motionVectors = makeResource(motionVectors_, motionVectorState_);
        const ffxReturnCode_t prepRc = rt.fn.Dispatch(&fgCtx_, &prep.header);
        if (prepRc != FFX_API_RETURN_OK) {
            logOnce(prepareFailLogged_, "fsr-fg: prepare failed 0x%X (render=%ux%u depth=%p mv=%p)",
                    static_cast<unsigned>(prepRc), renderWidth_, renderHeight_, depth_, motionVectors_);
            applied_ = false;
            return;
        }

        applied_ = true;
        preparedFrame_ = frameId_;
        preparedReset_ = reset_;
        reset_ = false;
        ++frameId_;
    }

    void afterPresent() override {
        std::lock_guard<std::mutex> lk(lock_);
        uiRegisteredForPresent_ = false;
        historyAccepted_ = applied_ && !retirementFailed_ &&
            (completedDispatchFrame_.load(std::memory_order_acquire) == preparedFrame_
                ? completedDispatchSucceeded_.load(std::memory_order_relaxed) : preparedReset_);
        if (gameQueue_ && inputFence_ && !retirementFailed_ &&
            (frameLease_ || registeredUiHold_)) {
            InputRead read;
            read.value = ++inputFenceValue_;
            read.frame = frameLease_;
            read.uiLease = registeredUiLease_;
            read.ui = registeredUiHold_;
            inputReads_.push_back(std::move(read));
            if (FAILED(gameQueue_->Signal(inputFence_.Get(), inputFenceValue_))) {
                retirementFailed_ = true;
                dbgLogError("fsr-fg: input retirement Signal failed; retaining leases until SDK drain");
            }
        }
        frameLease_.reset();
        hudlessColor_ = depth_ = motionVectors_ = nullptr;
        cmdList_ = nullptr;
        applied_ = false;
        collectInputReadsLocked();
    }

    int32_t presented() const override { return static_cast<int32_t>(presented_.load()); }

    void onResize(int32_t displayWidth, int32_t displayHeight) override {
        std::lock_guard<std::mutex> lk(lock_);
        displayWidth_ = displayWidth;
        displayHeight_ = displayHeight;
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lk(lock_);
        desiredMode_ = 0;
        applied_ = false;
        presented_.store(1);
    }

    void setUiTexture(void* object, std::shared_ptr<void> lease = {}) override {
        std::lock_guard<std::mutex> lk(lock_);
        if (uiTexture_ != object) uiRegisteredForPresent_ = false;
        if (uiTexture_ != object && registeredUi_ != nullptr) {
            needUiUnregister_ = true;
        }
        uiTextureHold_ = static_cast<ID3D12Resource*>(object);
        uiLease_ = std::move(lease);
        uiTexture_ = uiTextureHold_.Get();
    }

    void setUiComposition(int32_t enabled) override {
        std::lock_guard<std::mutex> lk(lock_);
        const bool on = enabled != 0;
        if (hudlessUi_ == on) {
            return;
        }
        hudlessUi_ = on;
        uiRegisteredForPresent_ = false;
        if (!on && registeredUi_ != nullptr) {
            needUiUnregister_ = true;
        }
        applied_ = false;
        syncUiResourceLocked(runtime());
        dbgLogWarn("fsr-fg: hudless ui composition %s",
                   on ? "on (present = world layer, ui composed by sdk)" : "off (present = world + ui)");
    }

    bool uiCompositionReady() override {
        std::lock_guard<std::mutex> lk(lock_);
        if (retirementFailed_ || !uiTexture_ || !swapCtx_ || !proxyChain_ || !runtime().fn.Configure)
            return false;
        if (!hudlessUi_) return true;
        syncUiResourceLocked(runtime());
        return uiRegisteredForPresent_ && registeredUi_ == uiTexture_ && !needUiUnregister_ && !uiFailLogged_;
    }

    void setFrameConstants(const mcdr_fg_constants& constants) override {
        std::lock_guard<std::mutex> lk(lock_);
        reset_ = reset_ || constants.resetHistory != 0;
        jitterX_ = constants.jitterX;
        jitterY_ = constants.jitterY;
        mvScaleX_ = constants.mvScaleX;
        mvScaleY_ = constants.mvScaleY;
        hasConstants_ = true;
    }

    FgInputAcceptance tagFrameInputs(const FgFrameInputs& inputs) override {
        std::lock_guard<std::mutex> lk(lock_);
        if (!inputs.resourceLease || retirementFailed_) return FgInputAcceptance::Failed;
        if (desiredMode_ == 0) return FgInputAcceptance::Skipped;
        if (!inputs.pColorOutput || !inputs.pDepth || !inputs.pMotionVectors || !inputs.pUi ||
            inputs.renderWidth != inputs.displayWidth || inputs.renderHeight != inputs.displayHeight ||
            inputs.displayWidth != static_cast<int32_t>(displayWidth_) ||
            inputs.displayHeight != static_cast<int32_t>(displayHeight_) ||
            !std::isfinite(inputs.cameraNear) || !(inputs.cameraNear > 0) ||
            !std::isfinite(inputs.cameraFar) || !(inputs.cameraFar > inputs.cameraNear) ||
            !std::isfinite(inputs.cameraVerticalFov) || !(inputs.cameraVerticalFov > 0) ||
            !std::isfinite(inputs.frameTimeDeltaMs) || !(inputs.frameTimeDeltaMs > 0) ||
            !std::isfinite(inputs.viewSpaceToMeters) || !(inputs.viewSpaceToMeters > 0))
            return FgInputAcceptance::Failed;
        for (void* resource : {inputs.pColorOutput, inputs.pDepth, inputs.pMotionVectors, inputs.pUi}) {
            const auto description = static_cast<ID3D12Resource*>(resource)->GetDesc();
            if (description.Width != displayWidth_ || description.Height != displayHeight_)
                return FgInputAcceptance::Failed;
        }
        frameLease_ = inputs.resourceLease;
        if (!tagLogged_) {
            tagLogged_ = true;
            dbgLogWarn("fsr-fg: first tag color=%p depth=%p mv=%p render=%dx%d display=%dx%d cmd=%p",
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
        renderWidth_ = static_cast<uint32_t>(inputs.renderWidth);
        renderHeight_ = static_cast<uint32_t>(inputs.renderHeight);
        cameraNear_ = inputs.cameraNear;
        cameraFar_ = inputs.cameraFar;
        cameraFovVertical_ = inputs.cameraVerticalFov;
        frameTimeDeltaMs_ = inputs.frameTimeDeltaMs;
        viewSpaceToMeters_ = inputs.viewSpaceToMeters;
        std::copy_n(inputs.cameraPosition, 3, cameraPosition_);
        std::copy_n(inputs.cameraUp, 3, cameraUp_);
        std::copy_n(inputs.cameraRight, 3, cameraRight_);
        std::copy_n(inputs.cameraForward, 3, cameraForward_);
        syncUiResourceLocked(runtime());
        if (!hudlessUi_ || needUiUnregister_ || registeredUi_ != inputs.pUi)
            return FgInputAcceptance::Failed;
        applied_ = false;
        applyFrameLocked(inputs.cmdList);
        return applied_ ? FgInputAcceptance::Accepted : FgInputAcceptance::Failed;
    }

    bool historyAcceptedAfterPresent() const override {
        std::lock_guard<std::mutex> lock(lock_);
        return historyAccepted_ && !retirementFailed_;
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
            if (!destroyContextsLocked(rt)) return 0;
        }
        ComPtr<ID3D12Fence> inputFence;
        if (queue == nullptr || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&inputFence)))) {
            dbgLogError("fsr-fg: cannot create source-reader retirement fence");
            return 0;
        }

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

        const uint32_t bbFormat = ffxApiGetSurfaceFormatDX12(format);
        ffxCreateContextDescFrameGenerationHudless hudlessDesc{};
        hudlessDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hudlessDesc.hudlessBackBufferFormat = bbFormat;
        fgVersion.header.pNext = &hudlessDesc.header;

        ffxCreateContextDescFrameGeneration fgDesc{};
        fgDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        fgDesc.header.pNext = &backendDesc.header;
        fgDesc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
            FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
        fgDesc.displaySize.width = static_cast<uint32_t>(width > 0 ? width : 1);
        fgDesc.displaySize.height = static_cast<uint32_t>(height > 0 ? height : 1);
        fgDesc.maxRenderSize = fgDesc.displaySize;
        fgDesc.backBufferFormat = bbFormat;

        ffxContext fgCtx = nullptr;
        const ffxReturnCode_t fgRc = rt.fn.CreateContext(&fgCtx, &fgDesc.header, nullptr);
        if (fgRc != FFX_API_RETURN_OK || fgCtx == nullptr) {
            swapCtx_ = swapCtx;
            fgCtx_ = fgCtx;
            failedChain_ = chain4;
            proxyChain_ = chain4;
            chain4->Release();
            (void)destroyContextsLocked(rt);
            dbgLogWarn("fsr-fg: frame generation context failed 0x%X", static_cast<unsigned>(fgRc));
            return 0;
        }

        swapCtx_ = swapCtx;
        fgCtx_ = fgCtx;
        registeredUi_ = nullptr;
        uiRegisteredForPresent_ = false;
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
            *outPresentChain = reinterpret_cast<IDXGISwapChain*>(chain4);
        } else {
            chain4->Release();
        }

        dbgLogInfo("fsr-fg: wrapped display=%ux%u format=%d proxy=%p ctx(swap=%p fg=%p)",
                   displayWidth_, displayHeight_, static_cast<int>(format),
                   static_cast<void*>(chain4), swapCtx, fgCtx);
        gameQueue_ = queue;
        inputFence_ = std::move(inputFence);
        inputFenceValue_ = 0;
        retirementFailed_ = false;
        return 1;
    }

    void unwrapSwapChain() override {
        (void)tryUnwrapSwapChain();
    }

    bool tryUnwrapSwapChain() override {
        FsrFgRuntime& rt = runtime();
        std::lock_guard<std::mutex> lk(lock_);
        return destroyContextsLocked(rt);
    }

private:
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

    void collectInputReadsLocked() {
        if (!inputFence_ || retirementFailed_) return;
        const UINT64 completed = inputFence_->GetCompletedValue();
        if (completed == UINT64_MAX) return;
        std::erase_if(inputReads_, [completed](const InputRead& read) { return read.value <= completed; });
    }

    bool destroyContextsLocked(FsrFgRuntime& rt) {
        if (fgCtx_ != nullptr && rt.fn.Configure != nullptr) {
            ffxConfigureDescFrameGeneration off{};
            off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            off.swapChain = proxyChain_;
            off.frameGenerationEnabled = false;
            if (rt.fn.Configure(&fgCtx_, &off.header) != FFX_API_RETURN_OK) {
                dbgLogError("fsr-fg: disable/drain failed; preserving contexts and input leases");
                return false;
            }
        }
        if (swapCtx_ != nullptr && rt.fn.Dispatch != nullptr) {
            ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
            wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
            if (rt.fn.Dispatch(&swapCtx_, &wait.header) != FFX_API_RETURN_OK) {
                dbgLogError("fsr-fg: WaitForPresents failed; preserving contexts and input leases");
                return false;
            }
        }
        if (fgCtx_ != nullptr) {
            if (rt.fn.DestroyContext(&fgCtx_, nullptr) != FFX_API_RETURN_OK) return false;
            fgCtx_ = nullptr;
        }
        if (swapCtx_ != nullptr) {
            if (rt.fn.DestroyContext(&swapCtx_, nullptr) != FFX_API_RETURN_OK) return false;
            swapCtx_ = nullptr;
        }
        gDeviceSupported.store(false);
        enabledInFg_ = false;
        applied_ = false;
        loggedRunning_ = false;
        registeredUi_ = nullptr;
        needUiUnregister_ = false;
        uiRegisteredForPresent_ = false;
        if (proxyChain3_ != nullptr) {
            proxyChain3_->Release();
            proxyChain3_ = nullptr;
        }
        proxyChain_ = nullptr;
        failedChain_.Reset();
        inputReads_.clear();
        frameLease_.reset();
        registeredUiHold_.Reset();
        registeredUiLease_.reset();
        gameQueue_.Reset();
        inputFence_.Reset();
        if (inputFenceEvent_) {
            CloseHandle(inputFenceEvent_);
            inputFenceEvent_ = nullptr;
        }
        hudlessColor_ = depth_ = motionVectors_ = nullptr;
        cmdList_ = nullptr;
        return true;
    }

    mutable std::mutex lock_;
    ffxContext swapCtx_ = nullptr;
    ffxContext fgCtx_ = nullptr;
    IDXGISwapChain* proxyChain_ = nullptr;
    IDXGISwapChain3* proxyChain3_ = nullptr;
    ComPtr<IDXGISwapChain> failedChain_;

    void* hudlessColor_ = nullptr;
    struct InputRead {
        UINT64 value = 0;
        std::shared_ptr<void> frame, uiLease;
        ComPtr<ID3D12Resource> ui;
    };
    std::vector<InputRead> inputReads_;
    std::shared_ptr<void> frameLease_, uiLease_, registeredUiLease_;
    ComPtr<ID3D12Resource> uiTextureHold_, registeredUiHold_;
    ComPtr<ID3D12CommandQueue> gameQueue_;
    ComPtr<ID3D12Fence> inputFence_;
    HANDLE inputFenceEvent_ = nullptr;
    UINT64 inputFenceValue_ = 0;
    bool retirementFailed_ = false;
    uint64_t preparedFrame_ = UINT64_MAX;
    bool preparedReset_ = false;
    bool historyAccepted_ = false;
    std::atomic<uint64_t> completedDispatchFrame_{UINT64_MAX};
    std::atomic<bool> completedDispatchSucceeded_{false};
    void* uiTexture_ = nullptr;
    void* registeredUi_ = nullptr;
    bool hudlessUi_ = false;
    bool uiRegisteredForPresent_ = false;
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
    float frameTimeDeltaMs_ = 0;
    float cameraNear_ = 0;
    float cameraFar_ = 0;
    float cameraFovVertical_ = 0;
    float viewSpaceToMeters_ = 0;
    float cameraPosition_[3]{}, cameraUp_[3]{}, cameraRight_[3]{}, cameraForward_[3]{};
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
    std::atomic<int32_t> dispatchFailures_{0};
    std::atomic<uint64_t> appPresents_{0};
    std::atomic<uint64_t> dispatchCount_{0};
    std::atomic<uint64_t> generatedFrames_{0};
    bool everEnabled_ = false;
    bool tagLogged_ = false;
    uint64_t presentsAfterEnable_ = 0;
};

}

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
    out.deviceSupported = gDeviceSupported.load() ? 1 : 0;
    out.chainCompatible = 1;
    out.needsReflex = 0;
    out.needsHudless = 1;
    out.needsUiResource = 1;
    out.transitionsInputs = 1;
    out.maxGeneratedFrames = 2;
    out.latencyProvider = MCDR_FG_LATENCY_ANTILAG2;
    out.reserved = 0;
    out.available = (loaded && kWrapWired) ? 1 : 0;

    if (!loaded) {
        writeCapsNote(out.note, sizeof(out.note), problem.empty() ? "fsr-fg runtime missing" : problem.c_str());
    } else if (!kWrapWired) {
        writeCapsNote(out.note, sizeof(out.note), "FSR-FG gated off in this build (kWrapWired=false); see known_bugs.md P306ac");
    } else {
        writeCapsNote(out.note, sizeof(out.note), "FSR-FG ready");
    }

    static std::atomic<bool> loggedCaps{false};
    if (!loggedCaps.exchange(true)) {
        dbgLogInfo("fsr-fg caps: runtime=%d device=%d chainCompat=%d available=%d latency=%d note=\"%s\"",
                   out.runtimeLoaded, out.deviceSupported, out.chainCompatible, out.available, out.latencyProvider,
                   out.note);
    }
}

std::unique_ptr<IFrameGenProvider> makeFsrFgProvider() {
    return std::make_unique<FsrFgFrameGenProvider>();
}

}
