module;

#include "_d3d12_module_prelude.h"

#include "mcdr_abi.h"
#include "mcdr_dlss_impl.h"

#include <cstdio>
#include <cmath>
#include <cwchar>
#include <string_view>

module mcdr.d3d12.device;

namespace d3d12_mc {
bool releaseProviderWithWait(CommandContext* ctx);

namespace {

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

void fillDlssCaps(mcdr_sr_caps& out) {
    char buf[192] = {};
    const bool up = queryDlss(buf, static_cast<int>(sizeof(buf))) == 1;
    out.available = up ? 1 : 0;
    out.coupledDenoise = 0;
    out.needsExposure = 0;
    out.takesMasks = 0;
    out.externalHeap = 1;
    out.jitterPhaseCount = 8;
    out.minRenderWidth = 320;
    out.minRenderHeight = 180;
    out.maxRenderWidth = 7680;
    out.maxRenderHeight = 4320;
    out.nativeAA = 1;
    copyNote(out.note, sizeof(out.note), up ? buf : "dlss unavailable");
}

mcdr_dlss_input toDlssInput(const mcdr_sr_dispatch& d) {
    mcdr_dlss_input in{};
    in.renderWidth = d.renderWidth;
    in.renderHeight = d.renderHeight;
    in.targetWidth = d.displayWidth;
    in.targetHeight = d.displayHeight;
    in.jitterOffsetX = d.jitterOffsetX;
    in.jitterOffsetY = d.jitterOffsetY;
    in.mvScaleX = d.motionVectorScaleX != 0.0f ? d.motionVectorScaleX : 1.0f;
    in.mvScaleY = d.motionVectorScaleY != 0.0f ? d.motionVectorScaleY : 1.0f;
    in.preExposure = d.preExposure != 0.0f ? d.preExposure : 1.0f;
    in.exposureScale = 1.0f;
    in.exposureMode = d.pExposure ? MCDR_DLSS_EXPOSURE_AUTO : MCDR_DLSS_EXPOSURE_NONE;
    in.alphaUpscaling = 0;
    in.nearZ = d.cameraNear;
    in.farZ = d.cameraFar;
    in.depthInverted = 1;
    in.reset = d.reset;
    in.depthFormatOrdinal = d.depthFormatOrdinal;
    in.pInColor = static_cast<mcdr_object*>(d.pColorInput);
    in.pInDepth = static_cast<mcdr_object*>(d.pDepth);
    in.pInMotionVectors = static_cast<mcdr_object*>(d.pMotionVectors);
    in.pInOutput = static_cast<mcdr_object*>(d.pColorOutput);
    in.pInExposure = static_cast<mcdr_object*>(d.pExposure);
    return in;
}

int32_t gSrPicked = MCDR_SR_BACKEND_NONE;
int32_t gSrActive = MCDR_SR_BACKEND_NONE;
std::string gSrNote;
std::unique_ptr<IUpscaleProvider> gSrProvider;

struct SrCreationSignature {
    int32_t backend = MCDR_SR_BACKEND_NONE;
    int32_t renderWidth = 0;
    int32_t renderHeight = 0;
    int32_t displayWidth = 0;
    int32_t displayHeight = 0;
};

SrCreationSignature gSrCreationSignature;

struct PreparedSrProvider {
    uint64_t token = 0;
    int32_t backend = MCDR_SR_BACKEND_NONE;
    mcdr_sr_dispatch description{};
    mcdr_raster_camera camera{};
    std::unique_ptr<IUpscaleProvider> provider;
    uint64_t completedQueueSubmission = 0;
    CommandContext* creationContext = nullptr;
    uint64_t creationFence = 0;
    void* dlssCandidate = nullptr;
    bool abandoned = false;
    std::vector<std::shared_ptr<void>> inputLeases;
};

std::unique_ptr<PreparedSrProvider> gPreparedSrProvider;
uint64_t gSrNextPreparationToken = 1;
std::vector<void*> gRetiredDlssPreparations;

uint64_t gSrEvalOk = 0;
uint64_t gSrEvalFail = 0;
int32_t gSrLastRenderWidth = 0;
int32_t gSrLastRenderHeight = 0;
int32_t gSrLastDisplayWidth = 0;
int32_t gSrLastDisplayHeight = 0;

class DlssSrProvider final : public IUpscaleProvider {
public:
    SrBackendId backend() const override { return SrBackendId::Dlss; }

    std::string failureText() const override { return dlssLastFailureText(); }

    void fillCaps(mcdr_sr_caps& out) const override { fillDlssCaps(out); }

    bool initialize(CommandContext* ctx, const mcdr_sr_dispatch& seed) override {
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
        mMaxRenderWidth = renderWidth;
        mMaxRenderHeight = renderHeight;
        mDisplayWidth = displayWidth;
        mDisplayHeight = displayHeight;
    }

    bool evaluate(CommandContext* ctx, const mcdr_sr_dispatch& desc) override {
        return applyDlssToTexture(ctx, toDlssInput(desc)) > 0;
    }

    void shutdown() override {
    }

private:
    int32_t mMaxRenderWidth = 0;
    int32_t mMaxRenderHeight = 0;
    int32_t mDisplayWidth = 0;
    int32_t mDisplayHeight = 0;
};

std::unique_ptr<IUpscaleProvider> makeProvider(SrBackendId id) {
    switch (id) {
        case SrBackendId::Dlss:
            return std::make_unique<DlssSrProvider>();
        case SrBackendId::Fsr:
            return makeFsrProvider();
        case SrBackendId::XeSS:
            return makeXessProvider();
        default:
            return nullptr;
    }
}

bool replaceProvider(CommandContext* ctx, int32_t backendId, const mcdr_sr_dispatch& seed);

bool ensureProvider(CommandContext* ctx, int32_t backendId, const mcdr_sr_dispatch& seed) {
    if (gSrProvider != nullptr && gSrActive == backendId) {
        return true;
    }
    const int32_t previous = gSrActive;
    if (!replaceProvider(ctx, backendId, seed)) return false;
    if (previous != MCDR_SR_BACKEND_NONE)
        dbgLogInfo("SR provider switch %s -> %s", nameOf(previous), nameOf(backendId));
    return true;
}

bool replaceProvider(CommandContext* ctx, int32_t backendId, const mcdr_sr_dispatch& seed) {
    std::unique_ptr<IUpscaleProvider> candidate = makeProvider(toId(backendId));
    if (candidate == nullptr) {
        gSrNote = "backend not implemented";
        return false;
    }
    if (!candidate->initialize(ctx, seed)) {
        const std::string problem = candidate->failureText();
        candidate->shutdown();
        gSrNote = std::string(nameOf(backendId)) + " initialize failed";
        if (!problem.empty()) gSrNote += ": " + problem;
        return false;
    }
    if (gSrProvider != nullptr) {
        if (!releaseProviderWithWait(ctx)) {
            candidate->shutdown();
            return false;
        }
    }
    gSrActive = backendId;
    gSrProvider = std::move(candidate);
    gSrCreationSignature = {
        backendId,
        seed.renderWidth,
        seed.renderHeight,
        seed.displayWidth,
        seed.displayHeight,
    };
    dbgLogInfo("SR provider ready: %s (render %dx%d display %dx%d)", nameOf(backendId),
               seed.renderWidth, seed.renderHeight, seed.displayWidth, seed.displayHeight);
    return true;
}

bool retirePreparedSrProvider(PreparedSrProvider& prepared) {
    if (prepared.creationContext != nullptr) {
        if (prepared.creationContext->submissionFailed.load() ||
            (prepared.creationFence > 0 &&
                !waitForFenceValue(prepared.creationContext, prepared.creationFence,
                    5'000'000'000ULL).has_value())) return false;
        destroyCommandEncoder(prepared.creationContext);
        prepared.creationContext = nullptr;
    }
    if (prepared.dlssCandidate != nullptr) {
        if (mcdr_dlss_abort_sr(prepared.dlssCandidate) != 1) return false;
        prepared.dlssCandidate = nullptr;
    }
    if (prepared.provider != nullptr) {
        prepared.provider->shutdown();
        prepared.provider.reset();
    }
    return true;
}

int32_t abandonPreparedSrProvider(std::unique_ptr<PreparedSrProvider> prepared,
    int32_t result, uint64_t* token) {
    prepared->abandoned = true;
    if (!retirePreparedSrProvider(*prepared)) {
        *token = prepared->token;
        gPreparedSrProvider = std::move(prepared);
    }
    return result;
}

mcdr_sr_dispatch seedFromSizes(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                               int32_t displayHeight) {
    mcdr_sr_dispatch seed{};
    seed.renderWidth = renderWidth;
    seed.renderHeight = renderHeight;
    seed.displayWidth = displayWidth;
    seed.displayHeight = displayHeight;
    return seed;
}

}

int32_t srPickBackend(int32_t backendId) {
    if (gPreparedSrProvider != nullptr) {
        gSrNote = "SR backend selection is blocked by a pending preparation";
        return 0;
    }
    if (!validBackend(backendId)) {
        gSrNote = "unknown backend id";
        return 0;
    }
    gSrPicked = backendId;
    return 1;
}

int32_t srSelectedBackend(void) { return gSrPicked; }

int32_t srActiveBackend(void) { return gSrActive; }

void srFillCaps(int32_t backendId, mcdr_sr_caps& out) {
    out = mcdr_sr_caps{};
    out.backend = backendId;
    switch (static_cast<int>(backendId)) {
        case MCDR_SR_BACKEND_NONE:
            out.available = 1;
            out.nativeAA = 1;
            copyNote(out.note, sizeof(out.note), "off");
            return;
        case MCDR_SR_BACKEND_DLSS:
            fillDlssCaps(out);
            return;
        case MCDR_SR_BACKEND_FSR:
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
    if (gPreparedSrProvider != nullptr) {
        gSrNote = "SR creation is blocked by a pending preparation";
        return 0;
    }
    if (ctx == nullptr || !validBackend(backendId) || maxRenderWidth <= 0 || maxRenderHeight <= 0 ||
        displayWidth <= 0 || displayHeight <= 0) {
        gSrNote = "srCreateProvider: invalid context, backend or dimensions";
        return -1;
    }
    if (backendId == MCDR_SR_BACKEND_NONE) {
        return releaseProviderWithWait(ctx) ? 1 : 0;
    }
    const mcdr_sr_dispatch seed =
        seedFromSizes(maxRenderWidth, maxRenderHeight, displayWidth, displayHeight);
    if (gSrProvider != nullptr &&
        gSrCreationSignature.backend == backendId &&
        gSrCreationSignature.renderWidth == maxRenderWidth &&
        gSrCreationSignature.renderHeight == maxRenderHeight &&
        gSrCreationSignature.displayWidth == displayWidth &&
        gSrCreationSignature.displayHeight == displayHeight) {
        return 1;
    }
    if (gSrProvider != nullptr && gSrActive == backendId) {
        return replaceProvider(ctx, backendId, seed) ? 1 : 0;
    }
    if (!ensureProvider(ctx, backendId, seed)) {
        if (gSrNote.empty()) gSrNote = std::string(nameOf(backendId)) + " initialize failed";
        return -1;
    }
    return 1;
}

int32_t srResizeProvider(CommandContext* ctx, int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                         int32_t displayHeight) {
    if (renderWidth <= 0 || renderHeight <= 0 || displayWidth <= 0 || displayHeight <= 0) {
        return -1;
    }
    if (gSrProvider == nullptr) {
        return 0;
    }
    return srCreateProvider(ctx, gSrActive, renderWidth, renderHeight, displayWidth, displayHeight);
}

int32_t srPrepareProvider(CommandContext* ctx, const mcdr_sr_dispatch& description,
    int32_t dlssQuality, int32_t dlssPreset, uint64_t* token) {
    if (token == nullptr) return MCDR_SR_PREPARE_INVALID_ARGUMENT;
    *token = 0;
    for (auto retired = gRetiredDlssPreparations.begin(); retired != gRetiredDlssPreparations.end();) {
        if (mcdr_dlss_abort_sr(*retired) == 1) retired = gRetiredDlssPreparations.erase(retired);
        else ++retired;
    }
    if (ctx == nullptr || ctx->listOpen || !validBackend(description.backend) ||
        description.renderWidth <= 0 || description.renderHeight <= 0 ||
        description.displayWidth <= 0 || description.displayHeight <= 0) {
        gSrNote = "SR preparation requires a valid description and a closed command context";
        return MCDR_SR_PREPARE_INVALID_ARGUMENT;
    }
    if (gPreparedSrProvider != nullptr) {
        gSrNote = "SR preparation is waiting for the previous candidate to retire";
        return MCDR_SR_PREPARE_NOT_READY;
    }
    if (description.backend != MCDR_SR_BACKEND_NONE &&
        (description.renderWidth < 320 || description.renderHeight < 180)) {
        gSrNote = "SR preparation render size is below the 320x180 minimum";
        return MCDR_SR_PREPARE_INVALID_ARGUMENT;
    }
    const uint64_t previousSubmission = currentQueueFenceValue();
    if (previousSubmission > 0 &&
        !waitForQueueFenceValue(previousSubmission, 5'000'000'000ULL).has_value()) {
        gSrNote = "SR preparation queue fence timed out";
        return MCDR_SR_PREPARE_NOT_READY;
    }
    auto candidate = std::make_unique<PreparedSrProvider>();
    candidate->token = gSrNextPreparationToken++;
    if (candidate->token == 0) candidate->token = gSrNextPreparationToken++;
    candidate->backend = description.backend;
    candidate->description = description;
    if (description.pCamera != nullptr) {
        candidate->camera = *description.pCamera;
        candidate->description.pCamera = &candidate->camera;
    }
    candidate->completedQueueSubmission = previousSubmission;
    if (description.backend == MCDR_SR_BACKEND_DLSS) {
        const auto encoder = createCommandEncoder();
        if (!encoder) {
            gSrNote = "DLSS preparation command context creation failed";
            return MCDR_SR_PREPARE_REJECTED;
        }
        candidate->creationContext = static_cast<CommandContext*>(*encoder);
        if (!beginCommandList(candidate->creationContext)) {
            destroyCommandEncoder(candidate->creationContext);
            gSrNote = "DLSS preparation command list creation failed";
            return MCDR_SR_PREPARE_REJECTED;
        }
        mcdr_dlss_input input = toDlssInput(description);
        int32_t resourceStates[5]{};
        uint32_t resourceIndex = 0;
        bool validResources = true;
        for (mcdr_object** field : {&input.pInColor, &input.pInDepth,
            &input.pInMotionVectors, &input.pInOutput, &input.pInExposure}) {
            auto* object = static_cast<Dx12Object*>(*field);
            if (object == nullptr && field == &input.pInExposure) continue;
            if (object == nullptr || object->resource == nullptr) {
                validResources = false;
                break;
            }
            auto lease = retainObjectLease(object);
            if (!lease) {
                validResources = false;
                break;
            }
            candidate->inputLeases.push_back(std::move(lease));
            retainObjectForRecording(candidate->creationContext, object);
            resourceStates[resourceIndex] = static_cast<int32_t>(resourceSubresourceState(
                ctx, object->resource.Get(), 0, D3D12_RESOURCE_STATE_COMMON));
            *field = reinterpret_cast<mcdr_object*>(object->resource.Get());
            ++resourceIndex;
        }
        const int32_t created = validResources ? mcdr_dlss_prepare_sr(
            candidate->creationContext->commandList, &input, description.pCamera, dlssQuality, dlssPreset,
            resourceStates, &candidate->dlssCandidate) : MCDR_SR_PREPARE_INVALID_ARGUMENT;
        const auto ended = endCommandList(candidate->creationContext);
        if (!ended) {
            gSrNote = "DLSS preparation command list close failed";
            return abandonPreparedSrProvider(std::move(candidate), MCDR_SR_PREPARE_REJECTED, token);
        }
        const auto submitted = submitCommandList(candidate->creationContext);
        if (submitted) candidate->creationFence = *submitted;
        if (!submitted || candidate->creationContext->submissionFailed.load() ||
            (candidate->creationFence > 0 && !waitForFenceValue(candidate->creationContext,
                candidate->creationFence, 5'000'000'000ULL).has_value())) {
            candidate->abandoned = true;
            *token = candidate->token;
            gPreparedSrProvider = std::move(candidate);
            gSrNote = "DLSS preparation command completion is unknown; resources remain retained";
            return MCDR_SR_PREPARE_COMPLETION_UNKNOWN;
        }
        destroyCommandEncoder(candidate->creationContext);
        candidate->creationContext = nullptr;
        candidate->completedQueueSubmission = currentQueueFenceValue();
        if (created != MCDR_SR_PREPARE_READY) {
            gSrNote = created == MCDR_SR_PREPARE_NOT_READY
                ? "DLSS preparation is waiting for real frame inputs or a free viewport"
                : "DLSS SDK rejected feature preparation; applied feature remains retained";
            return abandonPreparedSrProvider(std::move(candidate), created, token);
        }
    }
    if (description.backend != MCDR_SR_BACKEND_NONE) {
        candidate->provider = makeProvider(toId(description.backend));
        if (candidate->provider == nullptr) {
            gSrNote = "SR preparation backend is unavailable";
            return abandonPreparedSrProvider(std::move(candidate), MCDR_SR_PREPARE_REJECTED, token);
        }
        if (!candidate->provider->initialize(ctx, description)) {
            gSrNote = candidate->provider->failureText();
            return abandonPreparedSrProvider(std::move(candidate), MCDR_SR_PREPARE_REJECTED, token);
        }
    }
    *token = candidate->token;
    gPreparedSrProvider = std::move(candidate);
    return MCDR_SR_PREPARE_READY;
}

int32_t srCommitPreparedProvider(uint64_t token) {
    if (gPreparedSrProvider == nullptr || gPreparedSrProvider->token != token) return -1;
    if (currentQueueFenceValue() != gPreparedSrProvider->completedQueueSubmission) {
        gSrNote = "SR preparation became stale after a queue submission";
        return 0;
    }
    auto prepared = std::move(gPreparedSrProvider);
    if (prepared->dlssCandidate != nullptr) {
        void* retiredDlss = nullptr;
        mcdr_dlss_commit_sr(prepared->dlssCandidate, &retiredDlss);
        prepared->dlssCandidate = nullptr;
        if (retiredDlss != nullptr && mcdr_dlss_abort_sr(retiredDlss) != 1)
            gRetiredDlssPreparations.push_back(retiredDlss);
    }
    auto retired = std::move(gSrProvider);
    gSrProvider = std::move(prepared->provider);
    gSrActive = prepared->backend;
    gSrPicked = prepared->backend;
    gSrCreationSignature = {
        prepared->backend,
        prepared->description.renderWidth,
        prepared->description.renderHeight,
        prepared->description.displayWidth,
        prepared->description.displayHeight,
    };
    if (retired != nullptr) retired->shutdown();
    gSrNote.clear();
    return 1;
}

int32_t srAbortPreparedProvider(uint64_t token) {
    if (gPreparedSrProvider == nullptr || gPreparedSrProvider->token != token) return -1;
    gPreparedSrProvider->abandoned = true;
    if (!retirePreparedSrProvider(*gPreparedSrProvider)) return 0;
    gPreparedSrProvider.reset();
    return 1;
}

int32_t srEvaluateFrame(CommandContext* ctx, const mcdr_sr_dispatch& desc) {
    if (gPreparedSrProvider != nullptr && !gPreparedSrProvider->abandoned) {
        gSrNote = "SR evaluation is blocked by a pending preparation";
        return -1;
    }
    if (!std::isfinite(desc.preExposure) || desc.preExposure < 0.0f ||
        (desc.preExposure > 0.0f && !std::isfinite(1.0f / desc.preExposure))) {
        gSrNote = "srEvaluateFrame: invalid preExposure";
        return -1;
    }
    if (ctx == nullptr || !ctx->listOpen || ctx->commandList == nullptr) {
        gSrNote = "srEvaluateFrame: command list is not open";
        return -1;
    }
    int32_t backendId = static_cast<int32_t>(desc.backend);
    if (backendId == MCDR_SR_BACKEND_NONE) {
        backendId = gSrPicked;
    }
    if (!validBackend(backendId)) {
        gSrNote = "srEvaluateFrame: invalid backend " + std::to_string(backendId);
        return -1;
    }
    if (backendId == MCDR_SR_BACKEND_NONE) {
        return 0;
    }
    if (!ensureProvider(ctx, backendId, desc)) {
        if (gSrNote.empty()) gSrNote = std::string(nameOf(backendId)) + " initialize failed";
        dbgLogError("srEvaluateFrame: selected backend=%s initialization failed: %s",
                    nameOf(backendId), gSrNote.c_str());
        return -1;
    }
    wchar_t eventName[96] = {};
    swprintf_s(eventName, L"SR:%hs %dx%d->%dx%d", nameOf(backendId), desc.renderWidth,
               desc.renderHeight, desc.displayWidth, desc.displayHeight);
    ctx->commandList->BeginEvent(0, eventName, static_cast<UINT>(wcslen(eventName)));
    if (desc.pExposure) retainObjectForRecording(ctx, desc.pExposure);
    const bool ok = gSrProvider->evaluate(ctx, desc);
    ctx->commandList->EndEvent();
    gSrLastRenderWidth = desc.renderWidth;
    gSrLastRenderHeight = desc.renderHeight;
    gSrLastDisplayWidth = desc.displayWidth;
    gSrLastDisplayHeight = desc.displayHeight;
    if (ok) {
        ++gSrEvalOk;
        gSrNote.clear();
    } else {
        ++gSrEvalFail;
        gSrNote.assign(nameOf(backendId)).append(" evaluate failed");
        const std::string problem = gSrProvider->failureText();
        if (!problem.empty()) gSrNote.append(": ").append(problem);
        dbgLogError("srEvaluateFrame: selected backend=%s evaluate failed render=%dx%d display=%dx%d: %s",
                    nameOf(backendId), desc.renderWidth, desc.renderHeight,
                    desc.displayWidth, desc.displayHeight, gSrNote.c_str());
    }
    restoreDrawHeap(ctx);
    if (!ok) {
        return -1;
    }
    gSrActive = backendId;
    return 1;
}

std::string srLastFailureText() {
    return gSrNote;
}

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

bool srReleaseProvider(void) {
    if (gPreparedSrProvider != nullptr) {
        if (srAbortPreparedProvider(gPreparedSrProvider->token) != 1) return false;
    }
    for (auto retired = gRetiredDlssPreparations.begin(); retired != gRetiredDlssPreparations.end();) {
        if (mcdr_dlss_abort_sr(*retired) == 1) retired = gRetiredDlssPreparations.erase(retired);
        else return false;
    }
    if (gSrProvider != nullptr) {
        const UINT64 queueValue = currentQueueFenceValue();
        if (queueValue > 0) {
            const auto waited = waitForQueueFenceValue(queueValue, 5'000'000'000ULL);
            if (!waited.has_value()) {
                dbgLogWarn("SR release: queue fence timed out");
                return false;
            }
        }
        gSrProvider->shutdown();
        gSrProvider.reset();
        gSrCreationSignature = {};
    }
    gSrActive = MCDR_SR_BACKEND_NONE;
    gSrPicked = MCDR_SR_BACKEND_NONE;
    return true;
}

bool releaseProviderWithWait(CommandContext* ctx) {
    if (gSrProvider == nullptr) {
        return true;
    }
    if (ctx != nullptr && ctx->fenceValue > 0) {
        const auto waited = waitForFenceValue(ctx, ctx->fenceValue, 5'000'000'000ULL);
        if (!waited.has_value()) {
            dbgLogWarn("SR release: command fence timed out");
            return false;
        }
    }
    gSrProvider->shutdown();
    gSrProvider.reset();
    gSrActive = MCDR_SR_BACKEND_NONE;
    gSrCreationSignature = {};
    return true;
}

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

}
