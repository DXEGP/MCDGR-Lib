module;

#include "_d3d12_module_prelude.h"

#include "mcdr_abi.h"

#include "mcdr_dlss_impl.h"
#include <optional>

module mcdr.d3d12.device;

import mcdr.render.oit;

namespace d3d12_mc {

namespace {

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

constexpr uint64_t ENSURE_RETRY_FRAMES = 30;

constexpr uint64_t USABLE_GRACE_FRAMES = 300;

constexpr uint64_t ROLLBACK_FRAMES = 60;

struct FgRegistry {
    std::mutex mutex;
    std::unique_ptr<IFrameGenProvider> provider;
    int32_t picked = MCDR_FG_BACKEND_NONE;
    bool forcedPick = false;
    int32_t applied = MCDR_FG_BACKEND_NONE;
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
    std::shared_ptr<void> uiLease;
    ComPtr<ID3D12Resource> uiTexture;
    struct RecordingUse {
        ComPtr<ID3D12Fence> fence;
        UINT64 value = 0;
        std::weak_ptr<void> recording;
        bool submitted = false;
    };
    std::vector<RecordingUse> recordings;
    int32_t uiComposition = 0;
    int32_t desiredMode = 0;
    int32_t desiredFrames = 1;
    int32_t chainMode = 0;
    bool rebuildPending = false;
    int32_t rebuilds = 0;
    uint64_t appFrameId = 0;
    uint64_t backendFrameId = 0;
    uint64_t presentId = 0;
    uint64_t lastTrackLog = 0;
    uint64_t ensureTryAt = 0;
    bool warnedNotUsable = false;
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
    if (use.recording.expired()) return true;
    const UINT64 completed = use.fence ? use.fence->GetCompletedValue() : 0;
    return use.submitted && completed != UINT64_MAX && completed >= use.value;
}

bool recordingsCompleteLocked(FgRegistry& s) {
    std::erase_if(s.recordings, recordingComplete);
    if (s.recordings.empty()) return true;
    s.lastReason = "FG input recording still live";
    return false;
}

std::atomic<int32_t> gChainOwnerSnapshot{MCDR_FG_CHAIN_APP_NATIVE};

bool validBackendId(int32_t backendId) {
    return backendId == MCDR_FG_BACKEND_NONE || backendId == MCDR_FG_BACKEND_DLSSG ||
           backendId == MCDR_FG_BACKEND_FSRFG || backendId == MCDR_FG_BACKEND_XESSFG;
}

void logStateChange(const FgRegistry& s, const char* what, int32_t before, int32_t after) {
    if (before == after) {
        return;
    }
    dbgLogInfo("[FG] %s %d -> %d at present=%llu (picked=%d applied=%d chain=%d rebuilds=%d)",
               what, before, after, static_cast<unsigned long long>(s.presentId), s.picked, s.applied,
               s.chainMode, s.rebuilds);
}

class DlssGFrameGenProvider final : public IFrameGenProvider {
public:
    FgBackendId backend() const override { return FgBackendId::DlssG; }

    void fillCaps(mcdr_fg_caps& out) const override { fillDlssGCaps(out); }

    bool setMode(const mcdr_fg_dispatch& desc) override {
        const int32_t frames = desc.numFrames < 1 ? 1 : desc.numFrames;
        desiredMode_ = desc.mode;
        desiredFrames_ = frames;
        return true;
    }

    void beforePresent() override {
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
        static_cast<void>(displayWidth);
        static_cast<void>(displayHeight);
    }

    void shutdown() override {
        mcdr_dlss_set_fg_mode(0, desiredFrames_ < 1 ? 1 : desiredFrames_);
        (void)mcdr_dlss_invalidate_inputs();
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
    bool historyAccepted_ = false;
    mcdr_fg_constants constants_{};
};

}

void fillDlssGCaps(mcdr_fg_caps& out) {
    out.backend = MCDR_FG_BACKEND_DLSSG;

    const bool dlssReady = mcdr_dlss_available() != 0;
    const bool streamline = mcdr_dlss_streamline_requested() != 0;
    const bool reflexReady = mcdr_reflex_available() != 0;

    out.runtimeLoaded = dlssReady ? 1 : 0;
    out.deviceSupported = streamline ? 1 : 0;
    out.chainCompatible = streamline ? 1 : 0;
    out.needsReflex = 1;
    out.needsUiResource = 1;
    out.needsHudless = 1;
    out.transitionsInputs = 0;
    out.maxGeneratedFrames = 5;
    out.reserved = 0;
    out.latencyProvider = MCDR_FG_LATENCY_REFLEX;

    const bool available = dlssReady && streamline && reflexReady;
    out.available = available ? 1 : 0;

    if (!dlssReady) {
        writeNote(out.note, sizeof(out.note), "DLSS runtime not loaded");
    } else if (!streamline) {
        writeNote(out.note, sizeof(out.note), "not on the Streamline path");
    } else if (!reflexReady) {
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
    if (s.provider != nullptr && s.applied != target &&
        (s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP || s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP)) {
        s.lastReason = "restart to switch FG";
        return 0;
    }
    if (s.provider != nullptr) {
        invalidateCameraLocked(s);
        if (!recordingsCompleteLocked(s) || !s.provider->waitForPreviousFrame() ||
            !s.provider->tryUnwrapSwapChain()) {
            s.rebuildPending = true;
            return 0;
        }
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

    mcdr_fg_caps caps = mcdr_fg_caps{};
    made->fillCaps(caps);
    if (caps.available == 0) {
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
    if (s.uiTexture != nullptr) {
        s.provider->setUiTexture(s.uiTexture.Get(), s.uiLease);
    }
    if (s.uiComposition != 0) {
        s.provider->setUiComposition(s.uiComposition);
    }
    if (s.desiredMode != 0) {
        mcdr_fg_dispatch replay{};
        replay.backend = target;
        replay.mode = s.desiredMode;
        replay.numFrames = s.desiredFrames;
        replay.reset = 1;
        s.provider->setMode(replay);
    }
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
        if (s.provider != nullptr && s.applied != desc.backend) {
            s.lastReason = "restart to switch FG";
            return 0;
        }
        if (!s.forcedPick) {
            s.picked = desc.backend;
        } else if (s.picked != desc.backend) {
            s.lastReason = "picked backend is forced; ignoring the facade's suggestion";
        }
    }
    if (desc.mode < 0 || desc.mode > 2) {
        return -1;
    }
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
    if (desc.mode == 0 && s.provider == nullptr) {
        s.lastResult = 0;
        s.chainMode = 0;
        s.rebuildPending = false;
        logStateChange(s, "chainMode", beforeChainMode, s.chainMode);
        return 1;
    }
    if (!s.frameStarted && s.provider == nullptr && ensureProviderLocked(s) == 0) {
        s.lastResult = 0;
        s.ensureTryAt = s.appFrameId + ENSURE_RETRY_FRAMES;
        return 0;
    }
    const bool ok = s.frameStarted || (s.provider && s.provider->setMode(desc));
    s.lastResult = ok ? 1 : 0;
    if (!ok) {
        return s.lastResult;
    }
    const bool wantOn = s.desiredMode != 0;
    const bool wantsWrap = s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG;
    const bool chainIsWrap = s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP ||
                             s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP;
    const bool chainFamilyMatches =
        (s.picked == MCDR_FG_BACKEND_FSRFG && s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP) ||
        (s.picked == MCDR_FG_BACKEND_XESSFG && s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP);
    const bool needWrap = wantOn && wantsWrap && (!chainIsWrap || !chainFamilyMatches);
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
    if (s.provider != nullptr && s.desiredMode != 0) {
        mcdr_fg_caps live{};
        s.provider->fillCaps(live);
        if (live.available != 0) {
            s.unavailableFrames = 0;
        } else if (++s.unavailableFrames >= ROLLBACK_FRAMES) {
            const std::string why = live.note[0] != '\0' ? std::string(live.note)
                                                         : std::string("backend reports unavailable");
            s.provider->shutdown();
            invalidateCameraLocked(s);
            s.desiredMode = 0;
            s.desiredFrames = 1;
            s.lastReason = why;
            s.unavailableFrames = 0;
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
        mcdr_fg_caps caps{};
        if (s.provider != nullptr) {
            s.provider->fillCaps(caps);
        } else if (s.picked == MCDR_FG_BACKEND_FSRFG) {
            fillFsrFgCaps(caps);
        } else if (s.picked == MCDR_FG_BACKEND_XESSFG) {
            fillXessFgCaps(caps);
        } else {
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
        if (!recordingsCompleteLocked(s) || !s.provider->waitForPreviousFrame() ||
            !s.provider->tryUnwrapSwapChain()) return false;
        s.provider->shutdown();
        s.provider.reset();
    }
    s.applied = MCDR_FG_BACKEND_NONE;
    s.uiTexture.Reset();
    s.uiLease.reset();
    s.hasConstants = false;
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
    return gChainOwnerSnapshot.load(std::memory_order_relaxed);
}

int32_t fgPickedBackend(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.picked;
}

bool fgWantsAppNativeChain(void) {
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
    if (s.provider != nullptr) {
        text += " presented=";
        text += std::to_string(s.provider->presented());
    }
    text += " latency=";
    switch (s.picked) {
        case MCDR_FG_BACKEND_DLSSG:
            text += "reflex";
            break;
        case MCDR_FG_BACKEND_FSRFG:
            text += "antilag2(no-dll)";
            break;
        case MCDR_FG_BACKEND_XESSFG:
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
    const int32_t desired = s.desiredMode & 0xFF;
    const int32_t chain = s.chainMode & 0xFF;
    const int32_t active = s.provider != nullptr ? 1 : 0;
    const int32_t rebuilds = s.rebuilds > 255 ? 255 : (s.rebuilds < 0 ? 0 : s.rebuilds);
    return desired | (chain << 8) | (active << 16) | (rebuilds << 24);
}

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
    if (object == nullptr) {
        return nullptr;
    }
    auto* wrapper = static_cast<Dx12Object*>(object);
    return wrapper != nullptr ? wrapper->resource.Get() : nullptr;
}

void fgSetUiTexture(void* object) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto lease = retainObjectLease(object);
    ComPtr<ID3D12Resource> resource = unwrapDx12Resource(object);
    if (s.provider != nullptr) {
        s.provider->setUiTexture(resource.Get(), lease);
    }
    s.uiTexture = std::move(resource);
    s.uiLease = std::move(lease);
}

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
}
