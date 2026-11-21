#ifndef MCDR_OIT_EXECUTION_H
#define MCDR_OIT_EXECUTION_H

#include <stdint.h>

typedef struct mcdr_oit_execution_begin_v1 {
    uint32_t schemaVersion, structSize;
    mcdr_frame_identity identity;
    uint64_t frameSerial, historyGeneration, programGeneration;
    uint32_t maxEvents, maxScopes, maxResourceReferences, reserved;
} mcdr_oit_execution_begin_v1;

typedef struct mcdr_oit_execution_info_v1 {
    uint32_t schemaVersion, structSize;
    mcdr_frame_identity identity;
    uint64_t streamId, frameSerial, programGeneration;
    uint32_t nativeDrawCount, appendedDrawCount, eventCount, openScopeCount;
    uint32_t state, unavailableReason;
    uint64_t reserved[2];
} mcdr_oit_execution_info_v1;

typedef struct mcdr_oit_draw_receipt_query_v1 {
    uint32_t schemaVersion, structSize;
    uint64_t receiptId, afterSequence, producerScopeSerial;
    uint64_t reserved[2];
} mcdr_oit_draw_receipt_query_v1;

typedef struct mcdr_oit_draw_receipt_v1 {
    uint32_t schemaVersion, structSize;
    uint64_t receiptId, nativeSequence, frameSerial, producerScopeSerial;
    uint64_t producerId, producerGeneration, shaderGeneration, programGeneration;
    mcdr_frame_identity identity;
    uint32_t sectionBase, sectionCount, primitiveBase, primitiveCount;
    uint32_t contentState, producerKind;
    uint64_t reserved[2];
} mcdr_oit_draw_receipt_v1;

typedef struct mcdr_oit_terrain_stage_begin_v2 {
    uint32_t schemaVersion, structSize, stage, flags;
    uint64_t frameSerial, producerId, producerScopeSerial, sourcePassId;
    uint64_t sourceDrawId, parentSourceDrawId;
    mcdr_frame_identity identity;
    mcdr_pipeline* pipeline;
    uint64_t reserved[2];
} mcdr_oit_terrain_stage_begin_v2;

typedef struct mcdr_oit_terrain_stage_receipt_v2 {
    uint32_t schemaVersion, structSize, stage, flags;
    uint32_t producerKind, contentState, reserved0, reserved1;
    uint64_t receiptId, nativeSequence, frameSerial, producerId;
    uint64_t producerScopeSerial, sourcePassId, sourceDrawId, parentSourceDrawId;
    mcdr_frame_identity identity;
    uint64_t pipelineIdentity;
} mcdr_oit_terrain_stage_receipt_v2;

typedef struct mcdr_oit_terrain_replay_range_v2 {
    uint32_t schemaVersion, structSize, role, commandIndex;
    uint64_t generation, ownerIdentity, byteOffset, byteLength;
    mcdr_object* object;
} mcdr_oit_terrain_replay_range_v2;

typedef struct mcdr_oit_terrain_upload_read_v2 {
    uint32_t schemaVersion, structSize, role, reserved;
    mcdr_object* object;
    uint64_t byteOffset, byteLength, contentVersion, resourceVersion, ownerIdentity;
} mcdr_oit_terrain_upload_read_v2;

typedef struct mcdr_oit_terrain_replay_read_set_v2 {
    uint32_t schemaVersion, structSize, flags, geometryRangeCount;
    uint32_t uploadBindingCount, reserved0, reserved1, reserved2;
    uint64_t receiptId, frameSerial, producerScopeSerial, programGeneration, resourceVersion;
    const mcdr_oit_terrain_replay_range_v2* geometryRanges;
    const mcdr_oit_terrain_upload_read_v2* uploadBindings;
} mcdr_oit_terrain_replay_read_set_v2;

typedef struct mcdr_oit_execution_resource_v1 {
    uint32_t schemaVersion, structSize, role, roleIndex;
    uint32_t access, source;
    mcdr_object* object;
    uint64_t resourceIdentity, contentVersion, resourceVersion;
    uint32_t firstSubresource, subresourceCount;
    uint64_t byteOffset, byteLength, reserved[2];
} mcdr_oit_execution_resource_v1;

typedef struct mcdr_oit_execution_event_v1 {
    uint32_t schemaVersion, structSize;
    uint64_t sequence;
    uint32_t kind, producerKind, stage, flags;
    uint64_t producerId, producerScopeSerial, sourcePassId, sourceDrawId, drawGroupId, drawReceiptId;
    uint64_t shaderGeneration, programGeneration;
    uint32_t resourceCount, reserved;
    const mcdr_oit_execution_resource_v1* resources;
    uint64_t reservedTail[2];
} mcdr_oit_execution_event_v1;

typedef struct mcdr_oit_execution_scope_v1 {
    uint32_t schemaVersion, structSize;
    uint64_t producerId, producerScopeSerial, sourcePassId;
    uint32_t producerKind, stage, eventCount, nativeDrawCount, flags, reserved;
    uint64_t reservedTail;
} mcdr_oit_execution_scope_v1;

typedef struct mcdr_oit_execution_manifest_v1 {
    uint32_t schemaVersion, structSize, eventCount, scopeCount;
    uint32_t nativeDrawCount, flags;
    uint64_t lastSequence;
    const mcdr_oit_execution_scope_v1* scopes;
    uint64_t reserved[2];
} mcdr_oit_execution_manifest_v1;

typedef struct mcdr_oit_execution_seal_result_v1 {
    uint32_t schemaVersion, structSize, state, unavailableReason;
    uint64_t firstEventId, drawReceiptId;
    uint32_t expected, observed;
    uint64_t reserved[2];
} mcdr_oit_execution_seal_result_v1;

typedef struct mcdr_oit_execution_begin_v2 {
    uint32_t schemaVersion, structSize;
    mcdr_frame_identity identity;
    uint64_t frameSerial, historyGeneration, programGeneration;
    uint32_t maxEvents, maxScopes, maxPasses, maxResources;
    uint32_t maxReceipts, flags, reserved0, reserved1;
} mcdr_oit_execution_begin_v2;

typedef struct mcdr_oit_execution_key_v2 {
    uint64_t producerId, producerScopeSerial, sourcePassId;
    uint64_t sourceDrawId, parentSourceDrawId;
    uint32_t producerKind, stage, commandOrdinal, reserved;
} mcdr_oit_execution_key_v2;

typedef struct mcdr_oit_execution_operation_begin_v1 {
    uint32_t schemaVersion, structSize;
    mcdr_frame_identity identity;
    uint64_t frameSerial;
    mcdr_oit_execution_key_v2 key;
    uint32_t kind, expectedCommandFamily;
    uint64_t programGeneration, shaderGeneration;
    mcdr_pipeline* pipeline;
    uint32_t flags, reserved;
} mcdr_oit_execution_operation_begin_v1;

typedef struct mcdr_oit_execution_operation_receipt_v1 {
    uint32_t schemaVersion, structSize, kind, commandFamily;
    uint64_t receiptId, nativeSequence, frameSerial;
    mcdr_frame_identity identity;
    mcdr_oit_execution_key_v2 key;
    uint64_t pipelineIdentity, shaderGeneration, programGeneration, resourceVersion;
    uint64_t packetId, readSetId;
    uint32_t recordState, contentState, adapterState, reserved;
} mcdr_oit_execution_operation_receipt_v1;

#define MCDR_OIT_OPERATION_RECORDED 1u
#define MCDR_OIT_OPERATION_EMPTY 2u
#define MCDR_OIT_OPERATION_RECORDING_FAILED 3u
#define MCDR_OIT_OPERATION_CONTENT_UNFROZEN 0u
#define MCDR_OIT_OPERATION_CONTENT_FROZEN 1u
#define MCDR_OIT_OPERATION_CONTENT_UNSUPPORTED 2u
#define MCDR_OIT_OPERATION_CONTENT_STALE 3u
#define MCDR_OIT_OPERATION_ADAPTER_UNAVAILABLE 0u
#define MCDR_OIT_OPERATION_ADAPTER_READY 1u
#define MCDR_OIT_OPERATION_ADAPTER_UNSUPPORTED_TOPOLOGY 2u
#define MCDR_OIT_OPERATION_ADAPTER_UNSUPPORTED_BINDING 3u
#define MCDR_OIT_OPERATION_ADAPTER_UNSUPPORTED_ADDRESSING 4u

typedef struct mcdr_oit_execution_resource_v2 {
    uint32_t schemaVersion, structSize, role, roleIndex;
    uint32_t access, contentRequirement, firstSubresource, subresourceCount;
    mcdr_object* object;
    uint64_t resourceIdentity, ownerIdentity, ownerGeneration;
    uint64_t contentVersion, resourceVersion, byteOffset, byteLength;
    uint32_t reserved[2];
} mcdr_oit_execution_resource_v2;

typedef struct mcdr_oit_execution_event_v2 {
    uint32_t schemaVersion, structSize, kind, flags;
    uint64_t sequence;
    mcdr_oit_execution_key_v2 key;
    uint64_t drawGroupId, shaderGeneration, programGeneration;
    uint64_t pipelineIdentity;
    uint32_t resourceCount, receiptCount, payloadKind, payloadBytes;
    const mcdr_oit_execution_resource_v2* resources;
    const uint64_t* receiptIds;
    const void* payload;
    uint64_t reserved[2];
} mcdr_oit_execution_event_v2;

typedef struct mcdr_oit_execution_texture_v1 {
    uint32_t schemaVersion, structSize;
    uint64_t captureId;
    mcdr_object* texture;
    uint64_t ownerIdentity, ownerGeneration, contentVersion;
    uint32_t firstSubresource, subresourceCount;
    uint64_t resourceVersion;
} mcdr_oit_execution_texture_v1;

typedef struct mcdr_oit_execution_normalization_v2 {
    uint32_t schemaVersion, structSize, targetCount, certificationVersion;
    uint64_t certificationGeneration;
    mcdr_pipeline* pipeline;
    mcdr_object* sampler;
    uint8_t fragmentSourceHash[32], vertexSourceHash[32], contractHash[32];
    uint64_t inputContentIds[8];
    uint32_t outputWidth, outputHeight;
    uint64_t reserved[2];
} mcdr_oit_execution_normalization_v2;

typedef struct mcdr_oit_execution_normalization_v1 {
    uint32_t schemaVersion, structSize, targetCount, certificationVersion;
    uint64_t certificationGeneration;
    uint64_t pipelineOwnerIdentity, pipelineOwnerGeneration;
    uint64_t samplerOwnerIdentity, samplerOwnerGeneration;
    mcdr_pipeline* pipeline;
    mcdr_object* sampler;
    uint8_t fragmentSourceHash[32], vertexSourceHash[32], contractHash[32];
    uint64_t inputViewIdentities[8];
    mcdr_prepared_binding normalized, corrected;
    uint32_t outputWidth, outputHeight;
    uint64_t reserved[2];
} mcdr_oit_execution_normalization_v1;

typedef struct mcdr_oit_execution_scope_v2 {
    uint32_t schemaVersion, structSize, producerKind, stage;
    uint64_t producerId, producerScopeSerial, sourcePassId;
    uint32_t eventCount, receiptCount, flags, reserved;
} mcdr_oit_execution_scope_v2;

typedef struct mcdr_oit_execution_pass_v2 {
    uint32_t schemaVersion, structSize, completion, flags;
    uint64_t sourcePassId;
    uint32_t eventCount, reserved0, reserved1, reserved2;
} mcdr_oit_execution_pass_v2;

typedef struct mcdr_oit_execution_manifest_v2 {
    uint32_t schemaVersion, structSize, eventCount, scopeCount;
    uint32_t passCount, resourceCount, receiptCount, flags;
    uint64_t lastSequence;
    const mcdr_oit_execution_scope_v2* scopes;
    const mcdr_oit_execution_pass_v2* passes;
    uint32_t missingMask, reserved0, reserved1, reserved2;
} mcdr_oit_execution_manifest_v2;

typedef struct mcdr_oit_execution_info_v2 {
    uint32_t schemaVersion, structSize, state, missingMask;
    mcdr_frame_identity identity;
    uint64_t streamId, frameSerial, programGeneration;
    uint32_t eventCount, scopeCount, passCount, resourceCount;
    uint32_t receiptCount, consumedReceiptCount, openScopeCount, openPassCount;
    uint64_t firstIssueSequence;
} mcdr_oit_execution_info_v2;

typedef struct mcdr_oit_execution_terrain_receipt_v2 {
    uint32_t schemaVersion, structSize;
    mcdr_oit_terrain_stage_receipt_v2 receipt;
    uint64_t shaderGeneration, programGeneration, resourceVersion;
    uint32_t readSetRegistered, reserved;
} mcdr_oit_execution_terrain_receipt_v2;

typedef struct mcdr_oit_execution_seal_result_v2 {
    uint32_t schemaVersion, structSize, state, missingMask;
    uint64_t firstEventId, firstIssueSequence, receiptId;
    uint32_t expected, observed, reserved0, reserved1;
} mcdr_oit_execution_seal_result_v2;

#define MCDR_OIT_EXECUTION_V2_FRAME UINT32_MAX
#define MCDR_OIT_EXECUTION_V2_RESOURCE_EXTERNAL 1u
#define MCDR_OIT_EXECUTION_V2_RESOURCE_OPAQUE_DEPTH 2u
#define MCDR_OIT_EXECUTION_V2_RESOURCE_MAIN_DEPTH 3u
#define MCDR_OIT_EXECUTION_V2_RESOURCE_TERRAIN_DEPTH 4u
#define MCDR_OIT_EXECUTION_V2_RESOURCE_CLOUD_DEPTH 5u
#define MCDR_OIT_EXECUTION_V2_RESOURCE_BOUNDS 6u
#define MCDR_OIT_EXECUTION_V2_RESOURCE_COEFFICIENT 7u
#define MCDR_OIT_EXECUTION_V2_RESOURCE_Q 8u
#define MCDR_OIT_EXECUTION_V2_RESOURCE_CULLED_BOUNDS 9u
#define MCDR_OIT_EXECUTION_V2_RESOURCE_NORMALIZED 10u
#define MCDR_OIT_EXECUTION_V2_RESOURCE_CORRECTED 11u
#define MCDR_OIT_EXECUTION_V2_ACCESS_READ 1u
#define MCDR_OIT_EXECUTION_V2_ACCESS_WRITE 2u
#define MCDR_OIT_EXECUTION_V2_ACCESS_READ_WRITE 3u
#define MCDR_OIT_EXECUTION_V2_CONTENT_OBJECT_ONLY 0u
#define MCDR_OIT_EXECUTION_V2_CONTENT_OWNER_RANGE 1u
#define MCDR_OIT_EXECUTION_V2_CONTENT_UPLOAD_COPY 2u
#define MCDR_OIT_EXECUTION_V2_CONTENT_GPU_COPY 3u
#define MCDR_OIT_EXECUTION_V2_CONTENT_UNSUPPORTED 4u
#define MCDR_OIT_EXECUTION_V2_DRAW 1u
#define MCDR_OIT_EXECUTION_V2_PASS_BEGIN 2u
#define MCDR_OIT_EXECUTION_V2_PASS_END 3u
#define MCDR_OIT_EXECUTION_V2_SCOPE_BEGIN 4u
#define MCDR_OIT_EXECUTION_V2_SCOPE_END 5u
#define MCDR_OIT_EXECUTION_V2_EMPTY_SCOPE 6u
#define MCDR_OIT_EXECUTION_V2_COPY_DEPTH 7u
#define MCDR_OIT_EXECUTION_V2_COPY_BOUNDS 8u
#define MCDR_OIT_EXECUTION_V2_CULL_BOUNDS 9u
#define MCDR_OIT_EXECUTION_V2_NORMALIZATION 10u
#define MCDR_OIT_EXECUTION_V2_PAYLOAD_NORMALIZATION_V1 1u
#define MCDR_OIT_EXECUTION_V2_PAYLOAD_NORMALIZATION_V2 2u
#define MCDR_OIT_EXECUTION_V2_OPAQUE_DEPTH_SEED 11u
#define MCDR_OIT_EXECUTION_V2_WATER_MASK 12u
#define MCDR_OIT_EXECUTION_V2_CLOUD_DEPTH 13u
#define MCDR_OIT_EXECUTION_V2_UNCLASSIFIED_DRAW 14u
#define MCDR_OIT_EXECUTION_V2_GAP_CONTENT 0x0001u
#define MCDR_OIT_EXECUTION_V2_GAP_OPAQUE_SEED 0x0002u
#define MCDR_OIT_EXECUTION_V2_GAP_WATER 0x0004u
#define MCDR_OIT_EXECUTION_V2_GAP_CLOUD 0x0008u
#define MCDR_OIT_EXECUTION_V2_GAP_NORMALIZATION 0x0010u
#define MCDR_OIT_EXECUTION_V2_GAP_ADAPTER 0x0020u
#define MCDR_OIT_EXECUTION_V2_GAP_RECEIPT 0x0040u
#define MCDR_OIT_EXECUTION_V2_GAP_OPEN_SCOPE 0x0080u
#define MCDR_OIT_EXECUTION_V2_GAP_OPEN_PASS 0x0100u
#define MCDR_OIT_EXECUTION_V2_GAP_MANIFEST 0x0200u
#define MCDR_OIT_EXECUTION_V2_GAP_FAILED 0x0400u
#define MCDR_OIT_EXECUTION_V2_GAP_SELECTION 0x0800u
#define MCDR_OIT_EXECUTION_V2_GAP_READ_SET 0x1000u
#define MCDR_OIT_EXECUTION_V2_MANIFEST_FAILED 1u
#define MCDR_OIT_EXECUTION_V2_SCOPE_EMPTY 1u
#define MCDR_OIT_EXECUTION_V2_BOUNDARY_FAILED 2u
#define MCDR_OIT_EXECUTION_V2_PASS_RETURNED 1u
#define MCDR_OIT_EXECUTION_V2_PASS_FAILED 2u
#define MCDR_OIT_EXECUTION_V2_STATE_RECORDING 1u
#define MCDR_OIT_EXECUTION_V2_STATE_UNAVAILABLE 2u
#define MCDR_OIT_EXECUTION_V2_STATE_ABORTED 4u
#define MCDR_OIT_EXECUTION_V2_STATE_SEALED 5u

MCDR_API int32_t MCDR_CALL mcdr_dx12OitCaptureExecutionTextureV1(mcdr_command_ctx* context, void* frame,
    mcdr_object* view, uint64_t budget, mcdr_oit_execution_texture_v1* output);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitBeginExecutionStreamV2(mcdr_command_ctx* context, void* frame,
    const mcdr_oit_execution_begin_v2* input, void** stream);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitAppendExecutionEventV2(mcdr_command_ctx* context, void* stream,
    const mcdr_oit_execution_event_v2* input, uint64_t* eventId);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitAppendExecutionOperationEventV1(mcdr_command_ctx* context, void* stream,
    const mcdr_oit_execution_event_v2* event, const mcdr_oit_execution_operation_receipt_v1* receipt,
    uint64_t* eventId);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitQueryExecutionStreamV2(mcdr_command_ctx* context, void* stream,
    mcdr_oit_execution_info_v2* output);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitQueryExecutionTerrainReceiptV2(mcdr_command_ctx* context, void* stream,
    uint64_t receiptId, mcdr_oit_execution_terrain_receipt_v2* output);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitSealExecutionStreamV2(mcdr_command_ctx* context, void* stream,
    const mcdr_oit_execution_manifest_v2* input, void** sealedStream, mcdr_oit_execution_seal_result_v2* result);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitAbortExecutionStreamV2(mcdr_command_ctx* context, void* stream);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitExecuteTileStreamV2(mcdr_command_ctx* context, void* stream,
    const void* fourLaneBatches, mcdr_object* corrected);

#ifdef __cplusplus
static_assert(sizeof(mcdr_oit_execution_operation_begin_v1) == 144);
static_assert(sizeof(mcdr_oit_execution_operation_receipt_v1) == 192);
static_assert(sizeof(mcdr_oit_execution_begin_v2) == 96);
static_assert(sizeof(mcdr_oit_execution_key_v2) == 56);
static_assert(sizeof(mcdr_oit_execution_resource_v2) == 104);
static_assert(sizeof(mcdr_oit_execution_event_v2) == 168);
static_assert(sizeof(mcdr_oit_execution_texture_v1) == 64);
static_assert(sizeof(mcdr_oit_execution_normalization_v2) == 224);
static_assert(sizeof(mcdr_oit_execution_normalization_v1) == 288);
static_assert(sizeof(mcdr_oit_execution_scope_v2) == 56);
static_assert(sizeof(mcdr_oit_execution_pass_v2) == 40);
static_assert(sizeof(mcdr_oit_execution_manifest_v2) == 72);
static_assert(sizeof(mcdr_oit_execution_info_v2) == 112);
static_assert(sizeof(mcdr_oit_execution_terrain_receipt_v2) == 176);
static_assert(sizeof(mcdr_oit_execution_seal_result_v2) == 56);
static_assert(sizeof(mcdr_oit_execution_begin_v1) == 80);
static_assert(sizeof(mcdr_oit_execution_info_v1) == 104);
static_assert(sizeof(mcdr_oit_draw_receipt_query_v1) == 48);
static_assert(sizeof(mcdr_oit_draw_receipt_v1) == 144);
static_assert(sizeof(mcdr_oit_terrain_stage_begin_v2) == 120);
static_assert(sizeof(mcdr_oit_terrain_stage_receipt_v2) == 136);
static_assert(sizeof(mcdr_oit_terrain_replay_range_v2) == 56);
static_assert(sizeof(mcdr_oit_terrain_upload_read_v2) == 64);
static_assert(sizeof(mcdr_oit_terrain_replay_read_set_v2) == 88);
static_assert(sizeof(mcdr_oit_execution_resource_v1) == 96);
static_assert(sizeof(mcdr_oit_execution_event_v1) == 128);
static_assert(sizeof(mcdr_oit_execution_scope_v1) == 64);
static_assert(sizeof(mcdr_oit_execution_manifest_v1) == 56);
static_assert(sizeof(mcdr_oit_execution_seal_result_v1) == 56);
#endif

#define MCDR_OIT_EXECUTION_RECORDING 1u
#define MCDR_OIT_EXECUTION_UNAVAILABLE 2u
#define MCDR_OIT_EXECUTION_RECORDING_FAILED 3u
#define MCDR_OIT_EXECUTION_ABORTED 4u
#define MCDR_OIT_EXECUTION_SEALED 5u
#define MCDR_OIT_EXECUTION_UNAVAILABLE_NONE 0u
#define MCDR_OIT_EXECUTION_UNAVAILABLE_INCOMPLETE_MANIFEST 1u
#define MCDR_OIT_EXECUTION_UNAVAILABLE_OPEN_SCOPE 2u
#define MCDR_OIT_EXECUTION_UNAVAILABLE_MISSING_DRAW_RECEIPT 3u
#define MCDR_OIT_EXECUTION_UNAVAILABLE_UNSUPPORTED_CONTENT 4u
#define MCDR_OIT_EXECUTION_UNAVAILABLE_MISSING_NATIVE_ADAPTER 5u
#define MCDR_OIT_EXECUTION_UNAVAILABLE_RESOURCE_CLOSURE 6u
#define MCDR_OIT_EXECUTION_UNAVAILABLE_MISSING_EXECUTION_ADAPTERS 7u

#define MCDR_OIT_EXECUTION_SCOPE_BEGIN 1u
#define MCDR_OIT_EXECUTION_SCOPE_END 2u
#define MCDR_OIT_EXECUTION_EMPTY_SCOPE 3u
#define MCDR_OIT_EXECUTION_DRAW_TERRAIN 4u
#define MCDR_OIT_EXECUTION_DRAW_INDEXED 5u
#define MCDR_OIT_EXECUTION_DRAW_MESH 6u
#define MCDR_OIT_EXECUTION_COPY_DEPTH 7u
#define MCDR_OIT_EXECUTION_COPY_BOUNDS 8u
#define MCDR_OIT_EXECUTION_CULL_BOUNDS 9u
#define MCDR_OIT_EXECUTION_COMPOSITE_RECIPE 10u
#define MCDR_OIT_EXECUTION_STAGE_NONE 0u
#define MCDR_OIT_EXECUTION_STAGE_DEPTH_BOUNDS 1u
#define MCDR_OIT_EXECUTION_STAGE_TRANSMITTANCE 2u
#define MCDR_OIT_EXECUTION_STAGE_ACCUMULATE 3u
#define MCDR_OIT_EXECUTION_STAGE_WATER_DEPTH 4u
#define MCDR_OIT_EXECUTION_STAGE_CLOUD_DEPTH 5u
#define MCDR_OIT_EXECUTION_STAGE_TERMINAL 6u
#define MCDR_OIT_EXECUTION_CONTENT_UNSUPPORTED 1u
#define MCDR_OIT_EXECUTION_CONTENT_ORIGINAL 0u
#define MCDR_OIT_READSET_GEOMETRY_UNAVAILABLE (-20)
#define MCDR_OIT_READSET_MESHLET_UNAVAILABLE (-21)
#define MCDR_OIT_READSET_UPLOAD_UNAVAILABLE (-22)
#define MCDR_OIT_READSET_OWNER_UNAVAILABLE (-23)
#define MCDR_OIT_UPLOAD_TERRAIN_GLOBALS 0x10000000u
#define MCDR_OIT_UPLOAD_TERRAIN_SECTIONS 0x10000001u
#define MCDR_OIT_EXECUTION_SCOPE_EXPLICIT_EMPTY 1u
#define MCDR_OIT_EXECUTION_RESULT_RECORDING_FAILED (-8)

#endif
