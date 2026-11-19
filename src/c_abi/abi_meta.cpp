// abi_meta.cpp — the ABI's own surface: version, layout self-check, error
// reporting and log verbosity.
//
// The layout self-check is the piece JNI never had. Under JNI every one of the
// 115 entry points took `jlong` handles and hand-packed byte buffers, so a
// renamed package, a reordered struct field or a changed enum ordinal produced
// silent memory corruption at runtime with no compile-time or load-time signal
// (the repo already shipped a stale `com_dx12_dx12` symbol prefix once). Here
// the native side publishes one (name, offset, size) triple per mirrored field
// and the Java side compares it against its own MemoryLayouts before any other
// entry point is called.
module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"
#include <cstdlib>   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

// P108：DLSS 实现单元（普通 TU，src\advanced\d3d12_dlss.cpp）的 C 接口 ——
// mcdr_dx12SetDlssVendorDir 只是它的转发层。同 rtx_probe 的理由：模块看不到 NGX 类型。
#include "mcdr_dlss_impl.h"

import mcdr.d3d12.device;

namespace mcdr::abi {

namespace {

// (name, offset, size) for one mirrored field. sizeof() is applied to an
// unevaluated null dereference because a non-static data member cannot be named
// in a sizeof expression directly.
#define MCDR_FIELD(T, F)                               \
    {#T "." #F, static_cast<uint64_t>(offsetof(T, F)), \
     static_cast<uint64_t>(sizeof((static_cast<T*>(nullptr))->F))}

const std::vector<mcdr_abi_field>& layoutTable() {
    // Function-local static: initialised once, thread-safe under C++11 and
    // later, and never touched again after the first call.
    static const std::vector<mcdr_abi_field> table = {
        MCDR_FIELD(mcdr_present_receipt_v1, schemaVersion),
        MCDR_FIELD(mcdr_present_receipt_v1, structSize),
        MCDR_FIELD(mcdr_present_receipt_v1, serial),
        MCDR_FIELD(mcdr_present_receipt_v1, identity),
        MCDR_FIELD(mcdr_present_receipt_v1, providerGeneration),
        MCDR_FIELD(mcdr_present_receipt_v1, historyGeneration),
        MCDR_FIELD(mcdr_present_receipt_v1, commandFenceValue),
        MCDR_FIELD(mcdr_present_receipt_v1, submitQueueFenceValue),
        MCDR_FIELD(mcdr_present_receipt_v1, presentQueueFenceValue),
        MCDR_FIELD(mcdr_present_receipt_v1, submissionState),
        MCDR_FIELD(mcdr_present_receipt_v1, presentOutcome),
        MCDR_FIELD(mcdr_present_receipt_v1, dxgiStatus),
        MCDR_FIELD(mcdr_present_receipt_v1, flags),
        MCDR_FIELD(mcdr_present_receipt_v1, outputFrameCount),
        MCDR_FIELD(mcdr_present_receipt_v1, retirementState),
        MCDR_FIELD(mcdr_oit_capture_pipeline_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_capture_pipeline_v1, structSize),
        MCDR_FIELD(mcdr_oit_capture_pipeline_v1, shaderAbiVersion),
        MCDR_FIELD(mcdr_oit_capture_pipeline_v1, flags),
        MCDR_FIELD(mcdr_oit_capture_pipeline_v1, shaderContractHash),
        MCDR_FIELD(mcdr_oit_capture_pipeline_v1, programGeneration),
        MCDR_FIELD(mcdr_oit_execution_begin_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, structSize),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, identity),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, frameSerial),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, historyGeneration),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, programGeneration),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, maxEvents),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, maxScopes),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, maxPasses),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, maxResources),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, maxReceipts),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, flags),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, reserved0),
        MCDR_FIELD(mcdr_oit_execution_begin_v2, reserved1),
        MCDR_FIELD(mcdr_oit_execution_key_v2, producerId),
        MCDR_FIELD(mcdr_oit_execution_key_v2, producerScopeSerial),
        MCDR_FIELD(mcdr_oit_execution_key_v2, sourcePassId),
        MCDR_FIELD(mcdr_oit_execution_key_v2, sourceDrawId),
        MCDR_FIELD(mcdr_oit_execution_key_v2, parentSourceDrawId),
        MCDR_FIELD(mcdr_oit_execution_key_v2, producerKind),
        MCDR_FIELD(mcdr_oit_execution_key_v2, stage),
        MCDR_FIELD(mcdr_oit_execution_key_v2, commandOrdinal),
        MCDR_FIELD(mcdr_oit_execution_key_v2, reserved),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, structSize),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, identity),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, frameSerial),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, key),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, kind),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, expectedCommandFamily),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, programGeneration),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, shaderGeneration),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, pipeline),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, flags),
        MCDR_FIELD(mcdr_oit_execution_operation_begin_v1, reserved),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, structSize),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, kind),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, commandFamily),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, receiptId),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, nativeSequence),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, frameSerial),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, identity),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, key),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, pipelineIdentity),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, shaderGeneration),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, programGeneration),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, resourceVersion),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, packetId),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, readSetId),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, recordState),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, contentState),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, adapterState),
        MCDR_FIELD(mcdr_oit_execution_operation_receipt_v1, reserved),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, structSize),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, role),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, roleIndex),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, access),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, contentRequirement),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, firstSubresource),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, subresourceCount),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, object),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, resourceIdentity),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, ownerIdentity),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, ownerGeneration),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, contentVersion),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, resourceVersion),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, byteOffset),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, byteLength),
        MCDR_FIELD(mcdr_oit_execution_resource_v2, reserved),
        MCDR_FIELD(mcdr_oit_execution_event_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_event_v2, structSize),
        MCDR_FIELD(mcdr_oit_execution_event_v2, kind),
        MCDR_FIELD(mcdr_oit_execution_event_v2, flags),
        MCDR_FIELD(mcdr_oit_execution_event_v2, sequence),
        MCDR_FIELD(mcdr_oit_execution_event_v2, key),
        MCDR_FIELD(mcdr_oit_execution_event_v2, drawGroupId),
        MCDR_FIELD(mcdr_oit_execution_event_v2, shaderGeneration),
        MCDR_FIELD(mcdr_oit_execution_event_v2, programGeneration),
        MCDR_FIELD(mcdr_oit_execution_event_v2, pipelineIdentity),
        MCDR_FIELD(mcdr_oit_execution_event_v2, resourceCount),
        MCDR_FIELD(mcdr_oit_execution_event_v2, receiptCount),
        MCDR_FIELD(mcdr_oit_execution_event_v2, payloadKind),
        MCDR_FIELD(mcdr_oit_execution_event_v2, payloadBytes),
        MCDR_FIELD(mcdr_oit_execution_event_v2, resources),
        MCDR_FIELD(mcdr_oit_execution_event_v2, receiptIds),
        MCDR_FIELD(mcdr_oit_execution_event_v2, payload),
        MCDR_FIELD(mcdr_oit_execution_event_v2, reserved),
        MCDR_FIELD(mcdr_oit_normalization_pipeline_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_normalization_pipeline_v1, structSize),
        MCDR_FIELD(mcdr_oit_normalization_pipeline_v1, certificationVersion),
        MCDR_FIELD(mcdr_oit_normalization_pipeline_v1, targetCount),
        MCDR_FIELD(mcdr_oit_normalization_pipeline_v1, programGeneration),
        MCDR_FIELD(mcdr_oit_normalization_pipeline_v1, fragmentSourceHash),
        MCDR_FIELD(mcdr_oit_normalization_pipeline_v1, vertexSourceHash),
        MCDR_FIELD(mcdr_oit_normalization_pipeline_v1, contractHash),
        MCDR_FIELD(mcdr_oit_normalization_pipeline_v1, reserved),
        MCDR_FIELD(mcdr_oit_normalization_program_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_normalization_program_v1, structSize),
        MCDR_FIELD(mcdr_oit_normalization_program_v1, targetCount),
        MCDR_FIELD(mcdr_oit_normalization_program_v1, reserved),
        MCDR_FIELD(mcdr_oit_normalization_program_v1, programGeneration),
        MCDR_FIELD(mcdr_oit_normalization_program_v1, laneDxil),
        MCDR_FIELD(mcdr_oit_normalization_program_v1, laneDxilBytes),
        MCDR_FIELD(mcdr_oit_normalization_program_v1, averageDxil),
        MCDR_FIELD(mcdr_oit_normalization_program_v1, averageDxilBytes),
        MCDR_FIELD(mcdr_oit_execution_texture_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_texture_v1, structSize),
        MCDR_FIELD(mcdr_oit_execution_texture_v1, captureId),
        MCDR_FIELD(mcdr_oit_execution_texture_v1, texture),
        MCDR_FIELD(mcdr_oit_execution_texture_v1, ownerIdentity),
        MCDR_FIELD(mcdr_oit_execution_texture_v1, ownerGeneration),
        MCDR_FIELD(mcdr_oit_execution_texture_v1, contentVersion),
        MCDR_FIELD(mcdr_oit_execution_texture_v1, firstSubresource),
        MCDR_FIELD(mcdr_oit_execution_texture_v1, subresourceCount),
        MCDR_FIELD(mcdr_oit_execution_texture_v1, resourceVersion),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, structSize),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, targetCount),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, certificationVersion),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, certificationGeneration),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, pipeline),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, sampler),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, fragmentSourceHash),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, vertexSourceHash),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, contractHash),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, inputContentIds),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, outputWidth),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, outputHeight),
        MCDR_FIELD(mcdr_oit_execution_normalization_v2, reserved),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, structSize),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, targetCount),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, certificationVersion),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, certificationGeneration),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, pipelineOwnerIdentity),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, pipelineOwnerGeneration),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, samplerOwnerIdentity),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, samplerOwnerGeneration),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, pipeline),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, sampler),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, fragmentSourceHash),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, vertexSourceHash),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, contractHash),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, inputViewIdentities),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, normalized),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, corrected),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, outputWidth),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, outputHeight),
        MCDR_FIELD(mcdr_oit_execution_normalization_v1, reserved),
        MCDR_FIELD(mcdr_oit_execution_scope_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_scope_v2, structSize),
        MCDR_FIELD(mcdr_oit_execution_scope_v2, producerKind),
        MCDR_FIELD(mcdr_oit_execution_scope_v2, stage),
        MCDR_FIELD(mcdr_oit_execution_scope_v2, producerId),
        MCDR_FIELD(mcdr_oit_execution_scope_v2, producerScopeSerial),
        MCDR_FIELD(mcdr_oit_execution_scope_v2, sourcePassId),
        MCDR_FIELD(mcdr_oit_execution_scope_v2, eventCount),
        MCDR_FIELD(mcdr_oit_execution_scope_v2, receiptCount),
        MCDR_FIELD(mcdr_oit_execution_scope_v2, flags),
        MCDR_FIELD(mcdr_oit_execution_scope_v2, reserved),
        MCDR_FIELD(mcdr_oit_execution_pass_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_pass_v2, structSize),
        MCDR_FIELD(mcdr_oit_execution_pass_v2, completion),
        MCDR_FIELD(mcdr_oit_execution_pass_v2, flags),
        MCDR_FIELD(mcdr_oit_execution_pass_v2, sourcePassId),
        MCDR_FIELD(mcdr_oit_execution_pass_v2, eventCount),
        MCDR_FIELD(mcdr_oit_execution_pass_v2, reserved0),
        MCDR_FIELD(mcdr_oit_execution_pass_v2, reserved1),
        MCDR_FIELD(mcdr_oit_execution_pass_v2, reserved2),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, structSize),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, eventCount),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, scopeCount),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, passCount),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, resourceCount),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, receiptCount),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, flags),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, lastSequence),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, scopes),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, passes),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, missingMask),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, reserved0),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, reserved1),
        MCDR_FIELD(mcdr_oit_execution_manifest_v2, reserved2),
        MCDR_FIELD(mcdr_oit_execution_info_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_info_v2, structSize),
        MCDR_FIELD(mcdr_oit_execution_info_v2, state),
        MCDR_FIELD(mcdr_oit_execution_info_v2, missingMask),
        MCDR_FIELD(mcdr_oit_execution_info_v2, identity),
        MCDR_FIELD(mcdr_oit_execution_info_v2, streamId),
        MCDR_FIELD(mcdr_oit_execution_info_v2, frameSerial),
        MCDR_FIELD(mcdr_oit_execution_info_v2, programGeneration),
        MCDR_FIELD(mcdr_oit_execution_info_v2, eventCount),
        MCDR_FIELD(mcdr_oit_execution_info_v2, scopeCount),
        MCDR_FIELD(mcdr_oit_execution_info_v2, passCount),
        MCDR_FIELD(mcdr_oit_execution_info_v2, resourceCount),
        MCDR_FIELD(mcdr_oit_execution_info_v2, receiptCount),
        MCDR_FIELD(mcdr_oit_execution_info_v2, consumedReceiptCount),
        MCDR_FIELD(mcdr_oit_execution_info_v2, openScopeCount),
        MCDR_FIELD(mcdr_oit_execution_info_v2, openPassCount),
        MCDR_FIELD(mcdr_oit_execution_info_v2, firstIssueSequence),
        MCDR_FIELD(mcdr_oit_execution_terrain_receipt_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_terrain_receipt_v2, structSize),
        MCDR_FIELD(mcdr_oit_execution_terrain_receipt_v2, receipt),
        MCDR_FIELD(mcdr_oit_execution_terrain_receipt_v2, shaderGeneration),
        MCDR_FIELD(mcdr_oit_execution_terrain_receipt_v2, programGeneration),
        MCDR_FIELD(mcdr_oit_execution_terrain_receipt_v2, resourceVersion),
        MCDR_FIELD(mcdr_oit_execution_terrain_receipt_v2, readSetRegistered),
        MCDR_FIELD(mcdr_oit_execution_terrain_receipt_v2, reserved),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v2, structSize),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v2, state),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v2, missingMask),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v2, firstEventId),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v2, firstIssueSequence),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v2, receiptId),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v2, expected),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v2, observed),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v2, reserved0),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v2, reserved1),
        MCDR_FIELD(mcdr_oit_execution_begin_v1, structSize),
        MCDR_FIELD(mcdr_oit_execution_begin_v1, identity),
        MCDR_FIELD(mcdr_oit_execution_begin_v1, frameSerial),
        MCDR_FIELD(mcdr_oit_execution_begin_v1, historyGeneration),
        MCDR_FIELD(mcdr_oit_execution_begin_v1, programGeneration),
        MCDR_FIELD(mcdr_oit_execution_begin_v1, maxEvents),
        MCDR_FIELD(mcdr_oit_execution_begin_v1, maxScopes),
        MCDR_FIELD(mcdr_oit_execution_begin_v1, maxResourceReferences),
        MCDR_FIELD(mcdr_oit_execution_begin_v1, reserved),
        MCDR_FIELD(mcdr_oit_execution_info_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_info_v1, structSize),
        MCDR_FIELD(mcdr_oit_execution_info_v1, identity),
        MCDR_FIELD(mcdr_oit_execution_info_v1, streamId),
        MCDR_FIELD(mcdr_oit_execution_info_v1, frameSerial),
        MCDR_FIELD(mcdr_oit_execution_info_v1, programGeneration),
        MCDR_FIELD(mcdr_oit_execution_info_v1, nativeDrawCount),
        MCDR_FIELD(mcdr_oit_execution_info_v1, appendedDrawCount),
        MCDR_FIELD(mcdr_oit_execution_info_v1, eventCount),
        MCDR_FIELD(mcdr_oit_execution_info_v1, openScopeCount),
        MCDR_FIELD(mcdr_oit_execution_info_v1, state),
        MCDR_FIELD(mcdr_oit_execution_info_v1, unavailableReason),
        MCDR_FIELD(mcdr_oit_execution_info_v1, reserved),
        MCDR_FIELD(mcdr_oit_draw_receipt_query_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_draw_receipt_query_v1, structSize),
        MCDR_FIELD(mcdr_oit_draw_receipt_query_v1, receiptId),
        MCDR_FIELD(mcdr_oit_draw_receipt_query_v1, afterSequence),
        MCDR_FIELD(mcdr_oit_draw_receipt_query_v1, producerScopeSerial),
        MCDR_FIELD(mcdr_oit_draw_receipt_query_v1, reserved),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, structSize),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, receiptId),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, nativeSequence),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, frameSerial),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, producerScopeSerial),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, producerId),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, producerGeneration),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, shaderGeneration),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, programGeneration),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, identity),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, sectionBase),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, sectionCount),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, primitiveBase),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, primitiveCount),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, contentState),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, producerKind),
        MCDR_FIELD(mcdr_oit_draw_receipt_v1, reserved),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, structSize),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, stage),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, flags),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, frameSerial),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, producerId),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, producerScopeSerial),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, sourcePassId),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, sourceDrawId),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, parentSourceDrawId),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, identity),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, pipeline),
        MCDR_FIELD(mcdr_oit_terrain_stage_begin_v2, reserved),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, structSize),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, stage),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, flags),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, producerKind),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, contentState),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, reserved0),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, reserved1),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, receiptId),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, nativeSequence),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, frameSerial),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, producerId),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, producerScopeSerial),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, sourcePassId),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, sourceDrawId),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, parentSourceDrawId),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, identity),
        MCDR_FIELD(mcdr_oit_terrain_stage_receipt_v2, pipelineIdentity),
        MCDR_FIELD(mcdr_oit_terrain_replay_range_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_terrain_replay_range_v2, structSize),
        MCDR_FIELD(mcdr_oit_terrain_replay_range_v2, role),
        MCDR_FIELD(mcdr_oit_terrain_replay_range_v2, commandIndex),
        MCDR_FIELD(mcdr_oit_terrain_replay_range_v2, generation),
        MCDR_FIELD(mcdr_oit_terrain_replay_range_v2, ownerIdentity),
        MCDR_FIELD(mcdr_oit_terrain_replay_range_v2, byteOffset),
        MCDR_FIELD(mcdr_oit_terrain_replay_range_v2, byteLength),
        MCDR_FIELD(mcdr_oit_terrain_replay_range_v2, object),
        MCDR_FIELD(mcdr_oit_terrain_upload_read_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_terrain_upload_read_v2, structSize),
        MCDR_FIELD(mcdr_oit_terrain_upload_read_v2, role),
        MCDR_FIELD(mcdr_oit_terrain_upload_read_v2, reserved),
        MCDR_FIELD(mcdr_oit_terrain_upload_read_v2, object),
        MCDR_FIELD(mcdr_oit_terrain_upload_read_v2, byteOffset),
        MCDR_FIELD(mcdr_oit_terrain_upload_read_v2, byteLength),
        MCDR_FIELD(mcdr_oit_terrain_upload_read_v2, contentVersion),
        MCDR_FIELD(mcdr_oit_terrain_upload_read_v2, resourceVersion),
        MCDR_FIELD(mcdr_oit_terrain_upload_read_v2, ownerIdentity),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, structSize),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, flags),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, geometryRangeCount),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, uploadBindingCount),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, reserved0),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, reserved1),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, reserved2),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, receiptId),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, frameSerial),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, producerScopeSerial),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, programGeneration),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, resourceVersion),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, geometryRanges),
        MCDR_FIELD(mcdr_oit_terrain_replay_read_set_v2, uploadBindings),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, structSize),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, role),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, roleIndex),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, access),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, source),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, object),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, resourceIdentity),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, contentVersion),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, resourceVersion),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, firstSubresource),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, subresourceCount),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, byteOffset),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, byteLength),
        MCDR_FIELD(mcdr_oit_execution_resource_v1, reserved),
        MCDR_FIELD(mcdr_oit_execution_event_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_event_v1, structSize),
        MCDR_FIELD(mcdr_oit_execution_event_v1, sequence),
        MCDR_FIELD(mcdr_oit_execution_event_v1, kind),
        MCDR_FIELD(mcdr_oit_execution_event_v1, producerKind),
        MCDR_FIELD(mcdr_oit_execution_event_v1, stage),
        MCDR_FIELD(mcdr_oit_execution_event_v1, flags),
        MCDR_FIELD(mcdr_oit_execution_event_v1, producerId),
        MCDR_FIELD(mcdr_oit_execution_event_v1, producerScopeSerial),
        MCDR_FIELD(mcdr_oit_execution_event_v1, sourcePassId),
        MCDR_FIELD(mcdr_oit_execution_event_v1, sourceDrawId),
        MCDR_FIELD(mcdr_oit_execution_event_v1, drawGroupId),
        MCDR_FIELD(mcdr_oit_execution_event_v1, drawReceiptId),
        MCDR_FIELD(mcdr_oit_execution_event_v1, shaderGeneration),
        MCDR_FIELD(mcdr_oit_execution_event_v1, programGeneration),
        MCDR_FIELD(mcdr_oit_execution_event_v1, resourceCount),
        MCDR_FIELD(mcdr_oit_execution_event_v1, reserved),
        MCDR_FIELD(mcdr_oit_execution_event_v1, resources),
        MCDR_FIELD(mcdr_oit_execution_event_v1, reservedTail),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, structSize),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, producerId),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, producerScopeSerial),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, sourcePassId),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, producerKind),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, stage),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, eventCount),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, nativeDrawCount),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, flags),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, reserved),
        MCDR_FIELD(mcdr_oit_execution_scope_v1, reservedTail),
        MCDR_FIELD(mcdr_oit_execution_manifest_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_manifest_v1, structSize),
        MCDR_FIELD(mcdr_oit_execution_manifest_v1, eventCount),
        MCDR_FIELD(mcdr_oit_execution_manifest_v1, scopeCount),
        MCDR_FIELD(mcdr_oit_execution_manifest_v1, nativeDrawCount),
        MCDR_FIELD(mcdr_oit_execution_manifest_v1, flags),
        MCDR_FIELD(mcdr_oit_execution_manifest_v1, lastSequence),
        MCDR_FIELD(mcdr_oit_execution_manifest_v1, scopes),
        MCDR_FIELD(mcdr_oit_execution_manifest_v1, reserved),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v1, structSize),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v1, state),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v1, unavailableReason),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v1, firstEventId),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v1, drawReceiptId),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v1, expected),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v1, observed),
        MCDR_FIELD(mcdr_oit_execution_seal_result_v1, reserved),
        MCDR_FIELD(mcdr_oit_begin_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_begin_v1, structSize),
        MCDR_FIELD(mcdr_oit_begin_v1, identity),
        MCDR_FIELD(mcdr_oit_begin_v1, historyGeneration),
        MCDR_FIELD(mcdr_oit_begin_v1, previousRealFrameId),
        MCDR_FIELD(mcdr_oit_begin_v1, programGeneration),
        MCDR_FIELD(mcdr_oit_begin_v1, totalBudgetBytes),
        MCDR_FIELD(mcdr_oit_begin_v1, maxRecords),
        MCDR_FIELD(mcdr_oit_begin_v1, maxRecordsPerPixel),
        MCDR_FIELD(mcdr_oit_begin_v1, width),
        MCDR_FIELD(mcdr_oit_begin_v1, height),
        MCDR_FIELD(mcdr_oit_begin_v1, colorEncoding),
        MCDR_FIELD(mcdr_oit_begin_v1, flags),
        MCDR_FIELD(mcdr_oit_begin_v1, preExposure),
        MCDR_FIELD(mcdr_oit_begin_v1, reserved),
        MCDR_FIELD(mcdr_oit_begin_v1, maxSourceRows),
        MCDR_FIELD(mcdr_oit_begin_v1, maxPrimitiveRows),
        MCDR_FIELD(mcdr_oit_begin_v1, maxPreviousRows),
        MCDR_FIELD(mcdr_oit_begin_v1, reservedMetadata),
        MCDR_FIELD(mcdr_oit_begin_v1, opaqueColor),
        MCDR_FIELD(mcdr_oit_begin_v1, opaqueDepth),
        MCDR_FIELD(mcdr_oit_begin_v1, opaqueMotion),
        MCDR_FIELD(mcdr_oit_begin_v2, base),
        MCDR_FIELD(mcdr_oit_begin_v2, maxTerrainSectionRows),
        MCDR_FIELD(mcdr_oit_begin_v2, maxTerrainPrimitives),
        MCDR_FIELD(mcdr_oit_begin_v2, previousViewProjection),
        MCDR_FIELD(mcdr_oit_begin_v2, previousOrigin),
        MCDR_FIELD(mcdr_oit_begin_v2, previousCameraValid),
        MCDR_FIELD(mcdr_oit_begin_v2, reserved),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, surfaceId),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, geometryGeneration),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, materialId),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, materialGeneration),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, textureGeneration),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, primitiveBase),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, primitiveCount),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, sectionX),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, sectionY),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, sectionZ),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, flags),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, vertexBase),
        MCDR_FIELD(mcdr_oit_terrain_section_v2, vertexCount),
        MCDR_FIELD(mcdr_oit_terrain_metadata_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_terrain_metadata_v2, structSize),
        MCDR_FIELD(mcdr_oit_terrain_metadata_v2, source),
        MCDR_FIELD(mcdr_oit_terrain_metadata_v2, sectionCount),
        MCDR_FIELD(mcdr_oit_terrain_metadata_v2, primitiveCount),
        MCDR_FIELD(mcdr_oit_terrain_metadata_v2, sections),
        MCDR_FIELD(mcdr_oit_terrain_metadata_v2, vertexBuffer),
        MCDR_FIELD(mcdr_oit_terrain_metadata_v2, vertexOffset),
        MCDR_FIELD(mcdr_oit_terrain_metadata_v2, vertexBytes),
        MCDR_FIELD(mcdr_oit_terrain_draw_v2, schemaVersion),
        MCDR_FIELD(mcdr_oit_terrain_draw_v2, structSize),
        MCDR_FIELD(mcdr_oit_terrain_draw_v2, producerScopeSerial),
        MCDR_FIELD(mcdr_terrain_tile_stage_v2, schemaVersion),
        MCDR_FIELD(mcdr_terrain_tile_stage_v2, structSize),
        MCDR_FIELD(mcdr_terrain_tile_stage_v2, shaderAbiVersion),
        MCDR_FIELD(mcdr_terrain_tile_stage_v2, stage),
        MCDR_FIELD(mcdr_terrain_tile_stage_v2, quadsPerMeshlet),
        MCDR_FIELD(mcdr_terrain_tile_stage_v2, addressingVersion),
        MCDR_FIELD(mcdr_terrain_tile_stage_v2, shaderContractHash),
        MCDR_FIELD(mcdr_terrain_tile_stage_v2, programGeneration),
        MCDR_FIELD(mcdr_oit_source_v1, producerId),
        MCDR_FIELD(mcdr_oit_source_v1, producerGeneration),
        MCDR_FIELD(mcdr_oit_source_v1, shaderGeneration),
        MCDR_FIELD(mcdr_oit_source_v1, producerKind),
        MCDR_FIELD(mcdr_oit_source_v1, validity),
        MCDR_FIELD(mcdr_oit_primitive_v1, surfaceId),
        MCDR_FIELD(mcdr_oit_primitive_v1, geometryGeneration),
        MCDR_FIELD(mcdr_oit_primitive_v1, materialId),
        MCDR_FIELD(mcdr_oit_primitive_v1, materialGeneration),
        MCDR_FIELD(mcdr_oit_primitive_v1, textureGeneration),
        MCDR_FIELD(mcdr_oit_primitive_v1, sourceIndex),
        MCDR_FIELD(mcdr_oit_primitive_v1, previousIndex),
        MCDR_FIELD(mcdr_oit_primitive_v1, flags),
        MCDR_FIELD(mcdr_oit_primitive_v1, reserved),
        MCDR_FIELD(mcdr_oit_previous_triangle_v1, clip),
        MCDR_FIELD(mcdr_oit_previous_triangle_v1, materialUv),
        MCDR_FIELD(mcdr_oit_previous_triangle_v1, flags),
        MCDR_FIELD(mcdr_oit_previous_triangle_v1, reserved),
        MCDR_FIELD(mcdr_oit_metadata_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_metadata_v1, structSize),
        MCDR_FIELD(mcdr_oit_metadata_v1, sourceCount),
        MCDR_FIELD(mcdr_oit_metadata_v1, primitiveCount),
        MCDR_FIELD(mcdr_oit_metadata_v1, previousCount),
        MCDR_FIELD(mcdr_oit_metadata_v1, sectionCount),
        MCDR_FIELD(mcdr_oit_metadata_v1, sources),
        MCDR_FIELD(mcdr_oit_metadata_v1, primitives),
        MCDR_FIELD(mcdr_oit_metadata_v1, previous),
        MCDR_FIELD(mcdr_oit_metadata_v1, sectionPrimitiveBases),
        MCDR_FIELD(mcdr_oit_draw_v1, schemaVersion),
        MCDR_FIELD(mcdr_oit_draw_v1, structSize),
        MCDR_FIELD(mcdr_oit_draw_v1, sourceIndex),
        MCDR_FIELD(mcdr_oit_draw_v1, primitiveBase),
        MCDR_FIELD(mcdr_oit_draw_v1, primitiveCount),
        MCDR_FIELD(mcdr_oit_draw_v1, flags),
        MCDR_FIELD(mcdr_oit_draw_v1, producerScopeSerial),
        MCDR_FIELD(mcdr_oit_draw_v1, primitivesPerInstance),
        MCDR_FIELD(mcdr_oit_draw_v1, instanceCount),
        MCDR_FIELD(mcdr_oit_draw_v1, firstInstance),
        MCDR_FIELD(mcdr_oit_draw_v1, reserved),
        MCDR_FIELD(mcdr_oit_resolve_v3, schemaVersion),
        MCDR_FIELD(mcdr_oit_resolve_v3, structSize),
        MCDR_FIELD(mcdr_oit_resolve_v3, identity),
        MCDR_FIELD(mcdr_oit_resolve_v3, frame),
        MCDR_FIELD(mcdr_oit_resolve_v3, currentTransparency),
        MCDR_FIELD(mcdr_oit_resolve_v3, unrecordedCoverage),
        MCDR_FIELD(mcdr_oit_resolve_v3, correctedTransparency),
        MCDR_FIELD(mcdr_oit_resolve_v3, statusOutput),
        MCDR_FIELD(mcdr_oit_resolve_v3, flags),
        MCDR_FIELD(mcdr_oit_resolve_v3, reserved),
        MCDR_FIELD(mcdr_oit_resolve_v4, base),
        MCDR_FIELD(mcdr_oit_resolve_v4, terrainBatches),
        MCDR_FIELD(mcdr_oit_resolve_v4, terrainBatchCount),
        MCDR_FIELD(mcdr_oit_resolve_v4, reservedV4),
        MCDR_FIELD(mcdr_provider_mask_input, schemaVersion),
        MCDR_FIELD(mcdr_provider_mask_input, structSize),
        MCDR_FIELD(mcdr_provider_mask_input, identity),
        MCDR_FIELD(mcdr_provider_mask_input, historyGeneration),
        MCDR_FIELD(mcdr_provider_mask_input, previousRealFrameId),
        MCDR_FIELD(mcdr_provider_mask_input, color),
        MCDR_FIELD(mcdr_provider_mask_input, depth),
        MCDR_FIELD(mcdr_provider_mask_input, motion),
        MCDR_FIELD(mcdr_provider_mask_input, source0),
        MCDR_FIELD(mcdr_provider_mask_input, source1),
        MCDR_FIELD(mcdr_provider_mask_input, output),
        MCDR_FIELD(mcdr_provider_mask_input, compositionOutput),
        MCDR_FIELD(mcdr_provider_mask_input, width),
        MCDR_FIELD(mcdr_provider_mask_input, height),
        MCDR_FIELD(mcdr_provider_mask_input, outputFlags),
        MCDR_FIELD(mcdr_provider_mask_input, inputFlags),
        MCDR_FIELD(mcdr_provider_mask_input, historyJitterX),
        MCDR_FIELD(mcdr_provider_mask_input, historyJitterY),
        MCDR_FIELD(mcdr_provider_mask_input, currentToPrevious),
        MCDR_FIELD(mcdr_frame_identity, worldEpoch),
        MCDR_FIELD(mcdr_display_layer_input, structSize),
        MCDR_FIELD(mcdr_display_layer_input, kind),
        MCDR_FIELD(mcdr_display_layer_input, flags),
        MCDR_FIELD(mcdr_display_layer_input, composition),
        MCDR_FIELD(mcdr_display_layer_input, width),
        MCDR_FIELD(mcdr_display_layer_input, height),
        MCDR_FIELD(mcdr_display_layer_input, depth),
        MCDR_FIELD(mcdr_display_layer_input, motion),
        MCDR_FIELD(mcdr_display_layer_input, coverage),
        MCDR_FIELD(mcdr_display_layer_input, current),
        MCDR_FIELD(mcdr_display_layer_input, previous),
        MCDR_FIELD(mcdr_display_post_input, structSize),
        MCDR_FIELD(mcdr_display_post_input, kind),
        MCDR_FIELD(mcdr_display_post_input, flags),
        MCDR_FIELD(mcdr_display_post_input, reserved),
        MCDR_FIELD(mcdr_display_post_input, current),
        MCDR_FIELD(mcdr_display_post_input, previous),
        MCDR_FIELD(mcdr_display_frame_input, schemaVersion),
        MCDR_FIELD(mcdr_display_frame_input, structSize),
        MCDR_FIELD(mcdr_display_frame_input, identity),
        MCDR_FIELD(mcdr_display_frame_input, historyGeneration),
        MCDR_FIELD(mcdr_display_frame_input, previousRealFrameId),
        MCDR_FIELD(mcdr_display_frame_input, width),
        MCDR_FIELD(mcdr_display_frame_input, height),
        MCDR_FIELD(mcdr_display_frame_input, flags),
        MCDR_FIELD(mcdr_display_frame_input, resetReasons),
        MCDR_FIELD(mcdr_display_frame_input, frameDeltaMs),
        MCDR_FIELD(mcdr_display_frame_input, viewSpaceToMeters),
        MCDR_FIELD(mcdr_display_frame_input, frameGenerationBackend),
        MCDR_FIELD(mcdr_display_frame_input, frameGenerationMode),
        MCDR_FIELD(mcdr_display_frame_input, frameGenerationFrames),
        MCDR_FIELD(mcdr_display_frame_input, colorEncoding),
        MCDR_FIELD(mcdr_display_frame_input, scene),
        MCDR_FIELD(mcdr_display_frame_input, ui),
        MCDR_FIELD(mcdr_display_frame_input, current),
        MCDR_FIELD(mcdr_display_frame_input, previous),
        MCDR_FIELD(mcdr_display_frame_input, layerCount),
        MCDR_FIELD(mcdr_display_frame_input, layerStride),
        MCDR_FIELD(mcdr_display_frame_input, layers),
        MCDR_FIELD(mcdr_display_frame_input, postCount),
        MCDR_FIELD(mcdr_display_frame_input, postStride),
        MCDR_FIELD(mcdr_display_frame_input, posts),
        MCDR_FIELD(mcdr_frame_identity, realFrameId),
        MCDR_FIELD(mcdr_frame_identity, configVersion),
        MCDR_FIELD(mcdr_frame_identity, resourceVersion),
        MCDR_FIELD(mcdr_raster_camera, origin),
        MCDR_FIELD(mcdr_raster_camera, jitterX),
        MCDR_FIELD(mcdr_raster_camera, jitterY),
        MCDR_FIELD(mcdr_raster_camera, projection),
        MCDR_FIELD(mcdr_raster_camera, rasterProjection),
        MCDR_FIELD(mcdr_raster_camera, viewRotation),
        MCDR_FIELD(mcdr_raster_camera, nearPlane),
        MCDR_FIELD(mcdr_raster_camera, farPlane),
        MCDR_FIELD(mcdr_raster_camera, verticalFovRadians),
        MCDR_FIELD(mcdr_raster_camera, flags),
        MCDR_FIELD(mcdr_raster_camera, yaw),
        MCDR_FIELD(mcdr_raster_camera, pitch),
        MCDR_FIELD(mcdr_frame_settings, antiAliasing),
        MCDR_FIELD(mcdr_frame_settings, reconstructionBackend),
        MCDR_FIELD(mcdr_frame_settings, renderScalePercent),
        MCDR_FIELD(mcdr_frame_settings, jitterEnabled),
        MCDR_FIELD(mcdr_frame_settings, jitterSamples),
        MCDR_FIELD(mcdr_frame_settings, providerSharpness),
        MCDR_FIELD(mcdr_frame_settings, providerMasks),
        MCDR_FIELD(mcdr_frame_settings, dlssQuality),
        MCDR_FIELD(mcdr_frame_settings, dlssPreset),
        MCDR_FIELD(mcdr_frame_settings, frameGenerationBackend),
        MCDR_FIELD(mcdr_frame_settings, frameGenerationMode),
        MCDR_FIELD(mcdr_frame_settings, frameGenerationFrames),
        MCDR_FIELD(mcdr_frame_settings, taaBlend),
        MCDR_FIELD(mcdr_frame_settings, taaSharpness),
        MCDR_FIELD(mcdr_vanilla_frame_input, schemaVersion),
        MCDR_FIELD(mcdr_vanilla_frame_input, structSize),
        MCDR_FIELD(mcdr_vanilla_frame_input, identity),
        MCDR_FIELD(mcdr_vanilla_frame_input, historyGeneration),
        MCDR_FIELD(mcdr_vanilla_frame_input, frameTimeNanos),
        MCDR_FIELD(mcdr_vanilla_frame_input, jitterPhaseIndex),
        MCDR_FIELD(mcdr_vanilla_frame_input, renderWidth),
        MCDR_FIELD(mcdr_vanilla_frame_input, renderHeight),
        MCDR_FIELD(mcdr_vanilla_frame_input, displayWidth),
        MCDR_FIELD(mcdr_vanilla_frame_input, displayHeight),
        MCDR_FIELD(mcdr_vanilla_frame_input, flags),
        MCDR_FIELD(mcdr_vanilla_frame_input, colorEncoding),
        MCDR_FIELD(mcdr_vanilla_frame_input, depthConvention),
        MCDR_FIELD(mcdr_vanilla_frame_input, motionConvention),
        MCDR_FIELD(mcdr_vanilla_frame_input, frameDeltaMs),
        MCDR_FIELD(mcdr_vanilla_frame_input, preExposure),
        MCDR_FIELD(mcdr_vanilla_frame_input, motionScaleX),
        MCDR_FIELD(mcdr_vanilla_frame_input, motionScaleY),
        MCDR_FIELD(mcdr_vanilla_frame_input, viewSpaceToMeters),
        MCDR_FIELD(mcdr_vanilla_frame_input, reserved),
        MCDR_FIELD(mcdr_vanilla_frame_input, previousRealFrameId),
        MCDR_FIELD(mcdr_vanilla_frame_input, settings),
        MCDR_FIELD(mcdr_vanilla_frame_input, current),
        MCDR_FIELD(mcdr_vanilla_frame_input, previous),
        MCDR_FIELD(mcdr_world_output_input, schemaVersion),
        MCDR_FIELD(mcdr_world_output_input, structSize),
        MCDR_FIELD(mcdr_world_output_input, identity),
        MCDR_FIELD(mcdr_world_output_input, motionCoverage),
        MCDR_FIELD(mcdr_world_output_input, motionGuideCoverage),
        MCDR_FIELD(mcdr_world_output_input, color),
        MCDR_FIELD(mcdr_world_output_input, depth),
        MCDR_FIELD(mcdr_world_output_input, motion),
        MCDR_FIELD(mcdr_world_output_input, exposure),
        MCDR_FIELD(mcdr_world_output_input, reconstructionOutput),
        MCDR_FIELD(mcdr_world_output_input, scene),
        MCDR_FIELD(mcdr_world_output_input, masks),
        MCDR_FIELD(mcdr_world_output_input, dlssPostSharpness),
        MCDR_FIELD(mcdr_world_output_input, reserved),
        MCDR_FIELD(mcdr_world_output_input, sourceStage),
        MCDR_FIELD(mcdr_world_output_input, reservedStage),
        MCDR_FIELD(mcdr_prepared_binding, object),
        MCDR_FIELD(mcdr_prepared_binding, firstSubresource),
        MCDR_FIELD(mcdr_prepared_binding, subresourceCount),
        MCDR_FIELD(mcdr_prepared_frame, schemaVersion),
        MCDR_FIELD(mcdr_prepared_frame, structSize),
        MCDR_FIELD(mcdr_prepared_frame, identity),
        MCDR_FIELD(mcdr_prepared_frame, historyGeneration),
        MCDR_FIELD(mcdr_prepared_frame, availability),
        MCDR_FIELD(mcdr_prepared_frame, required),
        MCDR_FIELD(mcdr_prepared_frame, consumer),
        MCDR_FIELD(mcdr_prepared_frame, motionCoverage),
        MCDR_FIELD(mcdr_prepared_frame, motionGuideCoverage),
        MCDR_FIELD(mcdr_prepared_frame, resetReasons),
        MCDR_FIELD(mcdr_prepared_frame, color),
        MCDR_FIELD(mcdr_prepared_frame, depth),
        MCDR_FIELD(mcdr_prepared_frame, motion),
        MCDR_FIELD(mcdr_prepared_frame, exposure),
        MCDR_FIELD(mcdr_prepared_frame, reactive),
        MCDR_FIELD(mcdr_prepared_frame, composition),
        MCDR_FIELD(mcdr_prepared_frame, responsive),
        MCDR_FIELD(mcdr_reconstruction_request, schemaVersion),
        MCDR_FIELD(mcdr_reconstruction_request, structSize),
        MCDR_FIELD(mcdr_reconstruction_request, identity),
        MCDR_FIELD(mcdr_reconstruction_request, consumer),
        MCDR_FIELD(mcdr_reconstruction_request, flags),
        MCDR_FIELD(mcdr_reconstruction_request, output),
        MCDR_FIELD(mcdr_vanilla_resource_use, object),
        MCDR_FIELD(mcdr_vanilla_resource_use, version),
        MCDR_FIELD(mcdr_vanilla_resource_use, firstSubresource),
        MCDR_FIELD(mcdr_vanilla_resource_use, subresourceCount),
        MCDR_FIELD(mcdr_vanilla_resource_use, access),
        MCDR_FIELD(mcdr_vanilla_resource_use, requiredState),
        MCDR_FIELD(mcdr_vanilla_resource_use, finalState),
        MCDR_FIELD(mcdr_vanilla_resource_use, externalState),
        MCDR_FIELD(mcdr_binding_slot, type),
        MCDR_FIELD(mcdr_binding_slot, texelFormat),
        MCDR_FIELD(mcdr_binding_slot, buffer),
        MCDR_FIELD(mcdr_binding_slot, view),
        MCDR_FIELD(mcdr_binding_slot, offset),
        MCDR_FIELD(mcdr_binding_slot, length),

        MCDR_FIELD(mcdr_partition_draw, indexBuffer),
        MCDR_FIELD(mcdr_partition_draw, vertexBuffer),
        MCDR_FIELD(mcdr_partition_draw, vertexOffset),
        MCDR_FIELD(mcdr_partition_draw, indexType),
        MCDR_FIELD(mcdr_partition_draw, indexCount),
        MCDR_FIELD(mcdr_partition_draw, firstIndex),
        MCDR_FIELD(mcdr_partition_draw, baseVertex),
        MCDR_FIELD(mcdr_partition_draw, vertexSlot),
        MCDR_FIELD(mcdr_partition_draw, vertexStride),
        MCDR_FIELD(mcdr_partition_draw, _pad0),

        MCDR_FIELD(mcdr_color_target_desc, format),
        MCDR_FIELD(mcdr_color_target_desc, writeMask),
        MCDR_FIELD(mcdr_color_target_desc, blendEnabled),
        MCDR_FIELD(mcdr_color_target_desc, srcColor),
        MCDR_FIELD(mcdr_color_target_desc, dstColor),
        MCDR_FIELD(mcdr_color_target_desc, colorOp),
        MCDR_FIELD(mcdr_color_target_desc, srcAlpha),
        MCDR_FIELD(mcdr_color_target_desc, dstAlpha),
        MCDR_FIELD(mcdr_color_target_desc, alphaOp),

        MCDR_FIELD(mcdr_depth_state_desc, depthFormat),
        MCDR_FIELD(mcdr_depth_state_desc, depthWrite),
        MCDR_FIELD(mcdr_depth_state_desc, depthCompareOp),

        MCDR_FIELD(mcdr_input_element_desc, location),
        MCDR_FIELD(mcdr_input_element_desc, binding),
        MCDR_FIELD(mcdr_input_element_desc, format),
        MCDR_FIELD(mcdr_input_element_desc, offset),
        MCDR_FIELD(mcdr_input_element_desc, stride),
        MCDR_FIELD(mcdr_input_element_desc, stepRate),
        MCDR_FIELD(mcdr_input_element_desc, _pad0),
        MCDR_FIELD(mcdr_input_element_desc, semanticName),

        MCDR_FIELD(mcdr_pipeline_binding_desc, type),
        MCDR_FIELD(mcdr_pipeline_binding_desc, reg),

        MCDR_FIELD(mcdr_pipeline_desc, vsData),
        MCDR_FIELD(mcdr_pipeline_desc, vsLen),
        MCDR_FIELD(mcdr_pipeline_desc, _pad0),
        MCDR_FIELD(mcdr_pipeline_desc, psData),
        MCDR_FIELD(mcdr_pipeline_desc, psLen),
        MCDR_FIELD(mcdr_pipeline_desc, colorCount),
        MCDR_FIELD(mcdr_pipeline_desc, colorTargets),
        MCDR_FIELD(mcdr_pipeline_desc, hasDepth),
        MCDR_FIELD(mcdr_pipeline_desc, pipelineFlags),
        MCDR_FIELD(mcdr_pipeline_desc, depth),
        MCDR_FIELD(mcdr_pipeline_desc, topology),
        MCDR_FIELD(mcdr_pipeline_desc, cullEnabled),
        MCDR_FIELD(mcdr_pipeline_desc, polygonMode),
        MCDR_FIELD(mcdr_pipeline_desc, inputElementCount),
        MCDR_FIELD(mcdr_pipeline_desc, inputElements),
        MCDR_FIELD(mcdr_pipeline_desc, bindingCount),
        MCDR_FIELD(mcdr_pipeline_desc, _pad2),
        MCDR_FIELD(mcdr_pipeline_desc, bindings),

        // P72 mesh-shader pipeline. The Java mirror must carry exactly these
        // fields with these sizes, or MCDRLayout.verify() aborts at load time.
        MCDR_FIELD(mcdr_mesh_pipeline_desc, asData),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, asLen),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, _pad0),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, msData),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, msLen),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, _pad1),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, psData),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, psLen),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, colorCount),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, colorTargets),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, hasDepth),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, _pad2),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, depth),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, cullEnabled),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, polygonMode),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, bindingCount),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, _pad3),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, bindings),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, rasterMotion),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, _pad4),

        // P271 SR 抽象层的两张新表。与上面同理：Kotlin 的 MCDR_Layout.kt 逐字段镜像，
        // verify() 双向比对，任何一侧漏字段都会在设备创建时 abort。字段名必须与
        // mcdr_abi.h 的 typedef 逐字一致（含 _pad1）。
        MCDR_FIELD(mcdr_sr_caps, backend),
        MCDR_FIELD(mcdr_sr_caps, available),
        MCDR_FIELD(mcdr_sr_caps, coupledDenoise),
        MCDR_FIELD(mcdr_sr_caps, needsExposure),
        MCDR_FIELD(mcdr_sr_caps, takesMasks),
        MCDR_FIELD(mcdr_sr_caps, externalHeap),
        MCDR_FIELD(mcdr_sr_caps, jitterPhaseCount),
        MCDR_FIELD(mcdr_sr_caps, minRenderWidth),
        MCDR_FIELD(mcdr_sr_caps, minRenderHeight),
        MCDR_FIELD(mcdr_sr_caps, maxRenderWidth),
        MCDR_FIELD(mcdr_sr_caps, maxRenderHeight),
        MCDR_FIELD(mcdr_sr_caps, nativeAA),
        MCDR_FIELD(mcdr_sr_caps, reserved),
        MCDR_FIELD(mcdr_sr_caps, note),

        MCDR_FIELD(mcdr_sr_dispatch, renderWidth),
        MCDR_FIELD(mcdr_sr_dispatch, renderHeight),
        MCDR_FIELD(mcdr_sr_dispatch, displayWidth),
        MCDR_FIELD(mcdr_sr_dispatch, displayHeight),
        MCDR_FIELD(mcdr_sr_dispatch, jitterOffsetX),
        MCDR_FIELD(mcdr_sr_dispatch, jitterOffsetY),
        MCDR_FIELD(mcdr_sr_dispatch, motionVectorScaleX),
        MCDR_FIELD(mcdr_sr_dispatch, motionVectorScaleY),
        MCDR_FIELD(mcdr_sr_dispatch, preExposure),
        MCDR_FIELD(mcdr_sr_dispatch, frameTimeDeltaMs),
        MCDR_FIELD(mcdr_sr_dispatch, cameraNear),
        MCDR_FIELD(mcdr_sr_dispatch, cameraFar),
        MCDR_FIELD(mcdr_sr_dispatch, cameraVerticalFov),
        MCDR_FIELD(mcdr_sr_dispatch, sharpness),
        MCDR_FIELD(mcdr_sr_dispatch, reset),
        MCDR_FIELD(mcdr_sr_dispatch, hdrInput),
        MCDR_FIELD(mcdr_sr_dispatch, backend),
        MCDR_FIELD(mcdr_sr_dispatch, flags),
        MCDR_FIELD(mcdr_sr_dispatch, pColorInput),
        MCDR_FIELD(mcdr_sr_dispatch, pColorOutput),
        MCDR_FIELD(mcdr_sr_dispatch, pDepth),
        MCDR_FIELD(mcdr_sr_dispatch, pMotionVectors),
        MCDR_FIELD(mcdr_sr_dispatch, pReactiveMask),
        MCDR_FIELD(mcdr_sr_dispatch, pExposure),
        MCDR_FIELD(mcdr_sr_dispatch, depthFormatOrdinal),
        MCDR_FIELD(mcdr_sr_dispatch, cameraAspectRatio),
        MCDR_FIELD(mcdr_sr_dispatch, viewSpaceToMetersFactor),
        MCDR_FIELD(mcdr_sr_dispatch, _pad2),
        MCDR_FIELD(mcdr_sr_dispatch, pTransparencyAndComposition),
        MCDR_FIELD(mcdr_sr_dispatch, pCamera),

        // P295 FG 抽象层的两张新表。与 SR 同理：Kotlin 的 MCDR_Layout.kt 逐字段镜像，
        // verify() 双向比对，任何一侧漏字段都会在设备创建时 abort。字段名必须与
        // mcdr_abi.h 的 typedef 逐字一致（含两个 _pad）。
        MCDR_FIELD(mcdr_fg_caps, backend),
        MCDR_FIELD(mcdr_fg_caps, available),
        MCDR_FIELD(mcdr_fg_caps, runtimeLoaded),
        MCDR_FIELD(mcdr_fg_caps, deviceSupported),
        MCDR_FIELD(mcdr_fg_caps, chainCompatible),
        MCDR_FIELD(mcdr_fg_caps, needsReflex),
        MCDR_FIELD(mcdr_fg_caps, needsUiResource),
        MCDR_FIELD(mcdr_fg_caps, needsHudless),
        MCDR_FIELD(mcdr_fg_caps, transitionsInputs),
        MCDR_FIELD(mcdr_fg_caps, maxGeneratedFrames),
        MCDR_FIELD(mcdr_fg_caps, chainOwner),
        MCDR_FIELD(mcdr_fg_caps, reserved),
        MCDR_FIELD(mcdr_fg_caps, note),
        MCDR_FIELD(mcdr_fg_caps, latencyProvider),

        MCDR_FIELD(mcdr_fg_dispatch, displayWidth),
        MCDR_FIELD(mcdr_fg_dispatch, displayHeight),
        MCDR_FIELD(mcdr_fg_dispatch, backend),
        MCDR_FIELD(mcdr_fg_dispatch, mode),
        MCDR_FIELD(mcdr_fg_dispatch, numFrames),
        MCDR_FIELD(mcdr_fg_dispatch, reset),
        MCDR_FIELD(mcdr_fg_dispatch, appFrameId),
        MCDR_FIELD(mcdr_fg_dispatch, backendFrameId),
        MCDR_FIELD(mcdr_fg_dispatch, presentId),
        MCDR_FIELD(mcdr_fg_dispatch, pBackbuffer),
        MCDR_FIELD(mcdr_fg_dispatch, pHudlessColor),
        MCDR_FIELD(mcdr_fg_dispatch, pDepth),
        MCDR_FIELD(mcdr_fg_dispatch, pMotionVectors),
        MCDR_FIELD(mcdr_fg_dispatch, pUiColor),
        MCDR_FIELD(mcdr_fg_dispatch, _pad2),
    };
    return table;
}

#undef MCDR_FIELD

}  // namespace

}  // namespace mcdr::abi

extern "C" {

uint32_t MCDR_CALL mcdrABIVersion(void) { return MCDR_ABI_VERSION; }

void MCDR_CALL mcdrABILayout(const mcdr_abi_field** fields, int32_t* count) {
    const std::vector<mcdr_abi_field>& table = mcdr::abi::layoutTable();
    if (fields) {
        *fields = table.data();
    }
    if (count) {
        *count = static_cast<int32_t>(table.size());
    }
}

int32_t MCDR_CALL mcdrLastError(char* buf, int32_t cap) {
    const std::string message = mcdr::abi::lastErrorText();
    if (message.empty()) {
        return 0;
    }
    return mcdr::abi::writeString(message, buf, cap);
}

void MCDR_CALL mcdrSetLogVerbose(int32_t enabled) {
    // Mirrors the old getenv("DX12_LOG_VERBOSE") read, which JNI performed
    // inside dx12CreateDevice(). The decision now belongs to the Java side so
    // the native layer no longer reaches into the process environment.
    d3d12_mc::setLogLevel(enabled ? 3 : 1);
}

void MCDR_CALL mcdrSetLogLevel(int32_t level) {
    // 0=error, 1=warning, 2=info, 3=debug. d3d12_mc::setLogLevel() clamps to 0..3.
    // mcdrSetLogVerbose() above keeps its original 1-vs-3 mapping.
    d3d12_mc::setLogLevel(level);
}

void MCDR_CALL mcdrSetShutdownTraceAlways(int32_t always) {
    // P184 补记：配置项 `[logging] shutdownTrace`。1（默认）= destroySurface/destroyDevice 的
    // 进度标记无视 logLevel 恒定输出（退出卡死时唯一的线索）；0 = 它们退回普通 INFO。
    // 日志语义（INFO）不变，只是"是否被 logLevel 过滤"由 Java 侧决定。
    d3d12_mc::setShutdownTraceAlways(always != 0);
}

int32_t MCDR_CALL mcdrSetLogFile(const char* utf8Path) {
    // P250：配置项 `[logging] nativeLogFile`。启动器（Prism 等）不把子进程 stdout/stderr 落盘，
    // 所以"用户端出问题却拿不到 native 行"是常态；这条导出让 Java 侧把镜像文件的路径推进来。
    // 真正的 fopen 发生在下一次写日志时（见 d3d12_device.cpp 的 mirrorLogToFile）。
    d3d12_mc::setLogFile(utf8Path);
    return (utf8Path != nullptr && utf8Path[0] != '\0') ? 1 : 0;
}

void MCDR_CALL mcdr_dx12SetEnvironment(const char* name, const char* value) {
    // A small, deliberate escape hatch: the Java side owns every user-visible setting, while the
    // native layer keeps its plain getenv() reads. _putenv_s updates the CRT environment block,
    // which is the copy getenv()/_wgetenv() in this DLL reads; an empty value removes the entry.
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    (void)_putenv_s(name, value != nullptr ? value : "");
}
int32_t MCDR_CALL mcdrSetPixCapture(int32_t enabled) {
    // 1=loaded, 0=not requested, -1=requested but not loaded. Loading the capture
    // layer is irreversible for the process (PIX hooks D3D12 from inside it), so the
    // decision belongs to the Java side and has to arrive before the first device.
    return d3d12_mc::setPixCapture(enabled != 0);
}

int32_t MCDR_CALL mcdrSetPixDirectory(const char* utf8Path) {
    // Where to find PIX's DLLs, from the [debug] pixDirectory config. Must arrive before
    // mcdrSetPixCapture: the layer is loaded before the first D3D12 device exists.
    return d3d12_mc::setPixDirectory(utf8Path);
}

int32_t MCDR_CALL mcdrStartPixTimingCapture(const char* utf8Path) {
    // The one capture PIX's UI cannot produce for us: its continuous recording stays inside
    // the PIX session, so a file only exists once the process drives PIXBeginCapture itself.
    return d3d12_mc::startPixTimingCapture(utf8Path);
}

int32_t MCDR_CALL mcdrStopPixTimingCapture(void) {
    return d3d12_mc::stopPixTimingCapture();
}

int32_t MCDR_CALL mcdrSetShadingRate(int32_t rate) {
    // P97. rate is a raw D3D12_SHADING_RATE (1X1=0x0 1X2=0x1 2X1=0x4 2X2=0x5 2X4=0x6
    // 4X2=0x9 4X4=0xA), -1 = off. Returns 1=accepted (including "the device does not
    // exist yet, so it will be gated on the first command list"), 0=this device cannot
    // honour the rate (tier 0, or an additional rate that is not supported), -1=unknown
    // rate value.
    return d3d12_mc::setShadingRate(rate);
}

int32_t MCDR_CALL mcdr_dx12SetShadingRateNow(mcdr_command_ctx* ctx, int32_t rate) {
    // P231. The per-draw policy brackets the draw batches that may be shaded coarsely with this
    // call and restores 1X1 afterwards, so the coarse rate never reaches the sky, particles or the
    // GUI. Legal mid-"pass": this module's beginRenderPass is only OMSetRenderTargets, never
    // ID3D12GraphicsCommandList4::BeginRenderPass.
    // Returns 1=issued, 0=the device cannot honour it (tier 0 / missing additional rates) or the
    // list is not an ID3D12GraphicsCommandList5, -1=unknown rate, -2=ctx null or no open list.
    return d3d12_mc::setShadingRateNow(ctx, rate);
}

int32_t MCDR_CALL mcdr_dx12SetShadingRateCombiners(int32_t first, int32_t second) {
    // P231. Stage 0 combines the per-draw rate with the per-primitive rate (SV_ShadingRate),
    // stage 1 with the shading-rate image. Defaults stay {PASSTHROUGH, PASSTHROUGH} until the mesh
    // shader writes SV_ShadingRate: under MIN a per-primitive 1X1 would undo every requested 2X2.
    // Returns 1=stored, -1=a value outside 0..4.
    return d3d12_mc::setShadingRateCombiners(first, second);
}

int32_t MCDR_CALL mcdrSetAntiAliasing(int32_t mode) {
    // P104/P108/P152. mode mirrors the Java-side option: **0=OFF 1=SMAA 2=TAA 3=DLAA**. SMAA
    // (three passes glued in place) and TAA (P152: a native resolve with an RGBA16F history
    // ping-pong, see mcdr_dx12ApplyTaaToTexture) are implemented; DLAA is still accepted and
    // downgraded to OFF with a WARN.
    // P108 (user m10920): DLSS is NO LONGER a value here — it has its own switch, see
    // mcdr_dx12SetDlssEnabled. Anti-aliasing and DLSS are independent.
    // Returns 1=in effect (OFF/SMAA/TAA), 0=accepted but not implemented (DLAA), -1=unknown mode.
    return d3d12_mc::setAntiAliasing(mode);
}

int32_t MCDR_CALL mcdr_dx12SetDlssEnabled(int32_t enabled) {
    // P108 (user m10920). DLSS upscaling gets its own option (Java `dxrMcDlss`) instead of
    // borrowing a value of `dxrMcAaMode`. Two things happen:
    //   1. the authoritative gate the renderer reads — gCtx.dlssEnabled (device module);
    //   2. a mirror inside the DLSS implementation so the status query and the P108 log can
    //      report the switch. The NGX runtime and any live feature handle are deliberately
    //      left alone, so OFF -> ON does not redo Init/CreateFeature.
    // Returns the normalised state: 1=enabled afterwards, 0=disabled afterwards.
    const int32_t gate = d3d12_mc::setDlssEnabled(enabled);
    mcdr_dlss_set_enabled(enabled);
    return gate;
}

int32_t MCDR_CALL mcdr_dx12SnapshotDlssDepth(mcdr_command_ctx* ctx, mcdr_object* depth) {
    // P108. The Java side calls this where this frame's depth is still valid (alongside the
    // motion-vector capture, before vanilla clears the depth buffer). The device module unwraps
    // the Dx12Object handle and hands the transition callbacks to the DLSS implementation, which
    // owns the private D32 snapshot texture (see mcdr_dlss_impl.h for the full rationale).
    // Returns 1=taken/reused, 0=unusable args, -1=creation/copy failed (WARN once).
    return d3d12_mc::snapshotDlssDepth(ctx, depth);
}

int32_t MCDR_CALL mcdr_dx12SetDlssQuality(int32_t tier) {
    // P108 (user m11147). The Java side turned the DLSS tier into its own button, so MCDR no
    // longer has to guess it from the render:target ratio. The device module owns the state
    // (`gCtx.dlssQualityTier`) because the renderer reads it, and it forwards the change to the
    // DLSS implementation so `ensureFeature` rebuilds the feature — PerfQualityValue is a
    // CreateFeature input, so a tier change cannot be applied by editing Evaluate parameters.
    // Returns the normalised tier, or -1 for an out-of-range value (nothing changes).
    return d3d12_mc::setDlssQuality(tier);
}

void MCDR_CALL mcdr_dx12ReflexFrameBegin(void) { mcdr_reflex_frame_begin(); }

void MCDR_CALL mcdr_dx12ReflexMarker(int32_t marker) { mcdr_reflex_marker(marker); }

void MCDR_CALL mcdr_dx12SetReflexMode(int32_t mode) { mcdr_reflex_set_mode(mode); }

int32_t MCDR_CALL mcdr_dx12ReflexAvailable(void) { return mcdr_reflex_available(); }
// P208：Streamline 的 DLSS 插件加载状态（见 mcdr_dlss_available 的注释）。
int32_t MCDR_CALL mcdr_dx12DlssAvailable(void) { return mcdr_dlss_available(); }

// P213：帧生成（DLSS-G）的开关。0 = off，1 = on（固定倍率），2 = dynamic。
// 这里只存状态；真正的 slDLSSGSetOptions 在渲染线程的 evaluate 里下发（见 d3d12_dlss.cpp）。
void MCDR_CALL mcdr_dx12SetDlssgMode(int32_t mode, int32_t numFrames) {
    mcdr_dlss_set_fg_mode(mode, numFrames);
}

// P213c：上一次 Present 真正提交的帧数，给 JVM 的 F3 帧率行用。
int32_t MCDR_CALL mcdr_dx12GetDlssgPresented(void) {
    return mcdr_dlss_fg_presented();
}

void MCDR_CALL mcdr_dx12SetReflexFrameLimit(int32_t micros) {
    mcdr_reflex_set_frame_limit(static_cast<uint32_t>(micros < 0 ? 0 : micros));
}

int32_t MCDR_CALL mcdr_dx12SetDlssRenderPreset(int32_t preset) {
    // P108ac (user m08033). The remaining noise/flicker is DLSS's own behaviour, and the user's
    // remedy is to swap the model — so the JVM gets a knob instead of us picking a default.
    // `preset` is a NVSDK_NGX_DLSS_Hint_Render_Preset value (0 = Default, 10 = J, 11 = K, ...);
    // the device module owns the state and forwards the change so `ensureFeature` rebuilds the
    // feature (the preset is baked in at CreateFeature time).
    // Returns the recorded preset, or -1 for an out-of-range value (nothing changes).
    return d3d12_mc::setDlssRenderPreset(preset);
}

int32_t MCDR_CALL mcdr_dx12SetDlssOutput(mcdr_object* tex) {
    // P108 (user m11147 follow-up). The Java side creates a target-sized UAV texture through the
    // module's own texture path (`USAGE_STORAGE = 32`) and registers it here, because the DLSS
    // implementation's own `CreateCommittedResource` texture proved unwritable (control test: same
    // source, site and command list — the module-created target read 17.8/16.5/11.6/255, the private
    // texture read all zero, alpha included, and still zero in the settled read).
    // The device module unwraps the handle because only it knows the `Dx12Object` layout; the DLSS
    // unit must never reinterpret a JVM handle (that caused the null-vtable crash).
    // Returns 1=accepted, 0=cleared, -1=not a live texture.
    return d3d12_mc::setDlssOutput(tex);
}

int32_t MCDR_CALL mcdr_dx12SetDlssVendorDir(const char* utf8Dir) {    // P108. The Java side announces <user.dir>\dxrmc\ (the directory it extracted MCDR.dll
    // and the vendor DLLs into). The native side widens the process DLL search path with it,
    // because that directory is not on the default search order: the process working
    // directory is the game directory while the executable is the JDK's java.exe, so the
    // bare-name and "next to the executable" lookups both miss nvngx_dlss.dll.
    // Returns 1=accepted and the search path widened, 0=empty/rejected input.
    // Implemented in src\advanced\d3d12_dlss.cpp (the plain TU that owns the NGX types).
    return mcdr_dlss_register_vendor_dir(utf8Dir, utf8Dir != nullptr ? static_cast<int32_t>(std::strlen(utf8Dir) + 1) : 0);
}

}  // extern "C"
