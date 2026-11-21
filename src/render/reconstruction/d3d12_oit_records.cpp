module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include "mcdr_oit_shader.h"
#include "mcdr_oit_resolve_shader.h"
#include "mcdr_oit_budget.h"
#include "mcdr_oit_execution.h"
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

struct Programs {
    void* context = nullptr;
    uint64_t generation = 0;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> initialize, compact, resolve;
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

int shaderPrelude(char* destination, uint32_t capacity, uint32_t* required, uint32_t* abi, uint64_t* hash) {
    if (!required || !abi || !hash) return invalid;
    *required = static_cast<uint32_t>(mcdr::oit_shader::prelude.size()+1);
    *abi = mcdr::oit_shader::abiVersion; *hash = mcdr::oit_shader::contractHash;
    if (!destination || !capacity) return success;
    if (capacity < *required) return invalid;
    std::memcpy(destination,mcdr::oit_shader::prelude.data(),*required-1); destination[*required-1] = '\0';
    return success;
}

int preparePrograms(void* opaqueContext, uint64_t generation, void** output) {
    if (output) *output = nullptr;
    if (!opaqueContext || !output || !generation) return MCDR_FRAME_PROGRAMS_INVALID_REQUEST;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (context->oitFrameState) return MCDR_FRAME_PROGRAMS_BUSY;
    auto programs = std::make_shared<Programs>(); programs->context = context; programs->generation = generation;
    programs->budget = mcdr::oit_budget::reserve(4 * 1024 * 1024);
    if (!programs->budget) return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
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
int validateTerrainDraw(void* opaqueContext, void* vertex, uint64_t offset) {
    auto* context=static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->oitFrameState || !context->graphicsCapture.lease) return invalid;
    auto frame=std::static_pointer_cast<Frame>(context->oitFrameState);
    if (frame->phase!=Phase::Recording || !frame->bound || frame->boundTerrainVertex!=vertex ||
        frame->recordingIdentity != context->recordingIdentity || frame->boundTerrainOffset!=offset ||
        context->graphicsCapture.remainingDraws!=1) return invalid;
    return success;
}
namespace {
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
namespace {

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
