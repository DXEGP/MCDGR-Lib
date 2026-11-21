module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"
#include <cstdlib>

#include "mcdr_dlss_impl.h"

import mcdr.d3d12.device;

namespace mcdr::abi {

namespace {

#define MCDR_FIELD(T, F)                               \
    {#T "." #F, static_cast<uint64_t>(offsetof(T, F)), \
     static_cast<uint64_t>(sizeof((static_cast<T*>(nullptr))->F))}

const std::vector<mcdr_abi_field>& layoutTable() {
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

}

}

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
    d3d12_mc::setLogLevel(enabled ? 3 : 1);
}

void MCDR_CALL mcdrSetLogLevel(int32_t level) {
    d3d12_mc::setLogLevel(level);
}

void MCDR_CALL mcdrSetShutdownTraceAlways(int32_t always) {
    d3d12_mc::setShutdownTraceAlways(always != 0);
}

int32_t MCDR_CALL mcdrSetLogFile(const char* utf8Path) {
    d3d12_mc::setLogFile(utf8Path);
    return (utf8Path != nullptr && utf8Path[0] != '\0') ? 1 : 0;
}

void MCDR_CALL mcdr_dx12SetEnvironment(const char* name, const char* value) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    (void)_putenv_s(name, value != nullptr ? value : "");
}
int32_t MCDR_CALL mcdrSetPixCapture(int32_t enabled) {
    return d3d12_mc::setPixCapture(enabled != 0);
}

int32_t MCDR_CALL mcdrSetPixDirectory(const char* utf8Path) {
    return d3d12_mc::setPixDirectory(utf8Path);
}

int32_t MCDR_CALL mcdrStartPixTimingCapture(const char* utf8Path) {
    return d3d12_mc::startPixTimingCapture(utf8Path);
}

int32_t MCDR_CALL mcdrStopPixTimingCapture(void) {
    return d3d12_mc::stopPixTimingCapture();
}

int32_t MCDR_CALL mcdrSetShadingRate(int32_t rate) {
    return d3d12_mc::setShadingRate(rate);
}

int32_t MCDR_CALL mcdr_dx12SetShadingRateNow(mcdr_command_ctx* ctx, int32_t rate) {
    return d3d12_mc::setShadingRateNow(ctx, rate);
}

int32_t MCDR_CALL mcdr_dx12SetShadingRateCombiners(int32_t first, int32_t second) {
    return d3d12_mc::setShadingRateCombiners(first, second);
}

int32_t MCDR_CALL mcdrSetAntiAliasing(int32_t mode) {
    return d3d12_mc::setAntiAliasing(mode);
}

int32_t MCDR_CALL mcdr_dx12SetDlssEnabled(int32_t enabled) {
    const int32_t gate = d3d12_mc::setDlssEnabled(enabled);
    mcdr_dlss_set_enabled(enabled);
    return gate;
}

int32_t MCDR_CALL mcdr_dx12SnapshotDlssDepth(mcdr_command_ctx* ctx, mcdr_object* depth) {
    return d3d12_mc::snapshotDlssDepth(ctx, depth);
}

int32_t MCDR_CALL mcdr_dx12SetDlssQuality(int32_t tier) {
    return d3d12_mc::setDlssQuality(tier);
}

void MCDR_CALL mcdr_dx12ReflexFrameBegin(void) { mcdr_reflex_frame_begin(); }

void MCDR_CALL mcdr_dx12ReflexMarker(int32_t marker) { mcdr_reflex_marker(marker); }

void MCDR_CALL mcdr_dx12SetReflexMode(int32_t mode) { mcdr_reflex_set_mode(mode); }

int32_t MCDR_CALL mcdr_dx12ReflexAvailable(void) { return mcdr_reflex_available(); }
int32_t MCDR_CALL mcdr_dx12DlssAvailable(void) { return mcdr_dlss_available(); }

void MCDR_CALL mcdr_dx12SetDlssgMode(int32_t mode, int32_t numFrames) {
    mcdr_dlss_set_fg_mode(mode, numFrames);
}

int32_t MCDR_CALL mcdr_dx12GetDlssgPresented(void) {
    return mcdr_dlss_fg_presented();
}

void MCDR_CALL mcdr_dx12SetReflexFrameLimit(int32_t micros) {
    mcdr_reflex_set_frame_limit(static_cast<uint32_t>(micros < 0 ? 0 : micros));
}

int32_t MCDR_CALL mcdr_dx12SetDlssRenderPreset(int32_t preset) {
    return d3d12_mc::setDlssRenderPreset(preset);
}

int32_t MCDR_CALL mcdr_dx12SetDlssOutput(mcdr_object* tex) {
    return d3d12_mc::setDlssOutput(tex);
}

int32_t MCDR_CALL mcdr_dx12SetDlssVendorDir(const char* utf8Dir) {
    return mcdr_dlss_register_vendor_dir(utf8Dir, utf8Dir != nullptr ? static_cast<int32_t>(std::strlen(utf8Dir) + 1) : 0);
}

}
