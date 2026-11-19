module;
#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include "mcdr_terrain_tile.h"
module mcdr.abi.common;
import mcdr.d3d12.device;
import mcdr.render.oit;
namespace {
template<class Operation> int32_t callOit(const char* name, Operation&& operation) {
    try {
        const auto result=operation();
        if (result<1) mcdr::abi::setLastErrorf(name,"OIT request failed (%d)",result);
        return result;
    } catch (const std::bad_alloc&) {
        mcdr::abi::setLastError(name,"OIT allocation failed");
        return MCDR_OIT_RESULT_BUDGET_EXCEEDED;
    } catch (const std::exception& exception) {
        mcdr::abi::setLastError(name,exception.what());
        return MCDR_OIT_RESULT_INVALID_REQUEST;
    } catch (...) {
        mcdr::abi::setLastError(name,"OIT operation failed");
        return MCDR_OIT_RESULT_INVALID_REQUEST;
    }
}
}
extern "C" {
int32_t MCDR_CALL mcdr_dx12OitBeginExecutionOperationV1(mcdr_command_ctx* context, void* frame,
    const mcdr_oit_execution_operation_begin_v1* input, void** operation) {
    if (operation) *operation = nullptr;
    return callOit("oit.operation.begin", [&] {
        return input ? d3d12_mc::oit::beginExecutionOperation(context, frame, *input, operation) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitFinishExecutionOperationV1(mcdr_command_ctx* context, void* frame,
    void* operation, uint64_t* receiptCount) {
    if (receiptCount) *receiptCount = 0;
    return callOit("oit.operation.finish", [&] {
        return d3d12_mc::oit::finishExecutionOperation(context, frame, operation, receiptCount);
    });
}
int32_t MCDR_CALL mcdr_dx12OitCancelExecutionOperationV1(mcdr_command_ctx* context, void* frame, void* operation) {
    return callOit("oit.operation.cancel", [&] {
        return d3d12_mc::oit::cancelExecutionOperation(context, frame, operation);
    });
}
int32_t MCDR_CALL mcdr_dx12OitQueryExecutionOperationReceiptV1(mcdr_command_ctx* context, void* frame,
    void* operation, uint64_t afterSequence, mcdr_oit_execution_operation_receipt_v1* output) {
    if (output) *output = {};
    try {
        const int result = d3d12_mc::oit::queryExecutionOperationReceipt(context, frame, operation, afterSequence, output);
        if (result < 0) mcdr::abi::setLastErrorf("oit.operation.query", "OIT request failed (%d)", result);
        return result;
    } catch (const std::bad_alloc&) {
        mcdr::abi::setLastError("oit.operation.query", "OIT allocation failed");
        return MCDR_OIT_RESULT_BUDGET_EXCEEDED;
    } catch (const std::exception& failure) {
        mcdr::abi::setLastError("oit.operation.query", failure.what());
        return MCDR_OIT_RESULT_INVALID_REQUEST;
    } catch (...) {
        mcdr::abi::setLastError("oit.operation.query", "OIT operation failed");
        return MCDR_OIT_RESULT_INVALID_REQUEST;
    }
}
int32_t MCDR_CALL mcdr_dx12CaptureSampledTextureContentV1(
    mcdr_command_ctx* context, mcdr_object* view, uint64_t totalBudgetBytes,
    uint64_t* ownerIdentity, uint64_t* ownerGeneration, uint64_t* contentVersion,
    uint32_t* firstMip, uint32_t* mipCount, uint32_t* firstLayer, uint32_t* layerCount) {
    return callOit("oit.sampled.capture", [&] {
        return d3d12_mc::captureSampledTextureContentV1(context, view, totalBudgetBytes,
            ownerIdentity, ownerGeneration, contentVersion, firstMip, mipCount, firstLayer, layerCount);
    });
}
int32_t MCDR_CALL mcdr_dx12OitShaderPreludeV1(char* destination, uint32_t capacity, uint32_t* required, uint32_t* abi, uint64_t* hash) {
    return callOit("oit.prelude",[&] { return d3d12_mc::oit::shaderPrelude(destination,capacity,required,abi,hash); });
}
int32_t MCDR_CALL mcdr_dx12OitBeginFrameV1(mcdr_command_ctx* context, const mcdr_oit_begin_v1* input, void** frame) {
    if (frame) *frame=nullptr;
    return callOit("oit.begin",[&] { return input ? d3d12_mc::oit::beginFrame(context,*input,frame) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitBeginFrameV2(mcdr_command_ctx* context, const mcdr_oit_begin_v2* input, void** frame) {
    if (frame) *frame=nullptr;
    return callOit("oit.begin",[&] { return input ? d3d12_mc::oit::beginTerrainFrame(context,*input,frame) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitPublishTerrainMetadataV2(mcdr_command_ctx* context, void* frame, const mcdr_oit_terrain_metadata_v2* input, void** metadata) {
    if (metadata) *metadata=nullptr;
    return callOit("oit.terrain",[&] { return input ? d3d12_mc::oit::publishTerrainMetadata(context,frame,*input,metadata) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitBindTerrainDrawV2(mcdr_command_ctx* context, void* frame, void* metadata, const mcdr_oit_terrain_draw_v2* input) {
    return callOit("oit.bind",[&] { return input ? d3d12_mc::oit::bindTerrainDraw(context,frame,metadata,*input) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitPublishMetadataV1(mcdr_command_ctx* context, void* frame, const mcdr_oit_metadata_v1* input, void** metadata) {
    if (metadata) *metadata=nullptr;
    return callOit("oit.metadata",[&] { return input ? d3d12_mc::oit::publishMetadata(context,frame,*input,metadata) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitBindDrawV1(mcdr_command_ctx* context, void* frame, void* metadata, const mcdr_oit_draw_v1* input) {
    return callOit("oit.bind",[&] { return input ? d3d12_mc::oit::bindDraw(context,frame,metadata,*input) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitUnbindDrawV1(mcdr_command_ctx* context, void* frame) {
    return callOit("oit.unbind",[&] { return d3d12_mc::oit::unbindDraw(context,frame); });
}
int32_t MCDR_CALL mcdr_dx12OitSealFrameV1(mcdr_command_ctx* context, void* frame) {
    return callOit("oit.seal",[&] { return d3d12_mc::oit::sealFrame(context,frame); });
}
int32_t MCDR_CALL mcdr_dx12OitAbortFrameV1(mcdr_command_ctx* context, void* frame) {
    return callOit("oit.abort",[&] { return d3d12_mc::oit::abortFrame(context,frame); });
}
int32_t MCDR_CALL mcdr_dx12OitResolveRecordsV3(mcdr_command_ctx* context, const mcdr_oit_resolve_v3* input) {
    return callOit("oit.resolve",[&] { return input ? d3d12_mc::oit::resolveRecords(context,*input) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitResolveRecordsV4(mcdr_command_ctx* context, const mcdr_oit_resolve_v4* input) {
    return callOit("oit.resolve.v4",[&] { return input ? d3d12_mc::oit::resolveRecordsV4(context,*input) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitPrepareTerrainTilesV2(mcdr_command_ctx* context, void* frame, const void* batch) {
    return callOit("oit.terrain.tiles.v2", [&] {
        return batch ? static_cast<int>(d3d12_mc::oit::prepareTerrainTilesV2(
            context, frame, *static_cast<const mcdr::terrain_tile::BatchV2*>(batch))) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitPrepareTerrainTilesV3(mcdr_command_ctx* context, void* frame, const void* batch) {
    return callOit("oit.terrain.tiles.v3", [&] {
        return batch ? static_cast<int>(d3d12_mc::oit::prepareTerrainTilesV3(
            context, frame, *static_cast<const mcdr::terrain_tile::BatchV3*>(batch))) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitBeginExecutionStreamV1(
    mcdr_command_ctx* context, void* frame, const mcdr_oit_execution_begin_v1* input, void** stream) {
    if (stream) *stream = nullptr;
    return callOit("oit.execution.begin", [&] {
        return input ? d3d12_mc::oit::beginExecutionStream(context, frame, *input, stream) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitCaptureExecutionTextureV1(mcdr_command_ctx* context, void* frame,
    mcdr_object* view, uint64_t budget, mcdr_oit_execution_texture_v1* output) {
    if (output) *output = {};
    return callOit("oit.execution.texture.capture", [&] {
        return d3d12_mc::oit::captureExecutionTextureV1(context, frame, view, budget, output);
    });
}
int32_t MCDR_CALL mcdr_dx12OitBeginExecutionStreamV2(mcdr_command_ctx* context, void* frame,
    const mcdr_oit_execution_begin_v2* input, void** stream) {
    if (stream) *stream = nullptr;
    return callOit("oit.execution.v2.begin", [&] {
        return input ? d3d12_mc::oit::beginExecutionStreamV2(context, frame, *input, stream) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitAppendExecutionEventV2(mcdr_command_ctx* context, void* stream,
    const mcdr_oit_execution_event_v2* input, uint64_t* eventId) {
    if (eventId) *eventId = 0;
    return callOit("oit.execution.v2.append", [&] {
        return input ? d3d12_mc::oit::appendExecutionEventV2(context, stream, *input, eventId) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitQueryExecutionStreamV2(mcdr_command_ctx* context, void* stream,
    mcdr_oit_execution_info_v2* output) {
    if (output) *output = {};
    return callOit("oit.execution.v2.query", [&] {
        return d3d12_mc::oit::queryExecutionStreamV2(context, stream, output);
    });
}
int32_t MCDR_CALL mcdr_dx12OitAppendExecutionOperationEventV1(mcdr_command_ctx* context, void* stream,
    const mcdr_oit_execution_event_v2* event, const mcdr_oit_execution_operation_receipt_v1* receipt,
    uint64_t* eventId) {
    if (eventId) *eventId = 0;
    return callOit("oit.execution.operation.append", [&] {
        return event && receipt ? d3d12_mc::oit::appendExecutionOperationEventV1(
            context, stream, *event, *receipt, eventId) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitSealExecutionStreamV2(mcdr_command_ctx* context, void* stream,
    const mcdr_oit_execution_manifest_v2* input, void** sealedStream, mcdr_oit_execution_seal_result_v2* result) {
    if (sealedStream) *sealedStream = nullptr;
    if (result) *result = {};
    return callOit("oit.execution.v2.seal", [&] {
        return input ? d3d12_mc::oit::sealExecutionStreamV2(context, stream, *input, sealedStream, result) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitQueryExecutionTerrainReceiptV2(mcdr_command_ctx* context, void* stream,
    uint64_t receiptId, mcdr_oit_execution_terrain_receipt_v2* output) {
    if (output) *output = {};
    return callOit("oit.execution.v2.receipt", [&] {
        return d3d12_mc::oit::queryExecutionTerrainReceiptV2(context, stream, receiptId, output);
    });
}
int32_t MCDR_CALL mcdr_dx12OitAbortExecutionStreamV2(mcdr_command_ctx* context, void* stream) {
    return callOit("oit.execution.v2.abort", [&] {
        return d3d12_mc::oit::abortExecutionStreamV2(context, stream);
    });
}
int32_t MCDR_CALL mcdr_dx12OitExecuteTileStreamV2(mcdr_command_ctx* context, void* stream,
    const void* fourLaneBatches, mcdr_object* corrected) {
    return callOit("oit.execution.v2.execute", [&] {
        return d3d12_mc::oit::executeTileStreamV2(context, stream,
            static_cast<const mcdr::terrain_tile::BatchV2*>(fourLaneBatches), corrected);
    });
}
int32_t MCDR_CALL mcdr_dx12OitQueryExecutionStreamV1(
    mcdr_command_ctx* context, void* stream, mcdr_oit_execution_info_v1* output) {
    if (output) *output = {};
    return callOit("oit.execution.query", [&] {
        return d3d12_mc::oit::queryExecutionStream(context, stream, output);
    });
}
int32_t MCDR_CALL mcdr_dx12OitQueryTerrainDrawReceiptV1(
    mcdr_command_ctx* context, void* frame, const mcdr_oit_draw_receipt_query_v1* query,
    mcdr_oit_draw_receipt_v1* output) {
    if (output) *output = {};
    return callOit("oit.execution.receipt", [&] {
        return query ? d3d12_mc::oit::queryTerrainDrawReceipt(context, frame, *query, output) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitQueryTerrainDrawFrontierV1(
    mcdr_command_ctx* context, void* frame, uint64_t* afterSequence) {
    if (afterSequence) *afterSequence = 0;
    return callOit("oit.execution.frontier", [&] {
        return d3d12_mc::oit::queryTerrainDrawFrontier(context, frame, afterSequence);
    });
}
int32_t MCDR_CALL mcdr_dx12OitBeginTerrainStageDrawV2(mcdr_command_ctx* context, void* frame,
    const mcdr_oit_terrain_stage_begin_v2* input, void** token) {
    if (token) *token = nullptr;
    return callOit("oit.terrain.stage.begin", [&] {
        return input ? d3d12_mc::oit::beginTerrainStageDraw(context, frame, *input, token) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitFinishTerrainStageDrawV2(mcdr_command_ctx* context, void* frame,
    void* token, mcdr_oit_terrain_stage_receipt_v2* output) {
    if (output) *output = {};
    return callOit("oit.terrain.stage.finish", [&] {
        return d3d12_mc::oit::finishTerrainStageDraw(context, frame, token, output);
    });
}
int32_t MCDR_CALL mcdr_dx12OitAbortTerrainStageDrawV2(mcdr_command_ctx* context, void* frame, void* token) {
    return callOit("oit.terrain.stage.abort", [&] {
        return d3d12_mc::oit::abortTerrainStageDraw(context, frame, token);
    });
}
int32_t MCDR_CALL mcdr_dx12OitQueryTerrainStageReceiptV2(mcdr_command_ctx* context, void* frame,
    uint64_t receiptId, mcdr_oit_terrain_stage_receipt_v2* output) {
    if (output) *output = {};
    return callOit("oit.terrain.stage.query", [&] {
        return d3d12_mc::oit::queryTerrainStageReceipt(context, frame, receiptId, output);
    });
}
int32_t MCDR_CALL mcdr_dx12OitRegisterTerrainReplayReadSetV2(mcdr_command_ctx* context, void* frame,
    void* token, void* terrainBatch, mcdr_object* vertex, uint64_t vertexOffset,
    const mcdr_oit_terrain_replay_read_set_v2* input) {
    return callOit("oit.terrain.readset.register", [&] {
        return input ? d3d12_mc::oit::registerTerrainReplayReadSet(
            context, frame, token, terrainBatch, vertex, vertexOffset, *input) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitCancelTerrainReplayReadSetV2(mcdr_command_ctx* context, void* frame, void* token) {
    return callOit("oit.terrain.readset.cancel", [&] {
        return d3d12_mc::oit::cancelTerrainReplayReadSet(context, frame, token);
    });
}
int32_t MCDR_CALL mcdr_dx12OitAppendExecutionEventV1(
    mcdr_command_ctx* context, void* stream, const mcdr_oit_execution_event_v1* event, uint64_t* eventId) {
    if (eventId) *eventId = 0;
    return callOit("oit.execution.event", [&] {
        return event ? d3d12_mc::oit::appendExecutionEvent(context, stream, *event, eventId) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitSealExecutionStreamV1(
    mcdr_command_ctx* context, void* stream, const mcdr_oit_execution_manifest_v1* manifest,
    void** sealedStream, mcdr_oit_execution_seal_result_v1* result) {
    if (sealedStream) *sealedStream = nullptr;
    if (result) *result = {};
    return callOit("oit.execution.seal", [&] {
        return manifest ? d3d12_mc::oit::sealExecutionStream(context, stream, *manifest, sealedStream, result) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitCreateNormalizationProgramsV1(mcdr_command_ctx* context,
    const mcdr_oit_normalization_program_v1* input, void** programs) {
    if (programs) *programs = nullptr;
    return callOit("oit.normalization.programs", [&] {
        return input ? d3d12_mc::oit::createNormalizationPrograms(context, *input, programs) : -1;
    });
}
void MCDR_CALL mcdr_dx12OitDestroyNormalizationProgramsV1(void* programs) {
    d3d12_mc::oit::destroyNormalizationPrograms(programs);
}
int32_t MCDR_CALL mcdr_dx12OitCreateNormalizationBatchV1(mcdr_command_ctx* context, void* programs,
    const mcdr_oit_normalization_batch_v1* input, void** batch) {
    if (batch) *batch = nullptr;
    return callOit("oit.normalization.batch", [&] {
        return input ? d3d12_mc::oit::createNormalizationBatch(context, programs, *input, batch) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitRecordNormalizationLaneV1(mcdr_command_ctx* context, void* batch,
    const mcdr_oit_normalization_lane_v1* input) {
    return callOit("oit.normalization.lane", [&] {
        return input ? d3d12_mc::oit::recordNormalizationLane(context, batch, *input) : -1;
    });
}
int32_t MCDR_CALL mcdr_dx12OitRecordNormalizationAverageV1(mcdr_command_ctx* context, void* batch) {
    return callOit("oit.normalization.average", [&] {
        return d3d12_mc::oit::recordNormalizationAverage(context, batch);
    });
}
int32_t MCDR_CALL mcdr_dx12OitQueryNormalizationBatchV1(mcdr_command_ctx* context, void* batch,
    mcdr_oit_normalization_info_v1* output) {
    if (output) *output = {};
    return callOit("oit.normalization.query", [&] {
        return d3d12_mc::oit::queryNormalizationBatch(context, batch, output);
    });
}
void MCDR_CALL mcdr_dx12OitDestroyNormalizationBatchV1(void* batch) {
    d3d12_mc::oit::destroyNormalizationBatch(batch);
}
}
