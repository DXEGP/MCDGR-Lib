// abi_meta.cpp — the ABI's own surface: version, layout self-check, error reporting, log controls.
// The layout table publishes every mirrored field as (name, offset, size) and the JVM compares it
// before any other entry point runs, so a reordered field fails at load time. 见 docs\abi\README.md

module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件
#include <cstdlib>

// The DLSS implementation is a plain TU (src\advanced\d3d12_dlss.cpp), so this unit only sees
// its C interface: the module cannot name NGX types. 见 docs\advanced\dlss.md
#include "mcdr_dlss_impl.h"

import mcdr.d3d12.device;

namespace mcdr::abi {

namespace {

// (name, offset, size) for one mirrored field; sizeof() needs the null dereference because a member
// cannot be named in sizeof.
#define MCDR_FIELD(T, F)                               \
    {#T "." #F, static_cast<uint64_t>(offsetof(T, F)), \
     static_cast<uint64_t>(sizeof((static_cast<T*>(nullptr))->F))}

const std::vector<mcdr_abi_field>& layoutTable() {
    static const std::vector<mcdr_abi_field> table = {
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
        MCDR_FIELD(mcdr_pipeline_desc, _pad1),
        MCDR_FIELD(mcdr_pipeline_desc, depth),
        MCDR_FIELD(mcdr_pipeline_desc, topology),
        MCDR_FIELD(mcdr_pipeline_desc, cullEnabled),
        MCDR_FIELD(mcdr_pipeline_desc, polygonMode),
        MCDR_FIELD(mcdr_pipeline_desc, inputElementCount),
        MCDR_FIELD(mcdr_pipeline_desc, inputElements),
        MCDR_FIELD(mcdr_pipeline_desc, bindingCount),
        MCDR_FIELD(mcdr_pipeline_desc, _pad2),
        MCDR_FIELD(mcdr_pipeline_desc, bindings),

        // Mesh-shader pipeline descriptor (see docs\advanced\mesh.md).
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

        // DLSS per-frame parameter package. MCDR_Layout.kt mirrors it field by field -- every named
        // field (including _pad0/_pad1/_pad2) MUST be published: verifyABI() compares both
        // directions and a Kotlin-only field aborts backend creation.
        MCDR_FIELD(mcdr_dlss_input, renderWidth),
        MCDR_FIELD(mcdr_dlss_input, renderHeight),
        MCDR_FIELD(mcdr_dlss_input, targetWidth),
        MCDR_FIELD(mcdr_dlss_input, targetHeight),
        MCDR_FIELD(mcdr_dlss_input, jitterOffsetX),
        MCDR_FIELD(mcdr_dlss_input, jitterOffsetY),
        MCDR_FIELD(mcdr_dlss_input, mvScaleX),
        MCDR_FIELD(mcdr_dlss_input, mvScaleY),
        MCDR_FIELD(mcdr_dlss_input, preExposure),
        MCDR_FIELD(mcdr_dlss_input, exposureScale),
        MCDR_FIELD(mcdr_dlss_input, exposureMode),
        MCDR_FIELD(mcdr_dlss_input, alphaUpscaling),
        MCDR_FIELD(mcdr_dlss_input, nearZ),
        MCDR_FIELD(mcdr_dlss_input, farZ),
        MCDR_FIELD(mcdr_dlss_input, depthInverted),
        MCDR_FIELD(mcdr_dlss_input, reset),
        MCDR_FIELD(mcdr_dlss_input, depthFormatOrdinal),
        MCDR_FIELD(mcdr_dlss_input, _pad0),
        MCDR_FIELD(mcdr_dlss_input, pInColor),
        MCDR_FIELD(mcdr_dlss_input, pInDepth),
        MCDR_FIELD(mcdr_dlss_input, pInMotionVectors),
        MCDR_FIELD(mcdr_dlss_input, pInOutput),
        MCDR_FIELD(mcdr_dlss_input, _pad1),
        MCDR_FIELD(mcdr_dlss_input, _pad2),

        // SR abstraction tables: same bidirectional mirror; field names must match mcdr_abi.h verbatim.
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

        // FG abstraction tables: same rule, field names verbatim, both _pad fields included.
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
        // Array fields go through MCDR_FIELD too: offsetof + sizeof cover the whole array.
        MCDR_FIELD(mcdr_fg_constants, view),
        MCDR_FIELD(mcdr_fg_constants, projection),
        MCDR_FIELD(mcdr_fg_constants, jitterX),
        MCDR_FIELD(mcdr_fg_constants, jitterY),
        MCDR_FIELD(mcdr_fg_constants, mvScaleX),
        MCDR_FIELD(mcdr_fg_constants, mvScaleY),
        MCDR_FIELD(mcdr_fg_constants, resetHistory),
        MCDR_FIELD(mcdr_fg_constants, frameRenderTime),

        MCDR_FIELD(mcdr_rt_caps, raytracingTier),
        MCDR_FIELD(mcdr_rt_caps, meshShaderTier),
        MCDR_FIELD(mcdr_rt_caps, workGraphsTier),
        MCDR_FIELD(mcdr_rt_caps, shadingRateTier),
        MCDR_FIELD(mcdr_rt_caps, shaderModel),
        MCDR_FIELD(mcdr_rt_caps, samplerFeedbackTier),
        MCDR_FIELD(mcdr_rt_caps, options21Queried),
        MCDR_FIELD(mcdr_rt_caps, options22Queried),
        MCDR_FIELD(mcdr_rt_caps, serActuallyReorders),
        MCDR_FIELD(mcdr_rt_caps, enhancedBarriers),
        MCDR_FIELD(mcdr_rt_caps, dxcAvailable),
        MCDR_FIELD(mcdr_rt_caps, max1DDispatchSize),
        MCDR_FIELD(mcdr_rt_caps, max1DDispatchMeshSize),
        MCDR_FIELD(mcdr_rt_caps, reserved),
        MCDR_FIELD(mcdr_rt_caps, note),

        MCDR_FIELD(mcdr_rt_track_stats, transitions),
        MCDR_FIELD(mcdr_rt_track_stats, mismatches),
        MCDR_FIELD(mcdr_rt_track_stats, promotions),
        MCDR_FIELD(mcdr_rt_track_stats, widened),
        MCDR_FIELD(mcdr_rt_track_stats, firstUse),
        MCDR_FIELD(mcdr_rt_track_stats, uavBarriers),
        MCDR_FIELD(mcdr_rt_track_stats, tracked),
        MCDR_FIELD(mcdr_rt_track_stats, note),

        MCDR_FIELD(mcdr_rt_hit_group, name),
        MCDR_FIELD(mcdr_rt_hit_group, closestHit),
        MCDR_FIELD(mcdr_rt_hit_group, anyHit),
        MCDR_FIELD(mcdr_rt_hit_group, intersection),

        MCDR_FIELD(mcdr_rt_frame_program, rayDxil),
        MCDR_FIELD(mcdr_rt_frame_program, rayBytes),
        MCDR_FIELD(mcdr_rt_frame_program, guideDxil),
        MCDR_FIELD(mcdr_rt_frame_program, guideBytes),
        MCDR_FIELD(mcdr_rt_frame_program, exposureDxil),
        MCDR_FIELD(mcdr_rt_frame_program, exposureBytes),
        MCDR_FIELD(mcdr_rt_frame_program, motionDxil),
        MCDR_FIELD(mcdr_rt_frame_program, motionBytes),
        MCDR_FIELD(mcdr_rt_frame_program, packDxil),
        MCDR_FIELD(mcdr_rt_frame_program, packBytes),
        MCDR_FIELD(mcdr_rt_frame_program, signalDxil),
        MCDR_FIELD(mcdr_rt_frame_program, signalBytes),
        MCDR_FIELD(mcdr_rt_frame_program, remodulateDxil),
        MCDR_FIELD(mcdr_rt_frame_program, remodulateBytes),
        MCDR_FIELD(mcdr_rt_frame_program, compositeDxil),
        MCDR_FIELD(mcdr_rt_frame_program, compositeBytes),
        MCDR_FIELD(mcdr_rt_frame_program, environmentDxil),
        MCDR_FIELD(mcdr_rt_frame_program, environmentBytes),
        MCDR_FIELD(mcdr_rt_geometry_desc, worldEpoch),
        MCDR_FIELD(mcdr_rt_geometry_desc, geometryRevision),
        MCDR_FIELD(mcdr_rt_geometry_desc, materialRevision),
        MCDR_FIELD(mcdr_rt_geometry_desc, reuseGeometry),
        MCDR_FIELD(mcdr_rt_geometry_desc, reuseMaterials),
        MCDR_FIELD(mcdr_rt_geometry_desc, records),
        MCDR_FIELD(mcdr_rt_geometry_desc, recordBytes),
        MCDR_FIELD(mcdr_rt_geometry_desc, vertices),
        MCDR_FIELD(mcdr_rt_geometry_desc, vertexBytes),
        MCDR_FIELD(mcdr_rt_geometry_desc, indices),
        MCDR_FIELD(mcdr_rt_geometry_desc, indexBytes),
        MCDR_FIELD(mcdr_rt_geometry_desc, materials),
        MCDR_FIELD(mcdr_rt_geometry_desc, materialBytes),
        MCDR_FIELD(mcdr_rt_geometry_desc, quads),
        MCDR_FIELD(mcdr_rt_geometry_desc, quadBytes),
        MCDR_FIELD(mcdr_rt_geometry_output, snapshotId),
        MCDR_FIELD(mcdr_rt_geometry_output, records),
        MCDR_FIELD(mcdr_rt_geometry_output, vertices),
        MCDR_FIELD(mcdr_rt_geometry_output, indices),
        MCDR_FIELD(mcdr_rt_geometry_output, materials),
        MCDR_FIELD(mcdr_rt_geometry_output, quads),
        MCDR_FIELD(mcdr_rt_geometry_output, byteCount),
        MCDR_FIELD(mcdr_rt_geometry_content, vertexIdentity),
        MCDR_FIELD(mcdr_rt_geometry_content, indexIdentity),
        MCDR_FIELD(mcdr_rt_geometry_content, quadIdentity),
        MCDR_FIELD(mcdr_rt_geometry_content, vertices),
        MCDR_FIELD(mcdr_rt_geometry_content, vertexBytes),
        MCDR_FIELD(mcdr_rt_geometry_content, indices),
        MCDR_FIELD(mcdr_rt_geometry_content, indexBytes),
        MCDR_FIELD(mcdr_rt_geometry_content, quads),
        MCDR_FIELD(mcdr_rt_geometry_content, quadBytes),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, worldEpoch),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, geometryRevision),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, materialRevision),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, baseSnapshot),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, reuseMaterials),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, records),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, recordBytes),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, contents),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, contentCount),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, reserved),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, materials),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, materialBytes),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, vertexCapacity),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, indexCapacity),
        MCDR_FIELD(mcdr_rt_geometry_delta_desc, quadCapacity),
        MCDR_FIELD(mcdr_rt_geometry_delta_output, snapshotId),
        MCDR_FIELD(mcdr_rt_geometry_delta_output, records),
        MCDR_FIELD(mcdr_rt_geometry_delta_output, vertices),
        MCDR_FIELD(mcdr_rt_geometry_delta_output, indices),
        MCDR_FIELD(mcdr_rt_geometry_delta_output, materials),
        MCDR_FIELD(mcdr_rt_geometry_delta_output, quads),
        MCDR_FIELD(mcdr_rt_geometry_delta_output, byteCount),
        MCDR_FIELD(mcdr_rt_geometry_delta_output, uploadedBytes),
        MCDR_FIELD(mcdr_rt_geometry_delta_output, uploadCapacity),
        MCDR_FIELD(mcdr_rt_geometry_memory, gpuBytes),
        MCDR_FIELD(mcdr_rt_geometry_memory, uploadBytes),
        MCDR_FIELD(mcdr_rt_acceleration_output, snapshotId),
        MCDR_FIELD(mcdr_rt_acceleration_output, acceleration),
        MCDR_FIELD(mcdr_rt_acceleration_output, instances),
        MCDR_FIELD(mcdr_rt_acceleration_output, byteCount),
        MCDR_FIELD(mcdr_rt_acceleration_output, instanceCount),
        MCDR_FIELD(mcdr_rt_acceleration_output, blasCount),
        MCDR_FIELD(mcdr_rt_acceleration_output, buildCount),
        MCDR_FIELD(mcdr_rt_acceleration_output, ready),
        MCDR_FIELD(mcdr_rt_acceleration_output, blasMicros),
        MCDR_FIELD(mcdr_rt_acceleration_output, peakBuildMicros),
        MCDR_FIELD(mcdr_rt_acceleration_output, tlasMicros),
        MCDR_FIELD(mcdr_rt_acceleration_output, scratchBytes),
        MCDR_FIELD(mcdr_rt_section_version, node),
        MCDR_FIELD(mcdr_rt_section_version, layer),
        MCDR_FIELD(mcdr_rt_section_version, version),
        MCDR_FIELD(mcdr_rt_output_support, node),
        MCDR_FIELD(mcdr_rt_output_support, layer),
        MCDR_FIELD(mcdr_rt_output_support, version),
        MCDR_FIELD(mcdr_rt_output_support, flags),
        MCDR_FIELD(mcdr_rt_output_support, reserved),
        MCDR_FIELD(mcdr_rt_output_support, minimumX),
        MCDR_FIELD(mcdr_rt_output_support, minimumY),
        MCDR_FIELD(mcdr_rt_output_support, minimumZ),
        MCDR_FIELD(mcdr_rt_output_support, maximumX),
        MCDR_FIELD(mcdr_rt_output_support, maximumY),
        MCDR_FIELD(mcdr_rt_output_support, maximumZ),
        MCDR_FIELD(mcdr_rt_scene_desc, worldEpoch),
        MCDR_FIELD(mcdr_rt_scene_desc, snapshotId),
        MCDR_FIELD(mcdr_rt_scene_desc, liveRevision),
        MCDR_FIELD(mcdr_rt_scene_desc, sections),
        MCDR_FIELD(mcdr_rt_scene_desc, sectionCount),
        MCDR_FIELD(mcdr_rt_scene_desc, reserved),
        MCDR_FIELD(mcdr_rt_scene_desc, frameId),
        MCDR_FIELD(mcdr_rt_scene_desc, outputs),
        MCDR_FIELD(mcdr_rt_scene_desc, outputCount),
        MCDR_FIELD(mcdr_rt_scene_desc, outputMode),
        MCDR_FIELD(mcdr_rt_scene_output, snapshotId),
        MCDR_FIELD(mcdr_rt_scene_output, accelerationId),
        MCDR_FIELD(mcdr_rt_scene_output, pendingSnapshotId),
        MCDR_FIELD(mcdr_rt_scene_output, pendingAccelerationId),
        MCDR_FIELD(mcdr_rt_scene_output, sceneRevision),
        MCDR_FIELD(mcdr_rt_scene_output, geometryRevision),
        MCDR_FIELD(mcdr_rt_scene_output, materialRevision),
        MCDR_FIELD(mcdr_rt_scene_output, byteCount),
        MCDR_FIELD(mcdr_rt_scene_output, active),
        MCDR_FIELD(mcdr_rt_scene_output, candidate),
        MCDR_FIELD(mcdr_rt_scene_output, coverageFlags),
        MCDR_FIELD(mcdr_rt_frame_desc, frameId),
        MCDR_FIELD(mcdr_rt_frame_desc, worldEpoch),
        MCDR_FIELD(mcdr_rt_frame_desc, sceneRevision),
        MCDR_FIELD(mcdr_rt_frame_desc, materialRevision),
        MCDR_FIELD(mcdr_rt_frame_desc, snapshotId),
        MCDR_FIELD(mcdr_rt_frame_desc, width),
        MCDR_FIELD(mcdr_rt_frame_desc, height),
        MCDR_FIELD(mcdr_rt_frame_desc, accelerationId),
        MCDR_FIELD(mcdr_rt_frame_desc, gbufferRevision),
        MCDR_FIELD(mcdr_rt_frame_desc, constants),
        MCDR_FIELD(mcdr_rt_frame_desc, constantBytes),
        MCDR_FIELD(mcdr_rt_frame_desc, depthScale),
        MCDR_FIELD(mcdr_rt_frame_desc, frameNanos),
        MCDR_FIELD(mcdr_rt_frame_desc, exposureMode),
        MCDR_FIELD(mcdr_rt_frame_desc, exposureEv),
        MCDR_FIELD(mcdr_rt_frame_desc, exposureSpeed),
        MCDR_FIELD(mcdr_rt_frame_desc, resetExposure),
        MCDR_FIELD(mcdr_rt_frame_desc, signalEncoding),
        MCDR_FIELD(mcdr_rt_frame_desc, jitterDeltaX),
        MCDR_FIELD(mcdr_rt_frame_desc, jitterDeltaY),
        MCDR_FIELD(mcdr_rt_frame_desc, denoisingRange),
        MCDR_FIELD(mcdr_rt_frame_desc, contentJitterX),
        MCDR_FIELD(mcdr_rt_frame_desc, contentJitterY),
        MCDR_FIELD(mcdr_rt_frame_output, frameId),
        MCDR_FIELD(mcdr_rt_frame_output, snapshotId),
        MCDR_FIELD(mcdr_rt_frame_output, direct),
        MCDR_FIELD(mcdr_rt_frame_output, indirect),
        MCDR_FIELD(mcdr_rt_frame_output, aux),
        MCDR_FIELD(mcdr_rt_frame_output, specular),
        MCDR_FIELD(mcdr_rt_frame_output, specularGrazing),
        MCDR_FIELD(mcdr_rt_frame_output, constants),
        MCDR_FIELD(mcdr_rt_frame_output, byteCount),
        MCDR_FIELD(mcdr_rt_frame_output, exposure),
        MCDR_FIELD(mcdr_rt_frame_output, motion),
        MCDR_FIELD(mcdr_rt_frame_output, packedNormalRoughness),
        MCDR_FIELD(mcdr_rt_frame_output, viewZ),
        MCDR_FIELD(mcdr_rt_frame_output, diffuseSignal),
        MCDR_FIELD(mcdr_rt_frame_output, specularSignal),
        MCDR_FIELD(mcdr_rt_frame_output, specularHitDistance),
        MCDR_FIELD(mcdr_rt_denoise_desc, frameId),
        MCDR_FIELD(mcdr_rt_denoise_desc, historyGeneration),
        MCDR_FIELD(mcdr_rt_denoise_desc, historyFrame),
        MCDR_FIELD(mcdr_rt_denoise_desc, planRevision),
        MCDR_FIELD(mcdr_rt_denoise_desc, outputWidth),
        MCDR_FIELD(mcdr_rt_denoise_desc, outputHeight),
        MCDR_FIELD(mcdr_rt_denoise_desc, nrdMatrices),
        MCDR_FIELD(mcdr_rt_denoise_desc, nrdFloatCount),
        MCDR_FIELD(mcdr_rt_denoise_desc, slFloatCount),
        MCDR_FIELD(mcdr_rt_denoise_desc, slConstants),
        MCDR_FIELD(mcdr_rt_gbuffer_program, msData),
        MCDR_FIELD(mcdr_rt_gbuffer_program, msLen),
        MCDR_FIELD(mcdr_rt_gbuffer_program, psData),
        MCDR_FIELD(mcdr_rt_gbuffer_program, psLen),
        MCDR_FIELD(mcdr_rt_gbuffer_program, bindings),
        MCDR_FIELD(mcdr_rt_gbuffer_program, bindingCount),
        MCDR_FIELD(mcdr_rt_gbuffer_program, cullEnabled),
        MCDR_FIELD(mcdr_rt_gbuffer_program, polygonMode),
        MCDR_FIELD(mcdr_rt_gbuffer_program, _pad0),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, program),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, vertices),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, vertexOffset),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, sections),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, sectionsOffset),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, sectionsLength),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, meshlets),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, meshletsOffset),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, meshletsLength),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, meshletCount),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, bindingCount),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, bindings),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, projectionBinding),
        MCDR_FIELD(mcdr_rt_gbuffer_batch, _pad0),
        MCDR_FIELD(mcdr_rt_gbuffer_replay, revision),
        MCDR_FIELD(mcdr_rt_gbuffer_replay, batches),
        MCDR_FIELD(mcdr_rt_gbuffer_replay, batchCount),
        MCDR_FIELD(mcdr_rt_gbuffer_replay, projectionFloatCount),
        MCDR_FIELD(mcdr_rt_gbuffer_replay, projection),
        MCDR_FIELD(mcdr_rt_gbuffer_replay, cameraOffsetX),
        MCDR_FIELD(mcdr_rt_gbuffer_replay, cameraOffsetY),
        MCDR_FIELD(mcdr_rt_gbuffer_replay, cameraOffsetZ),
        MCDR_FIELD(mcdr_rt_gbuffer_replay, _pad0),
        MCDR_FIELD(mcdr_rt_gbuffer_output, revision),
        MCDR_FIELD(mcdr_rt_gbuffer_output, resourceRevision),
        MCDR_FIELD(mcdr_rt_gbuffer_output, frameNanos),
        MCDR_FIELD(mcdr_rt_gbuffer_output, width),
        MCDR_FIELD(mcdr_rt_gbuffer_output, height),
        MCDR_FIELD(mcdr_rt_gbuffer_output, albedo),
        MCDR_FIELD(mcdr_rt_gbuffer_output, specular),
        MCDR_FIELD(mcdr_rt_gbuffer_output, normalRoughness),
        MCDR_FIELD(mcdr_rt_gbuffer_output, emissive),
        MCDR_FIELD(mcdr_rt_gbuffer_output, linearDepth),
        MCDR_FIELD(mcdr_rt_gbuffer_output, depth),
        MCDR_FIELD(mcdr_rt_gbuffer_output, byteCount),

        MCDR_FIELD(mcdr_rt_denoise_output, frameId),
        MCDR_FIELD(mcdr_rt_denoise_output, snapshotId),
        MCDR_FIELD(mcdr_rt_denoise_output, diffuse),
        MCDR_FIELD(mcdr_rt_denoise_output, specular),
        MCDR_FIELD(mcdr_rt_denoise_output, byteCount),
        MCDR_FIELD(mcdr_rt_denoise_output, encoding),
        MCDR_FIELD(mcdr_rt_denoise_output, dispatchCount),

        MCDR_FIELD(mcdr_rt_transaction_desc, trace),
        MCDR_FIELD(mcdr_rt_transaction_desc, denoise),
        MCDR_FIELD(mcdr_rt_transaction_desc, liveRevision),
        MCDR_FIELD(mcdr_rt_transaction_desc, spatial),
        MCDR_FIELD(mcdr_rt_transaction_desc, spatialBytes),
        MCDR_FIELD(mcdr_rt_transaction_desc, source),
        MCDR_FIELD(mcdr_rt_transaction_desc, upscaler),
        MCDR_FIELD(mcdr_rt_transaction_desc, debugMode),
        MCDR_FIELD(mcdr_rt_transaction_desc, timestamps),
        MCDR_FIELD(mcdr_rt_transaction_desc, firstTimestamp),
        MCDR_FIELD(mcdr_rt_transaction_desc, reserved),
        MCDR_FIELD(mcdr_rt_transaction_output, trace),
        MCDR_FIELD(mcdr_rt_transaction_output, denoise),
        MCDR_FIELD(mcdr_rt_transaction_output, upscale),
        MCDR_FIELD(mcdr_rt_transaction_output, composite),
        MCDR_FIELD(mcdr_rt_transaction_output, fallbackFlags),
        MCDR_FIELD(mcdr_rt_transaction_output, recorded),
        MCDR_FIELD(mcdr_rt_transaction_output, timestampMask),
        MCDR_FIELD(mcdr_rt_transaction_output, reserved),

        MCDR_FIELD(mcdr_rt_geometry, vertexBuffer),
        MCDR_FIELD(mcdr_rt_geometry, vertexStride),
        MCDR_FIELD(mcdr_rt_geometry, vertexCount),
        MCDR_FIELD(mcdr_rt_geometry, vertexOffset),
        MCDR_FIELD(mcdr_rt_geometry, indexBuffer),
        MCDR_FIELD(mcdr_rt_geometry, indexCount),
        MCDR_FIELD(mcdr_rt_geometry, indexOffset),
        MCDR_FIELD(mcdr_rt_geometry, flags),
        MCDR_FIELD(mcdr_rt_geometry, indexStride),

        MCDR_FIELD(mcdr_barrier_desc, resource),
        MCDR_FIELD(mcdr_barrier_desc, resourceAfter),
        MCDR_FIELD(mcdr_barrier_desc, kind),
        MCDR_FIELD(mcdr_barrier_desc, flags),
        MCDR_FIELD(mcdr_barrier_desc, before),
        MCDR_FIELD(mcdr_barrier_desc, after),
        MCDR_FIELD(mcdr_barrier_desc, subresource),
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
    // The Java side owns the verbosity decision; native code never reads the process environment.
    d3d12_mc::setLogLevel(enabled ? 3 : 1);
}

void MCDR_CALL mcdrSetLogLevel(int32_t level) {
    // 0=error 1=warning 2=info 3=debug; d3d12_mc::setLogLevel() clamps to 0..3.
    d3d12_mc::setLogLevel(level);
}

void MCDR_CALL mcdrSetShutdownTraceAlways(int32_t always) {
    // 1 (default) = the destroySurface/destroyDevice progress markers bypass logLevel, the only
    // clue when shutdown hangs; 0 = they fall back to plain INFO.
    d3d12_mc::setShutdownTraceAlways(always != 0);
}

int32_t MCDR_CALL mcdrSetLogFile(const char* utf8Path) {
    // Launchers do not persist stdout/stderr, so a user-side failure otherwise leaves no native line;
    // the fopen happens on the next log write.
    d3d12_mc::setLogFile(utf8Path);
    return (utf8Path != nullptr && utf8Path[0] != '\0') ? 1 : 0;
}

void MCDR_CALL mcdr_dx12SetEnvironment(const char* name, const char* value) {
    // Deprecated escape hatch: _putenv_s updates the CRT block getenv() reads, an empty value removes
    // the entry. New settings belong in ABI functions, never here.
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    (void)_putenv_s(name, value != nullptr ? value : "");
}
int32_t MCDR_CALL mcdrSetPixCapture(int32_t enabled) {
    // 1=loaded, 0=not requested, -1=requested but not loaded. Loading is irreversible (PIX hooks
    // D3D12 in-process), so it must arrive before the first device.
    return d3d12_mc::setPixCapture(enabled != 0);
}

int32_t MCDR_CALL mcdr_dx12SetDebugLayer(int32_t enabled) {
    // The layer is enabled once, while the first device is created; this only records the request.
    return d3d12_mc::setDebugLayerRequested(enabled != 0) ? 1 : 0;
}

int32_t MCDR_CALL mcdrSetPixDirectory(const char* utf8Path) {
    // Must arrive before mcdrSetPixCapture: the layer loads before the first D3D12 device exists.
    return d3d12_mc::setPixDirectory(utf8Path);
}

int32_t MCDR_CALL mcdrStartPixTimingCapture(const char* utf8Path) {
    // The one capture PIX's UI cannot produce: continuous recording stays inside the PIX session, so
    // a file exists only once the process drives PIXBeginCapture itself.
    return d3d12_mc::startPixTimingCapture(utf8Path);
}

int32_t MCDR_CALL mcdrStopPixTimingCapture(void) {
    return d3d12_mc::stopPixTimingCapture();
}

int32_t MCDR_CALL mcdrSetShadingRate(int32_t rate) {
    // `rate` is a raw D3D12_SHADING_RATE (1X1=0x0 1X2=0x1 2X1=0x4 2X2=0x5 2X4=0x6 4X2=0x9 4X4=0xA), -1 = off.
    // Returns 1=accepted (also with no device yet: it is gated on the first command list), 0=this
    // device cannot honour it (tier 0 / unsupported additional rate), -1=unknown rate value.
    return d3d12_mc::setShadingRate(rate);
}

int32_t MCDR_CALL mcdr_dx12SetShadingRateNow(mcdr_command_ctx* ctx, int32_t rate) {
    // The per-draw policy brackets the coarsely shaded batches with this call and restores 1X1 after,
    // so the coarse rate never reaches sky, particles or GUI. Legal mid-pass: this module's
    // beginRenderPass is only OMSetRenderTargets, never ID3D12GraphicsCommandList4::BeginRenderPass.
    // Returns 1=issued, 0=device cannot honour it or the list is not a command list 5, -1=unknown
    // rate, -2=ctx null or no open list.
    return d3d12_mc::setShadingRateNow(ctx, rate);
}

int32_t MCDR_CALL mcdr_dx12SetShadingRateCombiners(int32_t first, int32_t second) {
    // Stage 0 combines the per-draw rate with SV_ShadingRate, stage 1 with the shading-rate image.
    // Defaults stay {PASSTHROUGH, PASSTHROUGH}: under MIN a per-primitive 1X1 would undo every 2X2.
    // Returns 1=stored, -1=value outside 0..4.
    return d3d12_mc::setShadingRateCombiners(first, second);
}

int32_t MCDR_CALL mcdrSetAntiAliasing(int32_t mode) {
    // 0=OFF 1=SMAA 2=TAA 3=DLAA. DLAA is accepted but downgraded to OFF with a WARN; DLSS is
    // deliberately not a value here -- it has its own switch.
    // Returns 1=in effect (OFF/SMAA/TAA), 0=accepted but not implemented (DLAA), -1=unknown mode.
    return d3d12_mc::setAntiAliasing(mode);
}

int32_t MCDR_CALL mcdr_dx12SetDlssEnabled(int32_t enabled) {
    // Sets the authoritative gate the renderer reads (gCtx.dlssEnabled) plus a DLSS-unit mirror for
    // the status query; NGX state and live features are untouched, so OFF -> ON does not redo
    // Init/CreateFeature. Returns 1=enabled, 0=disabled afterwards.
    const int32_t gate = d3d12_mc::setDlssEnabled(enabled);
    mcdr_dlss_set_enabled(enabled);
    return gate;
}

int32_t MCDR_CALL mcdr_dx12SnapshotDlssDepth(mcdr_command_ctx* ctx, mcdr_object* depth) {
    // Call it while this frame's depth is still valid (before vanilla clears it); the device module
    // unwraps the handle, the DLSS unit owns the private D32 snapshot texture.
    // Returns 1=taken/reused, 0=unusable args, -1=creation/copy failed (WARN once).
    return d3d12_mc::snapshotDlssDepth(ctx, depth);
}

int32_t MCDR_CALL mcdr_dx12SetDlssQuality(int32_t tier) {
    // The tier is a CreateFeature input (PerfQualityValue), so a change rebuilds the feature; editing
    // Evaluate parameters cannot apply it. Returns the normalised tier, -1 for an out-of-range value.
    return d3d12_mc::setDlssQuality(tier);
}

void MCDR_CALL mcdr_dx12ReflexFrameBegin(void) { mcdr_reflex_frame_begin(); }

void MCDR_CALL mcdr_dx12ReflexMarker(int32_t marker) { mcdr_reflex_marker(marker); }

void MCDR_CALL mcdr_dx12SetReflexMode(int32_t mode) { mcdr_reflex_set_mode(mode); }

int32_t MCDR_CALL mcdr_dx12ReflexAvailable(void) { return mcdr_reflex_available(); }
int32_t MCDR_CALL mcdr_dx12DlssAvailable(void) { return mcdr_dlss_available(); }

// Frame generation switch: 0 = off, 1 = on (fixed multiplier), 2 = dynamic. State only — the real
// slDLSSGSetOptions goes out on the render thread's evaluate.
void MCDR_CALL mcdr_dx12SetDlssgMode(int32_t mode, int32_t numFrames) {
    mcdr_dlss_set_fg_mode(mode, numFrames);
}

int32_t MCDR_CALL mcdr_dx12GetDlssgPresented(void) {
    return mcdr_dlss_fg_presented();
}

void MCDR_CALL mcdr_dx12SetReflexFrameLimit(int32_t micros) {
    mcdr_reflex_set_frame_limit(static_cast<uint32_t>(micros < 0 ? 0 : micros));
}

int32_t MCDR_CALL mcdr_dx12SetDlssConstants(const mcdr_dlss_constants* constants) {
    // Cached here, fed to slSetConstants at evaluate time (camera data exists only JVM-side).
    // Returns 1=cached, 0=arguments were null.
    return mcdr_dlss_set_constants(constants);
}

int32_t MCDR_CALL mcdr_dx12SetDlssRenderPreset(int32_t preset) {
    // `preset` is a NVSDK_NGX_DLSS_Hint_Render_Preset value (0=Default, 10=J, 11=K, ...) baked in at
    // CreateFeature time, so a change rebuilds the feature. Returns the recorded preset, -1 if invalid.
    return d3d12_mc::setDlssRenderPreset(preset);
}

int32_t MCDR_CALL mcdr_dx12SetDlssOutput(mcdr_object* tex) {
    // The JVM creates the target-sized UAV texture (USAGE_STORAGE = 32) and registers it here; only
    // the device module may unwrap the handle — a resource created inside the DLSS unit read back all
    // zero, and reinterpreting a JVM handle there crashed on a null vtable.
    // Returns 1=accepted, 0=cleared, -1=not a live texture.
    return d3d12_mc::setDlssOutput(tex);
}

int32_t MCDR_CALL mcdr_dx12SetDlssVendorDir(const char* utf8Dir) {
    // The JVM announces <user.dir>\dxrmc\ (where MCDR.dll and the vendor DLLs were extracted) so native
    // can widen the DLL search path: cwd is the game directory and the executable is the JDK's java.exe,
    // so bare-name and exe-relative lookups both miss nvngx_dlss.dll.
    // Returns 1=accepted and path widened, 0=empty/rejected input.
    return mcdr_dlss_register_vendor_dir(utf8Dir, utf8Dir != nullptr ? static_cast<int32_t>(std::strlen(utf8Dir) + 1) : 0);
}

}  // extern "C"
