module;

#include "_d3d12_module_prelude.h"
#include <cstdint>
#include <memory>
#include <string>
#include <cstdio>
#include <cmath>
#include <numbers>
#include <array>
#include <cstring>
#include <optional>
#include "mcdr_abi.h"
#include "mcdr_graphics_recording.h"
#include "mcdr_oit_budget.h"
#include "vanilla_pass_scheduler.h"

module mcdr.render.vanilla;

import mcdr.error;
import mcdr.render.frameprep;

namespace d3d12_mc::vanilla {
struct DeclaredPassToken;
namespace {

struct WorldDepthSeedJournal : WorldDepthSeedJournalV1 {
    enum class DrawState { Idle, Prepared, PreparedEmpty, Confirmed, ConfirmedEmpty };
    uint64_t totalBudgetBytes = 0;
    uint32_t maxEvents = 0;
    uint64_t sequence = 0;
    bool closed = false;
    bool incomplete = false;
    WorldDepthSeedStatus status = WorldDepthSeedStatus::Ready;
    mcdr::graphics_recording::Observer observer;
    DrawState drawState = DrawState::Idle;
    std::shared_ptr<const void> pendingDraw;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::shared_ptr<mcdr::oit_budget::Reservation> eventBudget;

    bool append(WorldDepthSeedEventV1 event) {
        if (events.size() >= maxEvents) {
            incomplete = true;
            status = WorldDepthSeedStatus::BudgetExceeded;
            return false;
        }
        if (events.size() == events.capacity()) {
            const auto capacity = (std::min)(static_cast<size_t>(maxEvents),
                (std::max)(size_t{8}, events.capacity() * 2));
            auto allocation = mcdr::oit_budget::reserve(capacity * sizeof(WorldDepthSeedEventV1), totalBudgetBytes);
            if (!allocation) {
                status = WorldDepthSeedStatus::BudgetExceeded;
                return false;
            }
            events.reserve(capacity);
            eventBudget = std::move(allocation);
        }
        event.sequence = ++sequence;
        events.push_back(std::move(event));
        return true;
    }
};

struct WorldDepthSeedRecording {
    void* context = nullptr;
    Dx12Object* worldDepth = nullptr;
    std::shared_ptr<void> depthOwner;
    std::shared_ptr<const void> recordingIdentity;
    ComPtr<ID3D12Fence> fence;
    uint64_t submission = 0;
    bool depthCommandSeen = false;
    uint64_t clearCount = 0;
    uint64_t writeCount = 0;
    bool missedRegistration = false;
    bool closed = false;
    std::shared_ptr<WorldDepthSeedJournal> capture;
};

void depthSeedClear(void* opaqueState, CommandContext* context, Dx12Object* target,
                    uint32_t subresource, double depth, const D3D12_RECT* rect,
                    D3D12_CPU_DESCRIPTOR_HANDLE dsv);
void depthSeedDraw(void* opaqueState, CommandContext* context, Dx12Object* target,
                   Dx12Pipeline* pipeline, uint32_t commandFamily, uint64_t commandOrdinal,
                   const std::shared_ptr<const void>& packet);

struct FrameState {
    struct ResourceLayout {
        ComPtr<ID3D12Resource> resource;
        uint32_t subresources = 0;
        D3D12_RESOURCE_FLAGS flags{};
        std::optional<D3D12_HEAP_TYPE> rawHeap;
    };
    void* context = nullptr;
    FrameInput frame{};
    mcdr_vanilla_frame_input inputs{};
    mcdr_prepared_frame prepared{};
    std::vector<std::shared_ptr<void>> preparedLeases;
    bool preparedPublished = false;
    bool reconstructionAttempted = false;
    Phase phase = Phase::Idle;
    uint32_t executedPasses = 0;
    uint32_t nextPass = 1;
    scheduler::PassResourceScheduler resources;
    std::unordered_map<ID3D12Resource*, ComPtr<ID3D12Resource>> resourceLeases;
    DeclaredPassToken* activeToken = nullptr;
    std::vector<std::shared_ptr<void>> declaredTokens;
    std::shared_ptr<DeclaredPassToken> rasterInputToken;
    std::vector<scheduler::ResourceUse> rasterDeclarations;
    std::vector<PassResourceUse> rasterAttachmentUses;
    std::unordered_map<ID3D12Resource*, ResourceLayout> resourceLayouts;
    WorldDepthSeedRecording depthSeed;
};

static_assert(sizeof(FrameState) > 0);
static_assert(sizeof(mcdr_frame_identity) == 32);
static_assert(sizeof(mcdr_raster_camera) == 256);
static_assert(sizeof(mcdr_frame_settings) == 56);
static_assert(sizeof(mcdr_vanilla_frame_input) == 696);
static_assert(sizeof(mcdr_prepared_binding) == 16);
static_assert(sizeof(mcdr_prepared_frame) == 184);
static_assert(sizeof(mcdr_reconstruction_request) == 64);

void detachDepthSeedJournal(CommandContext* context) {
    if (!context) return;
    context->vanillaDepthSeedState = nullptr;
    context->vanillaDepthSeedClear = nullptr;
    context->vanillaDepthSeedDraw = nullptr;
}

bool matchesDepthSeedRecordingIdentity(const WorldDepthSeedJournal& journal, const CommandContext* context) {
    if (!context || journal.context != context || !context->vanillaFrameState ||
        journal.recordingIdentity != context->recordingIdentity ||
        journal.fence.Get() != context->fence.Get() || journal.submission != context->fenceValue + 1) return false;
    const auto& state = *static_cast<const FrameState*>(context->vanillaFrameState.get());
    return journal.frame.schemaVersion == state.frame.schemaVersion &&
        journal.frame.width == state.frame.width && journal.frame.height == state.frame.height &&
        journal.frame.worldEpoch == state.inputs.identity.worldEpoch &&
        journal.frame.realFrameId == state.inputs.identity.realFrameId &&
        journal.frame.configVersion == state.inputs.identity.configVersion &&
        journal.frame.resourceVersion == state.inputs.identity.resourceVersion;
}

bool ownsDepthSeedObserver(const WorldDepthSeedJournal& journal, const CommandContext* context) {
    const auto& bound = context->graphicsCommandObserver;
    auto owner = journal.observer.owner.lock();
    return owner && owner == bound.owner.lock() && journal.observer.prepare == bound.prepare &&
        journal.observer.confirm == bound.confirm && journal.observer.complete == bound.complete &&
        journal.observer.totalBudgetBytes == bound.totalBudgetBytes &&
        journal.observer.contentPolicy == bound.contentPolicy &&
        journal.observer.depthWriteTarget == bound.depthWriteTarget &&
        journal.observer.budgetExceeded == bound.budgetExceeded;
}

bool validDepthSeedRecording(const WorldDepthSeedJournal& journal, const CommandContext* context) {
    return matchesDepthSeedRecordingIdentity(journal, context) && !journal.closed &&
        !context->submissionFailed.load(std::memory_order_acquire) &&
        !context->recordingFailed.load(std::memory_order_acquire) && ownsDepthSeedObserver(journal, context);
}

int prepareDepthSeedDraw(void* owner, void* opaqueContext, const mcdr::graphics_recording::PacketFacts& facts,
                         std::shared_ptr<const void> packet) {
    auto& journal = *static_cast<WorldDepthSeedJournal*>(owner);
    const auto* context = static_cast<const CommandContext*>(opaqueContext);
    if (!validDepthSeedRecording(journal, context) || journal.drawState != WorldDepthSeedJournal::DrawState::Idle ||
        !packet || !facts.packetIdentity || !facts.readSetIdentity ||
        context->activeDepthTarget != journal.worldDepth || !context->currentPipeline ||
        facts.pipelineIdentity != reinterpret_cast<uintptr_t>(context->currentPipeline))
        return -1;
    journal.pendingDraw = std::move(packet);
    journal.drawState = facts.empty ? WorldDepthSeedJournal::DrawState::PreparedEmpty : WorldDepthSeedJournal::DrawState::Prepared;
    return 1;
}

int confirmDepthSeedDraw(void* owner, void* opaqueContext, const void* packet) {
    auto& journal = *static_cast<WorldDepthSeedJournal*>(owner);
    const auto* context = static_cast<const CommandContext*>(opaqueContext);
    using DrawState = WorldDepthSeedJournal::DrawState;
    if (!validDepthSeedRecording(journal, context) || journal.pendingDraw.get() != packet ||
        (journal.drawState != DrawState::Prepared && journal.drawState != DrawState::PreparedEmpty)) return -1;
    journal.drawState = journal.drawState == DrawState::PreparedEmpty ? DrawState::ConfirmedEmpty : DrawState::Confirmed;
    return 1;
}

int completeDepthSeedDraws(void* owner, void* opaqueContext, mcdr::graphics_recording::Completion completion) {
    auto& journal = *static_cast<WorldDepthSeedJournal*>(owner);
    const auto* context = static_cast<const CommandContext*>(opaqueContext);
    if (completion == mcdr::graphics_recording::Completion::Failed) {
        journal.pendingDraw.reset();
        journal.drawState = WorldDepthSeedJournal::DrawState::Idle;
        journal.incomplete = true;
        journal.status = WorldDepthSeedStatus::RecordingFailed;
        return 1;
    }
    return validDepthSeedRecording(journal, context) &&
        (journal.drawState == WorldDepthSeedJournal::DrawState::Idle ||
            journal.drawState == WorldDepthSeedJournal::DrawState::Confirmed ||
            journal.drawState == WorldDepthSeedJournal::DrawState::ConfirmedEmpty) ? 1 : -1;
}

void failDepthSeedRecording(CommandContext* context, WorldDepthSeedJournal& journal) {
    journal.incomplete = true;
    journal.status = WorldDepthSeedStatus::RecordingFailed;
    if (context) context->recordingFailed.store(true, std::memory_order_release);
}

void retireDepthSeedCapture(CommandContext* context, WorldDepthSeedJournal& journal,
                            WorldDepthSeedStatus status) noexcept {
    if (!matchesDepthSeedRecordingIdentity(journal, context) ||
        context->submissionFailed.load(std::memory_order_acquire) ||
        context->recordingFailed.load(std::memory_order_acquire) || !ownsDepthSeedObserver(journal, context)) {
        failDepthSeedRecording(context, journal);
        return;
    }
    if (ownsDepthSeedObserver(journal, context)) unbindGraphicsCommandObserver(context);
    cancelOpaqueSeedPacketCapture(journal.pendingDraw);
    for (const auto& event : journal.events) cancelOpaqueSeedPacketCapture(event.packet);
    journal.pendingDraw.reset();
    journal.drawState = WorldDepthSeedJournal::DrawState::Idle;
    journal.incomplete = true;
    journal.closed = true;
    journal.status = status;
    auto& recording = static_cast<FrameState*>(context->vanillaFrameState.get())->depthSeed;
    recording.closed = true;
    detachDepthSeedJournal(context);
}

void retireDepthSeedBudget(void* owner, void* opaqueContext) noexcept {
    retireDepthSeedCapture(static_cast<CommandContext*>(opaqueContext),
        *static_cast<WorldDepthSeedJournal*>(owner), WorldDepthSeedStatus::BudgetExceeded);
}

WorldDepthSeedStatus retireUnconsumedDepthSeedCapture(CommandContext* context, WorldDepthSeedJournal& journal) {
    if (journal.observer.contentPolicy == mcdr::graphics_recording::ContentPolicy::FactsOnly && !journal.pendingDraw) {
        retireDepthSeedCapture(context, journal, WorldDepthSeedStatus::Incomplete);
        return journal.status;
    }
    return cancelWorldDepthSeedCaptureV1(context);
}

void depthSeedClear(void* opaqueState, CommandContext* context, Dx12Object* target,
                    uint32_t subresource, double depth, const D3D12_RECT* rect,
                    D3D12_CPU_DESCRIPTOR_HANDLE dsv) {
    if (!opaqueState || !target) return;
    auto& recording = *static_cast<WorldDepthSeedRecording*>(opaqueState);
    if (recording.closed || (recording.worldDepth && recording.worldDepth != target)) return;
    recording.depthCommandSeen = true;
    if (recording.clearCount != UINT64_MAX) ++recording.clearCount;
    if (!recording.worldDepth) recording.missedRegistration = true;
    if (!recording.capture) return;
    auto& journal = *recording.capture;
    if (!validDepthSeedRecording(journal, context)) {
        failDepthSeedRecording(context, journal);
        return;
    }
    if (journal.worldDepth != target || subresource != 0 || dsv.ptr == 0) {
        failDepthSeedRecording(context, journal);
        return;
    }
    WorldDepthSeedEventV1 event;
    event.kind = WorldDepthSeedEventKind::Clear;
    event.depthTarget = target;
    event.subresource = subresource;
    event.clearDepth = depth;
    event.programGeneration = journal.programGeneration;
    event.sourceDsv = dsv;
    event.dsv.Format = target->dxgiFormat;
    event.dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    event.dsv.Texture2D.MipSlice = subresource;
    if (rect) {
        event.clearRect = *rect;
        event.hasClearRect = true;
    }
    try {
        if (!journal.append(std::move(event)))
            retireDepthSeedCapture(context, journal, WorldDepthSeedStatus::BudgetExceeded);
    } catch (const std::bad_alloc&) {
        retireDepthSeedCapture(context, journal, WorldDepthSeedStatus::BudgetExceeded);
    }
}

void depthSeedDraw(void* opaqueState, CommandContext* context, Dx12Object* target,
                   Dx12Pipeline* pipeline, uint32_t commandFamily, uint64_t commandOrdinal,
                   const std::shared_ptr<const void>& packet) {
    if (!opaqueState || !target) return;
    auto& recording = *static_cast<WorldDepthSeedRecording*>(opaqueState);
    if (recording.closed || (recording.worldDepth && recording.worldDepth != target)) return;
    if (pipeline && (!pipeline->depthWrite || !context->graphicsHasDepth)) return;
    recording.depthCommandSeen = true;
    if (recording.writeCount != UINT64_MAX) ++recording.writeCount;
    if (!recording.worldDepth) recording.missedRegistration = true;
    if (!recording.capture) return;
    auto& journal = *recording.capture;
    if (!validDepthSeedRecording(journal, context)) {
        failDepthSeedRecording(context, journal);
        return;
    }
    using DrawState = WorldDepthSeedJournal::DrawState;
    if (journal.pendingDraw != packet ||
        (journal.drawState != DrawState::Confirmed && journal.drawState != DrawState::ConfirmedEmpty) ||
        journal.worldDepth != target || !pipeline || !context->cachedPso || !packet ||
        context->activePassDsv.ptr == 0 ||
        !graphicsCommandPacketMatchesObserver(context, packet, journal.observer, commandFamily) ||
        context->currentPipeline != pipeline || context->cachedPso != pipeline->withDepth.Get()) {
        failDepthSeedRecording(context, journal);
        return;
    }
    const bool empty = journal.drawState == DrawState::ConfirmedEmpty;
    journal.pendingDraw.reset();
    journal.drawState = DrawState::Idle;
    if (empty) return;
    const auto depthRange = std::find_if(context->activeAttachmentRanges.begin(),
        context->activeAttachmentRanges.end(), [target](const auto& range) { return range.object == target; });
    if (depthRange == context->activeAttachmentRanges.end() || depthRange->firstSubresource != 0 ||
        depthRange->subresourceCount != 1) {
        failDepthSeedRecording(context, journal);
        return;
    }
    WorldDepthSeedEventV1 event;
    event.kind = WorldDepthSeedEventKind::Draw;
    event.depthTarget = target;
    event.pipelineIdentity = reinterpret_cast<uintptr_t>(pipeline);
    event.pso = context->cachedPso;
    event.shaderGeneration = pipeline->shaderGeneration;
    event.programGeneration = journal.programGeneration;
    event.subresource = depthRange->firstSubresource;
    event.subresourceCount = depthRange->subresourceCount;
    event.sourceDsv = context->activePassDsv;
    event.dsv.Format = target->dxgiFormat;
    event.dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    event.dsv.Texture2D.MipSlice = depthRange->firstSubresource;
    event.commandFamily = commandFamily;
    event.commandOrdinal = commandOrdinal;
    event.packet = packet;
    try {
        if (!journal.append(std::move(event))) {
            cancelOpaqueSeedPacketCapture(packet);
            retireDepthSeedCapture(context, journal, WorldDepthSeedStatus::BudgetExceeded);
        }
    } catch (const std::bad_alloc&) {
        cancelOpaqueSeedPacketCapture(packet);
        retireDepthSeedCapture(context, journal, WorldDepthSeedStatus::BudgetExceeded);
    }
}

FrameState& stateFor(void* rawContext) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context->vanillaFrameState) {
        context->vanillaFrameState = std::make_shared<FrameState>();
    }
    return *static_cast<FrameState*>(context->vanillaFrameState.get());
}

bool validIdentity(const mcdr_frame_identity& input) {
    return input.worldEpoch != 0 && input.realFrameId != 0 && input.configVersion != 0 &&
        input.resourceVersion != 0;
}

bool sameIdentity(const mcdr_frame_identity& left, const mcdr_frame_identity& right) {
    return left.worldEpoch == right.worldEpoch && left.realFrameId == right.realFrameId &&
        left.configVersion == right.configVersion && left.resourceVersion == right.resourceVersion;
}

bool validCamera(const mcdr_raster_camera& camera) {
    if ((camera.flags & ~0x303u) != 0 || ((camera.flags >> 8) & 3u) == 3u ||
        !std::isfinite(camera.jitterX) || !std::isfinite(camera.jitterY) ||
        !std::isfinite(camera.yaw) || !std::isfinite(camera.pitch)) return false;
    for (const auto origin : camera.origin) if (!std::isfinite(origin)) return false;
    for (const auto value : camera.projection) if (!std::isfinite(value)) return false;
    for (const auto value : camera.rasterProjection) if (!std::isfinite(value)) return false;
    for (const auto value : camera.viewRotation) if (!std::isfinite(value)) return false;
    if ((camera.flags & 1u) != 0) {
        if ((camera.flags & 2u) != 0 || ((camera.flags >> 8) & 3u) != 1u ||
            !std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) ||
            !std::isfinite(camera.verticalFovRadians) || camera.nearPlane <= 0.0f ||
            camera.farPlane <= camera.nearPlane || camera.verticalFovRadians <= 0.0f ||
            camera.verticalFovRadians >= std::numbers::pi_v<float>) return false;
    }
    return true;
}

bool validFrame(const mcdr_vanilla_frame_input& input) {
    const auto& settings = input.settings;
    if (input.schemaVersion != kFrameSchemaVersion || input.structSize != sizeof(input) ||
        !validIdentity(input.identity) || input.renderWidth == 0 || input.renderHeight == 0 ||
        input.displayWidth == 0 || input.displayHeight == 0 || input.renderWidth > INT32_MAX ||
        input.renderHeight > INT32_MAX || input.displayWidth > INT32_MAX || input.displayHeight > INT32_MAX ||
        (input.flags & ~1u) != 0 || input.reserved != 0 || input.depthConvention != 1 ||
        input.motionConvention != 1 || input.colorEncoding < 1 || input.colorEncoding > 3 ||
        !std::isfinite(input.frameDeltaMs) || input.frameDeltaMs < 0.0f ||
        !std::isfinite(input.preExposure) || input.preExposure <= 0.0f ||
        !std::isfinite(input.motionScaleX) || !std::isfinite(input.motionScaleY) ||
        !std::isfinite(input.viewSpaceToMeters) || input.viewSpaceToMeters <= 0.0f ||
        settings.antiAliasing > 4 || settings.reconstructionBackend > 3 ||
        settings.renderScalePercent < 25 || settings.renderScalePercent > 400 ||
        settings.jitterEnabled > 1 || settings.jitterSamples == 0 || settings.providerMasks > 1 ||
        !std::isfinite(settings.providerSharpness) || settings.providerSharpness < 0.0f ||
        settings.providerSharpness > 1.0f || !std::isfinite(settings.taaBlend) ||
        settings.taaBlend < 0.0f || settings.taaBlend > 1.0f || !std::isfinite(settings.taaSharpness) ||
        settings.taaSharpness < 0.0f || settings.taaSharpness > 1.0f ||
        settings.frameGenerationBackend > 3 || settings.frameGenerationMode > 2 ||
        settings.frameGenerationFrames > 5 || !validCamera(input.current)) return false;
    if ((input.flags & 1u) == 0) return input.previousRealFrameId == 0;
    return input.identity.realFrameId > 1 && input.previousRealFrameId == input.identity.realFrameId - 1 &&
        validCamera(input.previous);
}

bool validPass(const PassInput& input) {
    constexpr uint32_t knownPasses = static_cast<uint32_t>(Pass::Velocity) |
        static_cast<uint32_t>(Pass::Smaa) | static_cast<uint32_t>(Pass::Taa) |
        static_cast<uint32_t>(Pass::Sr) | static_cast<uint32_t>(Pass::Dlss) |
        static_cast<uint32_t>(Pass::Exposure) | static_cast<uint32_t>(Pass::ProviderMasks) |
        static_cast<uint32_t>(Pass::WorldOutput);
    return input.schemaVersion == kFrameSchemaVersion && input.pass != 0 &&
           (input.pass & (input.pass - 1)) == 0 && (input.pass & ~knownPasses) == 0 && input.width > 0 &&
           input.height > 0 && input.worldEpoch != 0 && input.realFrameId != 0 &&
           input.configVersion != 0 && input.resourceVersion != 0;
}

bool sameFrame(const FrameInput& frame, const PassInput& pass) {
    return frame.schemaVersion == pass.schemaVersion && frame.width == pass.width &&
           frame.height == pass.height && frame.worldEpoch == pass.worldEpoch &&
           frame.realFrameId == pass.realFrameId && frame.configVersion == pass.configVersion &&
           frame.resourceVersion == pass.resourceVersion;
}

void restoreState(void* rawContext, uint32_t effects) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (context == nullptr || context->commandList == nullptr) {
        return;
    }
    if ((effects & scheduler::kDescriptors) != 0) {
        restoreDrawHeap(context);
        context->lastSrvCount = 0;
        context->lastSrvCbvCount = 0;
        context->lastSrvViews.clear();
    }
    if ((effects & scheduler::kGraphicsState) != 0) {
        setShadingRateNow(context, 0x0);
        restoreGraphicsInputAssembly(context);
        context->cachedPso = nullptr;
        context->currentPipeline = nullptr;
    }
    if (effects != 0) invalidateGraphicsInputBindings(context, false);
}

PassInput frozenPass(const FrameState& state, uint32_t pass) {
    return {state.frame.schemaVersion, pass, state.frame.width, state.frame.height,
            state.frame.worldEpoch, state.frame.realFrameId, state.frame.configVersion,
            state.frame.resourceVersion};
}

Dx12Object* resourceObject(void* object) {
    auto* texture = static_cast<Dx12Object*>(object);
    if (texture && texture->kind == Dx12Object::Kind::TextureView) texture = texture->sourceTexture;
    return texture && (texture->kind == Dx12Object::Kind::Texture ||
                       texture->kind == Dx12Object::Kind::Buffer) && texture->resource ? texture : nullptr;
}

bool subresourceCount(const D3D12_RESOURCE_DESC& desc, uint32_t& count) {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        count = 1;
        return true;
    }
    D3D12_FEATURE_DATA_FORMAT_INFO format{};
    format.Format = desc.Format;
    if (!deviceContext().device || FAILED(deviceContext().device->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format))) || format.PlaneCount == 0) return false;
    const uint32_t arrays = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
    const uint64_t total = static_cast<uint64_t>(desc.MipLevels) * arrays * format.PlaneCount;
    if (total == 0 || total > UINT32_MAX) return false;
    count = static_cast<uint32_t>(total);
    return true;
}

bool compatibleResourceState(D3D12_RESOURCE_FLAGS flags, D3D12_HEAP_TYPE heapType,
                             uint32_t required, uint32_t finalState) {
    const auto compatible = [&](uint32_t state) {
        if (state == UINT32_MAX) return true;
        if (heapType == D3D12_HEAP_TYPE_UPLOAD) return state == D3D12_RESOURCE_STATE_GENERIC_READ;
        if (heapType == D3D12_HEAP_TYPE_READBACK) return state == D3D12_RESOURCE_STATE_COPY_DEST;
        if ((state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0 &&
            (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) return false;
        if ((state & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0 &&
            (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) return false;
        if ((state & (D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_DEPTH_WRITE)) != 0 &&
            (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) return false;
        if ((state & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) != 0 &&
            (flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) return false;
        return true;
    };
    return compatible(required) && compatible(finalState);
}

FrameState::ResourceLayout* resourceLayout(FrameState& state, ID3D12Resource* resource) {
    if (const auto found = state.resourceLayouts.find(resource); found != state.resourceLayouts.end())
        return &found->second;
    const auto desc = resource->GetDesc();
    uint32_t subresources = 0;
    if (!subresourceCount(desc, subresources)) return nullptr;
    return &state.resourceLayouts.try_emplace(resource,
        FrameState::ResourceLayout{ComPtr<ID3D12Resource>(resource), subresources, desc.Flags, std::nullopt}).first->second;
}

void retainNativeResource(CommandContext& context, FrameState& state, ID3D12Resource* resource) {
    const auto [lease, inserted] = state.resourceLeases.try_emplace(resource, resource);
    if (inserted) {
        context.recordingLeases[context.currentListSlot].push_back(
            std::make_shared<ComPtr<ID3D12Resource>>(lease->second));
    }
}

thread_local std::string postFailure;

int reject(const char* reason) {
    postFailure = reason;
    dbgLogError("VanillaFrameExecutor rejected phase: %s", postFailure.c_str());
    return -1;
}

}

struct DeclaredPassToken {
    FrameState* state = nullptr;
    void* context = nullptr;
    DeclaredPassToken* parent = nullptr;
    uint32_t pass = 0;
    uint32_t schedulePass = 0;
    uint32_t effects = 0;
    scheduler::ScheduleResult plan;
    std::vector<PassResourceUse> sources;
    std::vector<uint32_t> initialStates;
    bool finished = false;
    bool rasterInputs = false;
};

bool resourceUsesOverlap(const scheduler::ResourceUse& left, const scheduler::ResourceUse& right) {
    if (left.resource != right.resource) return false;
    if (left.subresource == UINT32_MAX || right.subresource == UINT32_MAX) return true;
    const uint64_t leftEnd = static_cast<uint64_t>(left.subresource) + left.subresourceCount;
    const uint64_t rightEnd = static_cast<uint64_t>(right.subresource) + right.subresourceCount;
    return left.subresource < rightEnd && right.subresource < leftEnd;
}

bool overlapsAncestors(const DeclaredPassToken* parent, const std::vector<scheduler::ResourceUse>& uses) {
    for (const auto* ancestor = parent; ancestor != nullptr; ancestor = ancestor->parent) {
        for (const auto& childUse : uses) {
            for (const auto& ancestorUse : ancestor->plan.uses) {
                if (resourceUsesOverlap(childUse, ancestorUse)) return true;
            }
        }
    }
    return false;
}

std::string lastFailureText() {
    return postFailure;
}

uint64_t frozenResourceVersion(void* context) {
    auto* recording = static_cast<CommandContext*>(context);
    if (!recording || !recording->vanillaFrameState || !recording->listOpen ||
        !recording->commandList || recording->submissionFailed.load()) return 0;
    const auto& state = *static_cast<FrameState*>(recording->vanillaFrameState.get());
    return state.context == context && (state.phase == Phase::Recording ||
        state.phase == Phase::OpaqueReady || state.phase == Phase::OpaquePost) && !state.resources.failed()
        ? state.frame.resourceVersion : 0;
}

bool matchesFrozenFrame(void* context, const void* opaqueIdentity, uint64_t historyGeneration) {
    const auto* recording=static_cast<const CommandContext*>(context);
    if (!opaqueIdentity || !frozenResourceVersion(context) || !recording->vanillaFrameState) return false;
    const auto& state=*static_cast<const FrameState*>(recording->vanillaFrameState.get());
    const auto& identity=*static_cast<const mcdr_frame_identity*>(opaqueIdentity);
    return state.inputs.identity.worldEpoch==identity.worldEpoch && state.inputs.identity.realFrameId==identity.realFrameId &&
        state.inputs.identity.configVersion==identity.configVersion && state.inputs.identity.resourceVersion==identity.resourceVersion &&
        state.inputs.historyGeneration==historyGeneration;
}

WorldDepthSeedStatus armWorldDepthSeedCaptureV1(void* opaqueContext, uint32_t maxEvents, uint64_t totalBudgetBytes) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->vanillaFrameState || !context->listOpen || !context->commandList ||
        !context->recordingIdentity || !context->fence || context->inRenderPass ||
        context->submissionFailed.load(std::memory_order_acquire) ||
        context->recordingFailed.load(std::memory_order_acquire)) return WorldDepthSeedStatus::StaleRecording;
    auto& state = *static_cast<FrameState*>(context->vanillaFrameState.get());
    auto& recording = state.depthSeed;
    if (state.context != context || state.phase != Phase::Recording || recording.context != context ||
        recording.recordingIdentity != context->recordingIdentity || recording.fence != context->fence ||
        recording.submission != context->fenceValue + 1) return WorldDepthSeedStatus::StaleRecording;
    if (recording.closed || recording.depthCommandSeen || recording.missedRegistration)
        return WorldDepthSeedStatus::LateArm;
    if (recording.capture || context->graphicsCommandObserver.prepare) return WorldDepthSeedStatus::Busy;
    if (!context->frameProgramGeneration || !maxEvents || !totalBudgetBytes ||
        totalBudgetBytes > mcdr::oit_budget::limit || !deviceContext().device ||
        !matchesWorldDepthRegistrationV1(context, recording.worldDepth))
        return WorldDepthSeedStatus::InvalidRequest;
    auto* depth = recording.worldDepth;
    try {
        auto budget = mcdr::oit_budget::reserve(sizeof(WorldDepthSeedJournal) + 65536 + 512, totalBudgetBytes);
        if (!budget) return WorldDepthSeedStatus::BudgetExceeded;
        auto journal = std::make_shared<WorldDepthSeedJournal>();
        journal->context = context;
        journal->frame = state.frame;
        journal->recordingIdentity = context->recordingIdentity;
        journal->fence = context->fence;
        journal->submission = recording.submission;
        journal->worldDepth = depth;
        journal->depthOwner = recording.depthOwner;
        journal->programGeneration = context->frameProgramGeneration;
        journal->totalBudgetBytes = totalBudgetBytes;
        journal->maxEvents = maxEvents;
        journal->observer.owner = journal;
        journal->observer.totalBudgetBytes = totalBudgetBytes;
        journal->observer.depthWriteTarget = depth;
        journal->observer.prepare = &prepareDepthSeedDraw;
        journal->observer.confirm = &confirmDepthSeedDraw;
        journal->observer.complete = &completeDepthSeedDraws;
        journal->observer.budgetExceeded = &retireDepthSeedBudget;
        journal->budget = std::move(budget);
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heap.NumDescriptors = 1;
        if (FAILED(deviceContext().device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&journal->depthDescriptors))))
            return WorldDepthSeedStatus::RecordingFailed;
        journal->retainedDsv = journal->depthDescriptors->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = depth->dxgiFormat;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        deviceContext().device->CreateDepthStencilView(depth->resource.Get(), &view, journal->retainedDsv);
        auto& leases = context->recordingLeases[context->currentListSlot];
        leases.reserve(leases.size() + 1);
        if (bindGraphicsCommandObserver(context, journal->observer) != 1) return WorldDepthSeedStatus::Busy;
        leases.push_back(journal);
        recording.capture = std::move(journal);
        return WorldDepthSeedStatus::Ready;
    } catch (const std::bad_alloc&) {
        return WorldDepthSeedStatus::BudgetExceeded;
    }
}

WorldDepthSeedStatus sealWorldDepthSeedCaptureV1(void* opaqueContext,
    std::shared_ptr<const WorldDepthSeedJournalV1>* output) {
    if (output) output->reset();
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->vanillaFrameState || !context->listOpen || !context->commandList ||
        !context->recordingIdentity || !context->fence) return WorldDepthSeedStatus::StaleRecording;
    auto& state = *static_cast<FrameState*>(context->vanillaFrameState.get());
    auto& recording = state.depthSeed;
    if (recording.context != context || recording.recordingIdentity != context->recordingIdentity ||
        recording.fence != context->fence || recording.submission != context->fenceValue + 1)
        return WorldDepthSeedStatus::StaleRecording;
    if (context->submissionFailed.load(std::memory_order_acquire) ||
        context->recordingFailed.load(std::memory_order_acquire)) {
        if (recording.capture) failDepthSeedRecording(context, *recording.capture);
        return WorldDepthSeedStatus::RecordingFailed;
    }
    if (!recording.capture) {
        recording.closed = true;
        detachDepthSeedJournal(context);
        return WorldDepthSeedStatus::NotArmed;
    }
    auto& journal = *recording.capture;
    if (!matchesDepthSeedRecordingIdentity(journal, context)) return WorldDepthSeedStatus::StaleRecording;
    if (!matchesWorldDepthRegistrationV1(context, journal.worldDepth) ||
        journal.programGeneration != context->frameProgramGeneration) return WorldDepthSeedStatus::StaleRecording;
    if (journal.closed) return readWorldDepthSeedCaptureV1(context, output);
    if (context->inRenderPass || state.activeToken) return WorldDepthSeedStatus::Busy;
    const bool ownsObserver = ownsDepthSeedObserver(journal, context);
    if (!validDepthSeedRecording(journal, context) || journal.pendingDraw)
        failDepthSeedRecording(context, journal);
    else if (journal.events.empty()) {
        retireDepthSeedCapture(context, journal, WorldDepthSeedStatus::Incomplete);
        return journal.status;
    } else if (completeGraphicsCommandRecording(context, mcdr::graphics_recording::Completion::Succeeded) != 1)
        failDepthSeedRecording(context, journal);
    if (ownsObserver) unbindGraphicsCommandObserver(context);
    journal.closed = true;
    recording.closed = true;
    detachDepthSeedJournal(context);
    if (journal.incomplete || journal.status != WorldDepthSeedStatus::Ready) return journal.status;
    return readWorldDepthSeedCaptureV1(context, output);
}

WorldDepthSeedStatus readWorldDepthSeedCaptureV1(void* opaqueContext,
    std::shared_ptr<const WorldDepthSeedJournalV1>* output) {
    if (output) output->reset();
    const auto* context = static_cast<const CommandContext*>(opaqueContext);
    if (!context || !context->vanillaFrameState) return WorldDepthSeedStatus::StaleRecording;
    const auto& recording = static_cast<const FrameState*>(context->vanillaFrameState.get())->depthSeed;
    if (!recording.capture) return WorldDepthSeedStatus::NotArmed;
    const auto& journal = *recording.capture;
    if (!matchesDepthSeedRecordingIdentity(journal, context) ||
        !matchesWorldDepthRegistrationV1(context, journal.worldDepth) ||
        journal.programGeneration != context->frameProgramGeneration) return WorldDepthSeedStatus::StaleRecording;
    if (context->submissionFailed.load(std::memory_order_acquire) ||
        context->recordingFailed.load(std::memory_order_acquire)) return WorldDepthSeedStatus::RecordingFailed;
    if (!recording.closed || !journal.closed) return WorldDepthSeedStatus::Busy;
    if (journal.status != WorldDepthSeedStatus::Ready) return journal.status;
    if (journal.incomplete || journal.events.empty()) return WorldDepthSeedStatus::Incomplete;
    if (output) *output = recording.capture;
    return WorldDepthSeedStatus::Ready;
}

WorldDepthSeedStatus cancelWorldDepthSeedCaptureV1(void* opaqueContext) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->vanillaFrameState) return WorldDepthSeedStatus::StaleRecording;
    auto& state = *static_cast<FrameState*>(context->vanillaFrameState.get());
    auto& recording = state.depthSeed;
    if (recording.capture && !recording.capture->closed) {
        auto& journal = *recording.capture;
        if (journal.observer.contentPolicy == mcdr::graphics_recording::ContentPolicy::FactsOnly &&
            !journal.pendingDraw) {
            retireDepthSeedCapture(context, journal, WorldDepthSeedStatus::Cancelled);
            return journal.status;
        }
        if (ownsDepthSeedObserver(journal, context)) {
            completeGraphicsCommandRecording(context, mcdr::graphics_recording::Completion::Failed);
            unbindGraphicsCommandObserver(context);
        }
        context->recordingFailed.store(true, std::memory_order_release);
        journal.incomplete = true;
        journal.closed = true;
        journal.status = WorldDepthSeedStatus::Cancelled;
    }
    recording.closed = true;
    detachDepthSeedJournal(context);
    return recording.capture ? recording.capture->status : WorldDepthSeedStatus::NotArmed;
}

int beginFrame(void* context, const void* rawInput, uint32_t byteCount) {
    if (context == nullptr || rawInput == nullptr || byteCount != sizeof(mcdr_vanilla_frame_input)) {
        return reject("invalid context or frame input");
    }
    mcdr_vanilla_frame_input input{};
    std::memcpy(&input, rawInput, sizeof(input));
    if (!validFrame(input)) return reject("invalid full frame schema, camera, settings, or semantics");
    auto* recording = static_cast<CommandContext*>(context);
    if (!recording->listOpen || !recording->commandList || recording->submissionFailed.load())
        return reject("frame has no live recording");
    auto& state = stateFor(context);
    if (state.phase != Phase::Idle) {
        return reject("previous frame has not finished");
    }
    if (fgAttachWorldFrame(context, input) != 1)
        return reject("display frame did not begin before world recording");
    state.context = context;
    state.inputs = input;
    state.frame = {input.schemaVersion, input.renderWidth, input.renderHeight, input.identity.worldEpoch,
        input.identity.realFrameId, input.identity.configVersion, input.identity.resourceVersion};
    state.prepared = {};
    state.preparedLeases.clear();
    state.preparedPublished = false;
    state.reconstructionAttempted = false;
    state.phase = Phase::Recording;
    state.executedPasses = 0;
    state.nextPass = 1;
    state.resources.reset();
    state.resourceLayouts.clear();
    state.resourceLeases.clear();
    detachDepthSeedJournal(recording);
    state.depthSeed = {};
    state.depthSeed.context = context;
    state.depthSeed.recordingIdentity = recording->recordingIdentity;
    state.depthSeed.fence = recording->fence;
    state.depthSeed.submission = recording->fenceValue + 1;
    recording->vanillaDepthSeedState = &state.depthSeed;
    recording->vanillaDepthSeedClear = &depthSeedClear;
    recording->vanillaDepthSeedDraw = &depthSeedDraw;
    recording->vanillaDepthSeedOrdinal = 0;
    postFailure.clear();
    return 1;
}

int registerWorldDepthV1(void* opaqueContext, void* opaqueDepth) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* depth = static_cast<Dx12Object*>(opaqueDepth);
    if (!context || !context->vanillaFrameState || !context->listOpen || !context->commandList ||
        !context->recordingIdentity || !context->fence || context->fenceValue == UINT64_MAX || context->inRenderPass ||
        context->submissionFailed.load(std::memory_order_acquire) ||
        context->recordingFailed.load(std::memory_order_acquire))
        return reject("world depth registration has no live recording");
    auto& state = *static_cast<FrameState*>(context->vanillaFrameState.get());
    auto& recording = state.depthSeed;
    if (state.context != context || state.phase != Phase::Recording || recording.context != context ||
        recording.recordingIdentity != context->recordingIdentity || recording.fence != context->fence ||
        recording.submission != context->fenceValue + 1)
        return reject("world depth registration belongs to another recording");
    if (recording.closed || recording.depthCommandSeen || recording.missedRegistration)
        return reject("world depth registration follows depth commands");
    if (!depth || depth->kind != Dx12Object::Kind::Texture || !depth->resource ||
        depth->closeRequested.load(std::memory_order_acquire) || !(depth->usage & 8) ||
        depth->dxgiFormat != DXGI_FORMAT_D32_FLOAT)
        return reject("world depth registration requires a live D32 texture");
    const auto description = depth->resource->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        description.Width != state.frame.width || description.Height != state.frame.height ||
        description.MipLevels != 1 || description.DepthOrArraySize != 1 || description.SampleDesc.Count != 1 ||
        !(description.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
        (description.Format != DXGI_FORMAT_R32_TYPELESS && description.Format != DXGI_FORMAT_D32_FLOAT))
        return reject("world depth dimensions or format are incompatible");
    if (recording.worldDepth) {
        if (recording.worldDepth != depth)
            return reject("world depth registration changed its target");
        return 1;
    }
    try {
        auto owner = retainObjectLease(depth);
        if (!owner) return reject("world depth registration could not retain its target");
        context->recordingLeases[context->currentListSlot].push_back(owner);
        recording.depthOwner = std::move(owner);
        recording.worldDepth = depth;
        return 1;
    } catch (const std::bad_alloc&) {
        return reject("world depth registration could not retain its target");
    }
}

bool matchesWorldDepthRegistrationV1(const void* opaqueContext, const void* depth) {
    const auto* context = static_cast<const CommandContext*>(opaqueContext);
    if (!context || !depth || !context->vanillaFrameState || !context->listOpen || !context->commandList ||
        !context->recordingIdentity || !context->fence) return false;
    const auto& state = *static_cast<const FrameState*>(context->vanillaFrameState.get());
    const auto& recording = state.depthSeed;
    return state.context == context && recording.context == context && recording.worldDepth == depth &&
        recording.depthOwner && recording.submission && recording.recordingIdentity == context->recordingIdentity &&
        recording.fence == context->fence && recording.submission == context->fenceValue + 1;
}

int markWorldReady(void* context, const void* rawInput, uint32_t byteCount) {
    if (context == nullptr || rawInput == nullptr || byteCount != sizeof(mcdr_frame_identity))
        return reject("world-ready context or identity is invalid");
    mcdr_frame_identity input{};
    std::memcpy(&input, rawInput, sizeof(input));
    auto& state = stateFor(context);
    if (state.activeToken || state.context != context ||
        (state.phase != Phase::Recording && state.phase != Phase::OpaquePost) ||
        !validIdentity(input) || !sameIdentity(input, state.inputs.identity)) {
        return reject("world-ready phase is missing, duplicated, or stale");
    }
    if (state.phase == Phase::OpaquePost) {
        if (!state.preparedPublished || (state.executedPasses & static_cast<uint32_t>(Pass::WorldOutput)) == 0 ||
            (state.prepared.consumer != 0 && !state.reconstructionAttempted))
            return reject("opaque reconstruction did not complete before the full world");
        state.phase = Phase::WorldPost;
    } else {
        state.phase = Phase::WorldReady;
    }
    if (state.depthSeed.capture && !state.depthSeed.closed) {
        if (retireUnconsumedDepthSeedCapture(static_cast<CommandContext*>(context),
                *state.depthSeed.capture) != WorldDepthSeedStatus::Incomplete) {
            state.phase = Phase::Failed;
            return reject("world depth seed capture missed OIT begin");
        }
    }
    state.depthSeed.closed = true;
    detachDepthSeedJournal(static_cast<CommandContext*>(context));
    return 1;
}

int markGuiReady(void* context, const void* rawInput, uint32_t byteCount) {
    if (context == nullptr || rawInput == nullptr || byteCount != sizeof(mcdr_frame_identity))
        return reject("GUI context or identity is invalid");
    mcdr_frame_identity input{};
    std::memcpy(&input, rawInput, sizeof(input));
    auto& state = stateFor(context);
    if (state.activeToken || state.context != context || (state.phase != Phase::WorldReady && state.phase != Phase::WorldPost) ||
        !validIdentity(input) || !sameIdentity(input, state.inputs.identity) || !state.preparedPublished ||
        (state.prepared.consumer != 0 && !state.reconstructionAttempted)) {
        return reject("GUI phase is out of order or stale");
    }
    if (fgMarkGuiReady(context, input) != 1)
        return reject("display frame GUI identity is out of order or stale");
    state.phase = Phase::GuiReady;
    return 1;
}

namespace {

Dx12Object* preparedTexture(const mcdr_prepared_binding& binding) {
    auto* wrapper = static_cast<Dx12Object*>(binding.object);
    if (!wrapper || binding.firstSubresource != 0 || binding.subresourceCount != 1) return nullptr;
    auto* texture = resourceObject(wrapper);
    if (!texture || texture->kind != Dx12Object::Kind::Texture) return nullptr;
    const auto desc = texture->resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
        desc.SampleDesc.Count != 1 || desc.MipLevels == 0) return nullptr;
    if (wrapper->kind == Dx12Object::Kind::TextureView &&
        (!wrapper->hasSrvDesc || wrapper->srvDesc.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D ||
         wrapper->srvDesc.Texture2D.MostDetailedMip != 0 || wrapper->srvDesc.Texture2D.MipLevels != 1 ||
         wrapper->srvDesc.Texture2D.PlaneSlice != 0)) return nullptr;
    return texture;
}

bool preparedFormat(const mcdr_vanilla_frame_input& frame, uint32_t index, Dx12Object* object) {
    const auto desc = object->resource->GetDesc();
    const uint32_t width = index == 3 ? 1u : frame.renderWidth;
    const uint32_t height = index == 3 ? 1u : frame.renderHeight;
    if (desc.Width != width || desc.Height != height ||
        (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) return false;
    switch (index) {
    case 0:
        if (frame.colorEncoding == 1) return object->dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
            object->dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM;
        if (frame.colorEncoding == 2) return object->dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            object->dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
            object->dxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
            object->dxgiFormat == DXGI_FORMAT_R32G32B32A32_FLOAT;
        return object->dxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
            object->dxgiFormat == DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 1:
        return object->dxgiFormat == DXGI_FORMAT_D32_FLOAT ||
            object->dxgiFormat == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
            object->dxgiFormat == DXGI_FORMAT_D24_UNORM_S8_UINT ||
            object->dxgiFormat == DXGI_FORMAT_D16_UNORM;
    case 2: return object->dxgiFormat == DXGI_FORMAT_R16G16_FLOAT;
    case 3: return object->dxgiFormat == DXGI_FORMAT_R32_FLOAT;
    default: return object->dxgiFormat == DXGI_FORMAT_R8_UNORM || object->dxgiFormat == DXGI_FORMAT_R16_UNORM;
    }
}

std::array<mcdr_prepared_binding*, 7> preparedBindings(mcdr_prepared_frame& frame) {
    return {&frame.color, &frame.depth, &frame.motion, &frame.exposure,
        &frame.reactive, &frame.composition, &frame.responsive};
}

}

int publishPreparedFrame(void* context, const void* rawInput, uint32_t byteCount) {
    if (!context || !rawInput || byteCount != sizeof(mcdr_prepared_frame))
        return reject("prepared frame arguments are invalid");
    auto& state = stateFor(context);
    const auto failed = [&](const char* reason) {
        state.resources.fail();
        state.phase = Phase::Failed;
        return reject(reason);
    };
    mcdr_prepared_frame input{};
    std::memcpy(&input, rawInput, sizeof(input));
    auto* recording = static_cast<CommandContext*>(context);
    if (state.context != context || state.activeToken || !recording->listOpen || !recording->commandList ||
        recording->submissionFailed.load() || state.preparedPublished ||
        (state.phase != Phase::WorldReady && state.phase != Phase::WorldPost &&
         state.phase != Phase::OpaqueReady && state.phase != Phase::OpaquePost) ||
        input.schemaVersion != MCDR_PREPARED_FRAME_SCHEMA_VERSION || input.structSize != sizeof(input) ||
        !sameIdentity(input.identity, state.inputs.identity) || input.historyGeneration != state.inputs.historyGeneration ||
        (input.availability & ~0x7fu) != 0 || (input.required & ~0x7fu) != 0 ||
        (input.resetReasons & ~0x0fu) != 0 || input.consumer > 3 || input.motionCoverage > 2 ||
        input.motionGuideCoverage > 5) return failed("prepared frame is invalid, stale, duplicated, or out of order");
    const uint32_t required = input.consumer == 0 ? 1u : input.consumer == 1 ? 5u : 15u;
    if (input.required != required || (input.availability & required) != required ||
        (((state.inputs.flags & 1u) == 0) != ((input.resetReasons & 1u) != 0)) ||
        ((input.availability & 4u) == 0 && input.motionGuideCoverage != 5))
        return failed("prepared availability, reset, or required inputs conflict with the frame");
    if (input.consumer != 0 && ((state.inputs.flags & 1u) == 0 ||
        (input.consumer == 1 ? state.inputs.settings.antiAliasing != 2 :
            state.inputs.settings.reconstructionBackend == 0 || (state.inputs.current.flags & 1u) == 0) ||
        (input.consumer == 3 && state.inputs.settings.reconstructionBackend != MCDR_SR_BACKEND_DLSS)))
        return failed("prepared consumer conflicts with frozen settings or camera metadata");
    if ((input.availability & 0x70u) != 0 && (!state.inputs.settings.providerMasks ||
        input.consumer < 2 ||
        ((input.availability & 0x30u) != 0 && state.inputs.settings.reconstructionBackend != MCDR_SR_BACKEND_FSR) ||
        ((input.availability & 0x40u) != 0 && state.inputs.settings.reconstructionBackend != MCDR_SR_BACKEND_XESS)))
        return failed("provider masks conflict with the selected frozen backend");
    const auto bindings = preparedBindings(input);
    std::vector<std::shared_ptr<void>> leases;
    std::array<Dx12Object*, 7> textures{};
    for (uint32_t index = 0; index < bindings.size(); ++index) {
        auto& binding = *bindings[index];
        if ((input.availability & (1u << index)) == 0) {
            if (binding.object || binding.firstSubresource != 0 || binding.subresourceCount != 0)
                return failed("unavailable prepared input contains a binding");
            continue;
        }
        auto* texture = preparedTexture(binding);
        if (!texture || !preparedFormat(state.inputs, index, texture))
            return failed("prepared input has an unsupported view, format, or extent");
        for (uint32_t prior = 0; prior < index; ++prior) {
            if (textures[prior] && textures[prior]->resource.Get() == texture->resource.Get())
                return failed("prepared inputs unexpectedly alias");
        }
        auto lease = retainObjectLease(binding.object);
        if (!lease) return failed("prepared wrapper lease could not be retained");
        leases.push_back(std::move(lease));
        retainObjectForRecording(context, binding.object);
        textures[index] = texture;
        binding.object = texture;
    }
    state.prepared = input;
    state.preparedLeases = std::move(leases);
    state.preparedPublished = true;
    return 1;
}

int executePreparedReconstruction(void* context, const void* rawInput, uint32_t byteCount,
                                  PreparedAdapter adapter) {
    if (!context || !rawInput || byteCount != sizeof(mcdr_reconstruction_request) || !adapter)
        return reject("reconstruction request arguments are invalid");
    auto& state = stateFor(context);
    const auto failed = [&](const char* reason) {
        state.resources.fail();
        state.phase = Phase::Failed;
        return reject(reason);
    };
    mcdr_reconstruction_request input{};
    std::memcpy(&input, rawInput, sizeof(input));
    if (state.context != context || !state.preparedPublished || state.reconstructionAttempted ||
        input.schemaVersion != kFrameSchemaVersion || input.structSize != sizeof(input) ||
        (input.flags & ~1u) != 0 || input.consumer == 0 || input.consumer != state.prepared.consumer ||
        !sameIdentity(input.identity, state.inputs.identity))
        return failed("reconstruction request is stale, duplicated, or not prepared");
    auto* output = preparedTexture(input.output);
    if (!output) return failed("reconstruction output view is unsupported");
    const auto outputDesc = output->resource->GetDesc();
    const bool taa = input.consumer == 1;
    const uint32_t expectedWidth = taa ? state.inputs.renderWidth : state.inputs.displayWidth;
    const uint32_t expectedHeight = taa ? state.inputs.renderHeight : state.inputs.displayHeight;
    if (outputDesc.Width != expectedWidth || outputDesc.Height != expectedHeight ||
        (output->dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM &&
         output->dxgiFormat != DXGI_FORMAT_B8G8R8A8_UNORM &&
         output->dxgiFormat != DXGI_FORMAT_R16G16B16A16_FLOAT &&
         output->dxgiFormat != DXGI_FORMAT_R32G32B32A32_FLOAT) ||
        (outputDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
        return failed("reconstruction output extent or writable state is unsupported");
    const auto bindings = preparedBindings(state.prepared);
    for (uint32_t index = 0; index < bindings.size(); ++index) {
        if ((state.prepared.availability & (1u << index)) == 0) continue;
        auto* source = resourceObject(bindings[index]->object);
        if ((source->resource.Get() == output->resource.Get()) != (taa && index == 0))
            return failed("reconstruction output aliases an incompatible prepared input");
    }
    retainObjectForRecording(context, input.output.object);
    input.output.object = output;
    std::vector<PassResourceUse> uses;
    for (uint32_t index = 0; index < bindings.size(); ++index) {
        if ((state.prepared.availability & (1u << index)) == 0) continue;
        const auto& binding = *bindings[index];
        const uint32_t requiredState = taa ? (index == 0 ? D3D12_RESOURCE_STATE_RENDER_TARGET :
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        uses.push_back({binding.object, state.frame.resourceVersion, binding.firstSubresource,
            binding.subresourceCount, taa && index == 0 ? ResourceAccess::ReadWrite : ResourceAccess::Read,
            requiredState, taa && index == 0 ? D3D12_RESOURCE_STATE_RENDER_TARGET : UINT32_MAX});
    }
    if (!taa) {
        const uint32_t requiredState = (outputDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 ?
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST;
        uses.push_back({output, state.frame.resourceVersion, input.output.firstSubresource,
            input.output.subresourceCount, ResourceAccess::Write, requiredState, D3D12_RESOURCE_STATE_RENDER_TARGET});
    }
    struct PreparedInvocation {
        PreparedAdapter adapter;
        const mcdr_vanilla_frame_input* frame;
        const mcdr_prepared_frame* prepared;
        const mcdr_reconstruction_request* request;
    } invocation{adapter, &state.inputs, &state.prepared, &input};
    const auto invoke = [](void* owner, void* payload) -> int {
        const auto& call = *static_cast<PreparedInvocation*>(payload);
        return call.adapter(owner, call.frame, call.prepared, call.request);
    };
    state.reconstructionAttempted = true;
    const auto pass = taa ? Pass::Taa : input.consumer == 3 ? Pass::Dlss : Pass::Sr;
    return executeDeclaredWorldPost(context, frozenPass(state, static_cast<uint32_t>(pass)),
        uses.data(), static_cast<uint32_t>(uses.size()), invoke, &invocation);
}

int prepareWorldOutput(void* context, const void* rawInput, uint32_t byteCount) {
    static_assert(sizeof(mcdr_world_output_input) == 360);
    if (!context || !rawInput || byteCount != sizeof(mcdr_world_output_input))
        return reject("world output arguments are invalid");
    const auto input = *static_cast<const mcdr_world_output_input*>(rawInput);
    auto& state = stateFor(context);
    const auto failed = [&](const char* reason) {
        state.resources.fail();
        state.phase = Phase::Failed;
        return reject(reason);
    };
    if (input.schemaVersion != MCDR_WORLD_OUTPUT_SCHEMA_VERSION || input.structSize != sizeof(input) ||
        !sameIdentity(input.identity, state.inputs.identity) || state.context != context ||
        (input.sourceStage == MCDR_WORLD_SOURCE_COMPLETE_WORLD ? state.phase != Phase::WorldReady :
            state.phase != Phase::Recording) || state.activeToken || state.preparedPublished ||
        input.sourceStage > MCDR_WORLD_SOURCE_OPAQUE_BEFORE_COMPOSITE || input.reservedStage != 0 ||
        input.motionCoverage > 2 || input.motionGuideCoverage > 5 || input.reserved != 0 ||
        !std::isfinite(input.dlssPostSharpness) || input.dlssPostSharpness < 0.0f || input.dlssPostSharpness > 1.0f)
        return failed("world output is stale, duplicated, or out of order");
    auto* color = preparedTexture(input.color);
    auto* depth = preparedTexture(input.depth);
    auto* scene = preparedTexture(input.scene);
    auto* motion = input.motion.object ? preparedTexture(input.motion) : nullptr;
    if (!color || !depth || !scene || !preparedFormat(state.inputs, 0, color) ||
        !preparedFormat(state.inputs, 1, depth) ||
        (input.motion.object && (!motion || !preparedFormat(state.inputs, 2, motion))))
        return failed("world output sources have an unsupported extent, format, or view");
    const auto sceneDesc = scene->resource->GetDesc();
    if (sceneDesc.Width != state.inputs.displayWidth || sceneDesc.Height != state.inputs.displayHeight ||
        scene->dxgiFormat != color->dxgiFormat ||
        (sceneDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
        scene->resource == color->resource || scene->resource == depth->resource ||
        (motion && scene->resource == motion->resource))
        return failed("world output scene destination is incompatible or aliases its sources");
    const auto& frame = state.inputs;
    const bool provider = frame.settings.reconstructionBackend != MCDR_SR_BACKEND_NONE;
    const bool previous = (frame.flags & 1u) != 0;
    if (previous && (provider || frame.settings.antiAliasing == 2) && !motion)
        return failed("selected temporal world output has no current motion texture");
    if (provider && frame.settings.antiAliasing == 2)
        return failed("TAA and temporal reconstruction cannot consume the same world output");
    const uint32_t consumer = previous && motion && (!provider || (frame.current.flags & 1u) != 0)
        ? provider ? 2u : frame.settings.antiAliasing == 2 ? 1u : 0u : 0u;
    if (input.sourceStage == MCDR_WORLD_SOURCE_OPAQUE_BEFORE_COMPOSITE) {
        if (state.depthSeed.capture && !state.depthSeed.closed) {
            if (retireUnconsumedDepthSeedCapture(static_cast<CommandContext*>(context),
                    *state.depthSeed.capture) != WorldDepthSeedStatus::Incomplete)
                return failed("world depth seed capture missed OIT begin");
        }
        state.phase = Phase::OpaqueReady;
    }
    if (frame.settings.antiAliasing == 1) {
        const auto smaa = [](void* owner, void* payload) {
            return applySmaaToTexture(static_cast<CommandContext*>(owner), static_cast<Dx12Object*>(payload));
        };
        if (executeSmaaWorldPost(context, smaa, color) != 1) return -1;
    }
    mcdr_prepared_frame prepared{};
    prepared.schemaVersion = MCDR_PREPARED_FRAME_SCHEMA_VERSION;
    prepared.structSize = sizeof(prepared);
    prepared.identity = frame.identity;
    prepared.historyGeneration = frame.historyGeneration;
    prepared.consumer = consumer;
    prepared.required = consumer == 0 ? 1u : consumer == 1 ? 5u : 15u;
    prepared.availability = 3u | (motion ? 4u : 0u);
    prepared.motionCoverage = input.motionCoverage;
    prepared.motionGuideCoverage = motion ? input.motionGuideCoverage : 5u;
    prepared.resetReasons = previous ? 0u : 1u;
    prepared.color = input.color;
    prepared.depth = input.depth;
    prepared.motion = input.motion;
    if (consumer == 2) {
        auto* exposure = preparedTexture(input.exposure);
        if (!exposure || !preparedFormat(frame, 3, exposure))
            return failed("selected provider has no writable exposure target");
        frameprep::ExposureRequest exposureRequest{context, color, exposure,
            frame.renderWidth, frame.renderHeight, frame.colorEncoding == 1 ? 1u : 0u, frame.preExposure};
        const PassResourceUse exposureUses[] = {
            {color, frame.identity.resourceVersion, 0, 1, ResourceAccess::Read,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
            {exposure, frame.identity.resourceVersion, 0, 1, ResourceAccess::Uav,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        };
        const auto expose = [](void*, void* payload) {
            return frameprep::produceExposure(*static_cast<frameprep::ExposureRequest*>(payload));
        };
        if (executeDeclaredWorldPost(context, frozenPass(state, static_cast<uint32_t>(Pass::Exposure)),
            exposureUses, 2, expose, &exposureRequest) != 1) return -1;
        prepared.exposure = input.exposure;
        prepared.availability |= 8u;
        mcdr_sr_caps caps{};
        srFillCaps(static_cast<int32_t>(frame.settings.reconstructionBackend), caps);
        auto masks = input.masks;
        const bool oit = masks.source0 && masks.source1;
        masks.outputFlags = frame.settings.providerMasks
            ? frame.settings.reconstructionBackend == MCDR_SR_BACKEND_FSR
                ? (caps.takesMasks & MCDR_SR_MASK_REACTIVE ? 1u : 0u) |
                    (oit && (caps.takesMasks & MCDR_SR_MASK_TRANSPARENCY_AND_COMPOSITION) ? 2u : 0u)
                : frame.settings.reconstructionBackend == MCDR_SR_BACKEND_XESS &&
                    (caps.takesMasks & MCDR_SR_MASK_RESPONSIVE) ? 4u : 0u
            : 0u;
        if (masks.outputFlags != 0) {
            const uint32_t expectedFlags = 5u | (oit ? 2u : 0u);
            if (masks.schemaVersion != MCDR_PROVIDER_MASK_SCHEMA_VERSION || masks.structSize != sizeof(masks) ||
                !sameIdentity(masks.identity, frame.identity) || masks.historyGeneration != frame.historyGeneration ||
                masks.width != frame.renderWidth || masks.height != frame.renderHeight ||
                masks.color != input.color.object || masks.depth != input.depth.object || masks.motion != input.motion.object ||
                masks.previousRealFrameId != frame.previousRealFrameId || masks.inputFlags != expectedFlags)
                return failed("provider mask source facts do not match the frozen world frame");
            std::vector<PassResourceUse> maskUses;
            for (auto* source : {masks.color, masks.depth, masks.motion, masks.source0, masks.source1})
                if (source) maskUses.push_back({source, frame.identity.resourceVersion, 0, 1, ResourceAccess::Read,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
            for (auto* output : {(masks.outputFlags & 5u) ? masks.output : nullptr,
                (masks.outputFlags & 2u) ? masks.compositionOutput : nullptr})
                if (output) maskUses.push_back({output, frame.identity.resourceVersion, 0, 1, ResourceAccess::Uav,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
            const auto classify = [](void* owner, void* payload) {
                return frameprep::produceProviderMasks(owner, payload, sizeof(mcdr_provider_mask_input));
            };
            if (executeDeclaredWorldPost(context, frozenPass(state, static_cast<uint32_t>(Pass::ProviderMasks)),
                maskUses.data(), static_cast<uint32_t>(maskUses.size()), classify, &masks) != 1) return -1;
            if ((masks.outputFlags & 1u) != 0) { prepared.reactive = {masks.output, 0, 1}; prepared.availability |= 16u; }
            if ((masks.outputFlags & 2u) != 0) { prepared.composition = {masks.compositionOutput, 0, 1}; prepared.availability |= 32u; }
            if ((masks.outputFlags & 4u) != 0) { prepared.responsive = {masks.output, 0, 1}; prepared.availability |= 64u; }
        }
    }
    if (publishPreparedFrame(context, &prepared, sizeof(prepared)) != 1) return -1;
    auto* resolved = color;
    if (consumer != 0) {
        mcdr_reconstruction_request request{kFrameSchemaVersion, sizeof(request), frame.identity, consumer, 0,
            consumer == 1 ? input.color : input.reconstructionOutput};
        if (executePreparedReconstruction(context, &request, sizeof(request), evaluatePreparedReconstruction) != 1)
            return -1;
        resolved = preparedTexture(request.output);
    }
    struct Transfer { Dx12Object* source; Dx12Object* target; float sharpness; };
    Transfer transfer{resolved, scene, consumer == 2 && frame.settings.reconstructionBackend == MCDR_SR_BACKEND_DLSS
        ? input.dlssPostSharpness : 0.0f};
    const auto copy = [](void* owner, void* payload) {
        const auto& textures = *static_cast<Transfer*>(payload);
        const auto copied = textures.sharpness > 0.0f
            ? sharpenTextureToTexture(static_cast<CommandContext*>(owner), textures.source, textures.target, textures.sharpness)
            : blitTextureToTexture(static_cast<CommandContext*>(owner), textures.source, textures.target);
        return copied ? 1 : -1;
    };
    const PassResourceUse copyUses[] = {
        {resolved, frame.identity.resourceVersion, 0, 1, ResourceAccess::Read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
        {scene, frame.identity.resourceVersion, 0, 1, ResourceAccess::Write,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET},
    };
    return executeDeclaredWorldPost(context, frozenPass(state, static_cast<uint32_t>(Pass::WorldOutput)),
        copyUses, 2, copy, &transfer);
}

int executeWorldPost(void* context, const PassInput& input, PostAdapter adapter, void* payload) {
    return executeDeclaredWorldPost(context, input, nullptr, 0, adapter, payload);
}

int executeDeclaredWorldPost(void* context, const PassInput& input,
                             const PassResourceUse* uses, uint32_t useCount,
                             PostAdapter adapter, void* payload, uint32_t graphicsEffects) {
    FrameState* selectedState = nullptr;
    const auto reject = [&](const char* reason) {
        char diagnostic[1024]{};
        std::snprintf(diagnostic, sizeof(diagnostic),
            "operation=executeDeclaredWorldPost frame=%llu epoch=%llu config=%llu resource=%llu pass=0x%x reason=%s",
            static_cast<unsigned long long>(input.realFrameId),
            static_cast<unsigned long long>(input.worldEpoch),
            static_cast<unsigned long long>(input.configVersion),
            static_cast<unsigned long long>(input.resourceVersion), input.pass, reason);
        postFailure = diagnostic;
        dbgLogError("VanillaFrameExecutor rejected post: %s", postFailure.c_str());
        if (selectedState != nullptr) {
            selectedState->resources.fail();
            selectedState->phase = Phase::Failed;
        }
        return -1;
    };
    if (context == nullptr) return reject("post context is null");
    if (adapter == nullptr) return 0;
    auto& state = stateFor(context);
    selectedState = &state;
    if (state.activeToken || !validPass(input) || state.context != context ||
        (state.phase != Phase::WorldReady && state.phase != Phase::WorldPost &&
         state.phase != Phase::OpaqueReady && state.phase != Phase::OpaquePost)) {
        return reject("invalid post input, adapter, context, or phase");
    }
    if (!sameFrame(state.frame, input)) {
        return reject("post input does not match the immutable frame identity");
    }
    if ((state.executedPasses & input.pass) != 0) {
        return reject("duplicate post pass");
    }
    auto* nativeContext = static_cast<CommandContext*>(context);
    if (!nativeContext->listOpen || !nativeContext->commandList ||
        useCount == 0 || uses == nullptr) return reject("post requires an open command list and declarations");
    std::vector<scheduler::ResourceUse> declarations;
    std::vector<Dx12Object*> objects;
    std::vector<uint32_t> sources;
    std::vector<uint32_t> initialStates;
    for (uint32_t index = 0; index < useCount; ++index) {
        const auto& use = uses[index];
        auto* object = resourceObject(use.object);
        if (!object || use.version != state.frame.resourceVersion) {
            return reject("invalid resource handle, frozen version, or subresource metadata");
        }
        const auto* layout = resourceLayout(state, object->resource.Get());
        if (!layout) return reject("invalid resource handle, frozen version, or subresource metadata");
        const auto total = layout->subresources;
        if (!compatibleResourceState(layout->flags, object->heapType, use.requiredState, use.finalState)) {
            return reject("declared resource state conflicts with heap type or resource flags");
        }
        const uint32_t first = use.firstSubresource == UINT32_MAX ? 0 : use.firstSubresource;
        const uint32_t count = use.firstSubresource == UINT32_MAX && use.subresourceCount == 0
            ? total : use.subresourceCount;
        if (first >= total || count == 0 || count > total - first ||
            (use.firstSubresource == UINT32_MAX && count != total)) {
            return reject("invalid declared subresource range");
        }
        if (use.externalState == ExternalResourceState::Unknown ||
            (use.externalState != ExternalResourceState::Tracked &&
             use.externalState != ExternalResourceState::Adopt) ||
            (use.externalState == ExternalResourceState::Adopt && use.finalState == UINT32_MAX)) {
            return reject("external resource state must be tracked or explicitly known");
        }
        uint32_t initial = D3D12_RESOURCE_STATE_COMMON;
        if (object->kind == Dx12Object::Kind::Buffer) {
            if (object->heapType == D3D12_HEAP_TYPE_UPLOAD) initial = D3D12_RESOURCE_STATE_GENERIC_READ;
            if (object->heapType == D3D12_HEAP_TYPE_READBACK) initial = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        for (uint32_t subresource = first; subresource < first + count; ++subresource) {
            declarations.push_back({object->resource.Get(), use.version, subresource, 1, total,
                static_cast<scheduler::Access>(use.access), use.requiredState,
                resourceSubresourceState(context, object->resource.Get(), subresource, initial),
                use.finalState == UINT32_MAX ? use.requiredState : use.finalState,
                scheduler::ExternalState::Known});
            sources.push_back(index);
            initialStates.push_back(initial);
        }
        objects.push_back(object);
    }
    auto plan = state.resources.prepare(state.nextPass, declarations.data(),
        static_cast<uint32_t>(declarations.size()), graphicsEffects);
    if (!plan.ok) return reject(plan.error);
    struct RestoreScope {
        void* context;
        uint32_t effects;
        ~RestoreScope() { restoreState(context, effects); }
    } restore{context, plan.effects};
    const auto completedPhase = state.phase == Phase::OpaqueReady || state.phase == Phase::OpaquePost
        ? Phase::OpaquePost : Phase::WorldPost;
    state.phase = Phase::Failed;
    int result = -1;
    try {
        for (auto* object : objects) {
            retainObjectForRecording(context, object);
            state.resourceLeases.try_emplace(object->resource.Get(), object->resource);
        }
        if (nativeContext->inRenderPass) {
            const auto ended = endRenderPass(nativeContext);
            if (!ended) {
                state.resources.fail();
                return reject("could not settle the preceding render pass");
            }
            for (uint32_t index = 0; index < declarations.size(); ++index) {
                auto& use = declarations[index];
                use.initialState = resourceSubresourceState(context, use.resource, use.subresource, initialStates[index]);
            }
            plan = state.resources.prepare(state.nextPass, declarations.data(),
                static_cast<uint32_t>(declarations.size()), graphicsEffects);
            if (!plan.ok) {
                state.resources.fail();
                return reject(plan.error);
            }
        }
        for (const auto& barrier : plan.barriers) {
            auto* resource = static_cast<ID3D12Resource*>(barrier.resource);
            if (barrier.uav) {
                D3D12_RESOURCE_BARRIER nativeBarrier{};
                nativeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                nativeBarrier.UAV.pResource = resource;
                nativeContext->commandList->ResourceBarrier(1, &nativeBarrier);
            } else {
                const auto transitioned = transitionResourceRange(context, resource, barrier.subresource,
                    1, barrier.before, barrier.after);
                if (!transitioned) {
                    state.resources.fail();
                    return reject("declared resource transition failed");
                }
            }
        }
        result = adapter(context, payload);
        if (result > 0) {
            for (uint32_t index = 0; index < plan.uses.size(); ++index) {
                auto& use = plan.uses[index];
                const auto& source = uses[sources[index]];
                if (source.externalState == ExternalResourceState::Adopt) {
                    const auto adopted = adoptResourceRangeState(context, use.resource, use.subresource,
                        1, initialStates[index], source.finalState);
                    if (!adopted) {
                        state.resources.fail();
                        return reject("external resource state adoption failed");
                    }
                }
                const uint32_t actual = resourceSubresourceState(context, use.resource,
                    use.subresource, initialStates[index]);
                if (source.finalState != UINT32_MAX && actual != source.finalState) {
                    state.resources.fail();
                    return reject("external adapter violated its declared final state");
                }
                use.postExternalState = actual;
            }
            if (!state.resources.commit(plan)) {
                state.resources.fail();
                return reject("resource schedule commit failed");
            }
        }
    } catch (const std::exception& failure) {
        state.resources.fail();
        return reject(failure.what());
    } catch (...) {
        state.resources.fail();
        return reject("post recording threw; reset is required");
    }
    if (result <= 0) {
        state.resources.fail();
        char reason[128]{};
        std::snprintf(reason, sizeof(reason),
            "selected post adapter result=%d; recording failed; reset is required", result);
        reject(reason);
        return result < 0 ? result : -1;
    }
    ++state.nextPass;
    state.phase = completedPhase;
    state.executedPasses |= input.pass;
    return result;
}

int beginDeclaredPass(void* context, uint32_t pass, const PassResourceUse* uses,
                      uint32_t useCount, uint32_t graphicsEffects, DeclaredPassToken** token) {
    if (token) *token = nullptr;
    if (!context || (useCount != 0 && !uses) || !token || pass == 0) return reject("declared pass supplied incomplete arguments");
    auto& state = stateFor(context);
    auto* nativeContext = static_cast<CommandContext*>(context);
    if (state.context != context || state.phase == Phase::Idle || state.phase == Phase::Failed ||
        !nativeContext->listOpen || !nativeContext->commandList || nativeContext->submissionFailed.load()) {
        state.resources.fail();
        state.phase = Phase::Failed;
        return reject("declared pass requires an active, unpoisoned frame and open command list");
    }
    std::vector<scheduler::ResourceUse> declarations;
    std::vector<Dx12Object*> objects;
    std::vector<uint32_t> initialStates;
    std::vector<PassResourceUse> expandedSources;
    declarations.reserve(useCount);
    initialStates.reserve(useCount);
    expandedSources.reserve(useCount);
    objects.reserve(useCount);
    for (uint32_t index = 0; index < useCount; ++index) {
        const auto& use = uses[index];
        auto* object = resourceObject(use.object);
        auto* rawResource = static_cast<ID3D12Resource*>(use.resource);
        if ((use.object && !object) || (object && rawResource && object->resource.Get() != rawResource)) {
            state.resources.fail(); state.phase = Phase::Failed;
            return reject("declared pass object and raw resource disagree");
        }
        auto* resource = object ? object->resource.Get() : rawResource;
        if (!resource || use.version != state.frame.resourceVersion) {
            state.resources.fail(); state.phase = Phase::Failed;
            return reject("invalid declared pass resource identity, version, or layout");
        }
        auto* layout = resourceLayout(state, resource);
        if (!layout) {
            state.resources.fail(); state.phase = Phase::Failed;
            return reject("invalid declared pass resource identity, version, or layout");
        }
        const auto total = layout->subresources;
        D3D12_HEAP_TYPE heapType = object ? object->heapType : D3D12_HEAP_TYPE_DEFAULT;
        if (!object) {
            if (use.initialState == UINT32_MAX) {
                state.resources.fail(); state.phase = Phase::Failed;
                return reject("raw declared resources require known heap properties and an initial state");
            }
            if (!layout->rawHeap) {
                D3D12_HEAP_PROPERTIES heap{};
                if (FAILED(resource->GetHeapProperties(&heap, nullptr))) {
                    state.resources.fail(); state.phase = Phase::Failed;
                    return reject("raw declared resources require known heap properties and an initial state");
                }
                layout->rawHeap = heap.Type;
            }
            heapType = *layout->rawHeap;
        }
        if (!compatibleResourceState(layout->flags, heapType, use.requiredState, use.finalState)) {
            state.resources.fail(); state.phase = Phase::Failed;
            return reject("declared resource state conflicts with its heap or resource flags");
        }
        const uint32_t first = use.firstSubresource == UINT32_MAX ? 0 : use.firstSubresource;
        const uint32_t count = use.firstSubresource == UINT32_MAX && use.subresourceCount == 0
            ? total : use.subresourceCount;
        if (first >= total || count == 0 || count > total - first ||
            (use.firstSubresource == UINT32_MAX && count != total) ||
            (use.externalState != ExternalResourceState::Tracked && use.externalState != ExternalResourceState::Adopt) ||
            (use.externalState == ExternalResourceState::Adopt && use.finalState == UINT32_MAX)) {
            state.resources.fail(); state.phase = Phase::Failed;
            return reject("invalid declared pass subresource or external effect");
        }
        uint32_t initial = D3D12_RESOURCE_STATE_COMMON;
        if (heapType == D3D12_HEAP_TYPE_UPLOAD) initial = D3D12_RESOURCE_STATE_GENERIC_READ;
        if (heapType == D3D12_HEAP_TYPE_READBACK) initial = D3D12_RESOURCE_STATE_COPY_DEST;
        if (!object) initial = use.initialState;
        if (!compatibleResourceState(layout->flags, heapType, initial, UINT32_MAX)) {
            state.resources.fail(); state.phase = Phase::Failed;
            return reject("declared initial resource state conflicts with its heap or resource flags");
        }
        for (uint32_t subresource = first; subresource < first + count; ++subresource) {
            const uint32_t tracked = resourceSubresourceState(context, resource, subresource, initial);
            const auto alias = std::find_if(declarations.begin(), declarations.end(), [&](const auto& prior) {
                return prior.resource == resource && prior.subresource == subresource &&
                    prior.version == use.version && prior.totalSubresources == total && prior.initialState == tracked;
            });
            if (alias != declarations.end()) {
                const auto aliasIndex = static_cast<size_t>(alias - declarations.begin());
                auto& source = expandedSources[aliasIndex];
                if (use.access == ResourceAccess::Read && alias->access == scheduler::Access::Read &&
                    use.externalState == ExternalResourceState::Tracked &&
                    source.externalState == ExternalResourceState::Tracked &&
                    use.requiredState != 0 && alias->requiredState != 0 &&
                    ((use.requiredState | alias->requiredState) & ~scheduler::kReadStates) == 0 &&
                    (use.finalState == UINT32_MAX || use.finalState == use.requiredState) &&
                    alias->postExternalState == alias->requiredState) {
                    alias->requiredState |= use.requiredState;
                    alias->postExternalState = alias->requiredState;
                    source.requiredState = alias->requiredState;
                    source.finalState = alias->requiredState;
                    continue;
                }
            }
            declarations.push_back({resource, use.version, subresource, 1, total,
                static_cast<scheduler::Access>(use.access), use.requiredState, tracked,
                use.finalState == UINT32_MAX ? use.requiredState : use.finalState,
                scheduler::ExternalState::Known});
            initialStates.push_back(initial);
            expandedSources.push_back(use);
        }
        if (object) objects.push_back(object);
        else retainNativeResource(*nativeContext, state, resource);
    }
    auto* parent = state.activeToken;
    const uint32_t schedulePass = state.nextPass;
    auto plan = state.resources.prepare(schedulePass, declarations.data(),
        static_cast<uint32_t>(declarations.size()), graphicsEffects);
    if (!plan.ok) { state.resources.fail(); state.phase = Phase::Failed; return reject(plan.error); }
    if (parent != nullptr && overlapsAncestors(parent, plan.uses)) {
        state.resources.fail(); state.phase = Phase::Failed;
        return reject("nested declared pass overlaps an active ancestor resource");
    }
    for (const auto& barrier : plan.barriers) {
        auto* resource = static_cast<ID3D12Resource*>(barrier.resource);
        if (barrier.uav) {
            D3D12_RESOURCE_BARRIER nativeBarrier{};
            nativeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            nativeBarrier.UAV.pResource = resource;
            nativeContext->commandList->ResourceBarrier(1, &nativeBarrier);
        } else if (!transitionResourceRange(context, resource, barrier.subresource, 1,
                                             barrier.before, barrier.after)) {
            state.resources.fail(); state.phase = Phase::Failed;
            return reject("declared pass resource transition failed");
        }
    }
    if (!state.resources.commit(plan)) {
        state.resources.fail(); state.phase = Phase::Failed;
        return reject("declared pass schedule commit failed");
    }
    auto owner = std::make_shared<DeclaredPassToken>(DeclaredPassToken{&state, context, parent, pass, schedulePass,
        graphicsEffects, std::move(plan), std::move(expandedSources), std::move(initialStates), false});
    auto* pending = owner.get();
    state.declaredTokens.push_back(std::move(owner));
    for (auto* object : objects) {
        retainObjectForRecording(context, object);
        state.resourceLeases.try_emplace(object->resource.Get(), object->resource);
    }
    state.activeToken = pending;
    *token = pending;
    ++state.nextPass;
    return 1;
}

int finishDeclaredPass(void* context, DeclaredPassToken* token) {
    if (!context || !token) return reject("declared pass token is null");
    auto& state = stateFor(context);
    if (state.activeToken != token || token->finished || token->context != context || token->state != &state ||
        state.phase == Phase::Failed) {
        state.resources.fail(); state.phase = Phase::Failed;
        return reject("declared pass token is stale, repeated, or from another frame");
    }
    token->finished = true;
    state.activeToken = nullptr;
    for (uint32_t index = 0; index < token->plan.uses.size(); ++index) {
        const auto& use = token->plan.uses[index];
        if (token->rasterInputs) {
            if (resourceSubresourceState(context, use.resource, use.subresource, use.initialState) !=
                use.postExternalState) {
                state.resources.fail(); state.phase = Phase::Failed;
                return reject("raster input ended in a state different from the frozen read union");
            }
            continue;
        }
        const auto& source = token->sources[index];
        if (source.externalState == ExternalResourceState::Adopt &&
            !adoptResourceRangeState(context, use.resource, use.subresource, 1,
                                      token->initialStates[index], source.finalState)) {
            state.resources.fail(); state.phase = Phase::Failed;
            return reject("declared pass external state adoption failed");
        }
        const uint32_t actual = resourceSubresourceState(context, use.resource, use.subresource,
                                                         token->initialStates[index]);
        if (actual != use.postExternalState) {
            state.resources.fail(); state.phase = Phase::Failed;
            char diagnostic[256]{};
            std::snprintf(diagnostic, sizeof(diagnostic),
                "declared pass 0x%x resource %u subresource %u ended in 0x%x, expected 0x%x",
                token->pass, index, use.subresource, actual, use.postExternalState);
            return reject(diagnostic);
        }
    }
    restoreState(context, token->effects);
    state.activeToken = token->parent;
    return 1;
}

int beginRasterAttachmentPass(void* context, void** token) {
    if (token) *token = nullptr;
    auto* nativeContext = static_cast<CommandContext*>(context);
    if (!nativeContext || !token || !nativeContext->inRenderPass || !nativeContext->vanillaFrameState)
        return reject("raster attachment declaration requires the active native pass and frame");
    auto& state = *static_cast<FrameState*>(nativeContext->vanillaFrameState.get());
    if (state.context != context || state.activeToken || state.phase == Phase::Idle || state.phase == Phase::Failed) {
        state.resources.fail(); state.phase = Phase::Failed;
        return reject("raster attachment declaration has a stale or nested frame owner");
    }
    auto& uses = state.rasterAttachmentUses;
    uses.clear();
    uses.reserve(nativeContext->activeAttachmentRanges.size());
    for (const auto& range : nativeContext->activeAttachmentRanges) {
        uses.push_back({range.object, state.frame.resourceVersion, range.firstSubresource,
            range.subresourceCount, ResourceAccess::ReadWrite, range.state, D3D12_RESOURCE_STATE_COMMON});
    }
    DeclaredPassToken* pending = nullptr;
    const int result = beginDeclaredPass(context, 0x10000, uses.data(), static_cast<uint32_t>(uses.size()), 0, &pending);
    *token = pending;
    return result;
}

int declareAdditionalRasterAttachment(void* context, void* target) {
    auto* nativeContext = static_cast<CommandContext*>(context);
    if (!nativeContext || !nativeContext->vanillaFrameState || !target)
        return reject("additional raster attachment has no frame or resource");
    auto& state = *static_cast<FrameState*>(nativeContext->vanillaFrameState.get());
    auto* attachment = state.activeToken;
    auto* object = resourceObject(static_cast<Dx12Object*>(target));
    if (!attachment || attachment->pass != 0x10000 || attachment->finished || !object ||
        state.context != context || state.phase == Phase::Failed)
        return reject("additional raster attachment has no active attachment declaration");
    if (std::any_of(attachment->plan.uses.begin(), attachment->plan.uses.end(), [&](const auto& use) {
            return use.resource == object->resource.Get();
        })) return 1;
    const PassResourceUse use{target, state.frame.resourceVersion, 0, 1,
        ResourceAccess::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON};
    attachment->plan.uses.reserve(attachment->plan.uses.size() + 1);
    attachment->sources.reserve(attachment->sources.size() + 1);
    attachment->initialStates.reserve(attachment->initialStates.size() + 1);
    DeclaredPassToken* appended = nullptr;
    if (beginDeclaredPass(context, 0x10000, &use, 1, 0, &appended) != 1) return -1;
    attachment->plan.uses.insert(attachment->plan.uses.end(), appended->plan.uses.begin(), appended->plan.uses.end());
    attachment->sources.insert(attachment->sources.end(), appended->sources.begin(), appended->sources.end());
    attachment->initialStates.insert(attachment->initialStates.end(), appended->initialStates.begin(), appended->initialStates.end());
    appended->finished = true;
    state.activeToken = attachment;
    return 1;
}

void failRasterInputPass(void* context) {
    auto* nativeContext = static_cast<CommandContext*>(context);
    if (!nativeContext || !nativeContext->vanillaFrameState) return;
    auto& state = *static_cast<FrameState*>(nativeContext->vanillaFrameState.get());
    state.resources.fail();
    state.phase = Phase::Failed;
}

int beginRasterInputPass(void* context, uint32_t indexed, void* indirectCommands,
                         const void* extraBindings, uint32_t extraBindingCount, void** token) {
    if (token) *token = nullptr;
    auto* nativeContext = static_cast<CommandContext*>(context);
    if (!nativeContext || !token || indexed > 1 || (extraBindingCount != 0 && !extraBindings))
        return reject("raster input declaration supplied invalid arguments");
    if (!nativeContext->vanillaFrameState) return 0;
    auto& state = *static_cast<FrameState*>(nativeContext->vanillaFrameState.get());
    const auto failed = [&](const char* message) {
        failRasterInputPass(context);
        return reject(message);
    };
    if (state.phase == Phase::Failed || state.resources.failed()) return failed("raster input frame is poisoned");
    if (state.phase == Phase::Idle) return 0;
    if (state.context != context || !nativeContext->listOpen || !nativeContext->inRenderPass ||
        !nativeContext->commandList) return failed("raster input has no active recording owner");
    if (!state.activeToken || state.activeToken->pass != 0x10000) return 0;
    if (const auto collected = collectGraphicsInputs(context, indexed != 0, indirectCommands); !collected)
        return failed(collected.error().message.c_str());
    if (extraBindingCount != 0) {
        if (const auto appended = appendGraphicsInputBindings(context, extraBindings, extraBindingCount); !appended)
            return failed(appended.error().message.c_str());
    }
    auto& declarations = state.rasterDeclarations;
    declarations.clear();
    for (const auto& range : nativeContext->graphicsInputRanges) {
        auto* object = resourceObject(range.object);
        if (!object)
            return failed("raster input has an invalid resource or fixed heap state");
        const auto* layout = resourceLayout(state, object->resource.Get());
        if (!layout) return failed("raster input subresource metadata is invalid");
        if (!compatibleResourceState(layout->flags, object->heapType, range.state, range.state))
            return failed("raster input has an invalid resource or fixed heap state");
        const auto total = layout->subresources;
        if (range.firstSubresource >= total || range.subresourceCount == 0 ||
            range.subresourceCount > total - range.firstSubresource)
            return failed("raster input range exceeds the actual resource layout");
        const uint32_t initial = object->heapType == D3D12_HEAP_TYPE_UPLOAD
            ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
        for (uint32_t subresource = range.firstSubresource;
             subresource < range.firstSubresource + range.subresourceCount; ++subresource) {
            declarations.push_back({object->resource.Get(), state.frame.resourceVersion, subresource, 1, total,
                static_cast<scheduler::Access>(range.access), range.state,
                resourceSubresourceState(context, object->resource.Get(), subresource, initial),
                range.state, scheduler::ExternalState::Known});
        }
    }
    if (declarations.empty()) return 1;
    if (overlapsAncestors(state.activeToken, declarations)) return failed("raster inputs overlap an active attachment");
    if (!state.rasterInputToken) state.rasterInputToken = std::make_shared<DeclaredPassToken>();
    auto& pending = *state.rasterInputToken;
    state.resources.prepareInto(pending.plan, state.nextPass, declarations.data(),
        static_cast<uint32_t>(declarations.size()), 0);
    if (!pending.plan.ok) return failed(pending.plan.error);
    for (const auto& barrier : pending.plan.barriers) {
        if (barrier.uav) {
            D3D12_RESOURCE_BARRIER nativeBarrier{};
            nativeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            nativeBarrier.UAV.pResource = static_cast<ID3D12Resource*>(barrier.resource);
            nativeContext->commandList->ResourceBarrier(1, &nativeBarrier);
        } else if (!transitionResourceRange(context, barrier.resource, barrier.subresource, 1, barrier.before, barrier.after))
            return failed("raster input transition failed");
    }
    if (!state.resources.commit(pending.plan)) return failed("raster input schedule commit failed");
    for (const auto& range : nativeContext->graphicsInputRanges) {
        auto* object = resourceObject(range.object);
        if (!range.owner) retainObjectForRecording(context, object);
        state.resourceLeases.try_emplace(object->resource.Get(), object->resource);
    }
    pending.state = &state;
    pending.context = context;
    pending.parent = state.activeToken;
    pending.pass = 0x10001;
    pending.schedulePass = state.nextPass++;
    pending.effects = 0;
    pending.finished = false;
    pending.rasterInputs = true;
    state.activeToken = &pending;
    *token = &pending;
    return 1;
}

int finishRasterInputPass(void* context, void* token) {
    if (!token) return 1;
    return finishDeclaredPass(context, static_cast<DeclaredPassToken*>(token));
}

int executeSmaaWorldPost(void* context, PostAdapter adapter, void* scene) {
    if (!context || !adapter || !scene) return reject("SMAA context, adapter or scene is null");
    const auto& state = stateFor(context);
    const PassResourceUse uses[] = {
        {scene, state.frame.resourceVersion, UINT32_MAX, 0, ResourceAccess::ReadWrite,
         D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET},
    };
    return executeDeclaredWorldPost(context, frozenPass(state, static_cast<uint32_t>(Pass::Smaa)),
                                     uses, 1, adapter, scene);
}

void reset(void* context) {
    if (context == nullptr) return;
    auto* nativeContext = static_cast<CommandContext*>(context);
    bool completed = !nativeContext->vanillaFrameState;
    if (nativeContext->vanillaFrameState) {
        auto& state = *static_cast<FrameState*>(nativeContext->vanillaFrameState.get());
        completed = state.phase == Phase::GuiReady;
        if (state.phase != Phase::GuiReady || !state.preparedPublished || state.prepared.consumer == 0)
            invalidatePreparedReconstructionHistory();
        if (state.activeToken != nullptr) {
            state.resources.fail();
            state.activeToken->finished = true;
            state.activeToken = nullptr;
        }
        if (!state.depthSeed.closed) cancelWorldDepthSeedCaptureV1(context);
        if (const auto& capture = state.depthSeed.capture)
            for (const auto& event : capture->events) cancelOpaqueSeedPacketCapture(event.packet);
    }
    if (!completed || !fgDisplayFrameQueued(context)) fgAbortDisplayFrame(context);
    detachDepthSeedJournal(nativeContext);
    nativeContext->vanillaFrameState.reset();
    postFailure.clear();
}

}
