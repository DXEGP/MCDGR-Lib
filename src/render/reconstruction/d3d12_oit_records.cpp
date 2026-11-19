module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include "mcdr_oit_shader.h"
#include "mcdr_oit_resolve_shader.h"
#include "mcdr_oit_lane_cull_shader.h"
#include "mcdr_oit_budget.h"
#include "mcdr_terrain_tile.h"
#include "mcdr_graphics_recording.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <tuple>

module mcdr.render.oit;

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.render.vanilla;
import mcdr.error;

namespace d3d12_mc::oit {
namespace {
constexpr uint64_t allocationAlignment = 65536;
constexpr uint32_t drawConstantStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr uint32_t descriptorCount = 64;
constexpr uint32_t tileDescriptorBase = 16;
constexpr uint32_t tileDescriptorCount = 28;
constexpr int success = 1, invalid = -1, unsupported = -2, overBudget = -3, busy = -4, stale = -5,
              unknownSubmission = -6, recordingFailed = MCDR_OIT_EXECUTION_RESULT_RECORDING_FAILED;

struct NormalizationPipelineCertification {
    const mcdr_oit_normalization_pipeline_v1 value;
};

struct Programs {
    void* context = nullptr;
    uint64_t generation = 0;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> initialize, compact, resolve;
    ComPtr<ID3D12RootSignature> laneCullRoot;
    ComPtr<ID3D12PipelineState> laneCullPipeline;
    std::shared_ptr<void> normalizationPrograms;
};
struct PreparedPrograms {
    void* context = nullptr;
    std::shared_ptr<void> identity;
    uint64_t fence = 0;
    std::shared_ptr<Programs> programs;
};
struct Buffer {
    std::unique_ptr<Dx12Object> object;
    std::byte* mapped = nullptr;
    ~Buffer() { if (mapped && object && object->resource) object->resource->Unmap(0, nullptr); }
};
struct ConstantsPage {
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    Buffer buffer;
    uint32_t usedBytes = 0;
};
struct ConstantUpload {
    Dx12Object* buffer;
    D3D12_GPU_VIRTUAL_ADDRESS address;
};
struct Metadata {
    uint32_t sourceBase = 0, primitiveBase = 0, previousBase = 0, sectionByteBase = 0;
    uint32_t sourceCount = 0, primitiveCount = 0, previousCount = 0, sectionCount = 0;
    bool terrain = false;
    std::shared_ptr<void> vertexLease;
    Dx12Object* vertex = nullptr;
    uint64_t vertexOffset = 0;
    uint64_t vertexBytes = 0;
};
struct TerrainSection {
    uint64_t surfaceId, geometryGeneration, materialId, materialGeneration, textureGeneration;
    uint32_t sourceIndex, flags, primitiveBase, primitiveCount;
    float previousOrigin[3];
    uint32_t vertexBase, vertexCount, reserved;
};
static_assert(sizeof(TerrainSection) == 80);
struct Constants { std::array<uint32_t,52> values{}; };
static_assert(sizeof(Constants) == mcdr::oit_shader::captureConstantsBytes);
static_assert(sizeof(Constants) <= drawConstantStride && allocationAlignment % drawConstantStride == 0);
static_assert(mcdr::oit_shader::recordStride == 9 * sizeof(std::array<uint32_t,4>));
static_assert(MCDR_OIT_MAX_RECORDS_PER_PIXEL <= 255u);
enum class Phase { Recording, Sealed, Resolved, Aborted };
enum class FrameAccess { Recording, Retirement };
struct HistoryIdentity {
    mcdr_frame_identity identity{};
    uint64_t historyGeneration = 0, programGeneration = 0;
    uint32_t width = 0, height = 0, colorEncoding = 0;
    float preExposure = 0;
    uint32_t maxRecords = 0, maxRecordsPerPixel = 0;
};
struct HistoryResources {
    HistoryIdentity input{};
    uint64_t serial = 0;
    uint32_t primitiveCount = 0;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget, terrainBudget;
    Buffer records, pixels, sources, primitives, terrain;
    std::vector<mcdr_oit_source_v1> sourceRows;
    std::vector<TerrainSection> terrainRows;
    std::unordered_map<uint64_t,uint32_t> terrainBySurface;
};
struct TextureAllocation {
    ComPtr<ID3D12Resource> resource;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
};
std::mutex textureBudgetMutex;
std::unordered_map<ID3D12Resource*,std::weak_ptr<TextureAllocation>> textureAllocations;
struct Frame;
struct ExecutionScopeState {
    mcdr_oit_execution_scope_v1 value{};
    bool open = false;
};
struct ExecutionEvent {
    uint64_t eventId = 0;
    mcdr_oit_execution_event_v1 value{};
    std::vector<mcdr_oit_execution_resource_v1> resources;
    std::vector<std::shared_ptr<void>> resourceLeases;
    std::shared_ptr<const void> packet;
};
struct ExecutionStream {
    uintptr_t token = 0;
    void* context = nullptr;
    std::shared_ptr<const void> recordingIdentity;
    mcdr_oit_execution_begin_v1 input{};
    uint64_t nextSequence = 1;
    uint64_t nextEventId = 1;
    uint32_t state = MCDR_OIT_EXECUTION_RECORDING;
    uint32_t unavailableReason = MCDR_OIT_EXECUTION_UNAVAILABLE_NONE;
    uint32_t nativeDrawCount = 0;
    uint32_t appendedDrawCount = 0;
    uint32_t openScopeCount = 0;
    uint64_t lastNativeDrawSequence = 0;
    uint32_t resourceCount = 0;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::vector<ExecutionEvent> events;
    std::vector<ExecutionScopeState> scopes;
    std::unordered_set<uint64_t> usedDrawReceipts;
};
struct ExecutionScopeV2 {
    mcdr_oit_execution_scope_v2 value{};
    uint32_t drawCount = 0;
    bool open = false;
};
struct ExecutionScopeIdentity {
    uint64_t producerId = 0;
    uint64_t producerScopeSerial = 0;
    bool operator==(const ExecutionScopeIdentity& other) const noexcept {
        return producerId == other.producerId && producerScopeSerial == other.producerScopeSerial;
    }
};
struct ExecutionScopeIdentityHash {
    size_t operator()(const ExecutionScopeIdentity& identity) const noexcept {
        auto hash = identity.producerId;
        hash ^= identity.producerScopeSerial + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        return static_cast<size_t>(hash ^ (hash >> 32));
    }
};
struct ExecutionPassV2 {
    mcdr_oit_execution_pass_v2 value{};
    bool open = false;
};
struct LaneCopyCommand {
    uint32_t sourceRole = 0;
    uint32_t destinationRole = 0;
};
struct LaneGeometry {
    uint32_t depthRole = 0;
    uint32_t outputRole = 0;
    uint32_t coefficientCount = 0;
};
struct ExecutionOperationEvent {
    mcdr_oit_execution_operation_receipt_v1 receipt{};
    std::shared_ptr<const void> packet;
    mcdr::graphics_recording::AdapterFacts adapter;
    uint64_t eventId = 0;
    LaneCopyCommand laneCopy{};
    std::shared_ptr<const void> laneCullProjection{};
    bool laneWaterMask = false;
    LaneGeometry geometry{};
};
enum class ExecutionCommandSource { Terrain, Operation };
struct ExecutionCommand {
    uint64_t nativeSequence = 0;
    uint64_t receiptId = 0;
    uint64_t eventId = 0;
    ExecutionCommandSource source = ExecutionCommandSource::Terrain;
    uint32_t packetIndex = 0;
};
struct LaneTexture {
    const void* object = nullptr;
    uint32_t subresource = 0;
    bool operator==(const LaneTexture&) const = default;
};
using LaneOperationTextures = std::array<LaneTexture, MCDR_OIT_EXECUTION_V2_RESOURCE_CULLED_BOUNDS + 1>;
struct TerrainExecutionSelection {
    uint64_t nativeSequence = 0;
    uint64_t eventId = 0;
    uint64_t receiptId = 0;
    uint64_t sourcePassId = 0;
    uint64_t sourceDrawId = 0;
    uint64_t parentSourceDrawId = 0;
    uint32_t stage = 0;
    uint32_t packetIndex = UINT32_MAX;
    LaneGeometry geometry{};
};
struct SeedDepthViews {
    Dx12Object* depth;
    uint32_t slices;
    UINT descriptorStride;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::shared_ptr<void> depthLease;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
};
struct ExecutionStreamV2 {
    uintptr_t token = 0;
    mcdr_oit_execution_begin_v2 input{};
    std::shared_ptr<const void> recordingIdentity;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    uint32_t state = MCDR_OIT_EXECUTION_V2_STATE_RECORDING;
    uint32_t missingMask = MCDR_OIT_EXECUTION_V2_GAP_OPAQUE_SEED;
    uint64_t lastReceiptSequence = 0, firstIssueSequence = 0;
    std::vector<mcdr_oit_execution_event_v2> events;
    std::vector<ExecutionScopeV2> scopes;
    std::vector<ExecutionPassV2> passes;
    std::unordered_map<ExecutionScopeIdentity, uint32_t, ExecutionScopeIdentityHash> scopeIndex;
    std::unordered_map<uint64_t, uint32_t> passIndex;
    std::vector<mcdr_oit_execution_resource_v2> resources;
    std::vector<std::shared_ptr<void>> leases;
    std::vector<uint64_t> receipts;
    std::vector<ExecutionOperationEvent> operations;
    std::unique_ptr<const std::vector<ExecutionCommand>> commands;
    LaneOperationTextures laneOperationTextures{};
    std::unique_ptr<const std::vector<TerrainExecutionSelection>> terrainSelections;
    std::optional<mcdr_oit_execution_normalization_v2> normalizationPayload;
    std::optional<mcdr_oit_execution_normalization_v1> legacyNormalizationPayload;
    std::array<std::shared_ptr<const void>, 8> normalizationInputs;
    std::array<uint64_t, 3> opaqueSeedIssueSequences{};
    std::vector<SeedDepthViews> seedDepthViews;
    bool laneDepthCommandsReady = false;
    bool tileExecutionStarted = false;
    bool normalizedPublished = false;
};
struct TerrainTileReplayBatch {
    uint32_t laneIndex = 0;
    std::vector<mcdr::terrain_tile::Tile> tiles;
};
struct TerrainPacketLease {
    TerrainDrawContent content;
    std::shared_ptr<const void> packet;
    mcdr::terrain_tile::PacketPreparation prepare = nullptr;
    std::vector<TerrainTileReplayBatch> replayedBatches;
    uint64_t receiptId = 0;
    uint64_t nativeSequence = 0;
    uint32_t stagePacketIndex = UINT32_MAX;
    bool confirmed = false;
};
struct DrawReceipt {
    mcdr_oit_draw_receipt_v1 value{};
    std::shared_ptr<const void> packet;
};
struct TerrainStagePacket {
    void* batch = nullptr;
    void* vertex = nullptr;
    std::shared_ptr<const void> replayPacket;
    mcdr::terrain_tile::PacketPreparationV2 prepareReplay = nullptr;
    uint64_t vertexOffset = 0;
    std::vector<mcdr_oit_terrain_replay_range_v2> geometry;
    std::vector<mcdr_oit_terrain_upload_read_v2> uploads;
    std::vector<std::shared_ptr<void>> objectLeases;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::shared_ptr<const void> batchLease;
    bool validated = false;
    uint64_t programGeneration = 0, resourceVersion = 0, shaderGeneration = 0;
    uint32_t executionV2Ordinal = UINT32_MAX;
    LaneTexture depth;
    std::array<LaneTexture, 4> colors{};
    std::array<LaneTexture, 5> laneReads{};
    std::array<std::shared_ptr<const void>, mcdr::terrain_tile::laneCount> replayPlans;
};
struct TerrainStageDraw {
    uintptr_t token = 0;
    mcdr_oit_terrain_stage_begin_v2 input{};
    uint32_t terrainReceiptCountAtBegin = 0;
    bool prepared = false;
    uint64_t receiptId = 0;
    TerrainStagePacket packet;
};
struct TerrainStageReceiptReservation {
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::unique_ptr<TerrainStageReceiptReservation> previous;
};
struct ExecutionOperationPacket {
    mcdr_oit_execution_operation_receipt_v1 receipt{};
    std::shared_ptr<const void> packet;
    mcdr::graphics_recording::AdapterFacts adapter;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    bool confirmed = false;
    bool inputPassCompleted = false;
    uint64_t executionV2EventId = 0;
    std::unique_ptr<ExecutionOperationPacket> next;
    ~ExecutionOperationPacket() {
        while (next) {
            auto following = std::move(next->next);
            next = std::move(following);
        }
    }
};
enum class ExecutionOperationState { Active, Finished, Cancelled, Failed };
struct ExecutionOperation {
    uintptr_t token = 0;
    mcdr_oit_execution_operation_begin_v1 input{};
    ExecutionOperationState state = ExecutionOperationState::Active;
    ExecutionOperation* parent = nullptr;
    ExecutionOperationPacket* tail = nullptr;
    uint64_t receiptCount = 0;
    bool hasGpuEffects = false;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::unique_ptr<ExecutionOperationPacket> packets;
    std::unique_ptr<ExecutionOperation> previous;
    ~ExecutionOperation() {
        while (previous) {
            auto preceding = std::move(previous->previous);
            previous = std::move(preceding);
        }
    }
};
struct ExecutionOperationReceiptEntry {
    ExecutionOperation* operation = nullptr;
    ExecutionOperationPacket* packet = nullptr;
};
struct ExecutionTextureCapture {
    mcdr_oit_execution_texture_v1 value{};
    SampledTextureContentCapture content;
    Dx12Object* sourceView = nullptr;
    ComPtr<ID3D12Resource> sourceResource;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::unique_ptr<ExecutionTextureCapture> previous;
    ~ExecutionTextureCapture() { cancelSampledTextureContent(content.lease); }
};
std::mutex registryMutex;
std::unordered_map<uintptr_t,std::weak_ptr<Frame>> registry;
std::atomic<uintptr_t> nextToken{1};
struct Frame {
    void* context = nullptr;
    uintptr_t token = 0;
    uint64_t serial = 0;
    std::shared_ptr<const void> recordingIdentity;
    mcdr_oit_begin_v1 input{};
    std::shared_ptr<const vanilla::WorldDepthSeedJournalV1> opaqueDepthSeed;
    Phase phase = Phase::Recording;
    bool bound = false;
    Dx12Object* boundTerrainVertex = nullptr;
    uint64_t boundTerrainOffset = 0;
    TerrainDrawIdentity boundTerrainIdentity;
    std::vector<TerrainPacketLease> terrainDrawPackets;
    std::vector<DrawReceipt> terrainDrawReceipts;
    std::vector<mcdr_oit_terrain_stage_receipt_v2> terrainStageReceipts;
    std::vector<TerrainStagePacket> terrainStagePackets;
    std::vector<std::pair<uint64_t, uint32_t>> terrainStageOrdinals;
    std::shared_ptr<mcdr::oit_budget::Reservation> terrainStageOrdinalBudget;
    std::unique_ptr<TerrainStageReceiptReservation> terrainStageBudget;
    std::optional<TerrainStageDraw> terrainStageDraw;
    uint64_t nextNativeCommandSequence = 1;
    std::unique_ptr<ExecutionOperation> executionOperations;
    std::map<uint64_t, ExecutionOperationReceiptEntry> executionOperationReceipts;
    ExecutionOperation* activeExecutionOperation = nullptr;
    int32_t pendingTerrainPacket = -1;
    uint64_t nextTerrainSequence = 1;
    std::shared_ptr<ExecutionStream> executionStream;
    std::shared_ptr<ExecutionStreamV2> executionStreamV2;
    std::unique_ptr<ExecutionTextureCapture> executionTextures;
    uint32_t drawCount = 0;
    std::shared_ptr<Programs> programs;
    std::shared_ptr<HistoryResources> resources = std::make_shared<HistoryResources>();
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    Buffer allocator, previous, constants;
    uint32_t maxTerrainSections = 0, maxTerrainPrimitives = 0, terrainPrimitives = 0;
    std::array<float,16> previousProjection{};
    std::array<double,3> previousOrigin{};
    bool previousCameraValid = false;
    std::vector<std::unique_ptr<ConstantsPage>> drawConstantPages;
    D3D12_GPU_VIRTUAL_ADDRESS tileDrawConstants = 0;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    std::vector<mcdr_oit_primitive_v1> primitiveRows;
    uint32_t previousCount = 0, sectionCount = 0;
    std::unordered_map<uintptr_t,Metadata> metadata;
    std::vector<std::shared_ptr<void>> frozenResources;
    std::vector<std::shared_ptr<TextureAllocation>> textureBudgets;
    ~Frame() {
        if (token) { std::lock_guard lock(registryMutex); registry.erase(token); }
    }
};
struct HistoryState {
    struct Pending {
        std::shared_ptr<HistoryResources> resources;
        uint64_t presentFence = 0;
    };
    std::shared_ptr<HistoryResources> presented;
    std::unordered_map<uint64_t,Pending> pending;
};
std::mutex historyMutex;

int reject(int code, const char* reason) { dbgLogDebug("oit: %s", reason); return code; }
uint64_t aligned(uint64_t bytes) { return (bytes + allocationAlignment - 1) & ~(allocationAlignment - 1); }
int recordingStatus(void* opaqueContext) {
    const auto* context=static_cast<const CommandContext*>(opaqueContext);
    if (!context || !context->commandList || !context->recordingIdentity) return invalid;
    if (context->submissionFailed.load()) return unknownSubmission;
    return context->listOpen ? success : busy;
}
std::shared_ptr<Frame> findFrame(void* opaqueContext, void* opaqueFrame, FrameAccess access=FrameAccess::Recording) {
    const auto key = reinterpret_cast<uintptr_t>(opaqueFrame);
    std::shared_ptr<Frame> frame;
    {
        std::lock_guard lock(registryMutex);
        const auto found = registry.find(key);
        if (found == registry.end()) return {};
        frame = found->second.lock();
    }
    if (!frame || frame->context != opaqueContext) return {};
    const auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || (access==FrameAccess::Recording && context->oitFrameState.get()!=frame.get())) return {};
    if (access==FrameAccess::Recording && (!context->listOpen || !context->commandList ||
        context->fenceValue+1!=frame->serial || context->recordingIdentity != frame->recordingIdentity)) return {};
    return frame;
}
void cancelExecutionTextureCaptures(Frame& frame) {
    if (const auto& seed = frame.opaqueDepthSeed)
        for (const auto& event : seed->events) cancelOpaqueSeedPacketCapture(event.packet);
    frame.executionTextures.reset();
    if (frame.executionStreamV2) frame.executionStreamV2->normalizationInputs = {};
    for (auto* operation = frame.executionOperations.get(); operation; operation = operation->previous.get())
        if (operation->input.kind == MCDR_OIT_EXECUTION_V2_WATER_MASK ||
            operation->input.kind == MCDR_OIT_EXECUTION_V2_DRAW)
            for (auto* packet = operation->packets.get(); packet; packet = packet->next.get())
                cancelOpaqueSeedPacketCapture(packet->packet);
}
bool sameIdentity(const mcdr_frame_identity& first, const mcdr_frame_identity& second) {
    return first.worldEpoch == second.worldEpoch && first.realFrameId == second.realFrameId &&
        first.configVersion == second.configVersion && first.resourceVersion == second.resourceVersion;
}
void markExecutionOperationFailure(Frame& frame) {
    static_cast<CommandContext*>(frame.context)->recordingFailed.store(true, std::memory_order_release);
    if (frame.executionStream) frame.executionStream->state = MCDR_OIT_EXECUTION_RECORDING_FAILED;
    if (frame.executionStreamV2) frame.executionStreamV2->missingMask |= MCDR_OIT_EXECUTION_V2_GAP_FAILED;
}
bool cancelActiveExecutionOperations(Frame& frame) {
    bool failed = false;
    for (auto* operation = frame.activeExecutionOperation; operation; operation = operation->parent) {
        const bool operationFailed = operation->hasGpuEffects || operation->receiptCount ||
            operation->state == ExecutionOperationState::Failed ||
            static_cast<CommandContext*>(frame.context)->recordingFailed.load(std::memory_order_acquire);
        failed = failed || operationFailed;
        operation->state = operationFailed ? ExecutionOperationState::Failed : ExecutionOperationState::Cancelled;
        if (!operationFailed) {
            for (auto* packet = operation->packets.get(); packet; packet = packet->next.get())
                frame.executionOperationReceipts.erase(packet->receipt.receiptId);
            operation->packets.reset();
            operation->tail = nullptr;
        }
    }
    if (frame.activeExecutionOperation) unbindGraphicsCommandObserver(frame.context);
    frame.activeExecutionOperation = nullptr;
    if (failed) markExecutionOperationFailure(frame);
    return failed;
}
int prepareExecutionOperationPacket(void* opaqueOwner, void* opaqueContext,
    const mcdr::graphics_recording::PacketFacts& facts, std::shared_ptr<const void> packet) {
    auto& frame = *static_cast<Frame*>(opaqueOwner);
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* operation = frame.activeExecutionOperation;
    if (!operation || !context || frame.context != opaqueContext || frame.phase != Phase::Recording ||
        operation->state != ExecutionOperationState::Active ||
        context->oitFrameState.get() != &frame || frame.recordingIdentity != context->recordingIdentity ||
        frame.serial != context->fenceValue + 1 || context->recordingFailed.load(std::memory_order_acquire) ||
        context->submissionFailed.load(std::memory_order_acquire) || !packet || !context->currentPipeline ||
        (operation->tail && !operation->tail->confirmed)) return invalid;
    const auto& input = operation->input;
    if (facts.pipelineIdentity != reinterpret_cast<uintptr_t>(context->currentPipeline) ||
        (input.pipeline && facts.pipelineIdentity != reinterpret_cast<uintptr_t>(input.pipeline)) ||
        (input.expectedCommandFamily && input.expectedCommandFamily != static_cast<uint32_t>(facts.command.family)) ||
        (input.shaderGeneration && input.shaderGeneration != context->currentPipeline->captureGeneration) ||
        input.programGeneration != frame.input.programGeneration ||
        operation->receiptCount > UINT32_MAX - input.key.commandOrdinal ||
        frame.nextNativeCommandSequence == UINT64_MAX) return stale;
    if (frame.executionStreamV2 &&
        frame.executionOperationReceipts.size() >= frame.executionStreamV2->input.maxEvents) return overBudget;
    try {
        auto budget = mcdr::oit_budget::reserve(sizeof(ExecutionOperationPacket) + 256 + 128,
            frame.input.totalBudgetBytes);
        if (!budget) return overBudget;
        auto captured = std::make_unique<ExecutionOperationPacket>();
        captured->packet = std::move(packet);
        captured->adapter = facts.adapterFacts;
        captured->budget = std::move(budget);
        auto& receipt = captured->receipt;
        receipt.schemaVersion = 1;
        receipt.structSize = sizeof(receipt);
        receipt.kind = input.kind;
        receipt.commandFamily = static_cast<uint32_t>(facts.command.family);
        receipt.receiptId = nextToken.fetch_add(1);
        if (!receipt.receiptId) return recordingFailed;
        receipt.frameSerial = frame.serial;
        receipt.identity = frame.input.identity;
        receipt.key = input.key;
        receipt.key.commandOrdinal += static_cast<uint32_t>(operation->receiptCount);
        receipt.pipelineIdentity = facts.pipelineIdentity;
        receipt.shaderGeneration = context->currentPipeline->captureGeneration;
        receipt.programGeneration = frame.input.programGeneration;
        receipt.resourceVersion = frame.input.identity.resourceVersion;
        receipt.packetId = facts.packetIdentity;
        receipt.readSetId = facts.readSetIdentity;
        receipt.recordState = facts.empty ? MCDR_OIT_OPERATION_EMPTY : MCDR_OIT_OPERATION_RECORDED;
        receipt.contentState = static_cast<uint32_t>(facts.content);
        receipt.adapterState = static_cast<uint32_t>(facts.adapter);
        auto* tail = captured.get();
        if (!frame.executionOperationReceipts.emplace(receipt.receiptId,
                ExecutionOperationReceiptEntry{operation, tail}).second) return stale;
        if (operation->tail) operation->tail->next = std::move(captured);
        else operation->packets = std::move(captured);
        operation->tail = tail;
        return success;
    } catch (const std::bad_alloc&) {
        return overBudget;
    }
}
int confirmExecutionOperationPacket(void* opaqueOwner, void* opaqueContext, const void* packet) {
    auto& frame = *static_cast<Frame*>(opaqueOwner);
    auto* operation = frame.activeExecutionOperation;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!operation || !operation->tail || operation->tail->confirmed ||
        operation->tail->packet.get() != packet || frame.context != opaqueContext ||
        !context || context->oitFrameState.get() != &frame || frame.phase != Phase::Recording ||
        frame.recordingIdentity != context->recordingIdentity || frame.serial != context->fenceValue + 1 ||
        frame.nextNativeCommandSequence == UINT64_MAX) {
        markExecutionOperationFailure(frame);
        return recordingFailed;
    }
    auto& captured = *operation->tail;
    captured.receipt.nativeSequence = frame.nextNativeCommandSequence++;
    captured.confirmed = true;
    ++operation->receiptCount;
    operation->hasGpuEffects = operation->hasGpuEffects || captured.receipt.recordState != MCDR_OIT_OPERATION_EMPTY;
    return success;
}
int completeExecutionOperationPackets(void* opaqueOwner, void* opaqueContext,
    mcdr::graphics_recording::Completion completion) {
    auto& frame = *static_cast<Frame*>(opaqueOwner);
    auto* operation = frame.activeExecutionOperation;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!operation || !context || frame.context != opaqueContext ||
        context->oitFrameState.get() != &frame || frame.phase != Phase::Recording ||
        frame.recordingIdentity != context->recordingIdentity || frame.serial != context->fenceValue + 1)
        return stale;
    if (completion != mcdr::graphics_recording::Completion::Succeeded ||
        (operation->tail && !operation->tail->confirmed)) {
        operation->state = ExecutionOperationState::Failed;
        markExecutionOperationFailure(frame);
        return recordingFailed;
    }
    for (auto* packet = operation->packets.get(); packet; packet = packet->next.get())
        if (packet->confirmed) packet->inputPassCompleted = true;
    return success;
}
}
int beginExecutionOperation(void* opaqueContext, void* opaqueFrame,
    const mcdr_oit_execution_operation_begin_v1& input, void** output) {
    if (output) *output = nullptr;
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findFrame(opaqueContext, opaqueFrame);
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!frame || !output || frame->phase != Phase::Recording || !context ||
        input.schemaVersion != 1 || input.structSize != sizeof(input) ||
        !sameIdentity(input.identity, frame->input.identity) || input.frameSerial != frame->serial ||
        ((input.kind != MCDR_OIT_EXECUTION_V2_DRAW &&
         (input.kind < MCDR_OIT_EXECUTION_V2_COPY_DEPTH || input.kind > MCDR_OIT_EXECUTION_V2_UNCLASSIFIED_DRAW)) ||
         input.kind == MCDR_OIT_EXECUTION_V2_NORMALIZATION) ||
        input.expectedCommandFamily > static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::Mesh) ||
        input.flags || input.reserved || input.key.reserved ||
        !input.key.sourcePassId || !input.key.sourceDrawId ||
        input.key.stage > MCDR_OIT_EXECUTION_STAGE_TERMINAL ||
        input.key.sourceDrawId == input.key.parentSourceDrawId ||
        (input.key.sourceDrawId == 0 && input.key.parentSourceDrawId != 0)) return invalid;
    const bool frameOwned = input.key.producerKind == MCDR_OIT_EXECUTION_V2_FRAME;
    if (frameOwned ? input.key.producerId || input.key.producerScopeSerial :
        input.key.producerKind > MCDR_OIT_SOURCE_CUSTOM_TRANSLUCENT || !input.key.producerId ||
        !input.key.producerScopeSerial || !input.key.stage) return invalid;
    if (context->recordingFailed.load(std::memory_order_acquire)) return recordingFailed;
    if (input.programGeneration != frame->input.programGeneration) return stale;
    if (input.pipeline) {
        const auto* pipeline = reinterpret_cast<const Dx12Pipeline*>(input.pipeline);
        if (!pipeline->lifetimeReferences.load(std::memory_order_acquire) || !pipeline->rootSignature ||
            (!pipeline->isMesh && !pipeline->withDepth && !pipeline->withoutDepth) ||
            (input.shaderGeneration && pipeline->captureGeneration != input.shaderGeneration)) return stale;
    }
    try {
        auto budget = mcdr::oit_budget::reserve(sizeof(ExecutionOperation) + 256, frame->input.totalBudgetBytes);
        if (!budget) return overBudget;
        auto operation = std::make_unique<ExecutionOperation>();
        operation->token = nextToken.fetch_add(1);
        if (!operation->token) return recordingFailed;
        operation->input = input;
        operation->budget = std::move(budget);
        operation->parent = frame->activeExecutionOperation;
        auto* result = operation.get();
        operation->previous = std::move(frame->executionOperations);
        frame->executionOperations = std::move(operation);
        frame->activeExecutionOperation = result;
        mcdr::graphics_recording::Observer observer;
        observer.owner = frame;
        observer.totalBudgetBytes = frame->input.totalBudgetBytes;
        observer.tileDrawScope = [](void* owner) noexcept {
            using mcdr::graphics_recording::TileDrawScope;
            const auto* operation = static_cast<const Frame*>(owner)->activeExecutionOperation;
            if (!operation) return TileDrawScope::None;
            const auto& input = operation->input;
            if (input.kind == MCDR_OIT_EXECUTION_V2_WATER_MASK) return TileDrawScope::Seed;
            if (input.kind == MCDR_OIT_EXECUTION_V2_DRAW &&
                input.key.stage >= MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS &&
                input.key.stage <= MCDR_OIT_EXECUTION_STAGE_ACCUMULATE)
                return static_cast<TileDrawScope>(input.key.stage - MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS);
            return TileDrawScope::None;
        };
        observer.prepare = &prepareExecutionOperationPacket;
        observer.confirm = &confirmExecutionOperationPacket;
        observer.complete = &completeExecutionOperationPackets;
        if (!result->parent) {
            if (const auto status = bindGraphicsCommandObserver(opaqueContext, observer); status != success) {
                frame->activeExecutionOperation = result->parent;
                auto previous = std::move(result->previous);
                frame->executionOperations = std::move(previous);
                return status;
            }
        }
        *output = reinterpret_cast<void*>(result->token);
        return success;
    } catch (const std::bad_alloc&) {
        return overBudget;
    }
}
ExecutionOperation* findExecutionOperation(Frame& frame, void* opaqueOperation) {
    auto* operation = frame.executionOperations.get();
    while (operation) {
        if (operation->token == reinterpret_cast<uintptr_t>(opaqueOperation)) return operation;
        operation = operation->previous.get();
    }
    return nullptr;
}
int finishExecutionOperation(void* opaqueContext, void* opaqueFrame, void* opaqueOperation, uint64_t* output) {
    if (output) *output = 0;
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findFrame(opaqueContext, opaqueFrame);
    if (!frame || !output) return stale;
    auto* operation = findExecutionOperation(*frame, opaqueOperation);
    if (!operation || operation != frame->activeExecutionOperation)
        return stale;
    if (operation->state == ExecutionOperationState::Failed ||
        static_cast<CommandContext*>(opaqueContext)->recordingFailed.load(std::memory_order_acquire)) {
        cancelActiveExecutionOperations(*frame);
        return recordingFailed;
    }
    if (operation->state != ExecutionOperationState::Active) return stale;
    for (auto* packet = operation->packets.get(); packet; packet = packet->next.get())
        if (!packet->confirmed || !packet->inputPassCompleted) return busy;
    operation->state = ExecutionOperationState::Finished;
    *output = operation->receiptCount;
    frame->activeExecutionOperation = operation->parent;
    if (operation->parent) operation->parent->hasGpuEffects = operation->parent->hasGpuEffects || operation->hasGpuEffects;
    if (!frame->activeExecutionOperation) unbindGraphicsCommandObserver(opaqueContext);
    return success;
}
int cancelExecutionOperation(void* opaqueContext, void* opaqueFrame, void* opaqueOperation) {
    auto frame = findFrame(opaqueContext, opaqueFrame, FrameAccess::Retirement);
    if (!frame) return stale;
    auto* operation = findExecutionOperation(*frame, opaqueOperation);
    if (!operation || operation != frame->activeExecutionOperation)
        return stale;
    const bool failed = operation->hasGpuEffects || operation->receiptCount ||
        operation->state == ExecutionOperationState::Failed ||
        static_cast<CommandContext*>(opaqueContext)->recordingFailed.load(std::memory_order_acquire);
    operation->state = failed ? ExecutionOperationState::Failed : ExecutionOperationState::Cancelled;
    if (!failed) {
        for (auto* packet = operation->packets.get(); packet; packet = packet->next.get())
            frame->executionOperationReceipts.erase(packet->receipt.receiptId);
        operation->packets.reset();
    }
    operation->tail = nullptr;
    frame->activeExecutionOperation = operation->parent;
    if (operation->parent) operation->parent->hasGpuEffects = operation->parent->hasGpuEffects || operation->hasGpuEffects;
    if (!frame->activeExecutionOperation) unbindGraphicsCommandObserver(opaqueContext);
    if (failed) markExecutionOperationFailure(*frame);
    return failed ? recordingFailed : success;
}
int queryExecutionOperationReceipt(void* opaqueContext, void* opaqueFrame, void* opaqueOperation,
    uint64_t afterSequence, mcdr_oit_execution_operation_receipt_v1* output) {
    if (output) *output = {};
    auto frame = findFrame(opaqueContext, opaqueFrame, FrameAccess::Retirement);
    if (!frame || !output) return stale;
    auto* operation = findExecutionOperation(*frame, opaqueOperation);
    if (!operation || operation->state == ExecutionOperationState::Cancelled || operation->state == ExecutionOperationState::Failed)
        return stale;
    if (operation->state != ExecutionOperationState::Finished) return busy;
    for (auto* packet = operation->packets.get(); packet; packet = packet->next.get()) {
        if (packet->receipt.nativeSequence > afterSequence && packet->confirmed) {
            *output = packet->receipt;
            return success;
        }
    }
    return 0;
}
namespace {
std::shared_ptr<ExecutionStream> findExecutionStream(void* opaqueContext, void* opaqueStream) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->oitFrameState || !opaqueStream) return {};
    auto frame = std::static_pointer_cast<Frame>(context->oitFrameState);
    if (!frame || !frame->executionStream || frame->executionStream->context != context ||
        reinterpret_cast<uintptr_t>(opaqueStream) != frame->executionStream->token ||
        frame->executionStream->recordingIdentity != context->recordingIdentity ||
        !context->listOpen || !context->commandList || frame->serial != context->fenceValue + 1 ||
        frame->phase == Phase::Aborted) return {};
    return frame->executionStream;
}
bool validExecutionResource(const mcdr_oit_execution_resource_v1& resource) {
    return resource.schemaVersion == 1 && resource.structSize == sizeof(resource) && resource.role >= 1 &&
        resource.role <= 11 && (resource.role == 7 ? resource.roleIndex < 4 : resource.roleIndex == 0) &&
        resource.source >= 1 && resource.source <= 2 && resource.access >= 1 && resource.access <= 3 &&
        resource.object != nullptr && resource.resourceIdentity != 0 && resource.contentVersion != 0 &&
        resource.resourceVersion != 0 && resource.subresourceCount != 0 &&
        resource.firstSubresource <= UINT32_MAX - resource.subresourceCount &&
        resource.byteOffset <= UINT64_MAX - resource.byteLength && resource.reserved[0] == 0 &&
        resource.reserved[1] == 0;
}
std::shared_ptr<Frame> findExecutionFrameV2(void* opaqueContext, void* opaqueStream) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->oitFrameState || !opaqueStream) return {};
    auto frame = std::static_pointer_cast<Frame>(context->oitFrameState);
    if (!frame || frame->context != opaqueContext || !frame->executionStreamV2 ||
        frame->executionStreamV2->token != reinterpret_cast<uintptr_t>(opaqueStream) ||
        frame->recordingIdentity != context->recordingIdentity ||
        frame->executionStreamV2->recordingIdentity != frame->recordingIdentity ||
        frame->serial != context->fenceValue + 1 || !context->commandList || !context->listOpen ||
        frame->phase == Phase::Aborted) return {};
    return frame;
}
bool validExecutionResourceV2(const mcdr_oit_execution_resource_v2& value, uint64_t resourceVersion) {
    const bool objectOnly = value.contentRequirement == MCDR_OIT_EXECUTION_V2_CONTENT_OBJECT_ONLY &&
        !value.ownerIdentity && !value.ownerGeneration && !value.contentVersion;
    if (value.schemaVersion != 2 || value.structSize != sizeof(value) || value.role < 1 || value.role > 11 ||
        (value.role == 7 ? value.roleIndex >= 4 : value.roleIndex != 0) || value.access < 1 || value.access > 3 ||
        value.contentRequirement > 4 || !value.object || !value.resourceIdentity ||
        (!objectOnly && (!value.ownerIdentity || !value.ownerGeneration || !value.contentVersion)) ||
        value.resourceVersion != resourceVersion ||
        value.reserved[0] || value.reserved[1] || value.byteOffset > UINT64_MAX - value.byteLength ||
        value.firstSubresource > UINT32_MAX - value.subresourceCount) return false;
    const auto* object = static_cast<const Dx12Object*>(value.object);
    if (!object->resource || (object->sourceTexture && object->sourceTexture->sourceTexture)) return false;
    if (objectOnly && (object->kind != Dx12Object::Kind::Texture ||
        value.resourceIdentity != reinterpret_cast<uintptr_t>(object))) return false;
    if (object->kind == Dx12Object::Kind::Buffer)
        return value.role == MCDR_OIT_EXECUTION_V2_RESOURCE_EXTERNAL &&
            object->size >= 0 && value.firstSubresource == 0 && value.subresourceCount == 1 &&
            value.byteLength != 0 && value.byteOffset <= static_cast<uint64_t>(object->size) &&
            value.byteLength <= static_cast<uint64_t>(object->size) - value.byteOffset;
    if (object->kind != Dx12Object::Kind::Texture || value.byteOffset || value.byteLength ||
        value.subresourceCount == 0) return false;
    const auto description = object->resource->GetDesc();
    D3D12_FEATURE_DATA_FORMAT_INFO format{description.Format, 0};
    if (FAILED(deviceContext().device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format))))
        return false;
    const uint64_t slices = description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : description.DepthOrArraySize;
    const uint64_t count = slices * description.MipLevels * format.PlaneCount;
    return value.firstSubresource < count && value.subresourceCount <= count - value.firstSubresource;
}

bool coversOriginalLaneExtent(const GraphicsPacketInspection& inspection, const LaneTexture& texture,
    uint32_t width, uint32_t height) {
    const auto* object = static_cast<const Dx12Object*>(texture.object);
    if (!object || texture.subresource != 0 || !inspection.fullScreenTriangle) return false;
    const auto description = object->resource->GetDesc();
    const auto& viewport = inspection.viewport;
    const auto& scissor = inspection.scissor;
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.Width == width && description.Height == height &&
        description.DepthOrArraySize == 1 && description.MipLevels == 1 &&
        description.SampleDesc.Count == 1 &&
        viewport.TopLeftX == 0.f && viewport.TopLeftY == 0.f && viewport.MinDepth == 0.f &&
        viewport.MaxDepth == 1.f && viewport.Width == float(width) && viewport.Height == float(height) &&
        scissor.left == 0 && scissor.top == 0 && scissor.right == static_cast<LONG>(width) &&
        scissor.bottom == static_cast<LONG>(height);
}

bool validLaneScratch(const Dx12Object* texture, uint32_t residentCount, DXGI_FORMAT format) {
    if (!texture || !texture->resource || texture->kind != Dx12Object::Kind::Texture ||
        texture->dxgiFormat != format || !residentCount || residentCount > mcdr::terrain_tile::maximumResidentTiles)
        return false;
    const auto description = texture->resource->GetDesc();
    const auto backingFormat = format == DXGI_FORMAT_D32_FLOAT ? DXGI_FORMAT_R32_TYPELESS : format;
    return description.Format == backingFormat && description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.Width == mcdr::terrain_tile::scratchSize && description.Height == mcdr::terrain_tile::scratchSize &&
        description.DepthOrArraySize == residentCount && description.MipLevels == 1 &&
        description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0;
}
int sampledContentResult(SampledTextureContentStatus status) {
    switch (status) {
    case SampledTextureContentStatus::Ready: return success;
    case SampledTextureContentStatus::UnsupportedView: return unsupported;
    case SampledTextureContentStatus::OrderingUnavailable: return busy;
    case SampledTextureContentStatus::BudgetExceeded: return overBudget;
    case SampledTextureContentStatus::StaleRecording: return stale;
    case SampledTextureContentStatus::RecordingFailed: return recordingFailed;
    }
    return recordingFailed;
}
const ExecutionTextureCapture* findExecutionTexture(const Frame& frame, uint64_t captureId) {
    for (auto* capture = frame.executionTextures.get(); capture; capture = capture->previous.get())
        if (capture->value.captureId == captureId) return capture;
    return nullptr;
}
bool sameExecutionTextureView(const D3D12_SHADER_RESOURCE_VIEW_DESC& first, const D3D12_SHADER_RESOURCE_VIEW_DESC& second) {
    return first.ViewDimension == second.ViewDimension && first.Format == second.Format &&
        first.Shader4ComponentMapping == second.Shader4ComponentMapping &&
        first.Texture2D.MostDetailedMip == second.Texture2D.MostDetailedMip &&
        first.Texture2D.MipLevels == second.Texture2D.MipLevels && first.Texture2D.PlaneSlice == second.Texture2D.PlaneSlice &&
        first.Texture2D.ResourceMinLODClamp == second.Texture2D.ResourceMinLODClamp;
}
bool validNormalizationSourceHashes(const uint8_t* fragment, const uint8_t* vertex, const uint8_t* contract) {
    for (const auto* hash : {fragment, vertex, contract})
        if (std::none_of(hash, hash + 32, [](uint8_t value) { return value != 0; })) return false;
    return true;
}
bool validLegacyNormalizationPayload(const mcdr_oit_execution_event_v2& event, const ExecutionStreamV2& stream) {
    if (event.kind != MCDR_OIT_EXECUTION_V2_NORMALIZATION ||
        event.payloadKind != MCDR_OIT_EXECUTION_V2_PAYLOAD_NORMALIZATION_V1 ||
        event.payloadBytes != sizeof(mcdr_oit_execution_normalization_v1) || !event.payload ||
        event.resourceCount < 6) return false;
    const auto& payload = *static_cast<const mcdr_oit_execution_normalization_v1*>(event.payload);
    if (payload.schemaVersion != 1 || payload.structSize != sizeof(payload) ||
        (payload.targetCount != 1 && payload.targetCount != 2 && payload.targetCount != 4) ||
        payload.certificationVersion != 1 || payload.certificationGeneration != stream.input.programGeneration ||
        !payload.pipelineOwnerIdentity || !payload.pipelineOwnerGeneration ||
        !payload.samplerOwnerIdentity || !payload.samplerOwnerGeneration || !payload.pipeline || !payload.sampler ||
        !payload.outputWidth || !payload.outputHeight || payload.reserved[0] || payload.reserved[1] ||
        !payload.normalized.object || !payload.corrected.object ||
        !payload.normalized.subresourceCount || !payload.corrected.subresourceCount) return false;
    if (!validNormalizationSourceHashes(payload.fragmentSourceHash, payload.vertexSourceHash, payload.contractHash)) return false;
    const uint32_t inputCount = payload.targetCount + 4;
    if (event.resourceCount != inputCount + 2) return false;
    for (uint32_t index = 0; index < inputCount; ++index)
        if (!payload.inputViewIdentities[index] ||
            event.resources[index].resourceIdentity != payload.inputViewIdentities[index]) return false;
    const auto* pipeline = reinterpret_cast<const Dx12Pipeline*>(payload.pipeline);
    const auto* sampler = static_cast<const Dx12Object*>(payload.sampler);
    if (payload.pipelineOwnerIdentity != reinterpret_cast<uint64_t>(pipeline) ||
        payload.samplerOwnerIdentity != reinterpret_cast<uint64_t>(sampler) ||
        sampler->kind != Dx12Object::Kind::Sampler || !pipeline->rootSignature ||
        !pipeline->withDepth || !pipeline->withoutDepth) return false;
    return event.resources[inputCount].role == MCDR_OIT_EXECUTION_V2_RESOURCE_NORMALIZED &&
        event.resources[inputCount + 1].role == MCDR_OIT_EXECUTION_V2_RESOURCE_CORRECTED &&
        event.resources[inputCount].object == payload.normalized.object &&
        event.resources[inputCount + 1].object == payload.corrected.object &&
        event.resources[inputCount].resourceVersion == stream.input.identity.resourceVersion &&
        event.resources[inputCount + 1].resourceVersion == stream.input.identity.resourceVersion;
}
int validateNormalizationPayload(const mcdr_oit_execution_event_v2& event, const Frame& frame) {
    const auto& stream = *frame.executionStreamV2;
    if (event.payloadKind == MCDR_OIT_EXECUTION_V2_PAYLOAD_NORMALIZATION_V1)
        return validLegacyNormalizationPayload(event, stream) ? success : unsupported;
    if (event.kind != MCDR_OIT_EXECUTION_V2_NORMALIZATION ||
        event.payloadKind != MCDR_OIT_EXECUTION_V2_PAYLOAD_NORMALIZATION_V2 ||
        event.payloadBytes != sizeof(mcdr_oit_execution_normalization_v2) || !event.payload)
        return unsupported;
    const auto& payload = *static_cast<const mcdr_oit_execution_normalization_v2*>(event.payload);
    if (payload.schemaVersion != 2 || payload.structSize != sizeof(payload) ||
        (payload.targetCount != 1 && payload.targetCount != 2 && payload.targetCount != 4) ||
        payload.certificationVersion != 1 || payload.certificationGeneration != stream.input.programGeneration ||
        !payload.pipeline || !payload.sampler || payload.outputWidth != frame.input.width ||
        payload.outputHeight != frame.input.height || payload.reserved[0] || payload.reserved[1]) return invalid;
    if (!validNormalizationSourceHashes(payload.fragmentSourceHash, payload.vertexSourceHash, payload.contractHash)) return invalid;
    const uint32_t inputCount = payload.targetCount + 4;
    if (event.resourceCount != inputCount) return invalid;
    for (uint32_t index = inputCount; index < std::size(payload.inputContentIds); ++index)
        if (payload.inputContentIds[index]) return invalid;
    const auto* pipeline = reinterpret_cast<const Dx12Pipeline*>(payload.pipeline);
    const auto* sampler = static_cast<const Dx12Object*>(payload.sampler);
    if (sampler->kind != Dx12Object::Kind::Sampler || sampler->closeRequested.load(std::memory_order_acquire) ||
        !sampler->cpuHandle.ptr || !pipeline->rootSignature || !pipeline->withDepth || !pipeline->withoutDepth ||
        !pipeline->lifetimeReferences.load(std::memory_order_acquire)) return invalid;
    if (!pipeline->normalizationProvenance) return unsupported;
    const auto& certification = static_cast<const NormalizationPipelineCertification*>(
        pipeline->normalizationProvenance.get())->value;
    if (payload.certificationVersion != certification.certificationVersion ||
        payload.certificationGeneration != certification.programGeneration || payload.targetCount != certification.targetCount ||
        !std::equal(std::begin(payload.fragmentSourceHash), std::end(payload.fragmentSourceHash), certification.fragmentSourceHash) ||
        !std::equal(std::begin(payload.vertexSourceHash), std::end(payload.vertexSourceHash), certification.vertexSourceHash) ||
        !std::equal(std::begin(payload.contractHash), std::end(payload.contractHash), certification.contractHash)) return invalid;
    constexpr std::array<uint32_t, 4> inputRoles{MCDR_OIT_EXECUTION_V2_RESOURCE_EXTERNAL,
        MCDR_OIT_EXECUTION_V2_RESOURCE_MAIN_DEPTH, MCDR_OIT_EXECUTION_V2_RESOURCE_Q,
        MCDR_OIT_EXECUTION_V2_RESOURCE_BOUNDS};
    constexpr std::array<DXGI_FORMAT, 4> inputFormats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
    for (uint32_t index = 0; index < inputCount; ++index) {
        const auto& resource = event.resources[index];
        const auto expectedRole = index < inputRoles.size() ? inputRoles[index] : MCDR_OIT_EXECUTION_V2_RESOURCE_COEFFICIENT;
        const auto expectedFormat = index < inputFormats.size() ? inputFormats[index] : DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (resource.role != expectedRole || resource.roleIndex != (index < 4 ? 0 : index - 4) ||
            resource.access != MCDR_OIT_EXECUTION_V2_ACCESS_READ) return invalid;
        if (!payload.inputContentIds[index]) {
            const auto* texture = static_cast<const Dx12Object*>(resource.object);
            if (!texture || texture->kind != Dx12Object::Kind::Texture || !texture->resource ||
                texture->dxgiFormat != expectedFormat || resource.firstSubresource != 0 || resource.subresourceCount != 1 ||
                resource.contentRequirement != MCDR_OIT_EXECUTION_V2_CONTENT_OBJECT_ONLY ||
                (index == 1 && !vanilla::matchesWorldDepthRegistrationV1(frame.context, resource.object))) return invalid;
            const auto desc = texture->resource->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != payload.outputWidth ||
                desc.Height != payload.outputHeight || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
                desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0) return invalid;
            continue;
        }
        const auto* capture = findExecutionTexture(frame, payload.inputContentIds[index]);
        if (!capture || !capture->content.lease || !capture->content.ownerIdentity) return stale;
        const auto& expected = capture->value;
        if (
            resource.contentRequirement != MCDR_OIT_EXECUTION_V2_CONTENT_GPU_COPY ||
            resource.object != expected.texture || resource.resourceIdentity != expected.captureId ||
            resource.ownerIdentity != expected.ownerIdentity || resource.ownerGeneration != expected.ownerGeneration ||
            resource.contentVersion != expected.contentVersion || resource.resourceVersion != expected.resourceVersion ||
            resource.firstSubresource != expected.firstSubresource || resource.subresourceCount != expected.subresourceCount ||
            resource.byteOffset || resource.byteLength || capture->format != expectedFormat) return invalid;
        const auto* texture = static_cast<const Dx12Object*>(expected.texture);
        const auto* view = capture->sourceView;
        if (!texture || texture->kind != Dx12Object::Kind::Texture || !texture->resource ||
            texture->resource.Get() != capture->sourceResource.Get() ||
            texture->dxgiFormat != capture->format || !view || view->sourceTexture != texture || !view->hasSrvDesc ||
            !sameExecutionTextureView(view->srvDesc, capture->description)) return stale;
        const auto description = texture->resource->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1 ||
            description.MipLevels != 1 || description.Width != payload.outputWidth || description.Height != payload.outputHeight ||
            description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0) return invalid;
        const auto resolution = resolveSampledTextureContent(frame.context, capture->content.lease);
        if (const auto status = sampledContentResult(resolution.status); status != success) return status;
        if (!resolution.view) return stale;
    }
    return success;
}
int retainNormalizationPayload(const mcdr_oit_execution_event_v2& event, Frame& frame) {
    auto& stream = *frame.executionStreamV2;
    const auto* legacy = event.payloadKind == MCDR_OIT_EXECUTION_V2_PAYLOAD_NORMALIZATION_V1 ?
        static_cast<const mcdr_oit_execution_normalization_v1*>(event.payload) : nullptr;
    const auto* payload = event.payloadKind == MCDR_OIT_EXECUTION_V2_PAYLOAD_NORMALIZATION_V2 ?
        static_cast<const mcdr_oit_execution_normalization_v2*>(event.payload) : nullptr;
    auto* pipeline = reinterpret_cast<Dx12Pipeline*>(legacy ? legacy->pipeline : payload->pipeline);
    pipeline->lifetimeReferences.fetch_add(1, std::memory_order_relaxed);
    try {
        std::shared_ptr<void> pipelineLease(pipeline, [](void* value) {
            destroyPipeline(static_cast<Dx12Pipeline*>(value));
        });
        stream.leases.push_back(std::move(pipelineLease));
        auto samplerLease = retainObjectLease(legacy ? legacy->sampler : payload->sampler);
        if (!samplerLease) return invalid;
        stream.leases.push_back(std::move(samplerLease));
        if (legacy) {
            for (auto* object : {legacy->normalized.object, legacy->corrected.object}) {
                auto lease = retainObjectLease(object);
                if (!lease) return invalid;
                stream.leases.push_back(std::move(lease));
            }
        } else {
            for (uint32_t index = 0; index < payload->targetCount + 4; ++index) {
                if (!payload->inputContentIds[index]) continue;
                const auto* capture = findExecutionTexture(frame, payload->inputContentIds[index]);
                if (!capture) return stale;
                stream.normalizationInputs[index] = capture->content.lease;
            }
        }
    } catch (const std::bad_alloc&) {
        return overBudget;
    }
    return success;
}
ExecutionScopeState* findScope(ExecutionStream& stream, uint64_t producerId, uint64_t producerScopeSerial,
    uint64_t sourcePassId, uint32_t producerKind, uint32_t stage) {
    for (auto& scope : stream.scopes) {
        const auto& value = scope.value;
        if (value.producerId == producerId && value.producerScopeSerial == producerScopeSerial &&
            value.sourcePassId == sourcePassId && value.producerKind == producerKind && value.stage == stage)
            return &scope;
    }
    return nullptr;
}
ExecutionScopeV2* findScopeV2(ExecutionStreamV2& stream, uint64_t producerId, uint64_t scopeSerial) {
    const auto found = stream.scopeIndex.find(ExecutionScopeIdentity{producerId, scopeSerial});
    if (found == stream.scopeIndex.end() || found->second >= stream.scopes.size()) return nullptr;
    return &stream.scopes[found->second];
}
ExecutionPassV2* findPassV2(ExecutionStreamV2& stream, uint64_t passId) {
    const auto found = stream.passIndex.find(passId);
    if (found == stream.passIndex.end() || found->second >= stream.passes.size()) return nullptr;
    return &stream.passes[found->second];
}
std::pair<uint64_t, uint32_t>* findTerrainSourceOrdinal(
    std::vector<std::pair<uint64_t, uint32_t>>& entries, uint64_t sourceDrawId) {
    if (entries.empty() || !sourceDrawId) return nullptr;
    auto hash = sourceDrawId;
    hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ull;
    hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebull;
    hash ^= hash >> 31;
    auto slot = static_cast<size_t>(hash) & (entries.size() - 1);
    for (size_t visited = 0; visited < entries.size(); ++visited) {
        auto& entry = entries[slot];
        if (!entry.first || entry.first == sourceDrawId) return &entry;
        slot = (slot + 1) & (entries.size() - 1);
    }
    return nullptr;
}
bool compatibleHistory(const HistoryIdentity& previous, const mcdr_oit_begin_v1& current) {
    return previous.identity.worldEpoch==current.identity.worldEpoch &&
        previous.identity.configVersion==current.identity.configVersion && previous.identity.resourceVersion==current.identity.resourceVersion &&
        previous.historyGeneration==current.historyGeneration && previous.programGeneration==current.programGeneration &&
        previous.width==current.width && previous.height==current.height && previous.colorEncoding==current.colorEncoding &&
        previous.preExposure==current.preExposure;
}
bool validTexture(const mcdr_prepared_binding& binding, uint32_t width, uint32_t height) {
    auto* object = binding.object;
    if (!object || object->kind != Dx12Object::Kind::Texture || !object->resource ||
        binding.firstSubresource != 0 || binding.subresourceCount != 1) return false;
    const auto description = object->resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.Width == width &&
        description.Height == height && description.MipLevels == 1 && description.DepthOrArraySize == 1 && description.SampleDesc.Count == 1;
}
int reserveTextureAllocations(std::initializer_list<Dx12Object*> objects, uint64_t maximum,
    std::vector<std::shared_ptr<TextureAllocation>>& output) {
    auto* device = deviceContext().device.Get();
    if (!device) return unsupported;
    std::vector<std::shared_ptr<TextureAllocation>> reservations;
    reservations.reserve(objects.size());
    std::lock_guard lock(textureBudgetMutex);
    std::erase_if(textureAllocations, [](const auto& entry) { return entry.second.expired(); });
    for (const auto* object : objects) {
        auto* resource = object->resource.Get();
        auto& cached = textureAllocations[resource];
        auto allocation = cached.lock();
        if (!allocation) {
            const auto description = resource->GetDesc();
            const auto information = device->GetResourceAllocationInfo(0, 1, &description);
            if (!information.SizeInBytes || information.SizeInBytes == UINT64_MAX) return unsupported;
            auto budget = mcdr::oit_budget::reserve(information.SizeInBytes, maximum);
            if (!budget) return overBudget;
            allocation = std::make_shared<TextureAllocation>();
            allocation->resource = resource;
            allocation->budget = std::move(budget);
            cached = allocation;
        }
        reservations.push_back(std::move(allocation));
    }
    if (mcdr::oit_budget::committedBytes.load(std::memory_order_acquire) > maximum) return overBudget;
    output = std::move(reservations);
    return success;
}
bool createBuffer(Buffer& buffer, uint64_t bytes, D3D12_HEAP_TYPE heapType) {
    auto* device = deviceContext().device.Get();
    if (!device || bytes == 0 || bytes > mcdr::oit_budget::limit) return false;
    auto object = std::make_unique<Dx12Object>();
    object->kind = Dx12Object::Kind::Buffer;
    object->heapType = heapType;
    object->size = static_cast<long long>(bytes);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = heapType;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; description.Width = bytes;
    description.Height = 1; description.DepthOrArraySize = 1; description.MipLevels = 1;
    description.SampleDesc.Count = 1; description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = heapType == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    const auto initial = heapType == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, initial,
        nullptr, IID_PPV_ARGS(&object->resource)))) return false;
    if (heapType == D3D12_HEAP_TYPE_UPLOAD) {
        const D3D12_RANGE readRange{0,0};
        void* mapped = nullptr;
        if (FAILED(object->resource->Map(0, &readRange, &mapped))) return false;
        buffer.mapped = static_cast<std::byte*>(mapped);
    }
    buffer.object = std::move(object);
    return true;
}
std::expected<ConstantUpload, int> uploadFrameConstants(Frame& frame, std::span<const std::byte> data) {
    if (data.empty() || data.size() > allocationAlignment) return std::unexpected(invalid);
    const auto bytes = static_cast<uint32_t>((data.size() + drawConstantStride - 1) & ~size_t(drawConstantStride - 1));
    auto& pages = frame.drawConstantPages;
    if (pages.empty() || pages.back()->usedBytes > allocationAlignment - bytes) {
        auto budget = mcdr::oit_budget::reserve(allocationAlignment, frame.input.totalBudgetBytes);
        if (!budget) return std::unexpected(overBudget);
        auto page = std::make_unique<ConstantsPage>();
        page->budget = std::move(budget);
        if (!createBuffer(page->buffer, allocationAlignment, D3D12_HEAP_TYPE_UPLOAD))
            return std::unexpected(overBudget);
        pages.push_back(std::move(page));
    }
    auto& page = *pages.back();
    const auto offset = page.usedBytes;
    std::memcpy(page.buffer.mapped + offset, data.data(), data.size());
    page.usedBytes += bytes;
    return ConstantUpload{page.buffer.object.get(), page.buffer.object->resource->GetGPUVirtualAddress() + offset};
}
std::expected<mcdr::terrain_tile::ConstantAddresses, int> uploadTileConstants(
    Frame& frame, const mcdr::terrain_tile::BatchV2& batch) {
    if (!frame.tileDrawConstants) {
        constexpr std::array<std::byte, 16> draw{};
        const auto upload = uploadFrameConstants(frame, draw);
        if (!upload) return std::unexpected(upload.error());
        frame.tileDrawConstants = upload->address;
    }
    const mcdr::terrain_tile::BatchConstants constants(batch);
    const auto upload = uploadFrameConstants(frame, std::as_bytes(std::span(&constants, 1)));
    if (!upload) return std::unexpected(upload.error());
    return mcdr::terrain_tile::ConstantAddresses{frame.tileDrawConstants, upload->address};
}
Constants constantsFor(const Frame& frame) {
    Constants constants;
    constants.values[0] = frame.input.width; constants.values[1] = frame.input.height;
    constants.values[2] = frame.input.maxRecords; constants.values[3] = frame.input.maxRecordsPerPixel;
    constants.values[4] = static_cast<uint32_t>(frame.resources->sourceRows.size());
    constants.values[5] = static_cast<uint32_t>(frame.primitiveRows.size());
    constants.values[6] = frame.previousCount;
    constants.values[32] = static_cast<uint32_t>(frame.resources->terrainRows.size());
    constants.values[33] = frame.terrainPrimitives;
    std::memcpy(constants.values.data()+36,frame.previousProjection.data(),64);
    return constants;
}
void createRawView(ID3D12Resource* resource, uint64_t bytes, D3D12_CPU_DESCRIPTOR_HANDLE handle, bool unordered) {
    auto* device = deviceContext().device.Get();
    if (unordered) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS; view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = static_cast<UINT>(bytes / 4); view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(resource, nullptr, &view, handle);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS; view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.NumElements = static_cast<UINT>(bytes / 4); view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(resource, &view, handle);
    }
}
bool createFrameDescriptors(Frame& frame) {
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = descriptorCount; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    auto* device = deviceContext().device.Get();
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&frame.descriptors)))) return false;
    auto handle = frame.descriptors->GetCPUDescriptorHandleForHeapStart();
    const auto increment = deviceContext().srvInc;
    const Buffer* buffers[] = {&frame.resources->sources, &frame.resources->primitives, &frame.previous};
    for (UINT index = 0; index != 3; ++index) {
        createRawView(buffers[index]->object->resource.Get(), static_cast<uint64_t>(buffers[index]->object->size), handle, false);
        handle.ptr += increment;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC source{};
    source.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; source.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    source.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; source.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(nullptr, &source, handle); handle.ptr += increment;
    source.Format = DXGI_FORMAT_R8_UNORM;
    device->CreateShaderResourceView(nullptr, &source, handle); handle.ptr += increment;
    for (UINT index = 0; index != 6; ++index) {
        createRawView(nullptr,4,handle,false);
        handle.ptr += increment;
    }
    for (const auto* buffer : {&frame.resources->records,&frame.resources->pixels,&frame.allocator}) {
        createRawView(buffer->object->resource.Get(),static_cast<uint64_t>(buffer->object->size),handle,true);
        handle.ptr += increment;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC output{};
    output.Format = DXGI_FORMAT_R32_UINT; output.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(nullptr, nullptr, &output, handle);
    handle.ptr += increment;
    output.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(nullptr, nullptr, &output, handle);
    handle.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC tile{};
    tile.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    tile.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    tile.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    tile.Texture2DArray.MipLevels = 1;
    tile.Texture2DArray.ArraySize = mcdr::terrain_tile::maximumResidentTiles;
    for (uint32_t index = 16; index < descriptorCount; ++index) {
        device->CreateShaderResourceView(nullptr, &tile, handle);
        handle.ptr += increment;
    }
    return true;
}

bool validTileResolveTexture(const mcdr::terrain_tile::ResourceRef& reference, uint32_t residentCount) {
    auto* object = static_cast<Dx12Object*>(reference.object);
    if (!object || object->kind != Dx12Object::Kind::Texture || !object->resource ||
        object->dxgiFormat != DXGI_FORMAT_R32G32B32A32_FLOAT || object->resource->GetDesc().MipLevels != 1)
        return false;
    const auto description = object->resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.Width == mcdr::terrain_tile::scratchSize &&
        description.Height == mcdr::terrain_tile::scratchSize &&
        description.DepthOrArraySize >= residentCount && description.SampleDesc.Count == 1 &&
        !(description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}

bool validTileResolveBatch(const mcdr::terrain_tile::BatchV2& batch, const Frame& frame) {
    if (batch.schemaVersion != 2 || batch.structSize != sizeof(batch) || batch.flags != 0 ||
        batch.frameSerial != frame.serial || batch.worldEpoch != frame.input.identity.worldEpoch ||
        batch.programGeneration != frame.input.programGeneration ||
        batch.resourceVersion != frame.input.identity.resourceVersion || batch.laneIndex >= mcdr::terrain_tile::laneCount ||
        batch.residentTileCount == 0 || batch.residentTileCount > mcdr::terrain_tile::maximumResidentTiles)
        return false;
    for (uint32_t index = 0; index < batch.residentTileCount; ++index) {
        const auto& tile = batch.tiles[index];
        if ((tile.originX & 1u) || (tile.originY & 1u) || tile.effectiveWidth == 0 || tile.effectiveHeight == 0 ||
            tile.effectiveWidth > mcdr::terrain_tile::tileSize || tile.effectiveHeight > mcdr::terrain_tile::tileSize)
            return false;
    }
    if (!validTileResolveTexture(batch.scratchColor, batch.residentTileCount) ||
        !validTileResolveTexture(batch.scratchDepth, batch.residentTileCount) ||
        !validTileResolveTexture(batch.q, batch.residentTileCount)) return false;
    for (const auto& coefficient : batch.coefficients)
        if (!validTileResolveTexture(coefficient, batch.residentTileCount)) return false;
    const std::array<void*, 7> resolveObjects = {
        batch.scratchColor.object, batch.scratchDepth.object,
        batch.coefficients[0].object, batch.coefficients[1].object,
        batch.coefficients[2].object, batch.coefficients[3].object,
        batch.q.object,
    };
    for (size_t first = 0; first < resolveObjects.size(); ++first)
        for (size_t second = first + 1; second < resolveObjects.size(); ++second)
            if (resolveObjects[first] == resolveObjects[second]) return false;
    return true;
}

void createTileResolveSrv(Dx12Object* object, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const auto description = object->resource->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MostDetailedMip = 0;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.FirstArraySlice = 0;
    view.Texture2DArray.ArraySize = description.DepthOrArraySize;
    view.Texture2DArray.PlaneSlice = 0;
    view.Texture2DArray.ResourceMinLODClamp = 0.0f;
    deviceContext().device->CreateShaderResourceView(object->resource.Get(), &view, destination);
}

void bindTileResolveDescriptors(const std::shared_ptr<Frame>& frame,
    const std::array<mcdr::terrain_tile::BatchV2, mcdr::terrain_tile::laneCount>& batches) {
    auto handle = frame->descriptors->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(deviceContext().srvInc) * tileDescriptorBase;
    for (const auto& batch : batches) {
        const mcdr::terrain_tile::ResourceRef* resources[] = {
            &batch.scratchColor, &batch.scratchDepth, &batch.coefficients[0], &batch.coefficients[1],
            &batch.coefficients[2], &batch.coefficients[3], &batch.q,
        };
        for (const auto* resource : resources) {
            createTileResolveSrv(static_cast<Dx12Object*>(resource->object), handle);
            handle.ptr += deviceContext().srvInc;
        }
    }
}

void writeTileResolveConstants(const std::shared_ptr<Frame>& frame,
    const std::array<mcdr::terrain_tile::BatchV2, mcdr::terrain_tile::laneCount>& batches) {
    struct TileConstants { uint32_t info[4]; uint32_t regions[mcdr::terrain_tile::maximumResidentTiles][4]; };
    static_assert(sizeof(TileConstants) == 272);
    TileConstants constants{};
    constants.info[0] = 1;
    constants.info[1] = batches[0].residentTileCount;
    constants.info[2] = 15;
    for (uint32_t index = 0; index < batches[0].residentTileCount; ++index) {
        const auto& tile = batches[0].tiles[index];
        constants.regions[index][0] = tile.originX;
        constants.regions[index][1] = tile.originY;
        constants.regions[index][2] = tile.effectiveWidth;
        constants.regions[index][3] = tile.effectiveHeight;
    }
    std::memcpy(frame->constants.mapped + 512, &constants, sizeof(constants));
}

int dispatchRecords(const std::shared_ptr<Frame>& frame, const mcdr_oit_resolve_v3* resolve = nullptr,
    const std::shared_ptr<HistoryResources>& history = {},
    const std::array<mcdr::terrain_tile::BatchV2, mcdr::terrain_tile::laneCount>* tileBatches = nullptr) {
    const bool initialize = resolve == nullptr;
    auto* context = static_cast<CommandContext*>(frame->context);
    const uint64_t version = frame->input.identity.resourceVersion;
    std::vector<vanilla::PassResourceUse> uses;
    for (const auto* buffer : {&frame->resources->records,&frame->resources->pixels,&frame->allocator})
        uses.push_back({nullptr,version,0,1,vanilla::ResourceAccess::Uav,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            UINT32_MAX,vanilla::ExternalResourceState::Tracked,buffer->object->resource.Get(),D3D12_RESOURCE_STATE_COMMON});
    for (const auto* buffer : {&frame->resources->sources,&frame->resources->primitives,&frame->previous,&frame->constants})
        uses.push_back({nullptr,version,0,1,vanilla::ResourceAccess::Read,D3D12_RESOURCE_STATE_GENERIC_READ,
            UINT32_MAX,vanilla::ExternalResourceState::Tracked,buffer->object->resource.Get(),D3D12_RESOURCE_STATE_GENERIC_READ});
    if (frame->resources->terrain.object)
        uses.push_back({nullptr,version,0,1,vanilla::ResourceAccess::Read,D3D12_RESOURCE_STATE_GENERIC_READ,
            UINT32_MAX,vanilla::ExternalResourceState::Tracked,frame->resources->terrain.object->resource.Get(),D3D12_RESOURCE_STATE_GENERIC_READ});
    if (!initialize) {
        for (const auto* input : {&resolve->currentTransparency,&resolve->unrecordedCoverage})
            uses.push_back({input->object,version,0,1,vanilla::ResourceAccess::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
        uses.push_back({resolve->statusOutput.object,version,0,1,vanilla::ResourceAccess::Uav,D3D12_RESOURCE_STATE_UNORDERED_ACCESS});
        uses.push_back({resolve->correctedTransparency.object,version,0,1,vanilla::ResourceAccess::Uav,D3D12_RESOURCE_STATE_UNORDERED_ACCESS});
        if (tileBatches) {
            for (const auto& batch : *tileBatches) {
                const mcdr::terrain_tile::ResourceRef* resources[] = {
                    &batch.scratchColor, &batch.scratchDepth, &batch.coefficients[0], &batch.coefficients[1],
                    &batch.coefficients[2], &batch.coefficients[3], &batch.q,
                };
                for (const auto* resource : resources)
                    uses.push_back({resource->object,version,UINT32_MAX,0,vanilla::ResourceAccess::Read,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
            }
        }
        if (history) {
            for (const auto* buffer : {&history->records,&history->pixels})
                uses.push_back({nullptr,version,0,1,vanilla::ResourceAccess::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    UINT32_MAX,vanilla::ExternalResourceState::Tracked,buffer->object->resource.Get(),D3D12_RESOURCE_STATE_COMMON});
            for (const auto* buffer : {&history->sources,&history->primitives})
                uses.push_back({nullptr,version,0,1,vanilla::ResourceAccess::Read,D3D12_RESOURCE_STATE_GENERIC_READ,
                    UINT32_MAX,vanilla::ExternalResourceState::Tracked,buffer->object->resource.Get(),D3D12_RESOURCE_STATE_GENERIC_READ});
            if (history->terrain.object)
                uses.push_back({nullptr,version,0,1,vanilla::ResourceAccess::Read,D3D12_RESOURCE_STATE_GENERIC_READ,
                    UINT32_MAX,vanilla::ExternalResourceState::Tracked,history->terrain.object->resource.Get(),D3D12_RESOURCE_STATE_GENERIC_READ});
        }
    }
    vanilla::DeclaredPassToken* token = nullptr;
    if (vanilla::beginDeclaredPass(context,2,uses.data(),static_cast<uint32_t>(uses.size()),3,&token) != 1)
        return reject(invalid,"Record compute resource declaration failed");
    auto handle = frame->descriptors->GetGPUDescriptorHandleForHeapStart();
    const uint64_t increment = deviceContext().srvInc;
    auto* list = context->commandList;
    ID3D12DescriptorHeap* heaps[] = {frame->descriptors.Get()}; list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(frame->programs->root.Get());
    list->SetComputeRootConstantBufferView(0,frame->constants.object->resource->GetGPUVirtualAddress() + (initialize ? 0 : 256));
    list->SetComputeRootDescriptorTable(1,handle); handle.ptr += increment * 11;
    list->SetComputeRootDescriptorTable(2,handle);
    list->SetComputeRootConstantBufferView(4, frame->constants.object->resource->GetGPUVirtualAddress() + 512);
    if (tileBatches) {
        auto tileHandle = frame->descriptors->GetGPUDescriptorHandleForHeapStart();
        tileHandle.ptr += increment * tileDescriptorBase;
        list->SetComputeRootDescriptorTable(3, tileHandle);
        writeTileResolveConstants(frame, *tileBatches);
        bindTileResolveDescriptors(frame, *tileBatches);
    } else {
        uint32_t disabledTileConstants[4] = {0, 0, 0, 0};
        std::memcpy(frame->constants.mapped + 512, disabledTileConstants, sizeof(disabledTileConstants));
        auto tileHandle = frame->descriptors->GetGPUDescriptorHandleForHeapStart();
        tileHandle.ptr += increment * tileDescriptorBase;
        list->SetComputeRootDescriptorTable(3, tileHandle);
    }
    list->SetPipelineState(initialize ? frame->programs->initialize.Get() : frame->programs->compact.Get());
    list->Dispatch((frame->input.width+7)/8,(frame->input.height+7)/8,1);
    if (!initialize) {
        const Buffer* barriers[] = {&frame->resources->records,&frame->resources->pixels,&frame->allocator};
        for (const auto* buffer : barriers) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = buffer->object->resource.Get();
            list->ResourceBarrier(1,&barrier);
        }
        D3D12_RESOURCE_BARRIER statusBarrier{};
        statusBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        statusBarrier.UAV.pResource = resolve->statusOutput.object->resource.Get();
        list->ResourceBarrier(1,&statusBarrier);
        list->SetPipelineState(frame->programs->resolve.Get());
        list->Dispatch((frame->input.width+7)/8,(frame->input.height+7)/8,1);
    }
    const auto result = vanilla::finishDeclaredPass(context,token);
    restoreDrawHeap(context);
    return result == 1 ? success : reject(invalid,"Record compute completion failed");
}
}

int createNormalizationPipelineCertification(const mcdr_pipeline_desc& descriptor,
    const mcdr_oit_normalization_pipeline_v1& certification, std::shared_ptr<const void>* output) {
    if (!output) return invalid;
    *output = {};
    if (certification.schemaVersion != 1 || certification.structSize != sizeof(certification) ||
        certification.certificationVersion != 1 || !certification.programGeneration ||
        (certification.targetCount != 1 && certification.targetCount != 2 && certification.targetCount != 4) ||
        certification.reserved[0] || certification.reserved[1] ||
        !validNormalizationSourceHashes(certification.fragmentSourceHash, certification.vertexSourceHash,
            certification.contractHash)) return invalid;
    if (!descriptor.vsData || descriptor.vsLen <= 0 || !descriptor.psData || descriptor.psLen <= 0 ||
        descriptor.colorCount != 1 || !descriptor.colorTargets || descriptor.hasDepth != 1 ||
        descriptor.depth.depthFormat != 51 || descriptor.depth.depthWrite != 1 || descriptor.depth.depthCompareOp != 0 ||
        descriptor.topology != 4 || descriptor.polygonMode != 0 || descriptor.cullEnabled != 1 ||
        descriptor.inputElementCount != 0 || descriptor.pipelineFlags || !descriptor.bindings ||
        descriptor.bindingCount < static_cast<int32_t>(certification.targetCount + 3) ||
        descriptor.bindingCount > static_cast<int32_t>(certification.targetCount + 4)) return unsupported;
    const auto& target = descriptor.colorTargets[0];
    if (target.format != 6 || target.writeMask != 15 || target.blendEnabled != 1 ||
        target.srcColor != 4 || target.dstColor != 9 || target.colorOp != 0 ||
        target.srcAlpha != 4 || target.dstAlpha != 9 || target.alphaOp != 0) return unsupported;
    uint32_t uniformCount = 0, sampledCount = 0;
    for (int32_t index = 0; index < descriptor.bindingCount; ++index) {
        const auto& binding = descriptor.bindings[index];
        if (binding.type > 1 || binding.reg != index) return unsupported;
        if (binding.type == 0) ++uniformCount;
        else ++sampledCount;
    }
    if ((uniformCount != 1 && uniformCount != 2) || sampledCount != certification.targetCount + 2) return unsupported;
    *output = std::make_shared<const NormalizationPipelineCertification>(
        NormalizationPipelineCertification{certification});
    return success;
}

int shaderPrelude(char* destination, uint32_t capacity, uint32_t* required, uint32_t* abi, uint64_t* hash) {
    if (!required || !abi || !hash) return invalid;
    *required = static_cast<uint32_t>(mcdr::oit_shader::prelude.size()+1);
    *abi = mcdr::oit_shader::abiVersion; *hash = mcdr::oit_shader::contractHash;
    if (!destination || !capacity) return success;
    if (capacity < *required) return invalid;
    std::memcpy(destination,mcdr::oit_shader::prelude.data(),*required-1); destination[*required-1] = '\0';
    return success;
}

int preparePrograms(void* opaqueContext, uint64_t generation, void** output,
    const mcdr_oit_normalization_program_v1 (*normalization)[3]) {
    if (output) *output = nullptr;
    if (!opaqueContext || !output || !generation) return MCDR_FRAME_PROGRAMS_INVALID_REQUEST;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (context->oitFrameState) return MCDR_FRAME_PROGRAMS_BUSY;
    auto programs = std::make_shared<Programs>(); programs->context = context; programs->generation = generation;
    programs->budget = mcdr::oit_budget::reserve(4 * 1024 * 1024);
    if (!programs->budget) return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
    if (normalization && prepareNormalizationPrograms(opaqueContext, generation, *normalization,
            &programs->normalizationPrograms) != MCDR_OIT_NORMALIZATION_SUCCESS)
        return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    D3D12_ROOT_PARAMETER roots[5]{};
    roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; roots[0].Descriptor = {0,2};
    for (UINT index = 0; index != 2; ++index) {
        ranges[index].RangeType = index % 2 ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[index].BaseShaderRegister = 0;
        ranges[index].NumDescriptors = index == 0 ? 11 : 5;
        ranges[index].RegisterSpace = 2;
        roots[index+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[index+1].DescriptorTable = {1,&ranges[index]};
    }
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].BaseShaderRegister = 11;
    ranges[2].NumDescriptors = tileDescriptorCount;
    ranges[2].RegisterSpace = 2;
    roots[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    roots[3].DescriptorTable = {1,&ranges[2]};
    roots[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    roots[4].Descriptor = {1,2};
    D3D12_ROOT_SIGNATURE_DESC rootDescription{}; rootDescription.NumParameters = 5; rootDescription.pParameters = roots;
    ComPtr<ID3DBlob> serialized,diagnostic;
    auto* device = deviceContext().device.Get();
    if (!device || FAILED(D3D12SerializeRootSignature(&rootDescription,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&diagnostic)) ||
        FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&programs->root))))
        return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
    const std::string shader = std::string(mcdr::oit_shader::prelude) + std::string(mcdr::oit_shader::resolve);
    const char* entries[] = {"InitializeRecordsV1","CompactRecordsV1","ResolveRgbHistoryV3"};
    ComPtr<ID3D12PipelineState>* pipelines[] = {&programs->initialize,&programs->compact,&programs->resolve};
    for (UINT index = 0; index != 3; ++index) {
        std::vector<uint8_t> dxil;
        const auto compiled = compileHlslToDxil(shader,entries[index],"cs_6_9",dxil);
        if (!compiled) { dbgLogError("oit: shader compilation failed"); return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{}; description.pRootSignature = programs->root.Get();
        description.CS = {dxil.data(),dxil.size()};
        if (FAILED(device->CreateComputePipelineState(&description,IID_PPV_ARGS(pipelines[index]->GetAddressOf()))))
            return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
        options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation) {
        const D3D12_DESCRIPTOR_RANGE textureRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER laneRoots[2]{};
        laneRoots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        laneRoots[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        laneRoots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        laneRoots[1].DescriptorTable = {1, &textureRange};
        laneRoots[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC laneRoot{};
        laneRoot.NumParameters = static_cast<UINT>(std::size(laneRoots));
        laneRoot.pParameters = laneRoots;
        serialized.Reset();
        diagnostic.Reset();
        if (FAILED(D3D12SerializeRootSignature(&laneRoot, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostic)) ||
            FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(&programs->laneCullRoot)))) return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
        const std::string cullShader(mcdr::oit_shader::laneCull);
        std::vector<uint8_t> vertex, fragment;
        if (!compileHlslToDxil(cullShader, "CullBoundsVertex", "vs_6_9", vertex) ||
            !compileHlslToDxil(cullShader, "CullBoundsPixel", "ps_6_9", fragment))
            return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC cull{};
        cull.pRootSignature = programs->laneCullRoot.Get();
        cull.VS = {vertex.data(), vertex.size()};
        cull.PS = {fragment.data(), fragment.size()};
        cull.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        cull.SampleMask = UINT_MAX;
        cull.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        cull.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        cull.RasterizerState.DepthClipEnable = TRUE;
        cull.DepthStencilState.DepthEnable = TRUE;
        cull.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        cull.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        cull.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        cull.NumRenderTargets = 1;
        cull.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        cull.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        cull.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&cull, IID_PPV_ARGS(&programs->laneCullPipeline))))
            return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
    }
    auto candidate = std::make_unique<PreparedPrograms>(); candidate->context = context;
    candidate->identity = context->controlIdentity; candidate->fence = context->fenceValue; candidate->programs = std::move(programs);
    *output = candidate.release(); return MCDR_FRAME_PROGRAMS_SUCCESS;
}
int validatePreparedPrograms(void* opaqueContext, void* opaqueCandidate) {
    if (!opaqueContext || !opaqueCandidate) return MCDR_FRAME_PROGRAMS_INVALID_REQUEST;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* candidate = static_cast<PreparedPrograms*>(opaqueCandidate);
    if (candidate->context != context || candidate->identity != context->controlIdentity) return MCDR_FRAME_PROGRAMS_INVALID_CONTEXT;
    if (context->oitFrameState) return MCDR_FRAME_PROGRAMS_BUSY;
    return candidate->fence == context->fenceValue ? MCDR_FRAME_PROGRAMS_SUCCESS : MCDR_FRAME_PROGRAMS_STALE;
}
void exchangePreparedPrograms(void* opaqueContext, void* opaqueCandidate) noexcept {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* candidate = static_cast<PreparedPrograms*>(opaqueCandidate);
    auto previous=std::static_pointer_cast<Programs>(context->oitPrograms);
    context->oitPrograms=std::move(candidate->programs);
    candidate->programs=std::move(previous);
    {
        std::lock_guard lock(historyMutex);
        context->oitHistoryState.reset();
    }
}
void destroyPreparedPrograms(void* candidate) noexcept { delete static_cast<PreparedPrograms*>(candidate); }

int beginFrame(void* opaqueContext, const mcdr_oit_begin_v1& input, void** output) {
    if (output) *output = nullptr;
    if (const auto status=recordingStatus(opaqueContext); status!=success) return status;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !output || input.schemaVersion != 1 || input.structSize != sizeof(input) || !input.identity.worldEpoch ||
        !input.identity.realFrameId || !input.historyGeneration || !input.programGeneration || !input.width || !input.height ||
        input.width > 16384 || input.height > 16384 || !input.maxRecords || input.maxRecords > UINT32_MAX/mcdr::oit_shader::recordStride ||
        !input.maxRecordsPerPixel || input.maxRecordsPerPixel > MCDR_OIT_MAX_RECORDS_PER_PIXEL ||
        !input.maxSourceRows || !input.maxPrimitiveRows || input.maxSourceRows > UINT32_MAX/32 ||
        input.maxPrimitiveRows > UINT32_MAX/56 || input.maxPreviousRows > UINT32_MAX/80 || input.flags || input.reserved ||
        input.reservedMetadata || !std::isfinite(input.preExposure) || input.preExposure <= 0 || input.colorEncoding > 1 ||
        !input.totalBudgetBytes || input.totalBudgetBytes > mcdr::oit_budget::limit) return reject(invalid,"Invalid frame contract");
    if (context->submissionFailed.load()) return unknownSubmission;
    if (!context->listOpen || !context->commandList || context->inRenderPass || context->oitFrameState) return busy;
    if (!vanilla::matchesFrozenFrame(context,&input.identity,input.historyGeneration)) return stale;
    auto programs = std::static_pointer_cast<Programs>(context->oitPrograms);
    if (!programs || programs->generation != input.programGeneration) return reject(stale,"Capture programs are not prepared");
    for (const auto* binding : {&input.opaqueColor,&input.opaqueDepth,&input.opaqueMotion})
        if (!validTexture(*binding,input.width,input.height)) return reject(invalid,"Opaque frame inputs are invalid");
    if (input.opaqueColor.object->resource == input.opaqueDepth.object->resource ||
        input.opaqueColor.object->resource == input.opaqueMotion.object->resource ||
        input.opaqueDepth.object->resource == input.opaqueMotion.object->resource) return invalid;
    {
        std::lock_guard lock(historyMutex);
        auto history=std::static_pointer_cast<HistoryState>(context->oitHistoryState);
        if (history) {
            if (history->presented && !compatibleHistory(history->presented->input,input)) history->presented.reset();
            for (auto candidate=history->pending.begin(); candidate!=history->pending.end();) {
                if (!compatibleHistory(candidate->second.resources->input,input)) candidate=history->pending.erase(candidate);
                else ++candidate;
            }
            if (history->pending.contains(input.identity.realFrameId)) return stale;
        }
    }
    // GPU work can finish while the opaque pass for the new size is being recorded.
    const auto completed = context->fence->GetCompletedValue();
    if (completed == UINT64_MAX) return unknownSubmission;
    for (UINT slot = 0; slot < std::size(context->recordingLeases); ++slot) {
        if (slot != context->currentListSlot && context->slotFence[slot] && context->slotFence[slot] <= completed)
            context->recordingLeases[slot].clear();
    }
    const uint64_t pixels = static_cast<uint64_t>(input.width) * input.height;
    const uint64_t sourceBytes = static_cast<uint64_t>(input.maxSourceRows)*32;
    const uint64_t primitiveBytes = static_cast<uint64_t>(input.maxPrimitiveRows)*56;
    const uint64_t previousBytes = std::max<uint64_t>(4,static_cast<uint64_t>(input.maxPreviousRows)*80 + static_cast<uint64_t>(input.maxPrimitiveRows)*4);
    const uint64_t sizes[] = {static_cast<uint64_t>(input.maxRecords)*mcdr::oit_shader::recordStride,pixels*mcdr::oit_shader::pixelStride,
        16ull+static_cast<uint64_t>(input.maxRecords)*4,sourceBytes,primitiveBytes,previousBytes,1024};
    const uint64_t historyBytes = aligned(sizes[0]) + aligned(sizes[1]) + aligned(sourceBytes) + aligned(primitiveBytes) + sourceBytes;
    const uint64_t temporaryBytes = allocationAlignment + aligned(static_cast<uint64_t>(descriptorCount)*deviceContext().srvInc) +
        aligned(sizes[2]) + aligned(previousBytes) + aligned(sizes[6]) + primitiveBytes + static_cast<uint64_t>(input.maxPrimitiveRows)*128;
    auto historyBudget = mcdr::oit_budget::reserve(historyBytes,input.totalBudgetBytes);
    if (!historyBudget) return reject(overBudget,"History and inflight resources exceed the shared budget");
    auto budget = mcdr::oit_budget::reserve(temporaryBytes,input.totalBudgetBytes);
    if (!budget) {
        dbgLogDebug("OIT budget: %llu committed, %llu requested",
            static_cast<unsigned long long>(mcdr::oit_budget::committedBytes.load()),
            static_cast<unsigned long long>(temporaryBytes));
        return reject(overBudget,"Frame and inflight resources exceed the shared budget");
    }
    std::vector<std::shared_ptr<TextureAllocation>> textureBudgets;
    if (const auto result = reserveTextureAllocations({input.opaqueColor.object,input.opaqueDepth.object,input.opaqueMotion.object},
        input.totalBudgetBytes,textureBudgets); result != success) return result;
    auto frame = std::make_shared<Frame>(); frame->context = context; frame->serial = context->fenceValue+1;
    frame->recordingIdentity = context->recordingIdentity;
    frame->input = input; frame->programs = std::move(programs); frame->budget = std::move(budget);
    frame->resources->budget = std::move(historyBudget);
    frame->resources->serial = frame->serial;
    frame->resources->input = {input.identity,input.historyGeneration,input.programGeneration,input.width,input.height,
        input.colorEncoding,input.preExposure,input.maxRecords,input.maxRecordsPerPixel};
    frame->textureBudgets = std::move(textureBudgets);
    frame->frozenResources.reserve(6);
    Buffer* buffers[] = {&frame->resources->records,&frame->resources->pixels,&frame->allocator,&frame->resources->sources,&frame->resources->primitives,&frame->previous,&frame->constants};
    for (UINT index = 0; index != 7; ++index)
        if (!createBuffer(*buffers[index],sizes[index],index < 3 ? D3D12_HEAP_TYPE_DEFAULT : D3D12_HEAP_TYPE_UPLOAD))
            return reject(unsupported,"Frame buffer allocation failed");
    frame->resources->sourceRows.reserve(input.maxSourceRows); frame->primitiveRows.reserve(input.maxPrimitiveRows);
    if (!createFrameDescriptors(*frame)) return reject(unsupported,"Frame descriptor allocation failed");
    const auto constants = constantsFor(*frame);
    std::memcpy(frame->constants.mapped,&constants,sizeof(constants));
    for (const auto* binding : {&input.opaqueColor,&input.opaqueDepth,&input.opaqueMotion})
        frame->frozenResources.push_back(retainObjectLease(binding->object));
    auto seedStatus = vanilla::readWorldDepthSeedCaptureV1(context, &frame->opaqueDepthSeed);
    if (seedStatus == vanilla::WorldDepthSeedStatus::Busy)
        seedStatus = vanilla::sealWorldDepthSeedCaptureV1(context, &frame->opaqueDepthSeed);
    if (seedStatus == vanilla::WorldDepthSeedStatus::NotArmed)
        seedStatus = vanilla::sealWorldDepthSeedCaptureV1(context, nullptr);
    if (seedStatus != vanilla::WorldDepthSeedStatus::Ready && seedStatus != vanilla::WorldDepthSeedStatus::NotArmed &&
        seedStatus != vanilla::WorldDepthSeedStatus::BudgetExceeded && seedStatus != vanilla::WorldDepthSeedStatus::Incomplete &&
        seedStatus != vanilla::WorldDepthSeedStatus::Cancelled)
        return reject(seedStatus == vanilla::WorldDepthSeedStatus::StaleRecording ? stale : recordingFailed,
            "Opaque depth seed could not seal at OIT begin");
    if (const auto& seed = frame->opaqueDepthSeed) {
        const auto& identity = seed->frame;
        if (seed->schemaVersion != 1 || seed->context != context ||
            identity.schemaVersion != vanilla::kFrameSchemaVersion || identity.width != input.width ||
            identity.height != input.height || identity.worldEpoch != input.identity.worldEpoch ||
            identity.realFrameId != input.identity.realFrameId || identity.configVersion != input.identity.configVersion ||
            identity.resourceVersion != input.identity.resourceVersion || seed->recordingIdentity != frame->recordingIdentity ||
            seed->fence.Get() != context->fence.Get() || seed->submission != frame->serial ||
            !vanilla::matchesWorldDepthRegistrationV1(context, seed->worldDepth) || !seed->depthOwner ||
            seed->programGeneration != input.programGeneration || !seed->depthDescriptors || !seed->retainedDsv.ptr ||
            seed->events.empty()) return reject(stale,"Opaque depth seed identity differs from the OIT frame");
        uint64_t sequence = 0;
        for (const auto& event : seed->events) {
            if (event.sequence != ++sequence || event.depthTarget != seed->worldDepth ||
                event.programGeneration != seed->programGeneration || event.subresource != 0 ||
                event.subresourceCount != 1 || !event.sourceDsv.ptr || event.dsv.Format != DXGI_FORMAT_D32_FLOAT ||
                event.dsv.ViewDimension != D3D12_DSV_DIMENSION_TEXTURE2D || event.dsv.Texture2D.MipSlice != 0)
                return reject(recordingFailed,"Opaque depth seed event is incomplete");
            if (event.kind == vanilla::WorldDepthSeedEventKind::Draw &&
                (!event.packet || !event.pso || !event.pipelineIdentity || !event.shaderGeneration ||
                    !event.commandOrdinal || event.commandFamily < static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::Draw) ||
                    event.commandFamily > static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::Mesh)))
                return reject(recordingFailed,"Opaque depth seed draw packet is incomplete");
        }
    }
    context->recordingLeases[context->currentListSlot].push_back(frame);
    if (const auto result = dispatchRecords(frame); result != success) return result;
    frame->token = nextToken.fetch_add(1);
    if (!frame->token) return reject(unsupported,"Frame token space exhausted");
    { std::lock_guard lock(registryMutex); registry.emplace(frame->token,frame); }
    context->oitFrameState = frame;
    *output = reinterpret_cast<void*>(frame->token); return success;
}

int beginTerrainFrame(void* context, const mcdr_oit_begin_v2& input, void** output) {
    if (output) *output=nullptr;
    if (input.base.schemaVersion!=2 || input.base.structSize!=sizeof(input) ||
        !input.maxTerrainSectionRows || input.maxTerrainSectionRows>65536 ||
        !input.maxTerrainPrimitives || input.maxTerrainPrimitives>=0x80000000u ||
        input.previousCameraValid>1 || input.reserved) return invalid;
    for (const auto value : input.previousViewProjection) if (!std::isfinite(value)) return invalid;
    for (const auto value : input.previousOrigin) if (!std::isfinite(value)) return invalid;
    const uint64_t bytes=aligned(static_cast<uint64_t>(input.maxTerrainSectionRows)*sizeof(TerrainSection));
    auto budget=mcdr::oit_budget::reserve(bytes,input.base.totalBudgetBytes);
    if (!budget) return overBudget;
    Buffer terrain;
    if (!createBuffer(terrain,bytes,D3D12_HEAP_TYPE_UPLOAD)) return overBudget;
    std::vector<TerrainSection> rows;
    rows.reserve(input.maxTerrainSectionRows);
    auto base=input.base; base.schemaVersion=1; base.structSize=sizeof(base);
    const auto result=beginFrame(context,base,output);
    if (result!=success) return result;
    auto frame=findFrame(context,*output);
    if (!frame) return stale;
    frame->resources->terrain.object.swap(terrain.object);
    std::swap(frame->resources->terrain.mapped,terrain.mapped);
    frame->resources->terrainBudget=std::move(budget);
    frame->maxTerrainSections=input.maxTerrainSectionRows;
    frame->maxTerrainPrimitives=input.maxTerrainPrimitives;
    frame->resources->terrainRows=std::move(rows);
    std::copy_n(input.previousViewProjection,16,frame->previousProjection.begin());
    std::copy_n(input.previousOrigin,3,frame->previousOrigin.begin());
    frame->previousCameraValid=input.previousCameraValid!=0 && input.base.previousRealFrameId!=0;
    auto descriptor=frame->descriptors->GetCPUDescriptorHandleForHeapStart();
    descriptor.ptr+=static_cast<uint64_t>(deviceContext().srvInc)*9;
    createRawView(frame->resources->terrain.object->resource.Get(),bytes,descriptor,false);
    return success;
}

int publishTerrainMetadata(void* opaqueContext, void* opaqueFrame, const mcdr_oit_terrain_metadata_v2& input, void** output) {
    if (output) *output=nullptr;
    if (const auto status=recordingStatus(opaqueContext); status!=success) return status;
    auto frame=findFrame(opaqueContext,opaqueFrame);
    if (!frame || !output || frame->phase!=Phase::Recording || frame->bound || !frame->resources->terrain.object ||
        input.schemaVersion!=2 || input.structSize!=sizeof(input) || !input.sectionCount || !input.sections ||
        !input.primitiveCount || input.source.producerKind!=MCDR_OIT_SOURCE_TERRAIN ||
        !input.source.producerId || !input.source.producerGeneration ||
        input.source.shaderGeneration!=frame->input.programGeneration || (input.source.validity&~15u)) return invalid;
    auto* vertex=input.vertexBuffer;
    if (!vertex || vertex->kind!=Dx12Object::Kind::Buffer || !vertex->resource || vertex->size<=0 ||
        vertex->heapType==D3D12_HEAP_TYPE_READBACK || input.vertexOffset%4 ||
        input.vertexOffset>static_cast<uint64_t>(vertex->size) || !input.vertexBytes ||
        input.vertexBytes>static_cast<uint64_t>(vertex->size)-input.vertexOffset) return invalid;
    uint32_t prefix=0;
    for (uint32_t index=0;index<input.sectionCount;++index) {
        const auto& row=input.sections[index];
        if (!row.surfaceId || !row.geometryGeneration || !row.materialId || !row.materialGeneration ||
            !row.textureGeneration || row.primitiveBase!=prefix || !row.primitiveCount || row.primitiveCount%2 ||
            row.primitiveCount>0x7fffffffu-prefix || (row.flags&~15u) || row.vertexCount%4 ||
            static_cast<uint64_t>(row.vertexCount)*3!=static_cast<uint64_t>(row.primitiveCount)*6 ||
            static_cast<uint64_t>(row.vertexBase)*28>input.vertexBytes ||
            static_cast<uint64_t>(row.vertexCount)*28>input.vertexBytes-static_cast<uint64_t>(row.vertexBase)*28) return invalid;
        prefix+=row.primitiveCount;
    }
    if (prefix!=input.primitiveCount) return invalid;
    if (input.sectionCount>frame->maxTerrainSections-frame->resources->terrainRows.size() ||
        input.primitiveCount>frame->maxTerrainPrimitives-frame->terrainPrimitives ||
        frame->resources->sourceRows.size()>=frame->input.maxSourceRows || frame->metadata.size()>=frame->maxTerrainSections)
        return MCDR_OIT_RESULT_METADATA_CAPACITY_EXCEEDED;
    std::shared_ptr<HistoryResources> history;
    {
        std::lock_guard lock(historyMutex);
        auto state=std::static_pointer_cast<HistoryState>(static_cast<CommandContext*>(opaqueContext)->oitHistoryState);
        if (state) history=state->presented;
    }
    if (history && (history->input.identity.realFrameId!=frame->input.previousRealFrameId ||
        !compatibleHistory(history->input,frame->input))) history.reset();
    Metadata metadata;
    metadata.terrain=true;
    metadata.sourceBase=static_cast<uint32_t>(frame->resources->sourceRows.size()); metadata.sourceCount=1;
    metadata.sectionByteBase=static_cast<uint32_t>(frame->resources->terrainRows.size()); metadata.sectionCount=input.sectionCount;
    metadata.primitiveBase=frame->terrainPrimitives; metadata.primitiveCount=input.primitiveCount;
    metadata.vertex=vertex; metadata.vertexOffset=input.vertexOffset; metadata.vertexBytes=input.vertexBytes;
    metadata.vertexLease=retainObjectLease(vertex);
    const uintptr_t token=nextToken.fetch_add(1);
    if (!token) return unsupported;
    std::vector<TerrainSection> rows;
    rows.reserve(input.sectionCount);
    for (uint32_t index=0;index<input.sectionCount;++index) {
        const auto& row=input.sections[index];
        TerrainSection target{};
        target.surfaceId=row.surfaceId; target.geometryGeneration=row.geometryGeneration;
        target.materialId=row.materialId; target.materialGeneration=row.materialGeneration; target.textureGeneration=row.textureGeneration;
        target.sourceIndex=metadata.sourceBase; target.flags=row.flags&~6u;
        target.primitiveBase=frame->terrainPrimitives+row.primitiveBase; target.primitiveCount=row.primitiveCount;
        target.vertexBase=row.vertexBase; target.vertexCount=row.vertexCount;
        const int32_t coordinates[]{row.sectionX,row.sectionY,row.sectionZ};
        bool finite=true;
        for (uint32_t component=0;component<3;++component) {
            target.previousOrigin[component]=static_cast<float>(static_cast<double>(coordinates[component])-frame->previousOrigin[component]);
            finite=finite&&std::isfinite(target.previousOrigin[component]);
        }
        if (history && finite && frame->previousCameraValid) {
            const auto found=history->terrainBySurface.find(row.surfaceId);
            if (found!=history->terrainBySurface.end()) {
                const auto& old=history->terrainRows[found->second];
                if (old.surfaceId!=row.surfaceId || old.geometryGeneration!=row.geometryGeneration ||
                    old.primitiveCount!=row.primitiveCount) { rows.push_back(target); continue; }
                const auto& source=history->sourceRows[old.sourceIndex];
                if (source.producerId!=input.source.producerId || source.producerGeneration!=input.source.producerGeneration ||
                    source.shaderGeneration!=input.source.shaderGeneration) { rows.push_back(target); continue; }
                target.flags|=row.flags&2u;
                if (old.materialId==row.materialId && old.materialGeneration==row.materialGeneration &&
                    old.textureGeneration==row.textureGeneration) target.flags|=row.flags&4u;
            }
        }
        rows.push_back(target);
    }
    frame->metadata.emplace(token,std::move(metadata));
    frame->resources->sourceRows.push_back(input.source);
    std::memcpy(frame->resources->sources.mapped+static_cast<uint64_t>(frame->resources->sourceRows.size()-1)*32,&input.source,32);
    std::memcpy(frame->resources->terrain.mapped+frame->resources->terrainRows.size()*sizeof(TerrainSection),rows.data(),rows.size()*sizeof(TerrainSection));
    for (const auto& row : rows) {
        frame->resources->terrainBySurface.insert_or_assign(row.surfaceId,static_cast<uint32_t>(frame->resources->terrainRows.size()));
        frame->resources->terrainRows.push_back(row);
    }
    frame->terrainPrimitives+=input.primitiveCount;
    *output=reinterpret_cast<void*>(token); return success;
}

int publishMetadata(void* context, void* opaqueFrame, const mcdr_oit_metadata_v1& input, void** output) {
    if (output) *output = nullptr;
    if (const auto status=recordingStatus(context); status!=success) return status;
    auto frame = findFrame(context,opaqueFrame);
    if (!frame || !output || frame->phase != Phase::Recording || frame->bound || input.schemaVersion != 1 || input.structSize != sizeof(input) ||
        !input.sourceCount || !input.primitiveCount || !input.sources || !input.primitives ||
        (input.previousCount && !input.previous) || (input.sectionCount && !input.sectionPrimitiveBases)) return invalid;
    const auto& limits = frame->input;
    for (uint32_t index=0; index<input.sourceCount; ++index) {
        const auto& row = input.sources[index];
        if (!row.producerId || !row.producerGeneration || row.shaderGeneration!=frame->input.programGeneration ||
            row.producerKind==MCDR_OIT_SOURCE_TERRAIN || row.producerKind > MCDR_OIT_SOURCE_CUSTOM_TRANSLUCENT ||
            (row.validity&~15u) || (row.producerKind >= MCDR_OIT_SOURCE_CUSTOM_WEATHER && row.validity)) return invalid;
    }
    for (uint32_t index=0; index<input.primitiveCount; ++index) {
        const auto& row = input.primitives[index];
        if (!row.surfaceId || !row.geometryGeneration || !row.materialId || !row.materialGeneration || !row.textureGeneration ||
            row.sourceIndex >= input.sourceCount || (row.previousIndex != UINT32_MAX && row.previousIndex >= input.previousCount) ||
            row.reserved || (row.flags&~15u)) return invalid;
        if ((row.flags&6u) && (row.previousIndex==UINT32_MAX || (row.flags&6u&~input.previous[row.previousIndex].flags))) return invalid;
    }
    for (uint32_t index=0; index<input.previousCount; ++index) {
        const auto& row = input.previous[index];
        if (row.reserved || (row.flags&~15u)) return invalid;
        for (const float component : row.clip) if (!std::isfinite(component)) return invalid;
        for (const float component : row.materialUv) if (!std::isfinite(component)) return invalid;
    }
    uint32_t preceding = 0;
    for (uint32_t index=0; index<input.sectionCount; ++index) {
        const auto base = input.sectionPrimitiveBases[index];
        if (base < preceding || base > input.primitiveCount) return invalid;
        preceding = base;
    }
    if (input.sourceCount > limits.maxSourceRows-frame->resources->sourceRows.size() ||
        input.primitiveCount > limits.maxPrimitiveRows-frame->primitiveRows.size() || input.previousCount > limits.maxPreviousRows-frame->previousCount ||
        input.sectionCount > limits.maxPrimitiveRows-frame->sectionCount || frame->metadata.size() >= limits.maxPrimitiveRows)
        return MCDR_OIT_RESULT_METADATA_CAPACITY_EXCEEDED;
    Metadata metadata;
    metadata.sourceBase = static_cast<uint32_t>(frame->resources->sourceRows.size());
    metadata.primitiveBase = static_cast<uint32_t>(frame->primitiveRows.size()); metadata.previousBase = frame->previousCount;
    metadata.sectionByteBase = limits.maxPreviousRows*80 + frame->sectionCount*4;
    metadata.sourceCount=input.sourceCount; metadata.primitiveCount=input.primitiveCount;
    metadata.previousCount=input.previousCount; metadata.sectionCount=input.sectionCount;
    frame->resources->sourceRows.insert(frame->resources->sourceRows.end(),input.sources,input.sources+input.sourceCount);
    std::memcpy(frame->resources->sources.mapped+static_cast<uint64_t>(metadata.sourceBase)*32,input.sources,static_cast<size_t>(input.sourceCount)*32);
    for (uint32_t index=0; index<input.primitiveCount; ++index) {
        auto row=input.primitives[index]; row.sourceIndex+=metadata.sourceBase;
        if (row.previousIndex!=UINT32_MAX) row.previousIndex+=metadata.previousBase;
        frame->primitiveRows.push_back(row);
    }
    std::memcpy(frame->resources->primitives.mapped+static_cast<uint64_t>(metadata.primitiveBase)*56,
        frame->primitiveRows.data()+metadata.primitiveBase,static_cast<size_t>(input.primitiveCount)*56);
    if (input.previousCount) std::memcpy(frame->previous.mapped+static_cast<uint64_t>(metadata.previousBase)*80,
        input.previous,static_cast<size_t>(input.previousCount)*80);
    if (input.sectionCount) std::memcpy(frame->previous.mapped+metadata.sectionByteBase,input.sectionPrimitiveBases,static_cast<size_t>(input.sectionCount)*4);
    frame->previousCount+=input.previousCount; frame->sectionCount+=input.sectionCount;
    const uintptr_t token=nextToken.fetch_add(1);
    if (!token) return unsupported;
    frame->metadata.emplace(token,metadata);
    *output=reinterpret_cast<void*>(token); return success;
}

int bindDraw(void* opaqueContext, void* opaqueFrame, void* opaqueMetadata, const mcdr_oit_draw_v1& input) {
    if (const auto status=recordingStatus(opaqueContext); status!=success) return status;
    auto frame=findFrame(opaqueContext,opaqueFrame);
    if (!frame || frame->phase!=Phase::Recording || frame->bound || input.schemaVersion!=1 || input.structSize!=sizeof(input) ||
        input.flags || input.reserved || !input.producerScopeSerial || !input.primitiveCount) return invalid;
    const auto found=frame->metadata.find(reinterpret_cast<uintptr_t>(opaqueMetadata));
    if (found==frame->metadata.end()) return stale;
    const auto& metadata=found->second;
    if (input.sourceIndex>=metadata.sourceCount || input.primitiveBase>metadata.primitiveCount ||
        input.primitiveCount>metadata.primitiveCount-input.primitiveBase || !input.instanceCount ||
        input.firstInstance>UINT32_MAX-input.instanceCount ||
        (input.primitivesPerInstance && static_cast<uint64_t>(input.primitivesPerInstance)*input.instanceCount!=input.primitiveCount)) return invalid;
    if (metadata.terrain) {
        if (input.sourceIndex || input.primitiveBase || input.primitiveCount!=metadata.primitiveCount ||
            input.instanceCount!=1 || input.firstInstance) return invalid;
    } else {
        for (uint32_t index=0; index<input.primitiveCount; ++index)
            if (frame->primitiveRows[metadata.primitiveBase+input.primitiveBase+index].sourceIndex!=metadata.sourceBase+input.sourceIndex) return invalid;
    }
    // The recording already retains Frame. Captured packets can retain its buffers
    // independently without extending Frame through a cycle in its replay packets.
    const auto& draw = frame->resources;
    auto constants=constantsFor(*frame);
    constants.values[4]=metadata.sourceCount; constants.values[5]=metadata.primitiveCount; constants.values[6]=metadata.previousCount;
    constants.values[7]=metadata.sectionCount; constants.values[8]=metadata.sourceBase; constants.values[9]=metadata.primitiveBase;
    constants.values[10]=metadata.previousBase; constants.values[11]=metadata.sectionByteBase;
    constants.values[12]=input.sourceIndex; constants.values[13]=input.primitiveBase; constants.values[14]=input.primitiveCount;
    constants.values[16]=input.primitivesPerInstance; constants.values[17]=input.instanceCount; constants.values[18]=input.firstInstance;
    constants.values[20]=static_cast<uint32_t>(input.producerScopeSerial); constants.values[21]=static_cast<uint32_t>(input.producerScopeSerial>>32);
    if (metadata.terrain) {
        constants.values[4]=static_cast<uint32_t>(frame->resources->sourceRows.size());
        constants.values[5]=static_cast<uint32_t>(frame->primitiveRows.size());
        constants.values[34]=metadata.sectionByteBase; constants.values[35]=metadata.sectionCount;
    }
    const auto upload = uploadFrameConstants(*frame, std::as_bytes(std::span(&constants, 1)));
    if (!upload) return upload.error();
    auto* context=static_cast<CommandContext*>(opaqueContext);
    GraphicsCaptureBinding binding;
    binding.contractHash=mcdr::oit_shader::contractHash; binding.generation=frame->input.programGeneration;
    binding.constants=upload->address;
    binding.descriptors=frame->descriptors->GetCPUDescriptorHandleForHeapStart(); binding.lease=draw;
    for (auto* buffer : {frame->resources->sources.object.get(), frame->resources->primitives.object.get(),
                        frame->previous.object.get(), upload->buffer})
        binding.resources.push_back({buffer,0,1,D3D12_RESOURCE_STATE_GENERIC_READ,1,draw});
    if (metadata.terrain) {
        binding.resources.push_back({frame->resources->terrain.object.get(),0,1,D3D12_RESOURCE_STATE_GENERIC_READ,1,draw});
        const auto state=metadata.vertex->heapType==D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        binding.resources.push_back({metadata.vertex,0,1,static_cast<uint32_t>(state),1,metadata.vertexLease});
        frame->frozenResources.push_back(metadata.vertexLease);
        frame->boundTerrainVertex=metadata.vertex;
        frame->boundTerrainOffset=metadata.vertexOffset;
        const auto& source=frame->resources->sourceRows[metadata.sourceBase];
        auto& identity=frame->boundTerrainIdentity;
        identity={};
        identity.frame=frame->input.identity;
        identity.frameSerial=frame->serial;
        identity.producerScopeSerial=input.producerScopeSerial;
        identity.programGeneration=frame->input.programGeneration;
        identity.metadata=reinterpret_cast<uintptr_t>(opaqueMetadata);
        identity.vertexBytes=metadata.vertexBytes;
        identity.producerId=source.producerId;
        identity.producerGeneration=source.producerGeneration;
        identity.shaderGeneration=source.shaderGeneration;
        identity.sectionBase=metadata.sectionByteBase;
        identity.sectionCount=metadata.sectionCount;
        identity.primitiveBase=metadata.primitiveBase;
        identity.primitiveCount=metadata.primitiveCount;
    }
    for (auto* buffer : {&frame->resources->records,&frame->resources->pixels,&frame->allocator})
        binding.resources.push_back({buffer->object.get(),0,1,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4,draw});
    context->graphicsCapture=std::move(binding); frame->bound=true; ++frame->drawCount;
    return success;
}
int bindTerrainDraw(void* context, void* opaqueFrame, void* opaqueMetadata, const mcdr_oit_terrain_draw_v2& input) {
    if (input.schemaVersion!=2 || input.structSize!=sizeof(input) || !input.producerScopeSerial) return invalid;
    auto frame=findFrame(context,opaqueFrame);
    if (!frame) return stale;
    const auto found=frame->metadata.find(reinterpret_cast<uintptr_t>(opaqueMetadata));
    if (found==frame->metadata.end() || !found->second.terrain) return stale;
    mcdr_oit_draw_v1 draw{};
    draw.schemaVersion=1; draw.structSize=sizeof(draw); draw.primitiveCount=found->second.primitiveCount;
    draw.producerScopeSerial=input.producerScopeSerial; draw.instanceCount=1;
    return bindDraw(context,opaqueFrame,opaqueMetadata,draw);
}
int validateTerrainDraw(void* opaqueContext, void* vertex, uint64_t offset, TerrainDrawIdentity* identity) {
    auto* context=static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->oitFrameState || !context->graphicsCapture.lease) return invalid;
    auto frame=std::static_pointer_cast<Frame>(context->oitFrameState);
    if (!identity || frame->phase!=Phase::Recording || !frame->bound || frame->boundTerrainVertex!=vertex ||
        frame->recordingIdentity != context->recordingIdentity || frame->boundTerrainOffset!=offset ||
        context->graphicsCapture.remainingDraws!=1) return invalid;
    *identity=frame->boundTerrainIdentity;
    identity->sections.reserve(identity->sectionCount);
    for (uint32_t index=0; index<identity->sectionCount; ++index) {
        const auto& row=frame->resources->terrainRows[identity->sectionBase+index];
        identity->sections.push_back({row.surfaceId,row.geometryGeneration,row.materialId,row.materialGeneration,
            row.textureGeneration,row.vertexBase,row.vertexCount,row.primitiveBase,row.primitiveCount});
    }
    return success;
}
int retainTerrainDrawPacket(void* opaqueContext, const TerrainDrawIdentity& identity,
    TerrainDrawContent content, std::shared_ptr<const void> packet,
    mcdr::terrain_tile::PacketPreparation prepare) {
    auto* context=static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->oitFrameState || !packet ||
        (content == TerrainDrawContent::OriginalDrawOnly && !prepare) ||
        (content != TerrainDrawContent::OriginalDrawOnly && content != TerrainDrawContent::UnsupportedContent))
        return invalid;
    auto frame=std::static_pointer_cast<Frame>(context->oitFrameState);
    const auto& bound=frame->boundTerrainIdentity;
    if (frame->phase!=Phase::Recording || !frame->bound || !frame->boundTerrainVertex ||
        frame->recordingIdentity != context->recordingIdentity ||
        identity.schemaVersion!=1 || identity.frameSerial!=frame->serial ||
        identity.producerScopeSerial!=bound.producerScopeSerial || identity.metadata!=bound.metadata ||
        identity.programGeneration!=bound.programGeneration || identity.frame.realFrameId!=bound.frame.realFrameId ||
        identity.frame.worldEpoch!=bound.frame.worldEpoch || identity.frame.configVersion!=bound.frame.configVersion ||
        identity.frame.resourceVersion!=bound.frame.resourceVersion || frame->pendingTerrainPacket >= 0 ||
        !context->graphicsCapture.lease || context->graphicsCapture.remainingDraws!=0) return invalid;
    if (frame->terrainDrawPackets.size() >= INT32_MAX || frame->nextTerrainSequence == UINT64_MAX) return overBudget;
    frame->terrainDrawReceipts.reserve(frame->terrainDrawReceipts.size() + 1);
    frame->terrainDrawPackets.push_back({content, std::move(packet), prepare});
    frame->pendingTerrainPacket = static_cast<int32_t>(frame->terrainDrawPackets.size() - 1);
    return success;
}
int confirmTerrainDrawReceipt(void* opaqueContext) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->oitFrameState || context->submissionFailed.load()) return unknownSubmission;
    auto frame = std::static_pointer_cast<Frame>(context->oitFrameState);
    if (!frame || frame->phase != Phase::Recording || !frame->bound || frame->pendingTerrainPacket < 0 ||
        frame->pendingTerrainPacket >= static_cast<int32_t>(frame->terrainDrawPackets.size()) ||
        frame->recordingIdentity != context->recordingIdentity)
        return invalid;
    auto& packet = frame->terrainDrawPackets[static_cast<size_t>(frame->pendingTerrainPacket)];
    if (packet.confirmed || packet.receiptId || frame->terrainDrawReceipts.size() >= frame->terrainDrawReceipts.capacity())
        return recordingFailed;
    const uint64_t receiptId = nextToken.fetch_add(1);
    if (!receiptId) return recordingFailed;
    packet.receiptId = receiptId;
    packet.nativeSequence = frame->nextTerrainSequence++;
    packet.confirmed = true;
    DrawReceipt receipt;
    receipt.packet = packet.packet;
    receipt.value.schemaVersion = 1;
    receipt.value.structSize = sizeof(receipt.value);
    receipt.value.receiptId = packet.receiptId;
    receipt.value.nativeSequence = packet.nativeSequence;
    receipt.value.frameSerial = frame->serial;
    receipt.value.producerScopeSerial = frame->boundTerrainIdentity.producerScopeSerial;
    receipt.value.producerId = frame->boundTerrainIdentity.producerId;
    receipt.value.producerGeneration = frame->boundTerrainIdentity.producerGeneration;
    receipt.value.shaderGeneration = frame->boundTerrainIdentity.shaderGeneration;
    receipt.value.programGeneration = frame->boundTerrainIdentity.programGeneration;
    receipt.value.identity = frame->boundTerrainIdentity.frame;
    receipt.value.sectionBase = frame->boundTerrainIdentity.sectionBase;
    receipt.value.sectionCount = frame->boundTerrainIdentity.sectionCount;
    receipt.value.primitiveBase = frame->boundTerrainIdentity.primitiveBase;
    receipt.value.primitiveCount = frame->boundTerrainIdentity.primitiveCount;
    receipt.value.contentState = MCDR_OIT_EXECUTION_CONTENT_UNSUPPORTED;
    receipt.value.producerKind = MCDR_OIT_SOURCE_TERRAIN;
    frame->terrainDrawReceipts.push_back(std::move(receipt));
    if (frame->executionStream) ++frame->executionStream->nativeDrawCount;
    frame->pendingTerrainPacket = -1;
    return success;
}
int captureExecutionTextureV1(void* opaqueContext, void* opaqueFrame, void* opaqueView, uint64_t totalBudgetBytes,
    mcdr_oit_execution_texture_v1* output) {
    if (output) *output = {};
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (context->recordingFailed.load(std::memory_order_acquire)) return recordingFailed;
    auto frame = findFrame(opaqueContext, opaqueFrame);
    if (!frame) return stale;
    const auto failCapture = [&](int status) {
        cancelExecutionTextureCaptures(*frame);
        return status;
    };
    if (!output || !opaqueView || !totalBudgetBytes || frame->phase != Phase::Recording) return failCapture(invalid);
    if (context->inRenderPass || frame->bound || frame->terrainStageDraw || frame->pendingTerrainPacket >= 0)
        return failCapture(busy);
    if (frame->executionStreamV2 && (frame->executionStreamV2->state != MCDR_OIT_EXECUTION_V2_STATE_RECORDING ||
        frame->executionStreamV2->normalizationPayload || frame->executionStreamV2->legacyNormalizationPayload)) return failCapture(stale);
    auto* view = static_cast<Dx12Object*>(opaqueView);
    if (view->kind != Dx12Object::Kind::TextureView || !view->sourceTexture || !view->hasSrvDesc ||
        !view->cpuHandle.ptr || view->closeRequested.load(std::memory_order_acquire)) return failCapture(unsupported);
    auto* texture = view->sourceTexture;
    if (texture->kind != Dx12Object::Kind::Texture || !texture->resource ||
        texture->closeRequested.load(std::memory_order_acquire)) return failCapture(unsupported);
    const auto description = texture->resource->GetDesc();
    const auto& srv = view->srvDesc;
    const auto expectedSrvFormat = texture->dxgiFormat == DXGI_FORMAT_D32_FLOAT ? DXGI_FORMAT_R32_FLOAT : texture->dxgiFormat;
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.MipLevels != 1 ||
        description.DepthOrArraySize != 1 || description.Width != frame->input.width || description.Height != frame->input.height ||
        description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0 ||
        srv.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D || srv.Format != expectedSrvFormat ||
        srv.Texture2D.MostDetailedMip != 0 || (srv.Texture2D.MipLevels != 1 && srv.Texture2D.MipLevels != UINT32_MAX) ||
        srv.Texture2D.PlaneSlice != 0 || srv.Texture2D.ResourceMinLODClamp != 0 ||
        srv.Shader4ComponentMapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING ||
        (texture->dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM && texture->dxgiFormat != DXGI_FORMAT_D32_FLOAT &&
            texture->dxgiFormat != DXGI_FORMAT_R16G16B16A16_FLOAT && texture->dxgiFormat != DXGI_FORMAT_R32G32B32A32_FLOAT))
        return failCapture(unsupported);
    ComPtr<ID3D12Device> resourceDevice;
    if (FAILED(texture->resource->GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
        resourceDevice.Get() != deviceContext().device.Get()) return failCapture(unsupported);
    for (auto* existing = frame->executionTextures.get(); existing; existing = existing->previous.get()) {
        if (existing->sourceView != view || existing->value.texture != texture ||
            existing->sourceResource.Get() != texture->resource.Get() || existing->format != texture->dxgiFormat ||
            !sameExecutionTextureView(existing->description, srv)) continue;
        const auto resolution = resolveSampledTextureContent(opaqueContext, existing->content.lease);
        if (const auto status = sampledContentResult(resolution.status); status != success) return failCapture(status);
        if (resolution.view == view) {
            *output = existing->value;
            return success;
        }
    }
    const auto budgetLimit = std::min(totalBudgetBytes, frame->input.totalBudgetBytes);
    try {
        auto budget = mcdr::oit_budget::reserve(sizeof(ExecutionTextureCapture) + 256, budgetLimit);
        if (!budget) return failCapture(overBudget);
        auto captured = std::make_unique<ExecutionTextureCapture>();
        captured->budget = std::move(budget);
        captured->sourceView = view;
        captured->sourceResource = texture->resource;
        captured->format = texture->dxgiFormat;
        captured->description = srv;
        captured->content = captureSampledTextureContent(opaqueContext, opaqueView, budgetLimit);
        if (const auto status = sampledContentResult(captured->content.status); status != success) return failCapture(status);
        if (!captured->content.lease || !captured->content.ownerIdentity || !captured->content.ownerGeneration ||
            !captured->content.contentVersion || captured->content.firstMip || captured->content.firstLayer ||
            captured->content.mipCount != 1 || captured->content.layerCount != 1) return failCapture(recordingFailed);
        auto& value = captured->value;
        value.schemaVersion = 1;
        value.structSize = sizeof(value);
        value.captureId = nextToken.fetch_add(1);
        if (!value.captureId) return failCapture(recordingFailed);
        value.texture = texture;
        value.ownerIdentity = reinterpret_cast<uint64_t>(captured->content.ownerIdentity.get());
        value.ownerGeneration = captured->content.ownerGeneration;
        value.contentVersion = captured->content.contentVersion;
        value.firstSubresource = 0;
        value.subresourceCount = 1;
        value.resourceVersion = frame->input.identity.resourceVersion;
        captured->previous = std::move(frame->executionTextures);
        frame->executionTextures = std::move(captured);
        *output = frame->executionTextures->value;
        return success;
    } catch (const std::bad_alloc&) {
        return failCapture(context->recordingFailed.load(std::memory_order_acquire) ? recordingFailed : overBudget);
    }
}
int beginExecutionStreamV2(void* opaqueContext, void* opaqueFrame, const mcdr_oit_execution_begin_v2& input,
    void** output) {
    if (output) *output = nullptr;
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findFrame(opaqueContext, opaqueFrame);
    if (!frame || !output || frame->phase != Phase::Recording || frame->executionStreamV2 ||
        input.schemaVersion != 2 || input.structSize != sizeof(input) ||
        !sameIdentity(input.identity, frame->input.identity) || input.frameSerial != frame->serial ||
        input.historyGeneration != frame->input.historyGeneration || input.programGeneration != frame->input.programGeneration ||
        !input.maxEvents || !input.maxScopes || !input.maxPasses || !input.maxResources || !input.maxReceipts ||
        input.flags || input.reserved0 || input.reserved1) return invalid;
    if (frame->executionOperationReceipts.size() > input.maxEvents) return overBudget;
    constexpr uint64_t executionIndexEntryBudget = 96;
    const uint64_t bytes = sizeof(ExecutionStreamV2) + 256 +
        static_cast<uint64_t>(input.maxEvents) * (sizeof(mcdr_oit_execution_event_v2) +
            sizeof(ExecutionOperationEvent) + sizeof(ExecutionCommand)) +
        static_cast<uint64_t>(input.maxScopes) * sizeof(ExecutionScopeV2) +
        static_cast<uint64_t>(input.maxPasses) * sizeof(ExecutionPassV2) +
        static_cast<uint64_t>(input.maxScopes) * executionIndexEntryBudget +
        static_cast<uint64_t>(input.maxPasses) * executionIndexEntryBudget +
        static_cast<uint64_t>(input.maxResources) * (sizeof(mcdr_oit_execution_resource_v2) + sizeof(std::shared_ptr<void>) + 256) +
        static_cast<uint64_t>(input.maxReceipts) * (sizeof(uint64_t) + sizeof(ExecutionCommand)) +
        2 * (sizeof(std::shared_ptr<void>) + 256);
    auto budget = mcdr::oit_budget::reserve(bytes, frame->input.totalBudgetBytes);
    if (!budget) return overBudget;
    auto stream = std::make_shared<ExecutionStreamV2>();
    stream->input = input;
    stream->recordingIdentity = frame->recordingIdentity;
    stream->budget = std::move(budget);
    stream->events.reserve(input.maxEvents);
    stream->scopes.reserve(input.maxScopes);
    stream->passes.reserve(input.maxPasses);
    stream->scopeIndex.reserve(input.maxScopes);
    stream->passIndex.reserve(input.maxPasses);
    stream->resources.reserve(input.maxResources);
    stream->leases.reserve(static_cast<size_t>(input.maxResources) + 2);
    stream->receipts.reserve(input.maxReceipts);
    stream->operations.reserve(input.maxEvents);
    stream->token = nextToken.fetch_add(1);
    if (!stream->token) return recordingFailed;
    *output = reinterpret_cast<void*>(stream->token);
    frame->executionStreamV2 = std::move(stream);
    return success;
}
namespace {
int appendExecutionStreamEvent(void* opaqueContext, void* opaqueStream, const mcdr_oit_execution_event_v2& input,
    ExecutionOperationPacket* operationPacket, uint64_t* output) {
    if (output) *output = 0;
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    if (static_cast<CommandContext*>(opaqueContext)->recordingFailed.load(std::memory_order_acquire)) return recordingFailed;
    auto frame = findExecutionFrameV2(opaqueContext, opaqueStream);
    if (!frame || !output) return stale;
    auto& stream = *frame->executionStreamV2;
    const auto& key = input.key;
    const bool normalization = input.kind == MCDR_OIT_EXECUTION_V2_NORMALIZATION;
    if (stream.state != MCDR_OIT_EXECUTION_V2_STATE_RECORDING || stream.normalizationPayload ||
        stream.legacyNormalizationPayload || frame->phase != Phase::Recording ||
        input.schemaVersion != 2 || input.structSize != sizeof(input) || input.sequence != stream.events.size() + 1 ||
        input.kind < MCDR_OIT_EXECUTION_V2_DRAW || input.kind > MCDR_OIT_EXECUTION_V2_UNCLASSIFIED_DRAW ||
        key.reserved || key.stage > MCDR_OIT_EXECUTION_STAGE_TERMINAL ||
        input.programGeneration != stream.input.programGeneration ||
        (!normalization && (input.payloadKind || input.payloadBytes || input.payload)) ||
        input.reserved[0] || input.reserved[1] ||
        (input.resourceCount && !input.resources) || (input.receiptCount && !input.receiptIds) ||
        (key.sourceDrawId && key.sourceDrawId == key.parentSourceDrawId)) return invalid;
    if (stream.events.size() == stream.input.maxEvents ||
        input.resourceCount > stream.input.maxResources - stream.resources.size() ||
        input.receiptCount > stream.input.maxReceipts - stream.receipts.size() ||
        (operationPacket && stream.operations.size() == stream.input.maxEvents)) return overBudget;
    const bool frameOwned = key.producerKind == MCDR_OIT_EXECUTION_V2_FRAME;
    if (frameOwned ? key.producerId != 0 || key.producerScopeSerial != 0 :
        key.producerKind > MCDR_OIT_SOURCE_CUSTOM_TRANSLUCENT || !key.producerId || !key.producerScopeSerial) return invalid;
    auto* scope = frameOwned ? nullptr : findScopeV2(stream, key.producerId, key.producerScopeSerial);
    auto* pass = key.sourcePassId ? findPassV2(stream, key.sourcePassId) : nullptr;
    const bool scopeBegin = input.kind == MCDR_OIT_EXECUTION_V2_SCOPE_BEGIN;
    const bool scopeEnd = input.kind == MCDR_OIT_EXECUTION_V2_SCOPE_END;
    const bool emptyScope = input.kind == MCDR_OIT_EXECUTION_V2_EMPTY_SCOPE;
    const bool passBegin = input.kind == MCDR_OIT_EXECUTION_V2_PASS_BEGIN;
    const bool passEnd = input.kind == MCDR_OIT_EXECUTION_V2_PASS_END;
    const bool control = scopeBegin || scopeEnd || emptyScope || passBegin || passEnd || normalization;
    if (normalization && (!frameOwned || key.stage != MCDR_OIT_EXECUTION_STAGE_TERMINAL ||
        key.sourcePassId || key.sourceDrawId || key.parentSourceDrawId || key.commandOrdinal || input.receiptCount ||
        input.drawGroupId || input.pipelineIdentity || input.shaderGeneration || input.flags)) return invalid;
    if (normalization && (static_cast<CommandContext*>(opaqueContext)->inRenderPass || frame->bound ||
        frame->terrainStageDraw || frame->pendingTerrainPacket >= 0 ||
        std::any_of(stream.scopes.begin(), stream.scopes.end(), [](const auto& existing) { return existing.open; }) ||
        std::any_of(stream.passes.begin(), stream.passes.end(), [](const auto& existing) { return existing.open; }))) return busy;
    if (input.flags && !((scopeEnd && input.flags == MCDR_OIT_EXECUTION_V2_SCOPE_EMPTY) ||
        ((scopeEnd || passEnd) && input.flags == MCDR_OIT_EXECUTION_V2_BOUNDARY_FAILED))) return invalid;
    if ((scopeBegin || scopeEnd || emptyScope) && key.sourcePassId) return invalid;
    if (scopeBegin || emptyScope) {
        if (frameOwned || scope || key.sourcePassId || key.stage == 0 ||
            stream.scopes.size() == stream.input.maxScopes) return invalid;
        for (const auto& existing : stream.scopes)
            if (existing.value.producerScopeSerial == key.producerScopeSerial ||
                (existing.value.producerId == key.producerId && existing.value.producerKind != key.producerKind)) return stale;
    } else if (!frameOwned) {
        // CloudRenderer initializes its private depth during the depth-bounds producer scope.
        const bool cloudDepthCopy = operationPacket && input.kind == MCDR_OIT_EXECUTION_V2_CLOUD_DEPTH &&
            key.producerKind == MCDR_OIT_SOURCE_CLOUD && key.stage == MCDR_OIT_EXECUTION_STAGE_CLOUD_DEPTH &&
            scope && scope->value.stage == MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS;
        if (!scope || !scope->open || scope->value.producerKind != key.producerKind ||
            (scope->value.stage != key.stage && !cloudDepthCopy)) return stale;
    }
    if (scopeEnd && (frameOwned || ((input.flags & MCDR_OIT_EXECUTION_V2_SCOPE_EMPTY) &&
        (scope->drawCount || scope->value.receiptCount)))) return invalid;
    if (passBegin) {
        if (!frameOwned || !key.sourcePassId || pass || stream.passes.size() == stream.input.maxPasses) return invalid;
        if (std::any_of(stream.passes.begin(), stream.passes.end(), [](const auto& existing) { return existing.open; }))
            return stale;
    } else if (passEnd) {
        if (!frameOwned || !pass || !pass->open) return stale;
    } else if (!scopeBegin && !scopeEnd && !emptyScope && !normalization && (!pass || !pass->open)) return stale;
    if (control && !normalization && (key.sourceDrawId || key.parentSourceDrawId || key.commandOrdinal || input.receiptCount ||
        input.resourceCount || input.drawGroupId || input.pipelineIdentity || input.shaderGeneration)) return invalid;
    if (!control && !key.sourceDrawId) return invalid;
    const bool terrain = input.kind == MCDR_OIT_EXECUTION_V2_DRAW && !frameOwned &&
        key.producerKind == MCDR_OIT_SOURCE_TERRAIN && !operationPacket;
    uint64_t lastReceiptSequence = stream.lastReceiptSequence;
    uint32_t terrainGaps = 0;
    if (terrain) {
        if (!input.receiptCount || !input.pipelineIdentity ||
            key.stage < MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS || key.stage > MCDR_OIT_EXECUTION_STAGE_ACCUMULATE)
            return invalid;
        for (uint32_t index = 0; index < input.receiptCount; ++index) {
            const auto receiptId = input.receiptIds[index];
            if (!receiptId) return stale;
            const auto found = std::lower_bound(frame->terrainStageReceipts.begin(), frame->terrainStageReceipts.end(),
                receiptId, [](const auto& receipt, uint64_t id) { return receipt.receiptId < id; });
            if (found == frame->terrainStageReceipts.end() || found->receiptId != receiptId) return stale;
            const auto packetIndex = static_cast<size_t>(found - frame->terrainStageReceipts.begin());
            if (packetIndex >= frame->terrainStagePackets.size()) return stale;
            const auto& packet = frame->terrainStagePackets[packetIndex];
            if (found->frameSerial != stream.input.frameSerial || !sameIdentity(found->identity, stream.input.identity) ||
                found->producerKind != key.producerKind || found->producerId != key.producerId ||
                found->producerScopeSerial != key.producerScopeSerial || found->sourcePassId != key.sourcePassId ||
                found->sourceDrawId != key.sourceDrawId || found->parentSourceDrawId != key.parentSourceDrawId ||
                found->stage != key.stage || found->pipelineIdentity != input.pipelineIdentity ||
                packet.programGeneration != input.programGeneration || packet.shaderGeneration != input.shaderGeneration ||
                packet.resourceVersion != stream.input.identity.resourceVersion || found->nativeSequence <= lastReceiptSequence ||
                static_cast<uint64_t>(packet.executionV2Ordinal) != static_cast<uint64_t>(key.commandOrdinal) + index)
                return stale;
            lastReceiptSequence = found->nativeSequence;
            if (found->contentState != MCDR_OIT_EXECUTION_CONTENT_ORIGINAL)
                terrainGaps |= MCDR_OIT_EXECUTION_V2_GAP_CONTENT;
            if (!packet.validated || !packet.batchLease)
                terrainGaps |= MCDR_OIT_EXECUTION_V2_GAP_READ_SET;
        }
    } else if (input.receiptCount) return invalid;
    if (normalization) {
        if (const auto status = validateNormalizationPayload(input, *frame); status != success) return status;
    }
    for (uint32_t index = 0; index < input.resourceCount; ++index)
        if (!validExecutionResourceV2(input.resources[index], stream.input.identity.resourceVersion)) return invalid;
    const auto leaseStart = stream.leases.size();
    if (normalization) {
        if (const auto status = retainNormalizationPayload(input, *frame); status != success) {
            stream.leases.resize(leaseStart);
            stream.normalizationInputs = {};
            return status;
        }
    }
    try {
        for (uint32_t index = 0; index < input.resourceCount; ++index) {
            auto lease = retainObjectLease(input.resources[index].object);
            if (!lease) {
                stream.leases.resize(leaseStart);
                if (normalization) stream.normalizationInputs = {};
                return invalid;
            }
            stream.leases.push_back(std::move(lease));
        }
    } catch (...) {
        stream.leases.resize(leaseStart);
        if (normalization) stream.normalizationInputs = {};
        return overBudget;
    }
    const auto resourceStart = stream.resources.size();
    const auto receiptStart = stream.receipts.size();
    if (input.resourceCount)
        stream.resources.insert(stream.resources.end(), input.resources, input.resources + input.resourceCount);
    if (input.receiptCount)
        stream.receipts.insert(stream.receipts.end(), input.receiptIds, input.receiptIds + input.receiptCount);
    auto stored = input;
    stored.resources = input.resourceCount ? stream.resources.data() + resourceStart : nullptr;
    stored.receiptIds = input.receiptCount ? stream.receipts.data() + receiptStart : nullptr;
    if (normalization) {
        if (input.payloadKind == MCDR_OIT_EXECUTION_V2_PAYLOAD_NORMALIZATION_V1) {
            stream.legacyNormalizationPayload = *static_cast<const mcdr_oit_execution_normalization_v1*>(input.payload);
            stored.payload = &*stream.legacyNormalizationPayload;
        } else {
            stream.normalizationPayload = *static_cast<const mcdr_oit_execution_normalization_v2*>(input.payload);
            stored.payload = &*stream.normalizationPayload;
        }
    }
    if (scopeBegin || emptyScope) {
        ExecutionScopeV2 created;
        created.value.schemaVersion = 2;
        created.value.structSize = sizeof(created.value);
        created.value.producerKind = key.producerKind;
        created.value.stage = key.stage;
        created.value.producerId = key.producerId;
        created.value.producerScopeSerial = key.producerScopeSerial;
        created.value.flags = emptyScope ? MCDR_OIT_EXECUTION_V2_SCOPE_EMPTY : 0;
        created.open = !emptyScope;
        stream.scopes.push_back(created);
        stream.scopeIndex.emplace(ExecutionScopeIdentity{key.producerId, key.producerScopeSerial},
            static_cast<uint32_t>(stream.scopes.size() - 1));
        scope = &stream.scopes.back();
    }
    if (passBegin) {
        ExecutionPassV2 created;
        created.value.schemaVersion = 2;
        created.value.structSize = sizeof(created.value);
        created.value.sourcePassId = key.sourcePassId;
        created.open = true;
        stream.passes.push_back(created);
        stream.passIndex.emplace(key.sourcePassId, static_cast<uint32_t>(stream.passes.size() - 1));
        pass = &stream.passes.back();
    }
    if (scope) {
        ++scope->value.eventCount;
        scope->value.receiptCount += input.receiptCount;
        if (!control) ++scope->drawCount;
        if (scopeEnd) {
            scope->open = false;
            scope->value.flags = input.flags;
        }
    }
    if (pass) {
        ++pass->value.eventCount;
        if (passEnd) {
            pass->open = false;
            pass->value.completion = input.flags == MCDR_OIT_EXECUTION_V2_BOUNDARY_FAILED ?
                MCDR_OIT_EXECUTION_V2_PASS_FAILED : MCDR_OIT_EXECUTION_V2_PASS_RETURNED;
        }
    }
    if (terrain) {
        stream.lastReceiptSequence = lastReceiptSequence;
        stream.missingMask |= terrainGaps;
    }
    if (!control && !terrain && !operationPacket)
        stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_ADAPTER | MCDR_OIT_EXECUTION_V2_GAP_CONTENT;
    if (input.flags == MCDR_OIT_EXECUTION_V2_BOUNDARY_FAILED) stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_FAILED;
    if (!stream.firstIssueSequence && (!control || input.flags == MCDR_OIT_EXECUTION_V2_BOUNDARY_FAILED))
        stream.firstIssueSequence = input.sequence;
    stream.events.push_back(stored);
    if (operationPacket) {
        stream.operations.push_back({operationPacket->receipt, operationPacket->packet,
            operationPacket->adapter, input.sequence});
        operationPacket->executionV2EventId = input.sequence;
    }
    *output = input.sequence;
    return success;
}
bool sameExecutionKey(const mcdr_oit_execution_key_v2& first, const mcdr_oit_execution_key_v2& second) {
    return first.producerId == second.producerId && first.producerScopeSerial == second.producerScopeSerial &&
        first.sourcePassId == second.sourcePassId && first.sourceDrawId == second.sourceDrawId &&
        first.parentSourceDrawId == second.parentSourceDrawId && first.producerKind == second.producerKind &&
        first.stage == second.stage && first.commandOrdinal == second.commandOrdinal && first.reserved == second.reserved;
}
bool sameExecutionOperationReceipt(const mcdr_oit_execution_operation_receipt_v1& first,
    const mcdr_oit_execution_operation_receipt_v1& second) {
    return first.schemaVersion == second.schemaVersion && first.structSize == second.structSize &&
        first.kind == second.kind && first.commandFamily == second.commandFamily &&
        first.receiptId == second.receiptId && first.nativeSequence == second.nativeSequence &&
        first.frameSerial == second.frameSerial && sameIdentity(first.identity, second.identity) &&
        sameExecutionKey(first.key, second.key) && first.pipelineIdentity == second.pipelineIdentity &&
        first.shaderGeneration == second.shaderGeneration && first.programGeneration == second.programGeneration &&
        first.resourceVersion == second.resourceVersion && first.packetId == second.packetId &&
        first.readSetId == second.readSetId && first.recordState == second.recordState &&
        first.contentState == second.contentState && first.adapterState == second.adapterState &&
        first.reserved == second.reserved;
}
}
int appendExecutionEventV2(void* opaqueContext, void* opaqueStream, const mcdr_oit_execution_event_v2& input,
    uint64_t* output) {
    return appendExecutionStreamEvent(opaqueContext, opaqueStream, input, nullptr, output);
}
int appendExecutionOperationEventV1(void* opaqueContext, void* opaqueStream,
    const mcdr_oit_execution_event_v2& event, const mcdr_oit_execution_operation_receipt_v1& receipt,
    uint64_t* output) {
    if (output) *output = 0;
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findExecutionFrameV2(opaqueContext, opaqueStream);
    if (!frame || !output) return stale;
    if (receipt.schemaVersion != 1 || receipt.structSize != sizeof(receipt) || receipt.reserved ||
        !receipt.receiptId || !receipt.nativeSequence || event.receiptCount || event.receiptIds ||
        event.kind != receipt.kind || !sameExecutionKey(event.key, receipt.key) ||
        event.pipelineIdentity != receipt.pipelineIdentity || event.shaderGeneration != receipt.shaderGeneration ||
        event.programGeneration != receipt.programGeneration) return invalid;
    const auto found = frame->executionOperationReceipts.find(receipt.receiptId);
    if (found == frame->executionOperationReceipts.end()) return stale;
    const auto& entry = found->second;
    auto* packet = entry.packet;
    if (!entry.operation || entry.operation->state != ExecutionOperationState::Finished || !packet ||
        !packet->confirmed || !packet->inputPassCompleted || !packet->packet || packet->executionV2EventId ||
        !sameExecutionOperationReceipt(receipt, packet->receipt) || receipt.frameSerial != frame->serial ||
        !sameIdentity(receipt.identity, frame->input.identity) ||
        receipt.programGeneration != frame->input.programGeneration ||
        receipt.resourceVersion != frame->input.identity.resourceVersion) return stale;
    return appendExecutionStreamEvent(opaqueContext, opaqueStream, event, packet, output);
}
int queryExecutionStreamV2(void* opaqueContext, void* opaqueStream, mcdr_oit_execution_info_v2* output) {
    if (output) *output = {};
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findExecutionFrameV2(opaqueContext, opaqueStream);
    if (!frame || !output) return stale;
    const auto& stream = *frame->executionStreamV2;
    output->schemaVersion = 2;
    output->structSize = sizeof(*output);
    output->state = stream.state;
    output->missingMask = stream.missingMask;
    output->identity = stream.input.identity;
    output->streamId = stream.token;
    output->frameSerial = stream.input.frameSerial;
    output->programGeneration = stream.input.programGeneration;
    output->eventCount = static_cast<uint32_t>(stream.events.size());
    output->scopeCount = static_cast<uint32_t>(stream.scopes.size());
    output->passCount = static_cast<uint32_t>(stream.passes.size());
    output->resourceCount = static_cast<uint32_t>(stream.resources.size());
    output->receiptCount = static_cast<uint32_t>(frame->terrainStageReceipts.size());
    output->consumedReceiptCount = static_cast<uint32_t>(stream.receipts.size());
    output->openScopeCount = static_cast<uint32_t>(std::count_if(stream.scopes.begin(), stream.scopes.end(),
        [](const auto& scope) { return scope.open; }));
    output->openPassCount = static_cast<uint32_t>(std::count_if(stream.passes.begin(), stream.passes.end(),
        [](const auto& pass) { return pass.open; }));
    output->firstIssueSequence = stream.firstIssueSequence;
    return success;
}
int queryExecutionTerrainReceiptV2(void* opaqueContext, void* opaqueStream, uint64_t receiptId,
    mcdr_oit_execution_terrain_receipt_v2* output) {
    if (output) *output = {};
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findExecutionFrameV2(opaqueContext, opaqueStream);
    if (!frame || !receiptId || !output) return stale;
    const auto found = std::lower_bound(frame->terrainStageReceipts.begin(), frame->terrainStageReceipts.end(), receiptId,
        [](const auto& receipt, uint64_t id) { return receipt.receiptId < id; });
    if (found == frame->terrainStageReceipts.end() || found->receiptId != receiptId) return stale;
    const auto packetIndex = static_cast<size_t>(found - frame->terrainStageReceipts.begin());
    if (packetIndex >= frame->terrainStagePackets.size()) return stale;
    const auto& packet = frame->terrainStagePackets[packetIndex];
    output->schemaVersion = 2;
    output->structSize = sizeof(*output);
    output->receipt = *found;
    output->shaderGeneration = packet.shaderGeneration;
    output->programGeneration = packet.programGeneration;
    output->resourceVersion = packet.resourceVersion;
    output->readSetRegistered = packet.validated && packet.batchLease ? 1 : 0;
    return success;
}
namespace {
int sealExecutionCommands(Frame& frame, ExecutionStreamV2& stream, uint64_t& missingReceipt) {
    try {
        std::vector<ExecutionCommand> commands;
        commands.reserve(stream.receipts.size() + stream.operations.size());
        for (const auto& event : stream.events) {
            for (uint32_t index = 0; index < event.receiptCount; ++index) {
                const auto receiptId = event.receiptIds[index];
                const auto found = std::lower_bound(frame.terrainStageReceipts.begin(), frame.terrainStageReceipts.end(),
                    receiptId, [](const auto& receipt, uint64_t id) { return receipt.receiptId < id; });
                if (found == frame.terrainStageReceipts.end() || found->receiptId != receiptId) return stale;
                commands.push_back({found->nativeSequence, receiptId, event.sequence, ExecutionCommandSource::Terrain,
                    static_cast<uint32_t>(found - frame.terrainStageReceipts.begin())});
            }
        }
        for (uint32_t index = 0; index < stream.operations.size(); ++index) {
            const auto& appended = stream.operations[index];
            const auto found = frame.executionOperationReceipts.find(appended.receipt.receiptId);
            if (found == frame.executionOperationReceipts.end()) return stale;
            const auto& entry = found->second;
            if (!entry.operation || entry.operation->state != ExecutionOperationState::Finished || !entry.packet ||
                !entry.packet->confirmed || !entry.packet->inputPassCompleted ||
                entry.packet->executionV2EventId != appended.eventId ||
                entry.packet->packet != appended.packet ||
                !sameExecutionOperationReceipt(entry.packet->receipt, appended.receipt)) return stale;
            commands.push_back({appended.receipt.nativeSequence, appended.receipt.receiptId, appended.eventId,
                ExecutionCommandSource::Operation, index});
        }
        uint64_t firstMissingSequence = UINT64_MAX;
        for (const auto& [receiptId, entry] : frame.executionOperationReceipts) {
            if (!entry.operation || !entry.packet) return stale;
            if (entry.operation->state != ExecutionOperationState::Finished) continue;
            if (!entry.packet->confirmed || !entry.packet->inputPassCompleted) return stale;
            if (!entry.packet->executionV2EventId) {
                stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_RECEIPT;
                if (entry.packet->receipt.nativeSequence < firstMissingSequence) {
                    firstMissingSequence = entry.packet->receipt.nativeSequence;
                    missingReceipt = receiptId;
                }
            }
        }
        std::sort(commands.begin(), commands.end(), [](const auto& first, const auto& second) {
            return first.nativeSequence < second.nativeSequence;
        });
        for (size_t index = 1; index < commands.size(); ++index)
            if (commands[index - 1].nativeSequence == commands[index].nativeSequence) return stale;
        stream.commands = std::make_unique<const std::vector<ExecutionCommand>>(std::move(commands));
        return success;
    } catch (const std::bad_alloc&) {
        return overBudget;
    }
}

template<class MatchesBinding>
LaneGeometry matchLaneGeometry(const mcdr_oit_execution_event_v2& event, const LaneOperationTextures& textures,
    MatchesBinding&& matchesBinding) {
    using Binding = GraphicsCommandTextureBinding;
    constexpr auto mainDepth = MCDR_OIT_EXECUTION_V2_RESOURCE_MAIN_DEPTH;
    constexpr auto cloudDepth = MCDR_OIT_EXECUTION_V2_RESOURCE_CLOUD_DEPTH;
    constexpr auto bounds = MCDR_OIT_EXECUTION_V2_RESOURCE_BOUNDS;
    constexpr auto coefficients = MCDR_OIT_EXECUTION_V2_RESOURCE_COEFFICIENT;
    constexpr auto q = MCDR_OIT_EXECUTION_V2_RESOURCE_Q;
    constexpr auto culled = MCDR_OIT_EXECUTION_V2_RESOURCE_CULLED_BOUNDS;
    const auto stage = event.key.stage;
    if (stage < MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS || stage > MCDR_OIT_EXECUTION_STAGE_ACCUMULATE) return {};
    LaneGeometry geometry;
    uint32_t coefficientMask = 0;
    bool boundsRead = false;
    for (uint32_t index = 0; index < event.resourceCount; ++index) {
        const auto& resource = event.resources[index];
        if (resource.subresourceCount != 1 ||
            resource.contentRequirement != MCDR_OIT_EXECUTION_V2_CONTENT_OBJECT_ONLY) return {};
        const LaneTexture claimed{resource.object, resource.firstSubresource};
        if (resource.role >= mainDepth && resource.role <= cloudDepth &&
            resource.access == MCDR_OIT_EXECUTION_V2_ACCESS_READ && !geometry.depthRole &&
            textures[resource.role] == claimed && matchesBinding(claimed, Binding::DepthAttachment, 0))
            geometry.depthRole = resource.role;
        else if (resource.role == culled && stage != MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS &&
            resource.access == MCDR_OIT_EXECUTION_V2_ACCESS_READ && !boundsRead &&
            textures[culled] == claimed && matchesBinding(claimed, Binding::Sampled, 0))
            boundsRead = true;
        else if (resource.role == coefficients && stage != MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS &&
            !(coefficientMask & (1u << resource.roleIndex)) &&
            (stage == MCDR_OIT_EXECUTION_STAGE_TRANSMITTANCE
                ? resource.access == MCDR_OIT_EXECUTION_V2_ACCESS_READ_WRITE &&
                  matchesBinding(claimed, Binding::ColorTarget, resource.roleIndex)
                : resource.access == MCDR_OIT_EXECUTION_V2_ACCESS_READ &&
                  matchesBinding(claimed, Binding::Sampled, resource.roleIndex + 1)))
            coefficientMask |= 1u << resource.roleIndex;
        else if (((stage == MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS &&
                    (resource.role == bounds || resource.role == culled) && textures[resource.role] == claimed) ||
                  (stage == MCDR_OIT_EXECUTION_STAGE_ACCUMULATE && resource.role == q)) &&
            resource.access == MCDR_OIT_EXECUTION_V2_ACCESS_READ_WRITE && !geometry.outputRole &&
            matchesBinding(claimed, Binding::ColorTarget, 0))
            geometry.outputRole = resource.role;
        else return {};
    }
    if (!geometry.depthRole) return {};
    if (stage == MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS) return geometry.outputRole ? geometry : LaneGeometry{};
    if (!boundsRead || (coefficientMask != 1 && coefficientMask != 3 && coefficientMask != 15)) return {};
    geometry.coefficientCount = std::popcount(coefficientMask);
    if (stage == MCDR_OIT_EXECUTION_STAGE_TRANSMITTANCE) geometry.outputRole = coefficients;
    return geometry.outputRole ? geometry : LaneGeometry{};
}

int validateLaneOperationResources(Frame& frame, ExecutionStreamV2& stream, uint64_t& missingReceipt) {
    constexpr auto mainDepth = MCDR_OIT_EXECUTION_V2_RESOURCE_MAIN_DEPTH;
    constexpr auto terrainDepth = MCDR_OIT_EXECUTION_V2_RESOURCE_TERRAIN_DEPTH;
    constexpr auto cloudDepth = MCDR_OIT_EXECUTION_V2_RESOURCE_CLOUD_DEPTH;
    constexpr auto bounds = MCDR_OIT_EXECUTION_V2_RESOURCE_BOUNDS;
    constexpr auto culled = MCDR_OIT_EXECUTION_V2_RESOURCE_CULLED_BOUNDS;
    LaneOperationTextures textures{};
    if (frame.opaqueDepthSeed) textures[mainDepth].object = frame.opaqueDepthSeed->worldDepth;
    uint64_t boundsPass = 0, waterPass = 0;
    bool cullRecorded = false, cloudCopied = false;
    bool boundsCopyReady = false;
    for (const auto& command : *stream.commands) {
        if (command.source != ExecutionCommandSource::Operation) continue;
        auto& operation = stream.operations[command.packetIndex];
        if (operation.receipt.recordState == MCDR_OIT_OPERATION_EMPTY) continue;
        const auto& event = stream.events[command.eventId - 1];
        if (event.kind == MCDR_OIT_EXECUTION_V2_DRAW) continue;
        uint32_t sourceRole = 0, colorRole = 0, depthRole = 0;
        switch (event.kind) {
        case MCDR_OIT_EXECUTION_V2_COPY_DEPTH: sourceRole = mainDepth; depthRole = terrainDepth; break;
        case MCDR_OIT_EXECUTION_V2_CLOUD_DEPTH: sourceRole = mainDepth; depthRole = cloudDepth; break;
        case MCDR_OIT_EXECUTION_V2_COPY_BOUNDS: sourceRole = bounds; colorRole = culled; break;
        case MCDR_OIT_EXECUTION_V2_CULL_BOUNDS: sourceRole = bounds; colorRole = culled; depthRole = mainDepth; break;
        case MCDR_OIT_EXECUTION_V2_WATER_MASK: depthRole = terrainDepth; break;
        default:
            stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_ADAPTER;
            if (!missingReceipt) missingReceipt = command.receiptId;
            continue;
        }
        const auto inspection = inspectGraphicsCommandPacket(frame.context, operation.packet);
        if (inspection.status == GraphicsPacketInspectionStatus::StaleRecording) return stale;
        if (inspection.status == GraphicsPacketInspectionStatus::RecordingFailed) return recordingFailed;
        const auto roleBit = [](uint32_t role) { return role ? 1u << role : 0u; };
        const uint32_t required = roleBit(sourceRole) | roleBit(colorRole) | roleBit(depthRole);
        uint32_t found = 0;
        auto matchedTextures = textures;
        bool valid = inspection.status == GraphicsPacketInspectionStatus::Ready && inspection.confirmed;
        for (uint32_t index = 0; valid && index < event.resourceCount; ++index) {
            const auto& resource = event.resources[index];
            if (resource.role >= textures.size() || resource.subresourceCount != 1 ||
                resource.contentRequirement != MCDR_OIT_EXECUTION_V2_CONTENT_OBJECT_ONLY ||
                resource.ownerIdentity || resource.ownerGeneration || resource.contentVersion) {
                valid = false;
                break;
            }
            const uint32_t bit = roleBit(resource.role);
            if (!(required & bit) || (found & bit)) {
                valid = false;
                break;
            }
            const bool sampled = resource.role == sourceRole;
            const auto binding = sampled ? GraphicsCommandTextureBinding::Sampled :
                resource.role == depthRole ? GraphicsCommandTextureBinding::DepthTarget :
                GraphicsCommandTextureBinding::ColorTarget;
            const bool depthReadWrite = resource.role == depthRole &&
                (event.kind == MCDR_OIT_EXECUTION_V2_WATER_MASK || event.kind == MCDR_OIT_EXECUTION_V2_CULL_BOUNDS);
            const auto access = sampled ? MCDR_OIT_EXECUTION_V2_ACCESS_READ :
                depthReadWrite ? MCDR_OIT_EXECUTION_V2_ACCESS_READ_WRITE : MCDR_OIT_EXECUTION_V2_ACCESS_WRITE;
            auto& texture = matchedTextures[resource.role];
            const LaneTexture claimed{resource.object, resource.firstSubresource};
            const auto* object = static_cast<const Dx12Object*>(resource.object);
            const bool depthFormat = object->dxgiFormat == DXGI_FORMAT_D32_FLOAT;
            valid = resource.access == access &&
                depthFormat == (resource.role == mainDepth || resource.role == terrainDepth || resource.role == cloudDepth) &&
                (!texture.object || texture == claimed) &&
                matchesGraphicsCommandTexture(operation.packet, resource.object, resource.firstSubresource, binding);
            texture = claimed;
            found |= bit;
        }
        if (valid) valid = found == required &&
            (!sourceRole || !colorRole || matchedTextures[sourceRole] != matchedTextures[colorRole]) &&
            (!sourceRole || !depthRole || matchedTextures[sourceRole] != matchedTextures[depthRole]) &&
            (!colorRole || !depthRole || matchedTextures[colorRole] != matchedTextures[depthRole]);
        if (valid) {
            switch (event.kind) {
            case MCDR_OIT_EXECUTION_V2_COPY_DEPTH:
                valid = event.key.sourcePassId != waterPass && (!boundsPass || cullRecorded);
                if (valid) waterPass = event.key.sourcePassId;
                break;
            case MCDR_OIT_EXECUTION_V2_WATER_MASK:
                valid = waterPass != 0 && waterPass == event.key.sourcePassId;
                break;
            case MCDR_OIT_EXECUTION_V2_COPY_BOUNDS:
                valid = !boundsPass;
                if (valid) boundsPass = event.key.sourcePassId;
                break;
            case MCDR_OIT_EXECUTION_V2_CULL_BOUNDS:
                valid = !cullRecorded && boundsPass && boundsPass == event.key.sourcePassId;
                if (valid) cullRecorded = true;
                break;
            case MCDR_OIT_EXECUTION_V2_CLOUD_DEPTH:
                valid = cullRecorded && !cloudCopied;
                if (valid) cloudCopied = true;
                break;
            }
        }
        if (valid) {
            textures = matchedTextures;
            const bool depthCopy = event.kind == MCDR_OIT_EXECUTION_V2_COPY_DEPTH ||
                event.kind == MCDR_OIT_EXECUTION_V2_CLOUD_DEPTH;
            const bool boundsCopy = event.kind == MCDR_OIT_EXECUTION_V2_COPY_BOUNDS;
            const auto& provenance = inspection.oitLaneOperation;
            if (depthCopy || boundsCopy) {
                const uint32_t destination = depthCopy ? depthRole : colorRole;
                const auto& source = textures[sourceRole];
                const auto& target = textures[destination];
                const auto format = depthCopy ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
                if (provenance.kind == (depthCopy ? MCDR_OIT_LANE_DEPTH_COPY : MCDR_OIT_LANE_BOUNDS_COPY) &&
                    provenance.programGeneration == stream.input.programGeneration &&
                    coversOriginalLaneExtent(inspection, source, frame.input.width, frame.input.height) &&
                    coversOriginalLaneExtent(inspection, target, frame.input.width, frame.input.height) &&
                    static_cast<const Dx12Object*>(source.object)->dxgiFormat == format &&
                    static_cast<const Dx12Object*>(target.object)->dxgiFormat == format)
                    operation.laneCopy = {sourceRole, destination};
                if (boundsCopy) boundsCopyReady = operation.laneCopy.sourceRole != 0;
            } else if (event.kind == MCDR_OIT_EXECUTION_V2_CULL_BOUNDS && boundsCopyReady &&
                provenance.kind == MCDR_OIT_LANE_BOUNDS_CULL &&
                provenance.programGeneration == stream.input.programGeneration &&
                frame.programs->laneCullPipeline &&
                coversOriginalLaneExtent(inspection, textures[sourceRole], frame.input.width, frame.input.height) &&
                coversOriginalLaneExtent(inspection, textures[colorRole], frame.input.width, frame.input.height) &&
                coversOriginalLaneExtent(inspection, textures[depthRole], frame.input.width, frame.input.height) &&
                packetContentStatus(frame.context, inspection.laneProjection) == PacketContentStatus::Frozen) {
                operation.laneCullProjection = inspection.laneProjection;
            } else if (event.kind == MCDR_OIT_EXECUTION_V2_WATER_MASK &&
                inspection.opaqueSeedProgramGeneration == stream.input.programGeneration &&
                inspection.content == mcdr::graphics_recording::ContentState::Frozen) {
                operation.laneWaterMask = true;
            }
            if (operation.laneCopy.sourceRole || operation.laneCullProjection || operation.laneWaterMask) continue;
            stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_ADAPTER;
            if (event.kind == MCDR_OIT_EXECUTION_V2_WATER_MASK)
                stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_WATER;
            if (event.kind == MCDR_OIT_EXECUTION_V2_CLOUD_DEPTH)
                stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_CLOUD;
            continue;
        }
        stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_READ_SET;
        if (!missingReceipt) missingReceipt = command.receiptId;
    }
    stream.laneOperationTextures = textures;
    return success;
}

void validateLaneGeometryOperations(Frame& frame, ExecutionStreamV2& stream) {
    static thread_local std::unordered_set<uint64_t> reported;
    for (const auto& command : *stream.commands) {
        if (command.source != ExecutionCommandSource::Operation) continue;
        const auto& event = stream.events[command.eventId - 1];
        auto& operation = stream.operations[command.packetIndex];
        if (event.kind != MCDR_OIT_EXECUTION_V2_DRAW || operation.receipt.recordState == MCDR_OIT_OPERATION_EMPTY)
            continue;
        const auto inspection = inspectGraphicsCommandPacket(frame.context, operation.packet);
        if (inspection.opaqueSeedProgramGeneration != frame.input.programGeneration ||
            inspection.tileStage > 2 ||
            inspection.tileStage + MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS != event.key.stage ||
            inspection.content != mcdr::graphics_recording::ContentState::Frozen) {
            if (reported.insert(event.pipelineIdentity).second) {
                dbgLogDebug("OIT IA content gap: producer=%u stage=%u content=%u route=%u",
                    event.key.producerKind, event.key.stage,
                    static_cast<unsigned>(inspection.content), inspection.tileStage);
            }
            stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_CONTENT | MCDR_OIT_EXECUTION_V2_GAP_ADAPTER;
            continue;
        }
        operation.geometry = matchLaneGeometry(event, stream.laneOperationTextures,
            [&](const LaneTexture& texture, GraphicsCommandTextureBinding binding, uint32_t bindingIndex) {
                return matchesGraphicsCommandTexture(operation.packet, texture.object, texture.subresource,
                    binding, bindingIndex);
            });
        if (!operation.geometry.depthRole) {
            if (reported.insert(event.pipelineIdentity).second)
                dbgLogDebug("OIT IA bindings gap: producer=%u stage=%u rows=%u",
                    event.key.producerKind, event.key.stage, event.resourceCount);
            stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_READ_SET;
        }
    }
}

int prepareTerrainExecutionSelections(Frame& frame, ExecutionStreamV2& stream) {
    if (!stream.commands) {
        stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_SELECTION;
        return success;
    }
    try {
        std::vector<TerrainExecutionSelection> selections;
        selections.reserve(stream.commands->size());
        std::unordered_set<uint64_t> seenReceipts;
        std::vector<uint8_t> seenTerrainReceipts(frame.terrainStageReceipts.size(), 0);
        bool complete = true;
        uint64_t lastNativeSequence = 0;
        const auto issue = [&](uint32_t gap, uint64_t sequence) {
            stream.missingMask |= gap | MCDR_OIT_EXECUTION_V2_GAP_SELECTION;
            if (!stream.firstIssueSequence || sequence < stream.firstIssueSequence)
                stream.firstIssueSequence = sequence;
            complete = false;
        };
        static thread_local std::unordered_set<uint64_t> reported;
        for (const auto& command : *stream.commands) {
            if (command.source != ExecutionCommandSource::Terrain) continue;
            if (command.packetIndex >= frame.terrainStageReceipts.size()) {
                issue(MCDR_OIT_EXECUTION_V2_GAP_RECEIPT, command.nativeSequence);
                continue;
            }
            const auto& receipt = frame.terrainStageReceipts[command.packetIndex];
            if (receipt.receiptId != command.receiptId || receipt.nativeSequence != command.nativeSequence ||
                !command.eventId || command.eventId > stream.events.size()) {
                issue(MCDR_OIT_EXECUTION_V2_GAP_RECEIPT, command.nativeSequence);
                continue;
            }
            if (!seenReceipts.insert(receipt.receiptId).second || seenTerrainReceipts[command.packetIndex]) {
                issue(MCDR_OIT_EXECUTION_V2_GAP_RECEIPT, command.nativeSequence);
                continue;
            }
            seenTerrainReceipts[command.packetIndex] = 1;
            const auto& event = stream.events[command.eventId - 1];
            if (event.sequence != command.eventId || event.kind != MCDR_OIT_EXECUTION_V2_DRAW ||
                event.receiptCount == 0 || !event.receiptIds || event.key.producerKind != MCDR_OIT_SOURCE_TERRAIN ||
                event.key.stage != receipt.stage || event.key.producerId != receipt.producerId ||
                event.key.producerScopeSerial != receipt.producerScopeSerial ||
                event.key.sourcePassId != receipt.sourcePassId || event.key.sourceDrawId != receipt.sourceDrawId ||
                event.key.parentSourceDrawId != receipt.parentSourceDrawId ||
                event.pipelineIdentity != receipt.pipelineIdentity ||
                event.programGeneration != stream.input.programGeneration) {
                issue(MCDR_OIT_EXECUTION_V2_GAP_RECEIPT, receipt.nativeSequence);
                continue;
            }
            uint32_t receiptIndex = UINT32_MAX;
            for (uint32_t index = 0; index < event.receiptCount; ++index) {
                if (event.receiptIds[index] != receipt.receiptId) continue;
                if (receiptIndex != UINT32_MAX) {
                    receiptIndex = UINT32_MAX - 1;
                    break;
                }
                receiptIndex = index;
            }
            if (receiptIndex >= event.receiptCount) {
                issue(MCDR_OIT_EXECUTION_V2_GAP_RECEIPT, receipt.nativeSequence);
                continue;
            }
            if (command.nativeSequence <= lastNativeSequence) {
                issue(MCDR_OIT_EXECUTION_V2_GAP_RECEIPT, command.nativeSequence);
                continue;
            }
            lastNativeSequence = command.nativeSequence;
            if (command.packetIndex >= frame.terrainStagePackets.size()) {
                issue(MCDR_OIT_EXECUTION_V2_GAP_CONTENT, receipt.nativeSequence);
                continue;
            }
            const auto& packet = frame.terrainStagePackets[command.packetIndex];
            if (packet.executionV2Ordinal == UINT32_MAX ||
                packet.executionV2Ordinal != event.key.commandOrdinal + receiptIndex) {
                issue(MCDR_OIT_EXECUTION_V2_GAP_SELECTION, receipt.nativeSequence);
                continue;
            }
            if (receipt.contentState != MCDR_OIT_EXECUTION_CONTENT_ORIGINAL ||
                !packet.replayPacket || !packet.prepareReplay) {
                if (reported.insert(receipt.pipelineIdentity).second)
                    dbgLogDebug("OIT terrain content gap: stage=%u registered=%d", receipt.stage, packet.validated);
                issue(MCDR_OIT_EXECUTION_V2_GAP_CONTENT, receipt.nativeSequence);
            }
            if (!packet.validated || !packet.batchLease)
                issue(MCDR_OIT_EXECUTION_V2_GAP_READ_SET, receipt.nativeSequence);
            TerrainExecutionSelection selection;
            selection.nativeSequence = receipt.nativeSequence;
            selection.eventId = command.eventId;
            selection.receiptId = receipt.receiptId;
            selection.sourcePassId = receipt.sourcePassId;
            selection.sourceDrawId = receipt.sourceDrawId;
            selection.parentSourceDrawId = receipt.parentSourceDrawId;
            selection.stage = receipt.stage;
            selection.packetIndex = command.packetIndex;
            selection.geometry = matchLaneGeometry(event, stream.laneOperationTextures,
                [&](const LaneTexture& texture, GraphicsCommandTextureBinding binding, uint32_t bindingIndex) {
                    using Binding = GraphicsCommandTextureBinding;
                    if (binding == Binding::DepthAttachment) return texture == packet.depth;
                    if (binding == Binding::ColorTarget)
                        return bindingIndex < packet.colors.size() && texture == packet.colors[bindingIndex];
                    return bindingIndex < packet.laneReads.size() && texture == packet.laneReads[bindingIndex];
                });
            if (!selection.geometry.depthRole) {
                if (complete)
                    dbgLogDebug("OIT terrain binding gap: stage=%u rows=%u", receipt.stage, event.resourceCount);
                issue(MCDR_OIT_EXECUTION_V2_GAP_READ_SET, receipt.nativeSequence);
            }
            selections.push_back(std::move(selection));
        }
        for (const auto seen : seenTerrainReceipts)
            if (!seen) issue(MCDR_OIT_EXECUTION_V2_GAP_RECEIPT, frame.serial);
        if (complete)
            stream.terrainSelections = std::make_unique<const std::vector<TerrainExecutionSelection>>(std::move(selections));
        else
            stream.terrainSelections.reset();
        return success;
    } catch (const std::bad_alloc&) {
        stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_SELECTION;
        return overBudget;
    }
}

void validateLaneExecutionOrder(ExecutionStreamV2& stream) {
    if (!stream.normalizationPayload || !stream.terrainSelections) return;
    const auto& terminal = stream.events.back();
    if (terminal.kind != MCDR_OIT_EXECUTION_V2_NORMALIZATION) return;
    const auto coefficientCount = stream.normalizationPayload->targetCount;
    const auto matchesTerminal = [&](const mcdr_oit_execution_resource_v2& resource, uint32_t index) {
        const auto& expected = terminal.resources[index];
        return resource.object == expected.object && resource.firstSubresource == expected.firstSubresource;
    };
    constexpr auto main = MCDR_OIT_EXECUTION_V2_RESOURCE_MAIN_DEPTH;
    constexpr auto terrain = MCDR_OIT_EXECUTION_V2_RESOURCE_TERRAIN_DEPTH;
    constexpr auto cloud = MCDR_OIT_EXECUTION_V2_RESOURCE_CLOUD_DEPTH;
    constexpr auto culled = MCDR_OIT_EXECUTION_V2_RESOURCE_CULLED_BOUNDS;
    bool terrainReady = false, cloudReady = false, boundsCopied = false, boundsCulled = false;
    bool accumulateStarted = false;
    size_t terrainIndex = 0;
    for (const auto& command : *stream.commands) {
        const auto& event = stream.events[command.eventId - 1];
        LaneGeometry geometry;
        if (command.source == ExecutionCommandSource::Terrain) {
            geometry = (*stream.terrainSelections)[terrainIndex++].geometry;
        } else {
            const auto& operation = stream.operations[command.packetIndex];
            if (operation.receipt.recordState == MCDR_OIT_OPERATION_EMPTY) continue;
            geometry = operation.geometry;
            if (!geometry.depthRole) {
                terrainReady |= operation.laneCopy.destinationRole == terrain;
                cloudReady |= operation.laneCopy.destinationRole == cloud;
                boundsCopied |= operation.laneCopy.destinationRole == culled;
                boundsCulled |= bool(operation.laneCullProjection);
                continue;
            }
        }
        const auto stage = event.key.stage;
        bool valid = geometry.depthRole == main || (geometry.depthRole == terrain && terrainReady) ||
            (geometry.depthRole == cloud && cloudReady);
        if (stage == MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS)
            valid &= geometry.outputRole == culled ? boundsCulled : !boundsCopied;
        else
            valid &= boundsCulled && geometry.coefficientCount == coefficientCount &&
                (stage != MCDR_OIT_EXECUTION_STAGE_TRANSMITTANCE || !accumulateStarted);
        accumulateStarted |= stage == MCDR_OIT_EXECUTION_STAGE_ACCUMULATE;
        for (uint32_t index = 0; valid && index < event.resourceCount; ++index) {
            const auto& resource = event.resources[index];
            if (resource.role == MCDR_OIT_EXECUTION_V2_RESOURCE_Q)
                valid = matchesTerminal(resource, 2);
            if (resource.role == MCDR_OIT_EXECUTION_V2_RESOURCE_COEFFICIENT)
                valid = matchesTerminal(resource, 4 + resource.roleIndex);
        }
        if (!valid) {
            stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_READ_SET;
            if (!stream.firstIssueSequence || command.nativeSequence < stream.firstIssueSequence)
                stream.firstIssueSequence = command.nativeSequence;
        }
    }
}


}
int validateOpaqueDepthSeed(Frame& frame, ExecutionStreamV2& stream) {
    const auto& seed = frame.opaqueDepthSeed;
    stream.opaqueSeedIssueSequences.fill(0);
    stream.missingMask &= ~MCDR_OIT_EXECUTION_V2_GAP_OPAQUE_SEED;
    const auto issue = [&](uint32_t gap, size_t category, uint64_t sequence) {
        stream.missingMask |= gap;
        auto& first = stream.opaqueSeedIssueSequences[category];
        if (!first || sequence < first) first = sequence;
        if (!stream.firstIssueSequence || sequence < stream.firstIssueSequence) stream.firstIssueSequence = sequence;
    };
    const auto* context = static_cast<const CommandContext*>(frame.context);
    if (!seed || seed->schemaVersion != 1 || seed->context != frame.context ||
        seed->recordingIdentity != frame.recordingIdentity || seed->recordingIdentity != context->recordingIdentity ||
        seed->fence != context->fence || seed->submission != frame.serial || seed->submission != context->fenceValue + 1 ||
        seed->frame.width != frame.input.width || seed->frame.height != frame.input.height ||
        seed->frame.worldEpoch != frame.input.identity.worldEpoch || seed->frame.realFrameId != frame.input.identity.realFrameId ||
        seed->frame.configVersion != frame.input.identity.configVersion ||
        seed->frame.resourceVersion != frame.input.identity.resourceVersion ||
        seed->programGeneration != frame.input.programGeneration || seed->programGeneration != context->frameProgramGeneration ||
        !vanilla::matchesWorldDepthRegistrationV1(frame.context, seed->worldDepth) || !seed->depthOwner ||
        !seed->depthDescriptors || !seed->retainedDsv.ptr || seed->events.empty()) {
        issue(MCDR_OIT_EXECUTION_V2_GAP_OPAQUE_SEED, 0, 1);
        return success;
    }
    bool initialized = false;
    for (size_t index = 0; index < seed->events.size(); ++index) {
        const auto& event = seed->events[index];
        const auto sequence = static_cast<uint64_t>(index) + 1;
        if (event.sequence != sequence || event.depthTarget != seed->worldDepth || event.subresource ||
            event.subresourceCount != 1 || !event.sourceDsv.ptr || event.dsv.Format != DXGI_FORMAT_D32_FLOAT ||
            event.dsv.ViewDimension != D3D12_DSV_DIMENSION_TEXTURE2D || event.dsv.Texture2D.MipSlice ||
            event.dsv.Flags != D3D12_DSV_FLAG_NONE || event.programGeneration != seed->programGeneration) {
            issue(MCDR_OIT_EXECUTION_V2_GAP_OPAQUE_SEED, 0, sequence);
            continue;
        }
        if (event.kind == vanilla::WorldDepthSeedEventKind::Clear) {
            const auto& rect = event.clearRect;
            const bool validRect = !event.hasClearRect || (rect.left >= 0 && rect.top >= 0 &&
                rect.right > rect.left && rect.bottom > rect.top && uint64_t(rect.right) <= frame.input.width &&
                uint64_t(rect.bottom) <= frame.input.height);
            const bool fullClear = !event.hasClearRect || (rect.left == 0 && rect.top == 0 &&
                uint64_t(rect.right) == frame.input.width && uint64_t(rect.bottom) == frame.input.height);
            if (!std::isfinite(event.clearDepth) || event.clearDepth < 0.0 || event.clearDepth > 1.0 ||
                !validRect || (!initialized && (!fullClear || index != 0)))
                issue(MCDR_OIT_EXECUTION_V2_GAP_OPAQUE_SEED, 0, sequence);
            else initialized = true;
            continue;
        }
        if (event.kind != vanilla::WorldDepthSeedEventKind::Draw || !initialized || !event.packet || !event.pso ||
            !event.pipelineIdentity || !event.shaderGeneration || !event.commandOrdinal ||
            event.commandFamily < static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::Draw) ||
            event.commandFamily > static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::Mesh)) {
            issue(MCDR_OIT_EXECUTION_V2_GAP_OPAQUE_SEED, 0, sequence);
            continue;
        }
        const auto inspection = inspectGraphicsCommandPacket(frame.context, event.packet);
        if (inspection.status == GraphicsPacketInspectionStatus::StaleRecording) return stale;
        if (inspection.status == GraphicsPacketInspectionStatus::RecordingFailed) return recordingFailed;
        if (inspection.status != GraphicsPacketInspectionStatus::Ready || !inspection.confirmed ||
            !inspection.depthWrite || !inspection.depthTargetBound || inspection.depthObject != seed->worldDepth ||
            inspection.depthTarget.ptr != event.sourceDsv.ptr || inspection.commandFamily != event.commandFamily ||
            inspection.pipelineIdentity != event.pipelineIdentity || inspection.shaderGeneration != event.shaderGeneration ||
            !inspection.pso || inspection.pso.Get() != event.pso.Get()) {
            issue(MCDR_OIT_EXECUTION_V2_GAP_OPAQUE_SEED, 0, sequence);
            continue;
        }
        if (inspection.commandFamily == static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::Mesh)) {
            const auto& adapter = inspection.adapter;
            if (adapter.state != mcdr::graphics_recording::AdapterState::Ready ||
                adapter.registration != mcdr::graphics_recording::AdapterRegistration::Registered ||
                adapter.stage != terrainOpaqueSeedStage || adapter.programGeneration != seed->programGeneration ||
                !adapter.replayContent)
                issue(MCDR_OIT_EXECUTION_V2_GAP_ADAPTER, 2, sequence);
            continue;
        }
        if (inspection.content != mcdr::graphics_recording::ContentState::Frozen)
            issue(MCDR_OIT_EXECUTION_V2_GAP_CONTENT, 1, sequence);
        if (inspection.opaqueSeedProgramGeneration != seed->programGeneration)
            issue(MCDR_OIT_EXECUTION_V2_GAP_ADAPTER, 2, sequence);
    }
    if (!initialized) issue(MCDR_OIT_EXECUTION_V2_GAP_OPAQUE_SEED, 0, 1);
    return success;
}

int sealExecutionStreamV2(void* opaqueContext, void* opaqueStream, const mcdr_oit_execution_manifest_v2& input,
    void** output, mcdr_oit_execution_seal_result_v2* result) {
    if (output) *output = nullptr;
    if (result) *result = {};
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findExecutionFrameV2(opaqueContext, opaqueStream);
    if (!frame || !output || !result) return stale;
    auto& stream = *frame->executionStreamV2;
    if (frame->activeExecutionOperation) {
        const bool failed = cancelActiveExecutionOperations(*frame);
        stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_OPEN_SCOPE;
        return failed ? recordingFailed : busy;
    }
    constexpr uint32_t knownGaps = 0x1fffu;
    if (stream.state != MCDR_OIT_EXECUTION_V2_STATE_RECORDING || frame->phase != Phase::Recording || input.schemaVersion != 2 ||
        input.structSize != sizeof(input) || (input.flags & ~MCDR_OIT_EXECUTION_V2_MANIFEST_FAILED) ||
        (input.missingMask & ~knownGaps) || input.reserved0 || input.reserved1 || input.reserved2 ||
        (input.scopeCount && !input.scopes) || (input.passCount && !input.passes)) return invalid;
    const bool completeManifest = input.eventCount == stream.events.size() && input.scopeCount == stream.scopes.size() &&
        input.passCount == stream.passes.size() && input.resourceCount == stream.resources.size() &&
        input.receiptCount == stream.receipts.size() && input.lastSequence == stream.events.size();
    for (uint32_t index = 0; completeManifest && index < input.scopeCount; ++index) {
        const auto& supplied = input.scopes[index];
        const auto& expected = stream.scopes[index].value;
        if (supplied.schemaVersion != 2 || supplied.structSize != sizeof(supplied) || supplied.reserved ||
            supplied.producerKind != expected.producerKind || supplied.stage != expected.stage ||
            supplied.producerId != expected.producerId || supplied.producerScopeSerial != expected.producerScopeSerial ||
            supplied.sourcePassId != expected.sourcePassId || supplied.eventCount != expected.eventCount ||
            supplied.receiptCount != expected.receiptCount || supplied.flags != expected.flags) return invalid;
    }
    for (uint32_t index = 0; completeManifest && index < input.passCount; ++index) {
        const auto& supplied = input.passes[index];
        const auto& expected = stream.passes[index].value;
        if (supplied.schemaVersion != 2 || supplied.structSize != sizeof(supplied) || supplied.flags ||
            supplied.reserved0 || supplied.reserved1 || supplied.reserved2 || supplied.sourcePassId != expected.sourcePassId ||
            supplied.completion != expected.completion || supplied.eventCount != expected.eventCount) return invalid;
    }
    if (frame->terrainStageDraw || frame->bound || frame->pendingTerrainPacket >= 0 ||
        static_cast<CommandContext*>(opaqueContext)->inRenderPass) return busy;
    uint64_t missingOperationReceipt = 0;
    if (const auto status = sealExecutionCommands(*frame, stream, missingOperationReceipt); status != success)
        return status;
    if (const auto status = validateLaneOperationResources(*frame, stream, missingOperationReceipt); status != success)
        return status;
    validateLaneGeometryOperations(*frame, stream);
    if (const auto status = prepareTerrainExecutionSelections(*frame, stream); status != success)
        return status;
    validateLaneExecutionOrder(stream);
    if (const auto status = validateOpaqueDepthSeed(*frame, stream); status != success) return status;
    stream.laneDepthCommandsReady = std::ranges::none_of(stream.opaqueSeedIssueSequences,
        [](uint64_t sequence) { return sequence != 0; });
    stream.missingMask |= input.missingMask;
    if (!completeManifest) stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_MANIFEST;
    if (input.flags & MCDR_OIT_EXECUTION_V2_MANIFEST_FAILED) stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_FAILED;
    for (const auto& scope : stream.scopes) {
        if (scope.open) stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_OPEN_SCOPE;
    }
    for (const auto& pass : stream.passes)
        if (pass.open) stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_OPEN_PASS;
    if (stream.receipts.size() != frame->terrainStageReceipts.size()) stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_RECEIPT;
    if (!stream.normalizationPayload || !frame->programs->normalizationPrograms)
        stream.missingMask |= MCDR_OIT_EXECUTION_V2_GAP_NORMALIZATION;
    stream.state = stream.missingMask ? MCDR_OIT_EXECUTION_V2_STATE_UNAVAILABLE : MCDR_OIT_EXECUTION_V2_STATE_SEALED;
    if (stream.missingMask) cancelExecutionTextureCaptures(*frame);
    else *output = opaqueStream;
    result->schemaVersion = 2;
    result->structSize = sizeof(*result);
    result->state = stream.state;
    result->missingMask = stream.missingMask;
    result->firstEventId = stream.events.empty() ? 0 : 1;
    result->firstIssueSequence = stream.firstIssueSequence;
    result->expected = static_cast<uint32_t>(frame->terrainStageReceipts.size());
    result->observed = static_cast<uint32_t>(stream.receipts.size());
    result->receiptId = missingOperationReceipt;
    size_t consumedIndex = 0;
    for (const auto& receipt : frame->terrainStageReceipts) {
        if (consumedIndex == stream.receipts.size() || stream.receipts[consumedIndex] != receipt.receiptId) {
            result->receiptId = receipt.receiptId;
            break;
        }
        ++consumedIndex;
    }
    return stream.missingMask ? unsupported : success;
}
int recordLaneDepthEvents(Frame& frame, Dx12Object* depth, const mcdr::terrain_tile::BatchV2& batch,
    std::span<const vanilla::WorldDepthSeedEventV1> events, D3D12_GPU_VIRTUAL_ADDRESS constantsAddress) {
    using namespace mcdr::terrain_tile;
    auto* context = static_cast<CommandContext*>(frame.context);
    const auto count = batch.residentTileCount;
    if (context->inRenderPass || context->graphicsCommandObserver.prepare ||
        batch.schemaVersion != 2 || batch.structSize != sizeof(batch) || batch.flags || batch.reserved0 ||
        batch.laneIndex >= laneCount || batch.frameSerial != frame.serial ||
        batch.worldEpoch != frame.input.identity.worldEpoch ||
        batch.resourceVersion != frame.input.identity.resourceVersion ||
        batch.programGeneration != frame.input.programGeneration ||
        !validLaneScratch(depth, count, DXGI_FORMAT_D32_FLOAT) ||
        !(depth->resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        return invalid;
    for (uint32_t index = 0; index < count; ++index) {
        const auto& tile = batch.tiles[index];
        if (!tile.effectiveWidth || tile.effectiveWidth > tileSize ||
            !tile.effectiveHeight || tile.effectiveHeight > tileSize ||
            uint64_t(tile.originX) + tile.effectiveWidth > frame.input.width ||
            uint64_t(tile.originY) + tile.effectiveHeight > frame.input.height)
            return invalid;
    }
    for (const auto& event : events) {
        if (event.kind != vanilla::WorldDepthSeedEventKind::Draw) continue;
        const auto inputs = resolveGraphicsCommandContent(context, event.packet);
        if (!inputs || inputs->status != PacketContentStatus::Frozen) return unsupported;
    }
    auto& views = frame.executionStreamV2->seedDepthViews;
    auto found = std::ranges::find(views, depth, &SeedDepthViews::depth);
    if (found == views.end()) {
        auto budget = mcdr::oit_budget::reserve(allocationAlignment, frame.input.totalBudgetBytes);
        if (!budget) return overBudget;
        auto* device = deviceContext().device.Get();
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heap.NumDescriptors = count + 1;
        SeedDepthViews entry{depth, count, device->GetDescriptorHandleIncrementSize(heap.Type), {},
            retainObjectLease(depth), std::move(budget)};
        const HRESULT result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&entry.heap));
        if (FAILED(result)) return result == E_OUTOFMEMORY ? overBudget : recordingFailed;
        const auto dsv = entry.heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.ArraySize = count;
        device->CreateDepthStencilView(depth->resource.Get(), &desc, dsv);
        desc.Texture2DArray.ArraySize = 1;
        for (uint32_t index = 0; index < count; ++index) {
            desc.Texture2DArray.FirstArraySlice = index;
            device->CreateDepthStencilView(depth->resource.Get(), &desc, {dsv.ptr + (index + 1) * entry.descriptorStride});
        }
        views.push_back(std::move(entry));
        found = std::prev(views.end());
    }
    if (found->slices != count) return invalid;
    const auto dsv = found->heap->GetCPUDescriptorHandleForHeapStart();
    const auto increment = found->descriptorStride;
    auto* list = context->commandList;
    const auto& caps = deviceContext().caps;
    ComPtr<ID3D12GraphicsCommandList5> shadingRateList;
    if (caps.shadingRateSupported() && FAILED(list->QueryInterface(IID_PPV_ARGS(&shadingRateList))))
        return unsupported;
    const vanilla::PassResourceUse use{depth, batch.resourceVersion, 0, count, vanilla::ResourceAccess::ReadWrite,
        D3D12_RESOURCE_STATE_DEPTH_WRITE};
    vanilla::DeclaredPassToken* pass{};
    if (prepareSampledTextureWrite(context, depth, 0, count) != SampledTextureContentStatus::Ready ||
        vanilla::beginDeclaredPass(context, 0x227, &use, 1, 3, &pass) != success) {
        markExecutionOperationFailure(frame);
        return recordingFailed;
    }
    if (shadingRateList) {
        if (caps.shadingRateImageSupported()) shadingRateList->RSSetShadingRateImage(nullptr);
        const D3D12_SHADING_RATE_COMBINER combiners[]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
            D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
        shadingRateList->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
    }
    list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    restoreDrawHeap(context);
    const auto& lane = laneOffsets[batch.laneIndex];
    const auto shiftRect = [](D3D12_RECT rect, const Tile& tile) {
        constexpr auto guard = static_cast<int64_t>(guardPixels);
        const auto x = guard - tile.originX, y = guard - tile.originY;
        const auto width = tile.effectiveWidth + 2 * guard, height = tile.effectiveHeight + 2 * guard;
        return D3D12_RECT{static_cast<LONG>(std::clamp(rect.left + x, int64_t{0}, width)),
            static_cast<LONG>(std::clamp(rect.top + y, int64_t{0}, height)),
            static_cast<LONG>(std::clamp(rect.right + x, int64_t{0}, width)),
            static_cast<LONG>(std::clamp(rect.bottom + y, int64_t{0}, height))};
    };
    bool failed = false;
    for (const auto& event : events) {
        if (event.kind == vanilla::WorldDepthSeedEventKind::Clear) {
            const auto clear = static_cast<float>(event.clearDepth);
            if (!event.hasClearRect) list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clear, 0, 0, nullptr);
            else for (uint32_t index = 0; index < count; ++index) {
                const auto rect = shiftRect(event.clearRect, batch.tiles[index]);
                if (rect.left >= rect.right || rect.top >= rect.bottom) continue;
                list->ClearDepthStencilView({dsv.ptr + (index + 1) * increment}, D3D12_CLEAR_FLAG_DEPTH,
                    clear, 0, 1, &rect);
            }
            continue;
        }
        const auto input = inspectGraphicsCommandPacket(context, event.packet);
        std::array<D3D12_VIEWPORT, maximumResidentTiles> viewports{};
        std::array<D3D12_RECT, maximumResidentTiles> scissors{};
        for (uint32_t index = 0; index < count; ++index) {
            const auto& tile = batch.tiles[index];
            viewports[index] = input.viewport;
            viewports[index].TopLeftX += static_cast<float>(guardPixels) - static_cast<float>(tile.originX) - lane[0];
            viewports[index].TopLeftY += static_cast<float>(guardPixels) - static_cast<float>(tile.originY) - lane[1];
            scissors[index] = shiftRect(input.scissor, tile);
        }
        if (const auto draw = recordTileGraphicsPacket(context, event.packet,
                std::span(viewports).first(count), std::span(scissors).first(count), constantsAddress); !draw) {
            dbgLogError("OIT lane depth draw: %s", draw.error().message.c_str());
            failed = true;
            break;
        }
    }
    list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    if (shadingRateList) {
        if (caps.shadingRateImageSupported()) {
            auto* image = context->shadingRateImageBound;
            shadingRateList->RSSetShadingRateImage(image ? image->resource.Get() : nullptr);
        }
        shadingRateList->RSSetShadingRate(context->recordedShadingRate, context->recordedShadingCombiners);
    }
    invalidateGraphicsInputBindings(context, false);
    if (failed || vanilla::finishDeclaredPass(context, pass) != success) {
        vanilla::failRasterInputPass(context);
        markExecutionOperationFailure(frame);
        return recordingFailed;
    }
    return success;
}

int recordExecutionOpaqueSeed(void* opaqueContext, void* streamHandle, void* mainDepth,
    const mcdr::terrain_tile::BatchV2& batch, mcdr::terrain_tile::ConstantAddresses constants) {
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findExecutionFrameV2(opaqueContext, streamHandle);
    if (!frame || !frame->opaqueDepthSeed || !frame->executionStreamV2->laneDepthCommandsReady) return stale;
    auto* depth = static_cast<Dx12Object*>(mainDepth);
    const auto events = std::span(frame->opaqueDepthSeed->events);
    using mcdr::graphics_recording::CommandFamily;
    using mcdr::terrain_tile::Result;
    std::vector<std::pair<size_t, std::shared_ptr<const void>>> terrainPlans;
    for (size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (event.kind == vanilla::WorldDepthSeedEventKind::Clear) continue;
        if (event.commandFamily == static_cast<uint32_t>(CommandFamily::Mesh)) {
            const auto input = inspectGraphicsCommandPacket(opaqueContext, event.packet);
            std::shared_ptr<const void> plan;
            const auto prepared = prepareTerrainTilePacketV2(opaqueContext,
                input.adapter.replayContent, batch, {depth, 0, 0}, constants, &plan);
            if (prepared != Result::Prepared || !plan) {
                dbgLogError("OIT terrain seed: prepare failed (%d)", static_cast<int>(prepared));
                return unsupported;
            }
            terrainPlans.emplace_back(index, std::move(plan));
        } else {
            const auto inputs = resolveGraphicsCommandContent(opaqueContext, event.packet);
            if (!inputs || inputs->status != PacketContentStatus::Frozen) return unsupported;
        }
    }
    size_t next = 0;
    for (const auto& [index, plan] : terrainPlans) {
        if (index > next) {
            const auto result = recordLaneDepthEvents(*frame, depth, batch, events.subspan(next, index - next), constants.batch);
            if (result != success) return result;
        }
        if (const auto replay = recordTerrainTileReplayPlan(opaqueContext, plan, terrainOpaqueSeedStage,
                TerrainTileAttachmentMode::Load); !replay) {
            dbgLogError("OIT terrain seed: %s", replay.error().message.c_str());
            markExecutionOperationFailure(*frame);
            return recordingFailed;
        }
        next = index + 1;
    }
    if (next == events.size()) return success;
    return recordLaneDepthEvents(*frame, depth, batch, events.subspan(next), constants.batch);
}

int recordExecutionLaneBoundsCull(Frame& frame, const ExecutionOperationEvent& operation,
    const ExecutionLaneTargets& targets) {
    auto* context = static_cast<CommandContext*>(frame.context);
    auto* bounds = static_cast<Dx12Object*>(targets.bounds);
    auto* culled = static_cast<Dx12Object*>(targets.culledBounds);
    auto* depth = static_cast<Dx12Object*>(targets.mainDepth);
    constexpr auto boundsFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    const auto count = targets.residentTileCount;
    if (context->inRenderPass || context->graphicsCommandObserver.prepare ||
        !validLaneScratch(bounds, count, boundsFormat) || !validLaneScratch(culled, count, boundsFormat) ||
        !validLaneScratch(depth, count, DXGI_FORMAT_D32_FLOAT) || bounds->resource == culled->resource ||
        !(culled->resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        !(depth->resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) return invalid;
    const auto& lease = operation.laneCullProjection;
    if (packetContentStatus(context, lease) != PacketContentStatus::Frozen) return stale;
    const auto content = std::static_pointer_cast<const PacketContentLease>(lease);
    if (content->frozenReads.size() != 1) return invalid;
    const auto& projection = content->frozenReads.front();
    if (!projection.resource || !projection.object || projection.byteLength != 64 ||
        projection.byteOffset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) return invalid;
    const auto& programs = frame.programs;
    const auto& caps = deviceContext().caps;
    ComPtr<ID3D12GraphicsCommandList5> shadingRateList;
    if (caps.shadingRateSupported() && FAILED(context->commandList->QueryInterface(IID_PPV_ARGS(&shadingRateList))))
        return unsupported;
    auto descriptors = allocateDrawDescriptors(context, 1);
    if (!descriptors) return overBudget;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = boundsFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.ArraySize = count;
    deviceContext().device->CreateShaderResourceView(bounds->resource.Get(), &view, descriptors->cpu);
    const uint32_t projectionState = projection.initialState == D3D12_RESOURCE_STATE_GENERIC_READ
        ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    const auto version = frame.input.identity.resourceVersion;
    const vanilla::PassResourceUse uses[] = {
        {projection.object, version, 0, 1, vanilla::ResourceAccess::Read, projectionState},
        {bounds, version, 0, count, vanilla::ResourceAccess::Read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
        {culled, version, 0, count, vanilla::ResourceAccess::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COMMON},
        {depth, version, 0, count, vanilla::ResourceAccess::ReadWrite, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_COMMON},
    };
    vanilla::DeclaredPassToken* pass{};
    constexpr int extent = static_cast<int>(mcdr::terrain_tile::scratchSize);
    constexpr uint32_t descriptorEffects = 1u << 1;
    if (prepareSampledTextureWrite(context, culled, 0, count) != SampledTextureContentStatus::Ready ||
        prepareSampledTextureWrite(context, depth, 0, count) != SampledTextureContentStatus::Ready ||
        vanilla::beginDeclaredPass(context, 0x226, uses, static_cast<uint32_t>(std::size(uses)), descriptorEffects, &pass) != success ||
        !beginRenderPass(context, &culled, 1, nullptr, nullptr, nullptr, depth, 0, 0, 0.0, 0, 0, extent, extent)) {
        markExecutionOperationFailure(frame);
        return recordingFailed;
    }
    if (shadingRateList) {
        if (caps.shadingRateImageSupported()) shadingRateList->RSSetShadingRateImage(nullptr);
        const D3D12_SHADING_RATE_COMBINER combiners[]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
            D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
        shadingRateList->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
    }
    auto* commands = context->commandList;
    commands->SetGraphicsRootSignature(programs->laneCullRoot.Get());
    restoreDrawHeap(context);
    commands->SetGraphicsRootConstantBufferView(0, projection.resource->GetGPUVirtualAddress() + projection.byteOffset);
    commands->SetGraphicsRootDescriptorTable(1, descriptors->gpu);
    commands->SetPipelineState(programs->laneCullPipeline.Get());
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, count, 0, 0);
    if (shadingRateList) {
        if (caps.shadingRateImageSupported()) {
            auto* image = context->shadingRateImageBound;
            shadingRateList->RSSetShadingRateImage(image ? image->resource.Get() : nullptr);
        }
        shadingRateList->RSSetShadingRate(context->recordedShadingRate, context->recordedShadingCombiners);
    }
    context->currentPipeline = nullptr;
    std::ranges::fill(context->activeColorTargetsTouched, true);
    context->colorTargetsWritten = true;
    if (!endRenderPass(context) || vanilla::finishDeclaredPass(context, pass) != success) {
        markExecutionOperationFailure(frame);
        return recordingFailed;
    }
    return success;
}

int recordExecutionLaneOperation(void* opaqueContext, void* opaqueStream, uint64_t receiptId,
    const ExecutionLaneTargets& targets) {
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findExecutionFrameV2(opaqueContext, opaqueStream);
    if (!frame || !frame->executionStreamV2->commands) return stale;
    const auto& stream = *frame->executionStreamV2;
    const auto found = frame->executionOperationReceipts.find(receiptId);
    if (found == frame->executionOperationReceipts.end() || !found->second.packet ||
        !found->second.packet->executionV2EventId) return stale;
    const auto& receipt = found->second.packet->receipt;
    const auto& commands = *stream.commands;
    const auto command = std::ranges::lower_bound(commands, receipt.nativeSequence, {}, &ExecutionCommand::nativeSequence);
    if (command == commands.end() || command->source != ExecutionCommandSource::Operation ||
        command->receiptId != receiptId) return stale;
    const auto& operation = stream.operations[command->packetIndex];
    if (operation.receipt.recordState == MCDR_OIT_OPERATION_EMPTY) return success;
    if (operation.laneWaterMask) {
        if (!targets.batch || targets.batch->residentTileCount != targets.residentTileCount) return invalid;
        vanilla::WorldDepthSeedEventV1 event;
        event.kind = vanilla::WorldDepthSeedEventKind::Draw;
        event.packet = operation.packet;
        return recordLaneDepthEvents(*frame, static_cast<Dx12Object*>(targets.terrainDepth),
            *targets.batch, std::span(&event, 1), targets.batchConstants);
    }
    if (operation.laneCullProjection) return recordExecutionLaneBoundsCull(*frame, operation, targets);
    const auto& copy = operation.laneCopy;
    if (!copy.sourceRole || !copy.destinationRole) return unsupported;
    const auto resolve = [&targets](uint32_t role) -> Dx12Object* {
        switch (role) {
        case MCDR_OIT_EXECUTION_V2_RESOURCE_MAIN_DEPTH: return static_cast<Dx12Object*>(targets.mainDepth);
        case MCDR_OIT_EXECUTION_V2_RESOURCE_TERRAIN_DEPTH: return static_cast<Dx12Object*>(targets.terrainDepth);
        case MCDR_OIT_EXECUTION_V2_RESOURCE_CLOUD_DEPTH: return static_cast<Dx12Object*>(targets.cloudDepth);
        case MCDR_OIT_EXECUTION_V2_RESOURCE_BOUNDS: return static_cast<Dx12Object*>(targets.bounds);
        case MCDR_OIT_EXECUTION_V2_RESOURCE_CULLED_BOUNDS: return static_cast<Dx12Object*>(targets.culledBounds);
        default: return nullptr;
        }
    };
    auto* source = resolve(copy.sourceRole);
    auto* destination = resolve(copy.destinationRole);
    const bool depth = copy.sourceRole == MCDR_OIT_EXECUTION_V2_RESOURCE_MAIN_DEPTH;
    const auto format = depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    const auto* context = static_cast<CommandContext*>(opaqueContext);
    if (context->inRenderPass || !validLaneScratch(source, targets.residentTileCount, format) ||
        !validLaneScratch(destination, targets.residentTileCount, format) ||
        source->resource == destination->resource) return invalid;
    if (prepareSampledTextureWrite(opaqueContext, destination, 0, targets.residentTileCount) !=
        SampledTextureContentStatus::Ready) {
        markExecutionOperationFailure(*frame);
        return recordingFailed;
    }
    const auto version = frame->input.identity.resourceVersion;
    const vanilla::PassResourceUse uses[] = {
        {source, version, 0, targets.residentTileCount, vanilla::ResourceAccess::Read,
            D3D12_RESOURCE_STATE_COPY_SOURCE},
        {destination, version, 0, targets.residentTileCount, vanilla::ResourceAccess::Write,
            D3D12_RESOURCE_STATE_COPY_DEST},
    };
    vanilla::DeclaredPassToken* pass{};
    if (vanilla::beginDeclaredPass(opaqueContext, 0x225, uses, static_cast<uint32_t>(std::size(uses)), 0, &pass) != success) {
        markExecutionOperationFailure(*frame);
        return recordingFailed;
    }
    context->commandList->CopyResource(destination->resource.Get(), source->resource.Get());
    if (vanilla::finishDeclaredPass(opaqueContext, pass) != success) {
        markExecutionOperationFailure(*frame);
        return recordingFailed;
    }
    return success;
}

int abortExecutionStreamV2(void* opaqueContext, void* opaqueStream) {
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findExecutionFrameV2(opaqueContext, opaqueStream);
    if (!frame) return stale;
    const bool operationFailed = cancelActiveExecutionOperations(*frame);
    frame->executionStreamV2->state = MCDR_OIT_EXECUTION_V2_STATE_ABORTED;
    cancelExecutionTextureCaptures(*frame);
    return operationFailed ? recordingFailed : success;
}

namespace {
mcdr::terrain_tile::ResourceRef laneDepth(const mcdr::terrain_tile::BatchV2& batch, uint32_t role) {
    switch (role) {
    case MCDR_OIT_EXECUTION_V2_RESOURCE_MAIN_DEPTH: return batch.stages[0].depth;
    case MCDR_OIT_EXECUTION_V2_RESOURCE_TERRAIN_DEPTH: return batch.depthTerrain;
    case MCDR_OIT_EXECUTION_V2_RESOURCE_CLOUD_DEPTH: return batch.depthCloud;
    default: return {};
    }
}

mcdr::terrain_tile::StageBindingV2 laneStage(const mcdr::terrain_tile::BatchV2& batch, uint32_t stageIndex,
    const LaneGeometry& geometry) {
    auto stage = batch.stages[stageIndex];
    stage.pipeline = nullptr;
    stage.depth = laneDepth(batch, geometry.depthRole);
    if (stageIndex == 0 && geometry.outputRole == MCDR_OIT_EXECUTION_V2_RESOURCE_CULLED_BOUNDS)
        stage.renderTargets[0] = batch.culledBounds;
    return stage;
}

int recordLaneGeometry(Frame& frame, const mcdr::terrain_tile::BatchV2& batch,
    const ExecutionOperationEvent& operation, uint32_t stageIndex, D3D12_GPU_VIRTUAL_ADDRESS constants) {
    using namespace mcdr::terrain_tile;
    auto* context = static_cast<CommandContext*>(frame.context);
    const auto stage = laneStage(batch, stageIndex, operation.geometry);
    auto* depth = static_cast<Dx12Object*>(stage.depth.object);
    const auto count = batch.residentTileCount;
    std::array<Dx12Object*, 4> targets{};
    std::array<Dx12Object*, 5> reads{};
    std::vector<vanilla::PassResourceUse> uses;
    uses.reserve(10);
    const auto write = [&](Dx12Object* object, uint32_t state) {
        uses.push_back({object, batch.resourceVersion, 0, count, vanilla::ResourceAccess::ReadWrite,
            state, D3D12_RESOURCE_STATE_COMMON});
    };
    const auto read = [&](Dx12Object* object) {
        uses.push_back({object, batch.resourceVersion, 0, count, vanilla::ResourceAccess::Read,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE});
    };
    for (uint32_t index = 0; index < stage.renderTargetCount; ++index) {
        targets[index] = static_cast<Dx12Object*>(stage.renderTargets[index].object);
        write(targets[index], D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    write(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (stageIndex != 0) {
        reads[0] = static_cast<Dx12Object*>(batch.culledBounds.object);
        read(reads[0]);
    }
    if (stageIndex == 2)
        for (uint32_t index = 0; index < operation.geometry.coefficientCount; ++index) {
            reads[index + 1] = static_cast<Dx12Object*>(batch.coefficients[index].object);
            read(reads[index + 1]);
        }
    const auto inspection = inspectGraphicsCommandPacket(context, operation.packet);
    std::array<D3D12_VIEWPORT, maximumResidentTiles> viewports{};
    std::array<D3D12_RECT, maximumResidentTiles> scissors{};
    const auto& offset = laneOffsets[batch.laneIndex];
    for (uint32_t index = 0; index < count; ++index) {
        const auto& tile = batch.tiles[index];
        const auto x = static_cast<int64_t>(guardPixels) - tile.originX;
        const auto y = static_cast<int64_t>(guardPixels) - tile.originY;
        const auto w = static_cast<int64_t>(tile.effectiveWidth + 2 * guardPixels);
        const auto h = static_cast<int64_t>(tile.effectiveHeight + 2 * guardPixels);
        viewports[index] = inspection.viewport;
        viewports[index].TopLeftX += static_cast<float>(x) - offset[0];
        viewports[index].TopLeftY += static_cast<float>(y) - offset[1];
        scissors[index] = {
            static_cast<LONG>(std::clamp(inspection.scissor.left + x, int64_t{0}, w)),
            static_cast<LONG>(std::clamp(inspection.scissor.top + y, int64_t{0}, h)),
            static_cast<LONG>(std::clamp(inspection.scissor.right + x, int64_t{0}, w)),
            static_cast<LONG>(std::clamp(inspection.scissor.bottom + y, int64_t{0}, h)),
        };
    }
    vanilla::DeclaredPassToken* pass{};
    if (vanilla::beginDeclaredPass(context, 0x228u + stageIndex, uses.data(),
            static_cast<uint32_t>(uses.size()), 3, &pass) != success ||
        !beginRenderPass(context, targets.data(), static_cast<int>(stage.renderTargetCount), nullptr, nullptr,
            nullptr, depth, 0, 0, 0.0, 0, 0, scratchSize, scratchSize))
        return recordingFailed;
    const auto& caps = deviceContext().caps;
    ComPtr<ID3D12GraphicsCommandList5> shadingRateList;
    if (caps.shadingRateSupported() && FAILED(context->commandList->QueryInterface(IID_PPV_ARGS(&shadingRateList))))
        return recordingFailed;
    if (shadingRateList) {
        if (caps.shadingRateImageSupported()) shadingRateList->RSSetShadingRateImage(nullptr);
        const D3D12_SHADING_RATE_COMBINER combiners[]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
            D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
        shadingRateList->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
    }
    const auto draw = recordTileGraphicsPacket(context, operation.packet,
        std::span(viewports).first(count), std::span(scissors).first(count), constants, reads);
    if (shadingRateList) {
        if (caps.shadingRateImageSupported()) {
            auto* image = context->shadingRateImageBound;
            shadingRateList->RSSetShadingRateImage(image ? image->resource.Get() : nullptr);
        }
        shadingRateList->RSSetShadingRate(context->recordedShadingRate, context->recordedShadingCombiners);
    }
    if (draw) {
        std::ranges::fill(context->activeColorTargetsTouched, true);
        context->colorTargetsWritten = true;
    }
    context->currentPipeline = nullptr;
    const auto ended = endRenderPass(context);
    const auto finished = ended ? vanilla::finishDeclaredPass(context, pass) : recordingFailed;
    return draw && finished == success ? success : recordingFailed;
}
}

int executeTileStreamV2(void* opaqueContext, void* streamHandle,
    const mcdr::terrain_tile::BatchV2* batches, void* correctedHandle) try {
    using namespace mcdr::terrain_tile;
    if (const int status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findExecutionFrameV2(opaqueContext, streamHandle);
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!frame) return stale;
    auto& stream = *frame->executionStreamV2;
    if (stream.state != MCDR_OIT_EXECUTION_V2_STATE_SEALED || !stream.commands ||
        !stream.normalizationPayload || !stream.terrainSelections || stream.normalizedPublished) return stale;
    const auto failed = [&](int status) {
        if (status == overBudget && !context->recordingFailed.load() && !context->submissionFailed.load()) {
            // Completed groups only wrote scratch. The caller can still use this frame's original Q.
            stream.state = MCDR_OIT_EXECUTION_V2_STATE_UNAVAILABLE;
            cancelExecutionTextureCaptures(*frame);
            dbgLogDebug("OIT tile replay: budget exhausted");
            return status;
        }
        if (!stream.tileExecutionStarted) return status;
        vanilla::failRasterInputPass(context);
        markExecutionOperationFailure(*frame);
        return recordingFailed;
    };
    if (!batches || context->inRenderPass || context->graphicsCommandObserver.prepare) return failed(invalid);
    const auto coefficientCount = stream.normalizationPayload->targetCount;
    const auto count = batches[0].residentTileCount;
    if (count == 0 || count > maximumResidentTiles) return failed(invalid);
    auto* corrected = static_cast<Dx12Object*>(correctedHandle);
    if (corrected) {
        if (!corrected->resource || corrected->kind != Dx12Object::Kind::Texture ||
            corrected->dxgiFormat != DXGI_FORMAT_R32G32B32A32_FLOAT) return failed(invalid);
        const auto desc = corrected->resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != frame->input.width ||
            desc.Height != frame->input.height || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
            desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
            desc.Format != DXGI_FORMAT_R32G32B32A32_FLOAT) return failed(invalid);
    }
    for (uint32_t lane = 0; lane < laneCount; ++lane) {
        const auto& batch = batches[lane];
        if (batch.schemaVersion != 2 || batch.structSize != sizeof(batch) || batch.flags || batch.reserved0 ||
            batch.laneIndex != lane || batch.residentTileCount != count ||
            batch.frameSerial != frame->serial || batch.worldEpoch != frame->input.identity.worldEpoch ||
            batch.resourceVersion != frame->input.identity.resourceVersion ||
            batch.programGeneration != frame->input.programGeneration ||
            std::memcmp(batches[0].tiles, batch.tiles, count * sizeof(Tile)) != 0 ||
            batch.stages[0].renderTargetCount != 1 || batch.stages[1].renderTargetCount != coefficientCount ||
            batch.stages[2].renderTargetCount != 1) return failed(invalid);
        std::array<ID3D12Resource*, 10> used{};
        size_t usedCount = 0;
        const auto validScratch = [&](const ResourceRef& reference, DXGI_FORMAT format) {
            const auto* texture = static_cast<Dx12Object*>(reference.object);
            if (reference.offset || reference.bytes != 1 || !validLaneScratch(texture, count, format)) return false;
            auto* backing = texture->resource.Get();
            const auto prior = std::span(used).first(usedCount);
            const auto usage = format == DXGI_FORMAT_D32_FLOAT ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL :
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (!(backing->GetDesc().Flags & usage) || std::ranges::contains(prior, backing)) return false;
            used[usedCount++] = backing;
            return true;
        };
        for (const auto& reference : {batch.stages[0].depth, batch.depthTerrain, batch.depthCloud})
            if (!validScratch(reference, DXGI_FORMAT_D32_FLOAT)) return failed(invalid);
        for (const auto& reference : {batch.bounds, batch.culledBounds})
            if (!validScratch(reference, DXGI_FORMAT_R32G32B32A32_FLOAT)) return failed(invalid);
        if (!validScratch(batch.q, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            batch.stages[0].renderTargets[0].object != batch.bounds.object ||
            batch.stages[2].renderTargets[0].object != batch.q.object) return failed(invalid);
        for (uint32_t index = 0; index < coefficientCount; ++index)
            if (!validScratch(batch.coefficients[index], DXGI_FORMAT_R16G16B16A16_FLOAT) ||
                batch.stages[1].renderTargets[index].object != batch.coefficients[index].object) return failed(invalid);
    }
    mcdr_oit_normalization_batch_v1 normalization{};
    normalization.schemaVersion = 1;
    normalization.structSize = sizeof(normalization);
    normalization.identity = frame->input.identity;
    normalization.frameSerial = frame->serial;
    normalization.historyGeneration = frame->input.historyGeneration;
    normalization.programGeneration = frame->input.programGeneration;
    normalization.totalBudgetBytes = frame->input.totalBudgetBytes;
    normalization.outputWidth = frame->input.width;
    normalization.outputHeight = frame->input.height;
    normalization.residentCount = count;
    for (uint32_t index = 0; index < count; ++index) {
        const auto& tile = batches[0].tiles[index];
        normalization.regions[index] = {tile.originX, tile.originY, tile.effectiveWidth, tile.effectiveHeight};
    }
    void* handle{};
    const int prepared = createPreparedNormalizationBatch(context, frame->programs->normalizationPrograms, coefficientCount,
        normalization, &handle);
    if (prepared != MCDR_OIT_NORMALIZATION_SUCCESS) return failed(prepared);
    const std::unique_ptr<void, decltype(&destroyNormalizationBatch)> batchLease(handle, &destroyNormalizationBatch);
    std::array<ConstantAddresses, laneCount> uploads{};
    for (uint32_t lane = 0; lane < laneCount; ++lane) {
        const auto upload = uploadTileConstants(*frame, batches[lane]);
        if (!upload) return failed(upload.error());
        uploads[lane] = *upload;
    }
    stream.tileExecutionStarted = true;
    for (uint32_t lane = 0; lane < laneCount; ++lane) {
        const auto& batch = batches[lane];
        const auto constants = uploads[lane];
        if (const auto initialized = initializeTerrainTileLane(context, batch); !initialized) {
            dbgLogError("OIT lane initialization: %s", initialized.error().message.c_str());
            return failed(recordingFailed);
        }
        if (const int seed = recordExecutionOpaqueSeed(context, streamHandle, batch.stages[0].depth.object, batch, constants);
            seed != success) {
            if (seed == overBudget) return failed(seed);
            dbgLogError("OIT lane seed: recording failed (%d)", seed);
            return failed(seed);
        }
        const ExecutionLaneTargets targets{batch.stages[0].depth.object, batch.depthTerrain.object,
            batch.depthCloud.object, batch.bounds.object, batch.culledBounds.object, count, &batch, constants.batch};
        size_t terrainIndex = 0;
        for (const auto& command : *stream.commands) {
            const auto stage = stream.events[command.eventId - 1].key.stage;
            if (command.source == ExecutionCommandSource::Operation) {
                const auto& operation = stream.operations[command.packetIndex];
                const int result = operation.geometry.depthRole
                    ? recordLaneGeometry(*frame, batch, operation, stage - 1u, constants.batch)
                    : recordExecutionLaneOperation(context, streamHandle, command.receiptId, targets);
                if (result != success) {
                    dbgLogError("OIT lane operation: kind=%u stage=%u (%d)",
                        stream.events[command.eventId - 1].kind, stage, result);
                    return failed(result);
                }
                continue;
            }
            const auto& selection = (*stream.terrainSelections)[terrainIndex++];
            const auto& packet = frame->terrainStagePackets[selection.packetIndex];
            auto packetBatch = batch;
            packetBatch.stages[stage - 1u] = laneStage(batch, stage - 1u, selection.geometry);
            std::shared_ptr<const void> plan;
            if (const auto prepared = packet.prepareReplay(context, packet.replayPacket, packetBatch, constants, &plan);
                prepared != Result::Prepared) {
                dbgLogError("OIT terrain lane: stage=%u prepare failed (%d)", stage, static_cast<int>(prepared));
                return failed(recordingFailed);
            }
            if (const auto replay = recordTerrainTileReplayPlan(context, plan, stage - 1u, TerrainTileAttachmentMode::Load);
                !replay) {
                dbgLogError("OIT terrain lane: %s", replay.error().message.c_str());
                return failed(recordingFailed);
            }
        }
        mcdr_oit_normalization_lane_v1 input{};
        input.schemaVersion = 1;
        input.structSize = sizeof(input);
        input.laneIndex = lane;
        input.q = {static_cast<mcdr_object*>(batch.q.object), 0, count};
        for (uint32_t index = 0; index < coefficientCount; ++index)
            input.coefficients[index] = {static_cast<mcdr_object*>(batch.coefficients[index].object), 0, count};
        if (const auto result = recordNormalizationLane(context, handle, input);
            result != MCDR_OIT_NORMALIZATION_RECORDED_UNVERIFIED) {
            dbgLogError("OIT lane normalization: lane=%u (%d)", lane, result);
            return failed(result);
        }
    }
    if (const auto result = recordNormalizationAverage(context, handle);
        result != MCDR_OIT_NORMALIZATION_RECORDED_UNVERIFIED) return failed(result);
    if (!corrected) return success;
    ID3D12Resource* normalized{};
    if (const auto result = resolveNormalizationOutput(context, handle, &normalized);
        result != MCDR_OIT_NORMALIZATION_SUCCESS) return failed(result);
    if (prepareSampledTextureWrite(context, corrected, 0, 1) != SampledTextureContentStatus::Ready)
        return failed(recordingFailed);
    const auto version = frame->input.identity.resourceVersion;
    const vanilla::PassResourceUse uses[] = {
        {nullptr, version, 0, 1, vanilla::ResourceAccess::Read, D3D12_RESOURCE_STATE_COPY_SOURCE,
            UINT32_MAX, vanilla::ExternalResourceState::Tracked, normalized, D3D12_RESOURCE_STATE_COMMON},
        {corrected, version, 0, 1, vanilla::ResourceAccess::Write, D3D12_RESOURCE_STATE_COPY_DEST},
    };
    vanilla::DeclaredPassToken* pass{};
    if (vanilla::beginDeclaredPass(context, 0x22b, uses, static_cast<uint32_t>(std::size(uses)), 0, &pass) != success)
        return failed(recordingFailed);
    context->commandList->CopyResource(corrected->resource.Get(), normalized);
    if (vanilla::finishDeclaredPass(context, pass) != success) return failed(recordingFailed);
    stream.normalizedPublished = true;
    cancelExecutionTextureCaptures(*frame);
    return success;
} catch (...) {
    auto frame = findExecutionFrameV2(opaqueContext, streamHandle);
    if (!frame || !frame->executionStreamV2->tileExecutionStarted) throw;
    vanilla::failRasterInputPass(opaqueContext);
    markExecutionOperationFailure(*frame);
    dbgLogError("OIT tile command recording threw an exception");
    return recordingFailed;
}
int beginExecutionStream(void* opaqueContext, void* opaqueFrame, const mcdr_oit_execution_begin_v1& input,
    void** output) {
    if (output) *output = nullptr;
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findFrame(opaqueContext, opaqueFrame);
    if (!frame || !output || frame->phase != Phase::Recording || frame->executionStream ||
        input.schemaVersion != 1 || input.structSize != sizeof(input) || input.identity.worldEpoch == 0 ||
        input.identity.realFrameId == 0 || input.frameSerial != frame->serial ||
        !sameIdentity(input.identity, frame->input.identity) ||
        input.historyGeneration != frame->input.historyGeneration ||
        input.programGeneration != frame->input.programGeneration || input.maxEvents == 0 ||
        input.maxScopes == 0 || input.maxResourceReferences == 0 || input.reserved != 0)
        return invalid;
    const uint64_t bytes = static_cast<uint64_t>(input.maxEvents) * sizeof(ExecutionEvent) +
        static_cast<uint64_t>(input.maxScopes) * sizeof(ExecutionScopeState) +
        static_cast<uint64_t>(input.maxResourceReferences) *
            (sizeof(mcdr_oit_execution_resource_v1) + sizeof(std::shared_ptr<void>));
    auto budget = mcdr::oit_budget::reserve(bytes, frame->input.totalBudgetBytes);
    if (!budget) return overBudget;
    auto stream = std::make_shared<ExecutionStream>();
    stream->context = opaqueContext;
    stream->recordingIdentity = frame->recordingIdentity;
    stream->input = input;
    stream->budget = std::move(budget);
    stream->nativeDrawCount = static_cast<uint32_t>(frame->terrainDrawReceipts.size());
    stream->token = nextToken.fetch_add(1);
    if (!stream->token) return recordingFailed;
    stream->events.reserve(input.maxEvents);
    stream->scopes.reserve(input.maxScopes);
    frame->executionStream = std::move(stream);
    *output = reinterpret_cast<void*>(frame->executionStream->token);
    return success;
}
int queryExecutionStream(void* opaqueContext, void* opaqueStream, mcdr_oit_execution_info_v1* output) {
    if (output) std::memset(output, 0, sizeof(*output));
    auto stream = findExecutionStream(opaqueContext, opaqueStream);
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!stream || !output || !context || context->submissionFailed.load()) return stream ? unknownSubmission : stale;
    output->schemaVersion = 1;
    output->structSize = sizeof(*output);
    output->identity = stream->input.identity;
    output->streamId = stream->token;
    output->frameSerial = stream->input.frameSerial;
    output->programGeneration = stream->input.programGeneration;
    auto frame = std::static_pointer_cast<Frame>(static_cast<CommandContext*>(opaqueContext)->oitFrameState);
    output->nativeDrawCount = frame ? static_cast<uint32_t>(frame->terrainDrawReceipts.size()) : 0;
    output->appendedDrawCount = stream->appendedDrawCount;
    output->eventCount = static_cast<uint32_t>(stream->events.size());
    output->openScopeCount = stream->openScopeCount;
    output->state = stream->state;
    output->unavailableReason = stream->unavailableReason;
    return success;
}
int queryTerrainDrawReceipt(void* opaqueContext, void* opaqueFrame, const mcdr_oit_draw_receipt_query_v1& query,
    mcdr_oit_draw_receipt_v1* output) {
    if (output) std::memset(output, 0, sizeof(*output));
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findFrame(opaqueContext, opaqueFrame);
    if (!frame || !output || query.schemaVersion != 1 || query.structSize != sizeof(query) ||
        query.reserved[0] != 0 || query.reserved[1] != 0 || (query.receiptId != 0 && query.afterSequence != 0))
        return invalid;
    const DrawReceipt* selected = nullptr;
    if (query.receiptId != 0) {
        for (const auto& receipt : frame->terrainDrawReceipts) {
            if (receipt.value.receiptId == query.receiptId) { selected = &receipt; break; }
        }
    } else {
        const auto first = std::upper_bound(frame->terrainDrawReceipts.begin(), frame->terrainDrawReceipts.end(),
            query.afterSequence, [](uint64_t sequence, const DrawReceipt& receipt) {
                return sequence < receipt.value.nativeSequence;
            });
        const auto match = std::find_if(first, frame->terrainDrawReceipts.end(), [&](const DrawReceipt& receipt) {
            return query.producerScopeSerial == 0 || receipt.value.producerScopeSerial == query.producerScopeSerial;
        });
        if (match != frame->terrainDrawReceipts.end()) selected = &*match;
    }
    if (!selected) return busy;
    if (query.receiptId != 0 && query.producerScopeSerial != 0 &&
        selected->value.producerScopeSerial != query.producerScopeSerial) return stale;
    *output = selected->value;
    return success;
}
int queryTerrainDrawFrontier(void* opaqueContext, void* opaqueFrame, uint64_t* afterSequence) {
    if (afterSequence) *afterSequence = 0;
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findFrame(opaqueContext, opaqueFrame);
    if (!frame || !afterSequence) return invalid;
    if (!frame->terrainDrawReceipts.empty())
        *afterSequence = frame->terrainDrawReceipts.back().value.nativeSequence;
    return success;
}
int beginTerrainStageDraw(void* opaqueContext, void* opaqueFrame,
    const mcdr_oit_terrain_stage_begin_v2& input, void** output) {
    if (output) *output = nullptr;
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findFrame(opaqueContext, opaqueFrame);
    if (!frame || !output || frame->phase != Phase::Recording || frame->terrainStageDraw ||
        input.schemaVersion != 2 || input.structSize != sizeof(input) || input.flags != 0 ||
        input.stage < MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS || input.stage > MCDR_OIT_EXECUTION_STAGE_ACCUMULATE ||
        input.frameSerial != frame->serial || !sameIdentity(input.identity, frame->input.identity) ||
        input.producerId == 0 || input.producerScopeSerial == 0 || input.sourcePassId == 0 ||
        input.sourceDrawId == 0 || input.parentSourceDrawId == input.sourceDrawId || !input.pipeline ||
        input.reserved[0] != 0 || input.reserved[1] != 0) return invalid;
    if (frame->terrainStageReceipts.size() >= UINT32_MAX || frame->nextNativeCommandSequence == UINT64_MAX)
        return overBudget;
    if (frame->terrainStageReceipts.size() == frame->terrainStageReceipts.capacity()) {
        const auto capacity = std::max<size_t>(64, frame->terrainStageReceipts.capacity() * 2);
        if (capacity > UINT32_MAX) return overBudget;
        auto reservation = std::make_unique<TerrainStageReceiptReservation>();
        reservation->budget = mcdr::oit_budget::reserve(
            (capacity - frame->terrainStageReceipts.capacity()) *
                (sizeof(mcdr_oit_terrain_stage_receipt_v2) + sizeof(TerrainStagePacket)) +
                sizeof(TerrainStageReceiptReservation), frame->input.totalBudgetBytes);
        if (!reservation->budget) return overBudget;
        reservation->previous = std::move(frame->terrainStageBudget);
        frame->terrainStageBudget = std::move(reservation);
        frame->terrainStagePackets.reserve(capacity);
        frame->terrainStageReceipts.reserve(capacity);
    }
    uint64_t ordinalCapacity = 1;
    const auto requiredOrdinals = static_cast<uint64_t>(frame->terrainStageReceipts.capacity()) * 2;
    while (ordinalCapacity < requiredOrdinals) ordinalCapacity *= 2;
    if (frame->terrainStageOrdinals.size() < ordinalCapacity) {
        auto budget = mcdr::oit_budget::reserve(
            ordinalCapacity * sizeof(std::pair<uint64_t, uint32_t>) + 256, frame->input.totalBudgetBytes);
        if (!budget) return overBudget;
        std::vector<std::pair<uint64_t, uint32_t>> ordinals(static_cast<size_t>(ordinalCapacity));
        for (const auto& existing : frame->terrainStageOrdinals) {
            if (!existing.first) continue;
            auto* slot = findTerrainSourceOrdinal(ordinals, existing.first);
            if (!slot) return recordingFailed;
            *slot = existing;
        }
        frame->terrainStageOrdinals.swap(ordinals);
        frame->terrainStageOrdinalBudget.swap(budget);
    }
    const auto token = nextToken.fetch_add(1);
    if (!token) return recordingFailed;
    frame->terrainStageDraw = TerrainStageDraw{token, input,
        static_cast<uint32_t>(frame->terrainDrawReceipts.size())};
    *output = reinterpret_cast<void*>(token);
    return success;
}
int prepareTerrainStageDrawReceipt(void* opaqueContext, const void* vertex, uint64_t vertexOffset) {
    const auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->oitFrameState) return success;
    auto frame = std::static_pointer_cast<Frame>(context->oitFrameState);
    if (!frame->terrainStageDraw) return success;
    auto& draw = *frame->terrainStageDraw;
    if (frame->phase != Phase::Recording || draw.prepared || draw.receiptId != 0 || context->submissionFailed.load() ||
        frame->recordingIdentity != context->recordingIdentity || frame->serial != context->fenceValue + 1 ||
        reinterpret_cast<uintptr_t>(context->currentPipeline) != reinterpret_cast<uintptr_t>(draw.input.pipeline) ||
        frame->terrainStageReceipts.size() >= frame->terrainStageReceipts.capacity()) return invalid;
    if (frame->bound && (draw.input.stage != MCDR_OIT_EXECUTION_STAGE_ACCUMULATE ||
        frame->boundTerrainVertex != vertex || frame->boundTerrainOffset != vertexOffset ||
        frame->boundTerrainIdentity.producerScopeSerial != draw.input.producerScopeSerial)) return invalid;
    draw.prepared = true;
    return success;
}
int confirmTerrainStageDrawReceipt(void* opaqueContext, std::shared_ptr<const void> replayPacket,
    mcdr::terrain_tile::PacketPreparationV2 prepareReplay, PacketContentStatus content) {
    const auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->oitFrameState) return success;
    auto frame = std::static_pointer_cast<Frame>(context->oitFrameState);
    if (!frame->terrainStageDraw) return success;
    auto& draw = *frame->terrainStageDraw;
    if (!draw.prepared || draw.receiptId != 0 || frame->phase != Phase::Recording ||
        !context->currentPipeline || reinterpret_cast<uintptr_t>(context->currentPipeline) !=
            reinterpret_cast<uintptr_t>(draw.input.pipeline) ||
        (draw.packet.batch && !draw.packet.validated) ||
        context->submissionFailed.load() || frame->recordingIdentity != context->recordingIdentity ||
        frame->serial != context->fenceValue + 1 ||
        frame->terrainStageReceipts.size() >= frame->terrainStageReceipts.capacity() ||
        frame->terrainStagePackets.size() >= frame->terrainStagePackets.capacity()) return recordingFailed;
    auto* sourceOrdinal = findTerrainSourceOrdinal(frame->terrainStageOrdinals, draw.input.sourceDrawId);
    if (!sourceOrdinal || sourceOrdinal->second == UINT32_MAX) return recordingFailed;
    const bool historyCapture = frame->bound && frame->boundTerrainVertex;
    if (frame->terrainDrawReceipts.size() != static_cast<size_t>(draw.terrainReceiptCountAtBegin) +
            (historyCapture ? 1u : 0u))
        return reject(recordingFailed, "Terrain stage history receipt count differs");
    if (historyCapture) {
        const auto ordinal = draw.terrainReceiptCountAtBegin;
        const auto& terrainReceipt = frame->terrainDrawReceipts[ordinal];
        if (draw.input.stage != MCDR_OIT_EXECUTION_STAGE_ACCUMULATE ||
            terrainReceipt.value.frameSerial != frame->serial ||
            terrainReceipt.value.producerScopeSerial != draw.input.producerScopeSerial ||
            terrainReceipt.value.producerKind != MCDR_OIT_SOURCE_TERRAIN ||
            !sameIdentity(terrainReceipt.value.identity, draw.input.identity) ||
            ordinal >= frame->terrainDrawPackets.size() || !frame->terrainDrawPackets[ordinal].confirmed ||
            frame->terrainDrawPackets[ordinal].packet != terrainReceipt.packet)
            return reject(recordingFailed, "Terrain stage history receipt differs");
        auto& history = frame->terrainDrawPackets[ordinal];
        if (history.stagePacketIndex != UINT32_MAX || (replayPacket && replayPacket != history.packet))
            return reject(recordingFailed, "Terrain history packet already linked");
        history.stagePacketIndex = static_cast<uint32_t>(frame->terrainStagePackets.size());
    }
    const auto ordinal = sourceOrdinal->second;
    const uint64_t receiptId = nextToken.fetch_add(1);
    if (!receiptId) return recordingFailed;
    mcdr_oit_terrain_stage_receipt_v2 receipt{};
    receipt.schemaVersion = 2;
    receipt.structSize = sizeof(receipt);
    receipt.stage = draw.input.stage;
    receipt.producerKind = MCDR_OIT_SOURCE_TERRAIN;
    const bool replayableContent = draw.packet.validated && replayPacket && prepareReplay &&
        content == PacketContentStatus::Frozen;
    receipt.contentState = replayableContent ? MCDR_OIT_EXECUTION_CONTENT_ORIGINAL :
        MCDR_OIT_EXECUTION_CONTENT_UNSUPPORTED;
    receipt.receiptId = receiptId;
    receipt.nativeSequence = frame->nextNativeCommandSequence++;
    receipt.frameSerial = frame->serial;
    receipt.producerId = draw.input.producerId;
    receipt.producerScopeSerial = draw.input.producerScopeSerial;
    receipt.sourcePassId = draw.input.sourcePassId;
    receipt.sourceDrawId = draw.input.sourceDrawId;
    receipt.parentSourceDrawId = draw.input.parentSourceDrawId;
    receipt.identity = frame->input.identity;
    receipt.pipelineIdentity = reinterpret_cast<uintptr_t>(context->currentPipeline);
    draw.packet.programGeneration = frame->input.programGeneration;
    draw.packet.resourceVersion = frame->input.identity.resourceVersion;
    draw.packet.shaderGeneration = context->currentPipeline->shaderGeneration;
    draw.packet.executionV2Ordinal = ordinal;
    if (replayableContent) {
        draw.packet.replayPacket = std::move(replayPacket);
        draw.packet.prepareReplay = prepareReplay;
        const auto attachment = [&](const Dx12Object* object) -> LaneTexture {
            if (!object) return {};
            const auto range = std::ranges::find_if(context->activeAttachmentRanges, [object](const auto& entry) {
                return entry.object == object && entry.subresourceCount == 1;
            });
            return range == context->activeAttachmentRanges.end()
                ? LaneTexture{} : LaneTexture{object, range->firstSubresource};
        };
        draw.packet.depth = attachment(context->activeDepthTarget);
        for (size_t index = 0; index < context->activeColorTargets.size() && index < draw.packet.colors.size(); ++index)
            draw.packet.colors[index] = attachment(context->activeColorTargets[index]);
        const auto slots = terrainTileLaneBindingSlots(context->currentPipeline->terrainTileRoute.get());
        for (size_t index = 0; index < slots.size(); ++index) {
            if (slots[index] >= context->graphicsBindings.size()) continue;
            const auto& binding = context->graphicsBindings[slots[index]];
            const auto* view = binding.type == 1 ? binding.view : nullptr;
            if (view && view->hasSrvDesc && view->srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
                draw.packet.laneReads[index] = {view->sourceTexture, view->srvDesc.Texture2D.MostDetailedMip};
        }
    }
    frame->terrainStageReceipts.push_back(receipt);
    frame->terrainStagePackets.push_back(std::move(draw.packet));
    sourceOrdinal->first = receipt.sourceDrawId;
    ++sourceOrdinal->second;
    draw.receiptId = receiptId;
    return success;
}
int finishTerrainStageDraw(void* opaqueContext, void* opaqueFrame, void* opaqueToken,
    mcdr_oit_terrain_stage_receipt_v2* output) {
    if (output) *output = {};
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findFrame(opaqueContext, opaqueFrame);
    if (!frame || !output || !frame->terrainStageDraw ||
        frame->terrainStageDraw->token != reinterpret_cast<uintptr_t>(opaqueToken)) return stale;
    const auto receiptId = frame->terrainStageDraw->receiptId;
    if (!receiptId || frame->terrainStageReceipts.empty() ||
        frame->terrainStageReceipts.back().receiptId != receiptId) return busy;
    *output = frame->terrainStageReceipts.back();
    frame->terrainStageDraw.reset();
    return success;
}
int abortTerrainStageDraw(void* opaqueContext, void* opaqueFrame, void* opaqueToken) {
    auto frame = findFrame(opaqueContext, opaqueFrame, FrameAccess::Retirement);
    const auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!frame || !context || frame->phase != Phase::Recording ||
        frame->recordingIdentity != context->recordingIdentity || !frame->terrainStageDraw ||
        frame->terrainStageDraw->token != reinterpret_cast<uintptr_t>(opaqueToken)) return stale;
    frame->terrainStageDraw.reset();
    return success;
}
int queryTerrainStageReceipt(void* opaqueContext, void* opaqueFrame, uint64_t receiptId,
    mcdr_oit_terrain_stage_receipt_v2* output) {
    if (output) *output = {};
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findFrame(opaqueContext, opaqueFrame);
    if (!frame || !output || receiptId == 0) return invalid;
    const auto found = std::lower_bound(frame->terrainStageReceipts.begin(), frame->terrainStageReceipts.end(),
        receiptId, [](const auto& receipt, uint64_t identity) { return receipt.receiptId < identity; });
    if (found == frame->terrainStageReceipts.end() || found->receiptId != receiptId) return stale;
    *output = *found;
    return success;
}
int registerTerrainReplayReadSet(void* opaqueContext, void* opaqueFrame, void* token,
    void* terrainBatch, void* vertex, uint64_t vertexOffset, const mcdr_oit_terrain_replay_read_set_v2& input) {
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto frame = findFrame(opaqueContext, opaqueFrame);
    if (!frame || frame->phase != Phase::Recording || input.schemaVersion != 2 ||
        input.structSize != sizeof(input) || input.flags != 0 || input.reserved0 != 0 ||
        input.reserved1 != 0 || input.reserved2 != 0 || input.receiptId != 0 || !terrainBatch || !vertex ||
        input.frameSerial != frame->serial || input.programGeneration != frame->input.programGeneration ||
        input.resourceVersion != frame->input.identity.resourceVersion || input.geometryRangeCount == 0 ||
        input.geometryRangeCount > 65535u * 128u || input.geometryRangeCount % 2 != 0 ||
        input.uploadBindingCount > 64 || !input.geometryRanges ||
        (input.uploadBindingCount != 0 && !input.uploadBindings)) return invalid;
    if (!frame->terrainStageDraw || frame->terrainStageDraw->token != reinterpret_cast<uintptr_t>(token) ||
        frame->terrainStageDraw->input.producerScopeSerial != input.producerScopeSerial) return stale;
    auto& draw = *frame->terrainStageDraw;
    if (draw.prepared || draw.receiptId != 0 || draw.packet.batch) return invalid;
    TerrainStagePacket packet;
    packet.batch = terrainBatch;
    packet.vertex = vertex;
    packet.vertexOffset = vertexOffset;
    const auto bytes = input.geometryRangeCount * (sizeof(mcdr_oit_terrain_replay_range_v2) + sizeof(std::shared_ptr<void>)) +
        input.uploadBindingCount * (sizeof(mcdr_oit_terrain_upload_read_v2) + sizeof(std::shared_ptr<void>));
    packet.budget = mcdr::oit_budget::reserve(bytes, frame->input.totalBudgetBytes);
    if (!packet.budget) return overBudget;
    packet.geometry.reserve(input.geometryRangeCount);
    packet.uploads.reserve(input.uploadBindingCount);
    packet.objectLeases.reserve(static_cast<size_t>(input.geometryRangeCount) + input.uploadBindingCount);
    for (uint32_t index = 0; index < input.geometryRangeCount; ++index) {
        const auto& range = input.geometryRanges[index];
        if (range.schemaVersion != 2 || range.structSize != sizeof(range) || range.commandIndex != index / 2 ||
            range.role != index % 2 + 1 || !range.object || range.generation == 0 ||
            range.ownerIdentity == 0 || range.byteLength == 0 || range.byteOffset > UINT64_MAX - range.byteLength)
            return invalid;
        const auto* object = static_cast<Dx12Object*>(range.object);
        if (object->kind != Dx12Object::Kind::Buffer || !object->resource || object->size < 0 ||
            range.byteOffset > static_cast<uint64_t>(object->size) ||
            range.byteLength > static_cast<uint64_t>(object->size) - range.byteOffset)
            return range.role == 1 ? MCDR_OIT_READSET_GEOMETRY_UNAVAILABLE : MCDR_OIT_READSET_MESHLET_UNAVAILABLE;
        if (range.role == 1 && (range.object != vertex || range.byteOffset < vertexOffset))
            return MCDR_OIT_READSET_GEOMETRY_UNAVAILABLE;
        if (range.role == 2 && ((range.byteOffset % 16) != 0 || (range.byteLength % 16) != 0))
            return MCDR_OIT_READSET_MESHLET_UNAVAILABLE;
        auto lease = retainObjectLease(range.object);
        if (!lease) return MCDR_OIT_READSET_OWNER_UNAVAILABLE;
        packet.objectLeases.push_back(std::move(lease));
        packet.geometry.push_back(range);
    }
    auto sortBudget = mcdr::oit_budget::reserve(
        packet.geometry.size() * sizeof(mcdr_oit_terrain_replay_range_v2), frame->input.totalBudgetBytes);
    if (!sortBudget) return overBudget;
    auto ownerOrder = packet.geometry;
    std::sort(ownerOrder.begin(), ownerOrder.end(), [](const auto& first, const auto& second) {
        return std::tuple(first.role, first.ownerIdentity, first.generation, reinterpret_cast<uintptr_t>(first.object),
                   first.byteOffset, first.byteLength) <
            std::tuple(second.role, second.ownerIdentity, second.generation, reinterpret_cast<uintptr_t>(second.object),
                   second.byteOffset, second.byteLength);
    });
    for (size_t index = 1; index < ownerOrder.size(); ++index) {
        const auto& previous = ownerOrder[index - 1];
        const auto& current = ownerOrder[index];
        if (previous.role != current.role || previous.ownerIdentity != current.ownerIdentity) continue;
        if (previous.object != current.object || (current.role == 2 && previous.generation != current.generation))
            return MCDR_OIT_READSET_OWNER_UNAVAILABLE;
        if (current.role == 1 && previous.generation == current.generation &&
            (previous.byteOffset != current.byteOffset || previous.byteLength != current.byteLength))
            return MCDR_OIT_READSET_OWNER_UNAVAILABLE;
        if (previous.generation == current.generation && previous.byteOffset == current.byteOffset &&
            previous.byteLength != current.byteLength) return MCDR_OIT_READSET_OWNER_UNAVAILABLE;
    }
    for (uint32_t index = 0; index < input.uploadBindingCount; ++index) {
        const auto& range = input.uploadBindings[index];
        if (range.schemaVersion != 2 || range.structSize != sizeof(range) || range.reserved != 0 ||
            !range.object || range.contentVersion == 0 || range.ownerIdentity == 0 || range.byteLength == 0 ||
            range.resourceVersion != input.resourceVersion || range.byteOffset > UINT64_MAX - range.byteLength)
            return invalid;
        const auto* object = static_cast<Dx12Object*>(range.object);
        if (object->kind != Dx12Object::Kind::Buffer || !object->resource || object->heapType != D3D12_HEAP_TYPE_UPLOAD ||
            object->size < 0 || range.byteOffset > static_cast<uint64_t>(object->size) ||
            range.byteLength > static_cast<uint64_t>(object->size) - range.byteOffset) return MCDR_OIT_READSET_UPLOAD_UNAVAILABLE;
        auto lease = retainObjectLease(range.object);
        if (!lease) return MCDR_OIT_READSET_OWNER_UNAVAILABLE;
        packet.objectLeases.push_back(std::move(lease));
        packet.uploads.push_back(range);
    }
    std::sort(packet.uploads.begin(), packet.uploads.end(), [](const auto& first, const auto& second) {
        return first.role < second.role;
    });
    for (size_t index = 1; index < packet.uploads.size(); ++index) {
        if (packet.uploads[index - 1].role == packet.uploads[index].role)
            return MCDR_OIT_READSET_UPLOAD_UNAVAILABLE;
    }
    draw.packet = std::move(packet);
    return success;
}
int cancelTerrainReplayReadSet(void* opaqueContext, void* opaqueFrame, void* token) {
    auto frame = findFrame(opaqueContext, opaqueFrame, FrameAccess::Retirement);
    const auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!frame || !context || frame->recordingIdentity != context->recordingIdentity ||
        !frame->terrainStageDraw || frame->terrainStageDraw->token != reinterpret_cast<uintptr_t>(token) ||
        frame->terrainStageDraw->prepared || frame->terrainStageDraw->receiptId != 0) return stale;
    frame->terrainStageDraw->packet = {};
    return success;
}
int validateTerrainStageReads(void* opaqueContext, const void* terrainBatch, const void* vertex, uint64_t vertexOffset,
    const void* atlas, uint64_t atlasOffset, uint64_t atlasLength, uint32_t sectionCount,
    const void* globals, uint64_t globalsOffset, uint64_t globalsLength,
    const void* candidates, uint64_t candidatesOffset, uint64_t candidatesLength,
    std::shared_ptr<const void> batchLease, TerrainReplayDraw* replayDraw) {
    if (replayDraw) *replayDraw = {};
    const auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->oitFrameState) return success;
    auto frame = std::static_pointer_cast<Frame>(context->oitFrameState);
    if (!frame->terrainStageDraw || !frame->terrainStageDraw->packet.batch) return success;
    auto& packet = frame->terrainStageDraw->packet;
    if (packet.batch != terrainBatch || packet.vertex != vertex || packet.vertexOffset != vertexOffset ||
        frame->recordingIdentity != context->recordingIdentity) return stale;
    if (packet.geometry.size() != static_cast<size_t>(sectionCount) * 2) return MCDR_OIT_READSET_GEOMETRY_UNAVAILABLE;
    const auto* candidateObject = static_cast<const Dx12Object*>(candidates);
    const uint64_t requiredCandidates = static_cast<uint64_t>(sectionCount) * 40;
    if (!candidateObject || candidateObject->kind != Dx12Object::Kind::Buffer || !candidateObject->resource ||
        candidateObject->heapType != D3D12_HEAP_TYPE_UPLOAD || candidateObject->size < 0 ||
        candidatesLength < requiredCandidates || candidatesOffset > static_cast<uint64_t>(candidateObject->size) ||
        candidatesLength > static_cast<uint64_t>(candidateObject->size) - candidatesOffset)
        return MCDR_OIT_READSET_UPLOAD_UNAVAILABLE;
    void* candidateBytes = nullptr;
    const D3D12_RANGE candidateRead{static_cast<SIZE_T>(candidatesOffset),
        static_cast<SIZE_T>(candidatesOffset + requiredCandidates)};
    if (FAILED(candidateObject->resource->Map(0, &candidateRead, &candidateBytes)) || !candidateBytes)
        return MCDR_OIT_READSET_UPLOAD_UNAVAILABLE;
    struct CandidateMapping {
        ID3D12Resource* resource;
        ~CandidateMapping() {
            const D3D12_RANGE noWrites{0, 0};
            resource->Unmap(0, &noWrites);
        }
    } candidateMapping{candidateObject->resource.Get()};
    for (uint32_t index = 0; index < sectionCount; ++index) {
        uint32_t selection[4]{};
        std::memcpy(selection, static_cast<const std::byte*>(candidateBytes) + candidatesOffset +
            static_cast<uint64_t>(index) * 40, sizeof(selection));
        const auto& geometry = packet.geometry[static_cast<size_t>(index) * 2];
        const auto& meshlets = packet.geometry[static_cast<size_t>(index) * 2 + 1];
        const uint64_t selectedVertexOffset = static_cast<uint64_t>(selection[0]) * 28;
        if (selectedVertexOffset > UINT64_MAX - vertexOffset ||
            geometry.byteOffset != vertexOffset + selectedVertexOffset)
            return MCDR_OIT_READSET_GEOMETRY_UNAVAILABLE;
        const uint64_t selectedDescriptorOffset = static_cast<uint64_t>(selection[2]) * 16;
        if (selectedDescriptorOffset > UINT64_MAX - atlasOffset ||
            meshlets.byteOffset != atlasOffset + selectedDescriptorOffset ||
            meshlets.byteLength != static_cast<uint64_t>(selection[3]) * 16)
            return MCDR_OIT_READSET_MESHLET_UNAVAILABLE;
    }
    bool geometryFound = false, meshletFound = false;
    uint64_t vertexBytes = 0, primitiveCount = 0;
    for (const auto& range : packet.geometry) {
        if (range.role == 1) {
            if (range.object != vertex || range.byteOffset < vertexOffset || range.byteLength % (28u * 4u))
                return MCDR_OIT_READSET_GEOMETRY_UNAVAILABLE;
            vertexBytes = std::max(vertexBytes, range.byteOffset - vertexOffset + range.byteLength);
            primitiveCount += range.byteLength / (28u * 2u);
            geometryFound = true;
        } else {
            if (range.object != atlas || range.byteOffset < atlasOffset ||
                range.byteOffset - atlasOffset > atlasLength ||
                range.byteLength > atlasLength - (range.byteOffset - atlasOffset)) return MCDR_OIT_READSET_MESHLET_UNAVAILABLE;
            meshletFound = true;
        }
    }
    for (const auto& range : packet.uploads) {
        if (range.role == MCDR_OIT_UPLOAD_TERRAIN_GLOBALS || range.role == MCDR_OIT_UPLOAD_TERRAIN_SECTIONS) {
            const bool isGlobals = range.role == MCDR_OIT_UPLOAD_TERRAIN_GLOBALS;
            if (range.object != (isGlobals ? globals : candidates) ||
                range.byteOffset != (isGlobals ? globalsOffset : candidatesOffset) ||
                range.byteLength != (isGlobals ? globalsLength : candidatesLength))
                return MCDR_OIT_READSET_UPLOAD_UNAVAILABLE;
            continue;
        }
        if (range.role >= context->graphicsBindings.size()) return MCDR_OIT_READSET_UPLOAD_UNAVAILABLE;
        const auto& binding = context->graphicsBindings[range.role];
        if (binding.buffer != range.object || binding.offset < 0 || binding.length < 0 ||
            static_cast<uint64_t>(binding.offset) != range.byteOffset ||
            static_cast<uint64_t>(binding.length) != range.byteLength) return MCDR_OIT_READSET_UPLOAD_UNAVAILABLE;
    }
    if (!geometryFound) return MCDR_OIT_READSET_GEOMETRY_UNAVAILABLE;
    if (!meshletFound) return MCDR_OIT_READSET_MESHLET_UNAVAILABLE;
    packet.batchLease = std::move(batchLease);
    packet.validated = true;
    if (replayDraw && primitiveCount != 0 && primitiveCount <= UINT32_MAX) {
        *replayDraw = {frame->input.identity, frame->serial, frame->input.programGeneration,
            vertexBytes, sectionCount, static_cast<uint32_t>(primitiveCount),
            frame->terrainStageDraw->input.stage - MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS, frame->input.totalBudgetBytes};
    }
    return success;
}
int appendExecutionEvent(void* opaqueContext, void* opaqueStream, const mcdr_oit_execution_event_v1& input,
    uint64_t* outputEventId) {
    if (outputEventId) *outputEventId = 0;
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto stream = findExecutionStream(opaqueContext, opaqueStream);
    if (stream && stream->state == MCDR_OIT_EXECUTION_RECORDING_FAILED) return recordingFailed;
    if (!stream || !outputEventId || stream->state != MCDR_OIT_EXECUTION_RECORDING ||
        input.schemaVersion != 1 || input.structSize != sizeof(input) || input.sequence != stream->nextSequence ||
        input.kind < MCDR_OIT_EXECUTION_SCOPE_BEGIN || input.kind > MCDR_OIT_EXECUTION_COMPOSITE_RECIPE ||
        input.stage > MCDR_OIT_EXECUTION_STAGE_TERMINAL || input.producerKind > MCDR_OIT_SOURCE_CUSTOM_TRANSLUCENT ||
        input.producerId == 0 || input.producerScopeSerial == 0 || input.sourcePassId == 0 ||
        input.programGeneration != stream->input.programGeneration ||
        input.resourceCount > stream->input.maxResourceReferences ||
        (input.resourceCount != 0 && input.resources == nullptr) || input.reserved != 0 ||
        input.reservedTail[0] != 0 || input.reservedTail[1] != 0)
        return invalid;
    if (stream->events.size() >= stream->input.maxEvents ||
        input.resourceCount > stream->input.maxResourceReferences - stream->resourceCount) return overBudget;
    ExecutionEvent event;
    event.eventId = stream->nextEventId;
    event.value = input;
    if (input.resourceCount != 0) {
        event.resources.assign(input.resources, input.resources + input.resourceCount);
        for (const auto& resource : event.resources) {
            if (!validExecutionResource(resource) || resource.resourceVersion != stream->input.identity.resourceVersion)
                return invalid;
            auto lease = retainObjectLease(resource.object);
            if (!lease) return invalid;
            event.resourceLeases.push_back(std::move(lease));
        }
        event.value.resources = event.resources.data();
    } else {
        event.value.resources = nullptr;
    }
    bool terrainReceipt = false;
    bool unsupportedTerrainContent = false;
    uint64_t terrainReceiptId = 0;
    if (input.kind == MCDR_OIT_EXECUTION_DRAW_TERRAIN) {
        auto* scope = findScope(*stream, input.producerId, input.producerScopeSerial, input.sourcePassId,
                                input.producerKind, input.stage);
        if (!scope || !scope->open) return stale;
        if (!input.drawReceiptId || !input.sourceDrawId || !input.drawGroupId || stream->usedDrawReceipts.contains(input.drawReceiptId))
            return stale;
        auto frame = std::static_pointer_cast<Frame>(static_cast<CommandContext*>(opaqueContext)->oitFrameState);
        const auto found = std::find_if(frame->terrainDrawReceipts.begin(), frame->terrainDrawReceipts.end(),
            [&](const auto& receipt) { return receipt.value.receiptId == input.drawReceiptId; });
        if (found == frame->terrainDrawReceipts.end()) return stale;
        if (found->value.frameSerial != stream->input.frameSerial ||
            found->value.producerScopeSerial != input.producerScopeSerial ||
            found->value.producerId != input.producerId || found->value.producerKind != input.producerKind ||
            found->value.shaderGeneration != input.shaderGeneration ||
            found->value.programGeneration != input.programGeneration ||
            found->value.nativeSequence != stream->lastNativeDrawSequence + 1 ||
            input.stage != MCDR_OIT_EXECUTION_STAGE_ACCUMULATE) {
            return stale;
        }
        terrainReceipt = true;
        terrainReceiptId = input.drawReceiptId;
        event.packet = found->packet;
        unsupportedTerrainContent = found->value.contentState == MCDR_OIT_EXECUTION_CONTENT_UNSUPPORTED;
    }
    if (input.kind != MCDR_OIT_EXECUTION_EMPTY_SCOPE && input.flags != 0) return invalid;
    if (input.kind != MCDR_OIT_EXECUTION_SCOPE_BEGIN && input.kind != MCDR_OIT_EXECUTION_EMPTY_SCOPE) {
        const auto* scope = findScope(*stream, input.producerId, input.producerScopeSerial, input.sourcePassId,
                                      input.producerKind, input.stage);
        if (!scope || !scope->open) return stale;
    }
    if (input.kind == MCDR_OIT_EXECUTION_SCOPE_BEGIN) {
        if (stream->scopes.size() >= stream->input.maxScopes ||
            findScope(*stream, input.producerId, input.producerScopeSerial, input.sourcePassId,
                      input.producerKind, input.stage)) return stale;
        ExecutionScopeState scope;
        scope.value.schemaVersion = 1; scope.value.structSize = sizeof(scope.value);
        scope.value.producerId = input.producerId; scope.value.producerScopeSerial = input.producerScopeSerial;
        scope.value.sourcePassId = input.sourcePassId; scope.value.producerKind = input.producerKind;
        scope.value.stage = input.stage; scope.open = true;
        stream->scopes.push_back(scope); ++stream->openScopeCount;
    } else if (input.kind == MCDR_OIT_EXECUTION_SCOPE_END) {
        auto* scope = findScope(*stream, input.producerId, input.producerScopeSerial, input.sourcePassId,
                                input.producerKind, input.stage);
        if (!scope || !scope->open) return stale;
        scope->open = false; --stream->openScopeCount;
    } else if (input.kind == MCDR_OIT_EXECUTION_EMPTY_SCOPE) {
        if ((input.flags & MCDR_OIT_EXECUTION_SCOPE_EXPLICIT_EMPTY) == 0 ||
            (input.flags & ~MCDR_OIT_EXECUTION_SCOPE_EXPLICIT_EMPTY) != 0 ||
            stream->scopes.size() >= stream->input.maxScopes ||
            findScope(*stream, input.producerId, input.producerScopeSerial, input.sourcePassId,
                      input.producerKind, input.stage))
            return invalid;
        ExecutionScopeState scope;
        scope.value.schemaVersion = 1; scope.value.structSize = sizeof(scope.value);
        scope.value.producerId = input.producerId; scope.value.producerScopeSerial = input.producerScopeSerial;
        scope.value.sourcePassId = input.sourcePassId; scope.value.producerKind = input.producerKind;
        scope.value.stage = input.stage; scope.value.eventCount = 1; scope.value.flags = MCDR_OIT_EXECUTION_SCOPE_EXPLICIT_EMPTY;
        stream->scopes.push_back(scope);
    }
    if (input.kind != MCDR_OIT_EXECUTION_EMPTY_SCOPE) {
        auto* scope = findScope(*stream, input.producerId, input.producerScopeSerial, input.sourcePassId,
                                input.producerKind, input.stage);
        if (scope) {
            ++scope->value.eventCount;
            if (input.kind == MCDR_OIT_EXECUTION_DRAW_TERRAIN) ++scope->value.nativeDrawCount;
        }
    }
    try {
        stream->events.push_back(std::move(event));
    } catch (...) {
        stream->state = MCDR_OIT_EXECUTION_RECORDING_FAILED;
        stream->unavailableReason = MCDR_OIT_EXECUTION_UNAVAILABLE_RESOURCE_CLOSURE;
        return recordingFailed;
    }
    if (terrainReceipt) {
        try {
            stream->usedDrawReceipts.insert(terrainReceiptId);
        } catch (...) {
            stream->state = MCDR_OIT_EXECUTION_RECORDING_FAILED;
            return recordingFailed;
        }
        ++stream->appendedDrawCount;
        ++stream->lastNativeDrawSequence;
        if (unsupportedTerrainContent)
            stream->unavailableReason = MCDR_OIT_EXECUTION_UNAVAILABLE_UNSUPPORTED_CONTENT;
    }
    stream->resourceCount += input.resourceCount;
    ++stream->nextEventId;
    ++stream->nextSequence;
    *outputEventId = stream->events.back().eventId;
    return success;
}
int sealExecutionStream(void* opaqueContext, void* opaqueStream, const mcdr_oit_execution_manifest_v1& manifest,
    void** output, mcdr_oit_execution_seal_result_v1* result) {
    if (output) *output = nullptr;
    if (result) std::memset(result, 0, sizeof(*result));
    if (const auto status = recordingStatus(opaqueContext); status != success) return status;
    auto stream = findExecutionStream(opaqueContext, opaqueStream);
    auto operationFrame = std::static_pointer_cast<Frame>(static_cast<CommandContext*>(opaqueContext)->oitFrameState);
    if (operationFrame && operationFrame->activeExecutionOperation) {
        const bool failed = cancelActiveExecutionOperations(*operationFrame);
        if (stream && !failed) {
            stream->state = MCDR_OIT_EXECUTION_UNAVAILABLE;
            stream->unavailableReason = MCDR_OIT_EXECUTION_UNAVAILABLE_OPEN_SCOPE;
        }
        return failed ? recordingFailed : busy;
    }
    if (!stream || !output || !result || stream->state != MCDR_OIT_EXECUTION_RECORDING ||
        manifest.schemaVersion != 1 || manifest.structSize != sizeof(manifest) ||
        manifest.eventCount != stream->events.size() || manifest.scopeCount != stream->scopes.size() ||
        manifest.lastSequence != stream->nextSequence - 1 || manifest.flags != 0 ||
        manifest.reserved[0] != 0 || manifest.reserved[1] != 0 ||
        (manifest.scopeCount != 0 && manifest.scopes == nullptr))
        return invalid;
    result->schemaVersion = 1;
    result->structSize = sizeof(*result);
    result->firstEventId = stream->events.empty() ? 0 : stream->events.front().eventId;
    result->expected = static_cast<uint32_t>(stream->events.size());
    result->observed = manifest.eventCount;
    for (uint32_t index = 0; index < manifest.scopeCount; ++index) {
        const auto& supplied = manifest.scopes[index];
        if (supplied.schemaVersion != 1 || supplied.structSize != sizeof(supplied) ||
            supplied.reserved != 0 || supplied.reservedTail != 0) return invalid;
        const auto* expected = findScope(*stream, supplied.producerId, supplied.producerScopeSerial,
                                         supplied.sourcePassId, supplied.producerKind, supplied.stage);
        if (!expected || expected->value.eventCount != supplied.eventCount ||
            expected->value.nativeDrawCount != supplied.nativeDrawCount || expected->value.flags != supplied.flags)
            return invalid;
        if (supplied.eventCount == 2 && supplied.nativeDrawCount == 0 && supplied.flags == 0) return invalid;
        for (uint32_t priorIndex = 0; priorIndex < index; ++priorIndex) {
            const auto& prior = manifest.scopes[priorIndex];
            if (prior.producerId == supplied.producerId && prior.producerScopeSerial == supplied.producerScopeSerial &&
                prior.sourcePassId == supplied.sourcePassId && prior.producerKind == supplied.producerKind &&
                prior.stage == supplied.stage) return invalid;
        }
    }
    if (stream->openScopeCount != 0) {
        stream->state = MCDR_OIT_EXECUTION_UNAVAILABLE;
        stream->unavailableReason = MCDR_OIT_EXECUTION_UNAVAILABLE_OPEN_SCOPE;
        result->state = stream->state; result->unavailableReason = stream->unavailableReason;
        return unsupported;
    }
    auto frame = std::static_pointer_cast<Frame>(static_cast<CommandContext*>(opaqueContext)->oitFrameState);
    if (frame && (frame->bound || frame->terrainStageDraw || frame->pendingTerrainPacket >= 0 ||
        static_cast<CommandContext*>(opaqueContext)->inRenderPass)) return busy;
    if (!frame || manifest.nativeDrawCount != frame->terrainDrawReceipts.size() ||
        stream->usedDrawReceipts.size() != frame->terrainDrawReceipts.size()) {
        stream->state = MCDR_OIT_EXECUTION_UNAVAILABLE;
        stream->unavailableReason = MCDR_OIT_EXECUTION_UNAVAILABLE_MISSING_DRAW_RECEIPT;
        result->state = stream->state; result->unavailableReason = stream->unavailableReason;
        return unsupported;
    }
    for (const auto& receipt : frame->terrainDrawReceipts) {
        if (receipt.value.contentState == MCDR_OIT_EXECUTION_CONTENT_UNSUPPORTED) {
            stream->state = MCDR_OIT_EXECUTION_UNAVAILABLE;
            stream->unavailableReason = MCDR_OIT_EXECUTION_UNAVAILABLE_UNSUPPORTED_CONTENT;
            result->state = stream->state;
            result->unavailableReason = stream->unavailableReason;
            result->drawReceiptId = receipt.value.receiptId;
            return unsupported;
        }
    }
    for (const auto& event : stream->events) {
        if (event.value.kind > MCDR_OIT_EXECUTION_DRAW_TERRAIN) {
            stream->state = MCDR_OIT_EXECUTION_UNAVAILABLE;
            stream->unavailableReason = MCDR_OIT_EXECUTION_UNAVAILABLE_MISSING_NATIVE_ADAPTER;
            result->state = stream->state;
            result->unavailableReason = stream->unavailableReason;
            result->firstEventId = event.eventId;
            return unsupported;
        }
    }
    stream->state = MCDR_OIT_EXECUTION_UNAVAILABLE;
    stream->unavailableReason = MCDR_OIT_EXECUTION_UNAVAILABLE_MISSING_EXECUTION_ADAPTERS;
    result->state = stream->state;
    result->unavailableReason = stream->unavailableReason;
    return unsupported;
}
mcdr::terrain_tile::Result prepareTerrainTiles(void* context, void* token,
    const mcdr::terrain_tile::Batch& batch) {
    using mcdr::terrain_tile::Result;
    auto frame = findFrame(context, token);
    if (!frame) return Result::StaleRecording;
    if (frame->phase != Phase::Sealed || frame->bound || static_cast<CommandContext*>(context)->inRenderPass)
        return Result::FrameNotSealed;
    for (const auto& packet : frame->terrainDrawPackets) {
        if (!packet.prepare || !packet.packet) return Result::UnsupportedContent;
        const auto result = packet.prepare(context, packet.packet, batch);
        if (result != Result::UnsupportedRoute) return result;
    }
    return Result::UnsupportedRoute;
}

mcdr::terrain_tile::Result prepareTerrainTilesV2(void* context, void* token,
    const mcdr::terrain_tile::BatchV2& batch) {
    const auto frame = findFrame(context, token);
    if (!frame) return mcdr::terrain_tile::Result::StaleRecording;
    if (frame->terrainDrawPackets.size() > UINT32_MAX)
        return mcdr::terrain_tile::Result::InvalidBatch;
    mcdr::terrain_tile::BatchV3 selected;
    selected.structSize = sizeof(selected);
    selected.packetCount = static_cast<uint32_t>(frame->terrainDrawPackets.size());
    selected.payload = batch;
    return prepareTerrainTilesV3(context, token, selected);
}

mcdr::terrain_tile::Result prepareTerrainTilesV3(void* context, void* token,
    const mcdr::terrain_tile::BatchV3& batch) {
    using mcdr::terrain_tile::Result;
    auto frame = findFrame(context, token);
    auto* commandContext = static_cast<CommandContext*>(context);
    if (!frame || !commandContext || frame->phase != Phase::Sealed || frame->bound || commandContext->inRenderPass)
        return Result::FrameNotSealed;
    if (batch.schemaVersion != 3 || batch.structSize != sizeof(batch) ||
        batch.packetCount == 0 || batch.packetCount != frame->terrainDrawPackets.size() ||
        batch.packetOrdinal >= batch.packetCount)
        return Result::InvalidBatch;
    const auto& payload = batch.payload;
    if (payload.schemaVersion != 2 || payload.structSize != sizeof(payload) || payload.flags != 0 ||
        payload.frameSerial != frame->serial || payload.worldEpoch != frame->input.identity.worldEpoch ||
        payload.resourceVersion != frame->input.identity.resourceVersion ||
        payload.programGeneration != frame->input.programGeneration)
        return Result::StaleRecording;
    if (payload.laneIndex >= mcdr::terrain_tile::laneCount || payload.residentTileCount == 0 ||
        payload.residentTileCount > mcdr::terrain_tile::maximumResidentTiles)
        return Result::InvalidBatch;
    const auto& history = frame->terrainDrawPackets[batch.packetOrdinal];
    if (history.stagePacketIndex >= frame->terrainStagePackets.size()) return Result::UnsupportedContent;
    auto& packet = frame->terrainStagePackets[history.stagePacketIndex];
    if (!packet.replayPacket || !packet.prepareReplay || packet.replayPacket != history.packet)
        return Result::UnsupportedContent;
    auto& plan = packet.replayPlans[payload.laneIndex];
    if (plan) return Result::InvalidBatch;
    const auto constants = uploadTileConstants(*frame, payload);
    if (!constants) return Result::UnsupportedContent;
    const auto prepared = packet.prepareReplay(context, packet.replayPacket, payload, *constants, &plan);
    return prepared == Result::Prepared ? Result::UnsupportedContent : prepared;
}

int unbindDraw(void* context, void* token) {
    if (const auto status=recordingStatus(context); status!=success) return status;
    auto frame=findFrame(context,token);
    if (!frame || frame->phase!=Phase::Recording || !frame->bound) return invalid;
    if (frame->pendingTerrainPacket >= 0) {
        static_cast<CommandContext*>(context)->submissionFailed.store(true, std::memory_order_release);
        if (frame->executionStream) frame->executionStream->state = MCDR_OIT_EXECUTION_RECORDING_FAILED;
        return recordingFailed;
    }
    auto& capture=static_cast<CommandContext*>(context)->graphicsCapture;
    const bool consumed=capture.lease && capture.remainingDraws==0;
    capture={}; restoreGraphicsCaptureRate(context);
    frame->bound=false;
    frame->boundTerrainVertex=nullptr;
    frame->boundTerrainIdentity={};
    if (!consumed) {
        const bool operationFailed = cancelActiveExecutionOperations(*frame);
        frame->phase=Phase::Aborted;
        if (operationFailed) return recordingFailed;
        return reject(invalid,"capture draw lease was not consumed by an actual native draw");
    }
    return success;
}
int sealFrame(void* context, void* token) {
    if (const auto status=recordingStatus(context); status!=success) return status;
    auto frame=findFrame(context,token);
    if (frame && frame->activeExecutionOperation)
        return cancelActiveExecutionOperations(*frame) ? recordingFailed : busy;
    if (!frame || frame->phase!=Phase::Recording || frame->bound || frame->terrainStageDraw ||
        static_cast<CommandContext*>(context)->inRenderPass) return invalid;
    if (!frame->executionStreamV2 || frame->executionStreamV2->state != MCDR_OIT_EXECUTION_V2_STATE_SEALED)
        cancelExecutionTextureCaptures(*frame);
    frame->phase=Phase::Sealed; return success;
}
int abortFrame(void* context, void* token) {
    auto frame=findFrame(context,token,FrameAccess::Retirement);
    if (!frame) return stale;
    const bool operationFailed = cancelActiveExecutionOperations(*frame);
    if (static_cast<CommandContext*>(context)->submissionFailed.load()) return unknownSubmission;
    cancelExecutionTextureCaptures(*frame);
    auto* commandContext=static_cast<CommandContext*>(context);
    if (commandContext->oitFrameState.get()==frame.get()) {
        commandContext->graphicsCapture={}; restoreGraphicsCaptureRate(context);
        commandContext->oitFrameState.reset();
    }
    if (frame->phase!=Phase::Resolved) frame->phase=Phase::Aborted;
    frame->bound=false; frame->terrainStageDraw.reset(); frame->metadata.clear();
    { std::lock_guard lock(registryMutex); registry.erase(frame->token); }
    return operationFailed ? recordingFailed : success;
}
int resolveRecords(void* opaqueContext, const mcdr_oit_resolve_v3& input,
    const std::array<mcdr::terrain_tile::BatchV2, mcdr::terrain_tile::laneCount>* tileBatches = nullptr) {
    if (const auto status=recordingStatus(opaqueContext); status!=success) return status;
    auto frame=findFrame(opaqueContext,input.frame);
    auto* context=static_cast<CommandContext*>(opaqueContext);
    if (!frame || frame->phase!=Phase::Sealed || context->inRenderPass || input.schemaVersion!=3 || input.structSize!=sizeof(input) ||
        (input.flags&~MCDR_OIT_RESOLVE_UNRECORDED_COVERAGE_UNAVAILABLE) || input.reserved || !sameIdentity(input.identity,frame->input.identity) ||
        !validTexture(input.currentTransparency,frame->input.width,frame->input.height) ||
        !validTexture(input.unrecordedCoverage,frame->input.width,frame->input.height) ||
        !validTexture(input.correctedTransparency,frame->input.width,frame->input.height) ||
        !validTexture(input.statusOutput,frame->input.width,frame->input.height)) return invalid;
    if (!vanilla::matchesFrozenFrame(context,&input.identity,frame->input.historyGeneration)) return stale;
    auto* current=input.currentTransparency.object; auto* coverage=input.unrecordedCoverage.object; auto* output=input.statusOutput.object;
    auto* corrected=input.correctedTransparency.object;
    const auto sourceDescription=current->resource->GetDesc(); const auto coverageDescription=coverage->resource->GetDesc();
    const auto outputDescription=output->resource->GetDesc();
    const auto correctedDescription=corrected->resource->GetDesc();
    if (output->dxgiFormat!=DXGI_FORMAT_R32_UINT || !(outputDescription.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        coverage->dxgiFormat!=DXGI_FORMAT_R8_UNORM || (coverageDescription.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        (sourceDescription.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || current->resource==output->resource ||
        coverage->resource==output->resource || current->dxgiFormat!=DXGI_FORMAT_R16G16B16A16_FLOAT ||
        corrected->dxgiFormat!=DXGI_FORMAT_R16G16B16A16_FLOAT || !(correctedDescription.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        corrected->resource==current->resource || corrected->resource==coverage->resource || corrected->resource==output->resource) return invalid;
    for (const auto* frozen : {frame->input.opaqueColor.object,frame->input.opaqueDepth.object,frame->input.opaqueMotion.object})
        if (output->resource==frozen->resource || corrected->resource==frozen->resource) return invalid;
    std::vector<std::shared_ptr<TextureAllocation>> resolveTextureBudgets;
    if (const auto result = reserveTextureAllocations({coverage,output,corrected},frame->input.totalBudgetBytes,
        resolveTextureBudgets); result != success) return result;
    frame->textureBudgets.insert(frame->textureBudgets.end(),resolveTextureBudgets.begin(),resolveTextureBudgets.end());
    for (auto* object : {current,coverage,output,corrected}) frame->frozenResources.push_back(retainObjectLease(object));
    auto handle=frame->descriptors->GetCPUDescriptorHandleForHeapStart(); handle.ptr+=static_cast<uint64_t>(deviceContext().srvInc)*3;
    D3D12_SHADER_RESOURCE_VIEW_DESC source{}; source.Format=current->dxgiFormat; source.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    source.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; source.Texture2D.MipLevels=1;
    deviceContext().device->CreateShaderResourceView(current->resource.Get(),&source,handle); handle.ptr+=deviceContext().srvInc;
    source.Format=DXGI_FORMAT_R8_UNORM;
    deviceContext().device->CreateShaderResourceView(coverage->resource.Get(),&source,handle); handle.ptr+=deviceContext().srvInc;
    handle=frame->descriptors->GetCPUDescriptorHandleForHeapStart(); handle.ptr+=static_cast<uint64_t>(deviceContext().srvInc)*14;
    D3D12_UNORDERED_ACCESS_VIEW_DESC destination{}; destination.Format=DXGI_FORMAT_R32_UINT; destination.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    deviceContext().device->CreateUnorderedAccessView(output->resource.Get(),nullptr,&destination,handle);
    handle.ptr+=deviceContext().srvInc;
    destination.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    deviceContext().device->CreateUnorderedAccessView(corrected->resource.Get(),nullptr,&destination,handle);
    std::shared_ptr<HistoryResources> history;
    {
        std::lock_guard lock(historyMutex);
        if (!context->oitHistoryState) context->oitHistoryState=std::make_shared<HistoryState>();
        history=std::static_pointer_cast<HistoryState>(context->oitHistoryState)->presented;
    }
    if (history) {
        const auto& previous=history->input;
        if (previous.identity.realFrameId!=frame->input.previousRealFrameId || !compatibleHistory(previous,frame->input)) history.reset();
    }
    if (history) {
        handle=frame->descriptors->GetCPUDescriptorHandleForHeapStart(); handle.ptr+=static_cast<uint64_t>(deviceContext().srvInc)*5;
        for (const auto* buffer : {&history->records,&history->pixels,&history->sources,&history->primitives}) {
            createRawView(buffer->object->resource.Get(),static_cast<uint64_t>(buffer->object->size),handle,false);
            handle.ptr+=deviceContext().srvInc;
        }
        handle=frame->descriptors->GetCPUDescriptorHandleForHeapStart(); handle.ptr+=static_cast<uint64_t>(deviceContext().srvInc)*10;
        createRawView(history->terrain.object ? history->terrain.object->resource.Get() : nullptr,
            std::max<uint64_t>(4,history->terrainRows.size()*sizeof(TerrainSection)),handle,false);
        context->recordingLeases[context->currentListSlot].push_back(history);
    }
    auto constants=constantsFor(*frame);
    constants.values[15]=(input.flags&MCDR_OIT_RESOLVE_UNRECORDED_COVERAGE_UNAVAILABLE) ? 1u : 0u;
    if (history) {
        constants.values[24]=history->input.width; constants.values[25]=history->input.height;
        constants.values[26]=history->input.maxRecords; constants.values[27]=history->input.maxRecordsPerPixel;
        constants.values[28]=static_cast<uint32_t>(history->sourceRows.size());
        constants.values[29]=history->primitiveCount; constants.values[31]=1;
    }
    std::memcpy(frame->constants.mapped+256,&constants,sizeof(constants));
    const int result=dispatchRecords(frame,&input,history,tileBatches);
    if (result!=success) { frame->phase=Phase::Aborted; return result; }
    frame->resources->primitiveCount = static_cast<uint32_t>(frame->primitiveRows.size());
    frame->phase=Phase::Resolved;
    {
        std::lock_guard lock(historyMutex);
        auto& pending=std::static_pointer_cast<HistoryState>(context->oitHistoryState)->pending;
        if (!pending.emplace(frame->input.identity.realFrameId,HistoryState::Pending{frame->resources,0}).second) return stale;
    }
    return success;
}

int resolveRecordsV4(void* opaqueContext, const mcdr_oit_resolve_v4& input) {
    if (input.base.schemaVersion != 3 || input.base.structSize != sizeof(input.base) || input.base.reserved ||
        input.reservedV4 || input.terrainBatchCount == 0 || !input.terrainBatches)
        return invalid;
    const auto frame = findFrame(opaqueContext, input.base.frame);
    if (!frame) return stale;
    const auto* packets = static_cast<const mcdr::terrain_tile::BatchV3*>(input.terrainBatches);
    for (uint32_t index = 0; index < input.terrainBatchCount; ++index) {
        const auto& packet = packets[index];
        if (packet.schemaVersion != 3 || packet.structSize != sizeof(packet) ||
            packet.packetCount == 0 || packet.packetCount != frame->terrainDrawPackets.size() ||
            packet.packetOrdinal >= packet.packetCount || !validTileResolveBatch(packet.payload, *frame)) return invalid;
    }
    return unsupported;
}

int resolveRecords(void* opaqueContext, const mcdr_oit_resolve_v3& input) {
    return resolveRecords(opaqueContext, input, nullptr);
}
int queuePresented(void* opaqueContext, uint64_t realFrameId, uint64_t finalFenceValue) {
    auto* context=static_cast<CommandContext*>(opaqueContext);
    if (!context || !realFrameId || !finalFenceValue) return invalid;
    std::lock_guard lock(historyMutex);
    auto history=std::static_pointer_cast<HistoryState>(context->oitHistoryState);
    if (!history) return 0;
    const auto found=history->pending.find(realFrameId);
    if (found==history->pending.end()) return 0;
    if (found->second.presentFence || finalFenceValue<found->second.resources->serial) return stale;
    found->second.presentFence=finalFenceValue;
    return success;
}
int commitPresented(void* opaqueContext, uint64_t realFrameId, uint64_t fenceValue) {
    auto* context=static_cast<CommandContext*>(opaqueContext);
    if (!context || !realFrameId || !fenceValue) return invalid;
    std::lock_guard lock(historyMutex);
    auto history=std::static_pointer_cast<HistoryState>(context->oitHistoryState);
    if (!history) return 0;
    const auto found=history->pending.find(realFrameId);
    if (found==history->pending.end()) return 0;
    if (found->second.presentFence!=fenceValue) return stale;
    auto candidate=std::move(found->second.resources);
    history->pending.erase(found);
    if (history->presented && history->presented->input.identity.worldEpoch==candidate->input.identity.worldEpoch &&
        history->presented->input.identity.realFrameId>=realFrameId) return stale;
    history->presented=std::move(candidate);
    return success;
}
void discardResolved(void* opaqueContext, uint64_t realFrameId, uint64_t fenceValue) {
    auto* context=static_cast<CommandContext*>(opaqueContext);
    if (!context) return;
    std::lock_guard lock(historyMutex);
    auto history=std::static_pointer_cast<HistoryState>(context->oitHistoryState);
    if (!history) return;
    const auto found=history->pending.find(realFrameId);
    if (found!=history->pending.end() && found->second.presentFence==fenceValue) history->pending.erase(found);
}
void reset(void* opaqueContext) {
    auto* context=static_cast<CommandContext*>(opaqueContext);
    if (!context) return;
    context->graphicsCapture={}; restoreGraphicsCaptureRate(context);
    if (auto frame=std::static_pointer_cast<Frame>(context->oitFrameState)) {
        cancelActiveExecutionOperations(*frame);
        if (!context->submissionFailed.load()) cancelExecutionTextureCaptures(*frame);
        if (frame->phase!=Phase::Resolved) frame->phase=Phase::Aborted;
        frame->metadata.clear();
        std::lock_guard lock(registryMutex); registry.erase(frame->token);
    }
    context->oitFrameState.reset();
}
}
