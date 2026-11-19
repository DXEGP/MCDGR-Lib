module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include "mcdr_terrain_tile.h"

export module mcdr.render.oit;

import mcdr.d3d12.device;

export namespace d3d12_mc::oit {
enum class TerrainDrawContent { OriginalDrawOnly, UnsupportedContent };

struct TerrainDrawSectionIdentity {
    uint64_t surfaceId = 0;
    uint64_t geometryGeneration = 0;
    uint64_t materialId = 0;
    uint64_t materialGeneration = 0;
    uint64_t textureGeneration = 0;
    uint32_t vertexBase = 0;
    uint32_t vertexCount = 0;
    uint32_t primitiveBase = 0;
    uint32_t primitiveCount = 0;
};

struct TerrainDrawIdentity {
    uint32_t schemaVersion = 1;
    mcdr_frame_identity frame{};
    uint64_t frameSerial = 0;
    uint64_t producerScopeSerial = 0;
    uint64_t programGeneration = 0;
    uintptr_t metadata = 0;
    uint64_t vertexBytes = 0;
    uint64_t producerId = 0;
    uint64_t producerGeneration = 0;
    uint64_t shaderGeneration = 0;
    uint32_t sectionBase = 0;
    uint32_t sectionCount = 0;
    uint32_t primitiveBase = 0;
    uint32_t primitiveCount = 0;
    std::vector<TerrainDrawSectionIdentity> sections;
};

struct TerrainReplayDraw {
    mcdr_frame_identity frame{};
    uint64_t frameSerial = 0;
    uint64_t programGeneration = 0;
    uint64_t vertexBytes = 0;
    uint32_t sectionCount = 0;
    uint32_t primitiveCount = 0;
    uint32_t stage = UINT32_MAX;
    uint64_t totalBudgetBytes = 0;
};
int preparePrograms(void* context, uint64_t generation, void** prepared,
    const mcdr_oit_normalization_program_v1 (*normalization)[3] = nullptr);
int validatePreparedPrograms(void* context, void* prepared);
void exchangePreparedPrograms(void* context, void* prepared) noexcept;
void destroyPreparedPrograms(void* prepared) noexcept;
int shaderPrelude(char* destination, uint32_t capacity, uint32_t* required, uint32_t* abi, uint64_t* hash);
int createNormalizationPipelineCertification(const mcdr_pipeline_desc& descriptor,
    const mcdr_oit_normalization_pipeline_v1& certification, std::shared_ptr<const void>* output);
int beginFrame(void* context, const mcdr_oit_begin_v1& input, void** frame);
int beginTerrainFrame(void* context, const mcdr_oit_begin_v2& input, void** frame);
int publishTerrainMetadata(void* context, void* frame, const mcdr_oit_terrain_metadata_v2& input, void** metadata);
int bindTerrainDraw(void* context, void* frame, void* metadata, const mcdr_oit_terrain_draw_v2& input);
int validateTerrainDraw(void* context, void* vertex, uint64_t offset, TerrainDrawIdentity* identity);
int retainTerrainDrawPacket(void* context, const TerrainDrawIdentity& identity,
    TerrainDrawContent content, std::shared_ptr<const void> packet,
    mcdr::terrain_tile::PacketPreparation prepare);
int confirmTerrainDrawReceipt(void* context);
int prepareTerrainStageDrawReceipt(void* context, const void* vertex, uint64_t vertexOffset);
int confirmTerrainStageDrawReceipt(void* context, std::shared_ptr<const void> packet,
    mcdr::terrain_tile::PacketPreparationV2 prepareReplay, PacketContentStatus content);
int validateTerrainStageReads(void* context, const void* terrainBatch, const void* vertex, uint64_t vertexOffset,
    const void* atlas, uint64_t atlasOffset, uint64_t atlasLength, uint32_t sectionCount,
    const void* globals, uint64_t globalsOffset, uint64_t globalsLength,
    const void* candidates, uint64_t candidatesOffset, uint64_t candidatesLength,
    std::shared_ptr<const void> batchLease, TerrainReplayDraw* replayDraw);
int beginExecutionStream(void* context, void* frame, const mcdr_oit_execution_begin_v1& input, void** stream);
int captureExecutionTextureV1(void* context, void* frame, void* view, uint64_t totalBudgetBytes,
    mcdr_oit_execution_texture_v1* output);
int beginExecutionStreamV2(void* context, void* frame, const mcdr_oit_execution_begin_v2& input, void** stream);
int appendExecutionEventV2(void* context, void* stream, const mcdr_oit_execution_event_v2& input, uint64_t* eventId);
int appendExecutionOperationEventV1(void* context, void* stream, const mcdr_oit_execution_event_v2& event,
    const mcdr_oit_execution_operation_receipt_v1& receipt, uint64_t* eventId);
int queryExecutionStreamV2(void* context, void* stream, mcdr_oit_execution_info_v2* output);
int queryExecutionTerrainReceiptV2(void* context, void* stream, uint64_t receiptId,
    mcdr_oit_execution_terrain_receipt_v2* output);
int sealExecutionStreamV2(void* context, void* stream, const mcdr_oit_execution_manifest_v2& input,
    void** sealedStream, mcdr_oit_execution_seal_result_v2* result);
int abortExecutionStreamV2(void* context, void* stream);
int executeTileStreamV2(void* context, void* stream, const mcdr::terrain_tile::BatchV2* batches, void* corrected);
struct ExecutionLaneTargets {
    void* mainDepth = nullptr;
    void* terrainDepth = nullptr;
    void* cloudDepth = nullptr;
    void* bounds = nullptr;
    void* culledBounds = nullptr;
    uint32_t residentTileCount = 0;
    const mcdr::terrain_tile::BatchV2* batch = nullptr;
    uint64_t batchConstants = 0;
};
int recordExecutionOpaqueSeed(void* context, void* stream, void* mainDepth,
    const mcdr::terrain_tile::BatchV2& batch, mcdr::terrain_tile::ConstantAddresses constants);
int recordExecutionLaneOperation(void* context, void* stream, uint64_t receiptId,
    const ExecutionLaneTargets& targets);
int beginExecutionOperation(void* context, void* frame,
    const mcdr_oit_execution_operation_begin_v1& input, void** operation);
int finishExecutionOperation(void* context, void* frame, void* operation, uint64_t* receiptCount);
int cancelExecutionOperation(void* context, void* frame, void* operation);
int queryExecutionOperationReceipt(void* context, void* frame, void* operation,
    uint64_t afterSequence, mcdr_oit_execution_operation_receipt_v1* output);
int queryExecutionStream(void* context, void* stream, mcdr_oit_execution_info_v1* output);
int queryTerrainDrawReceipt(void* context, void* frame, const mcdr_oit_draw_receipt_query_v1& query,
    mcdr_oit_draw_receipt_v1* output);
int queryTerrainDrawFrontier(void* context, void* frame, uint64_t* afterSequence);
int beginTerrainStageDraw(void* context, void* frame, const mcdr_oit_terrain_stage_begin_v2& input, void** token);
int finishTerrainStageDraw(void* context, void* frame, void* token, mcdr_oit_terrain_stage_receipt_v2* output);
int abortTerrainStageDraw(void* context, void* frame, void* token);
int queryTerrainStageReceipt(void* context, void* frame, uint64_t receiptId, mcdr_oit_terrain_stage_receipt_v2* output);
int registerTerrainReplayReadSet(void* context, void* frame, void* token, void* terrainBatch, void* vertex,
    uint64_t vertexOffset, const mcdr_oit_terrain_replay_read_set_v2& input);
int cancelTerrainReplayReadSet(void* context, void* frame, void* token);
int appendExecutionEvent(void* context, void* stream, const mcdr_oit_execution_event_v1& event, uint64_t* eventId);
int sealExecutionStream(void* context, void* stream, const mcdr_oit_execution_manifest_v1& manifest,
    void** sealedStream, mcdr_oit_execution_seal_result_v1* result);
int createNormalizationPrograms(void* context, const mcdr_oit_normalization_program_v1& input, void** programs);
int prepareNormalizationPrograms(void* context, uint64_t generation,
    const mcdr_oit_normalization_program_v1 (&inputs)[3], std::shared_ptr<void>* output);
void destroyNormalizationPrograms(void* programs) noexcept;
int createNormalizationBatch(void* context, void* programs, const mcdr_oit_normalization_batch_v1& input, void** batch);
int createPreparedNormalizationBatch(void* context, const std::shared_ptr<void>& programs,
    uint32_t targetCount, const mcdr_oit_normalization_batch_v1& input, void** batch);
int recordNormalizationLane(void* context, void* batch, const mcdr_oit_normalization_lane_v1& input);
int recordNormalizationAverage(void* context, void* batch);
// The last batch exposes full-frame output for the same recording. The recording lease owns the texture.
int resolveNormalizationOutput(void* context, void* batch, ID3D12Resource** output);
int queryNormalizationBatch(void* context, void* batch, mcdr_oit_normalization_info_v1* output);
void destroyNormalizationBatch(void* batch) noexcept;
mcdr::terrain_tile::Result prepareTerrainTiles(void* context, void* frame,
    const mcdr::terrain_tile::Batch& batch);
mcdr::terrain_tile::Result prepareTerrainTilesV2(void* context, void* frame,
    const mcdr::terrain_tile::BatchV2& batch);
mcdr::terrain_tile::Result prepareTerrainTilesV3(void* context, void* frame,
    const mcdr::terrain_tile::BatchV3& batch);
int publishMetadata(void* context, void* frame, const mcdr_oit_metadata_v1& input, void** metadata);
int bindDraw(void* context, void* frame, void* metadata, const mcdr_oit_draw_v1& input);
int unbindDraw(void* context, void* frame);
int sealFrame(void* context, void* frame);
int abortFrame(void* context, void* frame);
int resolveRecords(void* context, const mcdr_oit_resolve_v3& input);
int resolveRecordsV4(void* context, const mcdr_oit_resolve_v4& input);
int queuePresented(void* context, uint64_t realFrameId, uint64_t finalFenceValue);
int commitPresented(void* context, uint64_t realFrameId, uint64_t fenceValue);
void discardResolved(void* context, uint64_t realFrameId, uint64_t fenceValue);
void reset(void* context);
}
