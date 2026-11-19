// abi_pipeline.cpp — graphics pipeline creation/destruction
//
// Group: graphics pipelines (originally abi_bridge_p4.cpp).
// 合并自：abi_bridge_p4.cpp
//
module;

#include "_d3d12_module_prelude.h"
#include "mcdr_graphics_capture.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;
import mcdr.render.oit;

using namespace d3d12_mc;

// ===========================================================================
// 本文件是合并后的 C ABI 适配单元，按**功能**命名（不再按移植阶段 pX）。
// 共享前言（prelude / mcdr_abi.h / 各模块 import）统一放在文件顶部，下面每一段
// 保留原单元的文件头注释与代码本体，内容逐字节未改。
// ===========================================================================

// ---------------------------------------------------------------------------
// 来源单元：abi_bridge_p4.cpp
// ---------------------------------------------------------------------------

// abi_pipeline.cpp — graphics pipeline creation/destruction.（合并自 abi_bridge_p4.cpp）
//
// Replaces jni_bridge_p4.cpp (172 lines). That file's entire body was a
// hand-written little-endian unpacker for the packed descriptor ByteBuffer the
// Java side built with putInt/putU8. Because the blob carried no field
// identifiers, every offset was a magic number on both sides and a single
// mis-ordered field silently produced a wrong pipeline; the semantic names even
// had to travel in a separate trailing section and were re-associated with
// their input elements by array index.
//
// Here the descriptor is a real struct (mcdr_pipeline_desc) whose layout the
// Java side mirrors with MemoryLayout.structLayout and whose offsets are
// verified at load time by mcdrABILayout(). There is nothing left to unpack.

namespace {

// Bounds the old unpacker enforced inline. Kept as named constants because they
// are the contract with createGraphicsPipeline(), not incidental limits.
constexpr int32_t kMaxColorTargets = 8;
constexpr int32_t kMaxInputElements = 64;
constexpr int32_t kMaxPipelineBindings = 64;

constexpr const char* kCreate = "mcdr_dx12CreateGraphicsPipeline";

using PipelinePtr = d3d12_mc::Dx12Pipeline;

}  // namespace

extern "C" {

PipelinePtr* MCDR_CALL mcdr_dx12CreateGraphicsPipeline(const mcdr_pipeline_desc* desc) {
    return mcdr_dx12CreateGraphicsPipelineWithConstants(desc, 0);
}

static PipelinePtr* createGraphicsPipelineErased(const mcdr_pipeline_desc* desc, int32_t byteSize,
    const mcdr_oit_capture_pipeline_v1* capture, std::shared_ptr<const void> normalizationProvenance = {},
    d3d12_mc::PipelineDesc::OpaqueSeedShaders opaqueSeed = {}) {
    using mcdr::abi::failWithPtr;
    using mcdr::abi::setLastErrorf;

    if (!desc) {
        return failWithPtr<PipelinePtr>(kCreate, "descriptor is null");
    }

    d3d12_mc::PipelineDesc d;
    d.normalizationProvenance = std::move(normalizationProvenance);
    d.opaqueSeed = opaqueSeed;
    if (byteSize < 0 || byteSize > 128 || byteSize % 4 != 0) {
        return failWithPtr<PipelinePtr>(kCreate, "invalid root constant size");
    }
    d.pushConstantBytes = static_cast<uint32_t>(byteSize);
    if (capture) {
        if (!mcdr::graphics_capture::valid(capture))
            return failWithPtr<PipelinePtr>(kCreate, "Invalid capture shader contract");
        d.captureHash = capture->shaderContractHash;
        d.captureGeneration = capture->programGeneration;
    }
    if ((desc->pipelineFlags & ~1u) != 0u) {
        return failWithPtr<PipelinePtr>(kCreate, "unsupported pipeline flags");
    }
    d.rasterMotion = (desc->pipelineFlags & 1u) != 0u;

    // --- shader bytecode ---------------------------------------------------
    // The old path went through NewDirectByteBuffer + GetDirectBufferAddress,
    // which rejected heap buffers with a bare stderr line. A null/length
    // mismatch is now an explicit, reportable error.
    if (desc->vsLen < 0 || (desc->vsLen > 0 && desc->vsData == nullptr)) {
        return failWithPtr<PipelinePtr>(kCreate, "vsData/vsLen mismatch");
    }
    if (desc->psLen < 0 || (desc->psLen > 0 && desc->psData == nullptr)) {
        return failWithPtr<PipelinePtr>(kCreate, "psData/psLen mismatch");
    }
    if (desc->vsLen > 0) {
        const auto* first = static_cast<const uint8_t*>(desc->vsData);
        d.vsBytes.assign(first, first + desc->vsLen);
    }
    if (desc->psLen > 0) {
        const auto* first = static_cast<const uint8_t*>(desc->psData);
        d.psBytes.assign(first, first + desc->psLen);
    }

    // --- colour targets ----------------------------------------------------
    if (desc->colorCount < 0 || desc->colorCount > kMaxColorTargets) {
        setLastErrorf(kCreate, "bad colorCount %d", static_cast<int>(desc->colorCount));
        return nullptr;
    }
    if (desc->colorCount > 0 && desc->colorTargets == nullptr) {
        return failWithPtr<PipelinePtr>(kCreate, "colorTargets is null");
    }
    d.colorCount = desc->colorCount;
    d.colorTargets.resize(static_cast<size_t>(desc->colorCount));
    for (int32_t i = 0; i < desc->colorCount; ++i) {
        const mcdr_color_target_desc& src = desc->colorTargets[i];
        d3d12_mc::PipelineDesc::ColorTarget& dst = d.colorTargets[static_cast<size_t>(i)];
        dst.format = src.format;
        dst.writeMask = src.writeMask;
        dst.blendEnabled = src.blendEnabled != 0;
        dst.srcColor = src.srcColor;
        dst.dstColor = src.dstColor;
        dst.colorOp = src.colorOp;
        dst.srcAlpha = src.srcAlpha;
        dst.dstAlpha = src.dstAlpha;
        dst.alphaOp = src.alphaOp;
    }

    // --- depth state -------------------------------------------------------
    d.hasDepth = desc->hasDepth != 0;
    if (d.hasDepth) {
        d.depthFormat = desc->depth.depthFormat;
        d.depthWrite = desc->depth.depthWrite != 0;
        d.depthCompareOp = desc->depth.depthCompareOp;
    }

    d.topology = desc->topology;
    d.cullEnabled = desc->cullEnabled != 0;
    d.polygonMode = desc->polygonMode;

    // --- input elements ----------------------------------------------------
    if (desc->inputElementCount < 0 || desc->inputElementCount > kMaxInputElements) {
        setLastErrorf(kCreate, "bad inputElementCount %d", static_cast<int>(desc->inputElementCount));
        return nullptr;
    }
    if (desc->inputElementCount > 0 && desc->inputElements == nullptr) {
        return failWithPtr<PipelinePtr>(kCreate, "inputElements is null");
    }
    d.inputElements.resize(static_cast<size_t>(desc->inputElementCount));
    for (int32_t i = 0; i < desc->inputElementCount; ++i) {
        const mcdr_input_element_desc& src = desc->inputElements[i];
        d3d12_mc::PipelineDesc::InputElement& dst = d.inputElements[static_cast<size_t>(i)];
        dst.location = src.location;
        dst.binding = src.binding;
        dst.format = src.format;
        dst.offset = src.offset;
        dst.stride = src.stride;
        dst.stepRate = src.stepRate;
        // NULL or "" keeps std::string empty, which createGraphicsPipeline()
        // already understands as "fall back to TEXCOORD". This is the field the
        // packed Java blob could not express at all: it had to ship the names in
        // a separate trailing section and hope the ordering matched.
        if (src.semanticName != nullptr && src.semanticName[0] != '\0') {
            dst.semanticName = src.semanticName;
        }
    }

    // --- root bindings -----------------------------------------------------
    if (desc->bindingCount < 0 || desc->bindingCount > kMaxPipelineBindings) {
        setLastErrorf(kCreate, "bad bindingCount %d", static_cast<int>(desc->bindingCount));
        return nullptr;
    }
    if (desc->bindingCount > 0 && desc->bindings == nullptr) {
        return failWithPtr<PipelinePtr>(kCreate, "bindings is null");
    }
    d.bindings.resize(static_cast<size_t>(desc->bindingCount));
    for (int32_t i = 0; i < desc->bindingCount; ++i) {
        d.bindings[static_cast<size_t>(i)].type = desc->bindings[i].type;
        d.bindings[static_cast<size_t>(i)].reg = desc->bindings[i].reg;
    }

    const auto r = d3d12_mc::createGraphicsPipeline(d);
    if (!r) {
        // The old bridge printed err to stderr and returned 0, so Java only ever
        // saw "pipeline handle is 0". The message now survives the boundary.
        return failWithPtr<PipelinePtr>(kCreate, r.error().message);
    }
    return static_cast<PipelinePtr*>(*r);
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateGraphicsPipelineWithConstants(const mcdr_pipeline_desc* desc, int32_t byteSize) {
    return createGraphicsPipelineErased(desc, byteSize, nullptr);
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateOitNormalizationPipelineV1(const mcdr_pipeline_desc* descriptor,
    const mcdr_oit_normalization_pipeline_v1* certification) {
    constexpr const char* operation = "mcdr_dx12CreateOitNormalizationPipelineV1";
    if (!descriptor || !certification)
        return mcdr::abi::failWithPtr<PipelinePtr>(operation, "Missing normalization descriptor or certification");
    try {
        std::shared_ptr<const void> provenance;
        const int result = d3d12_mc::oit::createNormalizationPipelineCertification(*descriptor, *certification, &provenance);
        if (result != 1 || !provenance) {
            mcdr::abi::setLastErrorf(operation, "Normalization certification failed (%d)", result);
            return nullptr;
        }
        return createGraphicsPipelineErased(descriptor, 0, nullptr, std::move(provenance));
    } catch (const std::bad_alloc&) {
        return mcdr::abi::failWithPtr<PipelinePtr>(operation, "Normalization pipeline allocation failed");
    } catch (const std::exception& failure) {
        return mcdr::abi::failWithPtr<PipelinePtr>(operation, failure.what());
    } catch (...) {
        return mcdr::abi::failWithPtr<PipelinePtr>(operation, "Normalization pipeline creation failed");
    }
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateOitLaneOperationPipelineV1(const mcdr_pipeline_desc* desc,
    uint32_t kind, uint64_t programGeneration, uint32_t projectionBinding) {
    constexpr const char* operation = "mcdr_dx12CreateOitLaneOperationPipelineV1";
    const bool depthCopy = kind == MCDR_OIT_LANE_DEPTH_COPY;
    const bool boundsCopy = kind == MCDR_OIT_LANE_BOUNDS_COPY;
    const bool boundsCull = kind == MCDR_OIT_LANE_BOUNDS_CULL;
    if (!desc || !programGeneration || (!depthCopy && !boundsCopy && !boundsCull) || desc->pipelineFlags ||
        desc->topology != 4 || desc->inputElementCount || desc->polygonMode != 0 ||
        (depthCopy ? (desc->colorCount < 0 || desc->colorCount > 1) : desc->colorCount != 1) ||
        (desc->colorCount && !desc->colorTargets) || desc->bindingCount < 1 ||
        desc->bindingCount > kMaxPipelineBindings || !desc->bindings)
        return mcdr::abi::failWithPtr<PipelinePtr>(operation, "Invalid lane operation pipeline");
    if ((!boundsCopy && (desc->hasDepth != 1 || desc->depth.depthFormat != 51 ||
            desc->depth.depthWrite != 1 || desc->depth.depthCompareOp != (boundsCull ? 5u : 0u))) ||
        (boundsCopy && desc->hasDepth))
        return mcdr::abi::failWithPtr<PipelinePtr>(operation, "Invalid lane operation depth state");
    if (desc->colorCount) {
        const auto& color = desc->colorTargets[0];
        if (toDxgiFormat(color.format) != DXGI_FORMAT_R32G32B32A32_FLOAT ||
            color.writeMask != (depthCopy ? 0u : 15u) || color.blendEnabled)
            return mcdr::abi::failWithPtr<PipelinePtr>(operation, "Invalid lane operation color state");
    }
    if (boundsCull ? projectionBinding >= static_cast<uint32_t>(desc->bindingCount) ||
            desc->bindings[projectionBinding].type != 0 :
            projectionBinding != UINT32_MAX || desc->bindingCount != 1 || desc->bindings[0].type != 1)
        return mcdr::abi::failWithPtr<PipelinePtr>(operation, "Invalid lane operation bindings");
    auto* pipeline = createGraphicsPipelineErased(desc, 0, nullptr);
    if (pipeline) pipeline->oitLaneOperation = {kind, projectionBinding, programGeneration};
    return pipeline;
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateOpaqueSeedPipelineV1(const mcdr_pipeline_desc* desc,
    int32_t pushConstantBytes, const void* tileVertex, int32_t tileVertexBytes, const void* tileFragment,
    int32_t tileFragmentBytes, uint64_t programGeneration) {
    constexpr const char* operation = "mcdr_dx12CreateOpaqueSeedPipelineV1";
    if (!desc || !programGeneration || !tileVertex || tileVertexBytes <= 0 || !tileFragment || tileFragmentBytes <= 0 ||
        desc->hasDepth != 1 || desc->depth.depthWrite != 1 || toDxgiFormat(desc->depth.depthFormat) != DXGI_FORMAT_D32_FLOAT ||
        (desc->pipelineFlags & ~1u) || desc->polygonMode != 0)
        return mcdr::abi::failWithPtr<PipelinePtr>(operation, "Invalid opaque depth seed pipeline");
    try {
        return createGraphicsPipelineErased(desc, pushConstantBytes, nullptr, {}, {
            {static_cast<const char*>(tileVertex), static_cast<size_t>(tileVertexBytes)},
            {static_cast<const char*>(tileFragment), static_cast<size_t>(tileFragmentBytes)}, programGeneration});
    } catch (const std::exception& failure) {
        return mcdr::abi::failWithPtr<PipelinePtr>(operation, failure.what());
    }
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateOitTileIAPipelineV1(const mcdr_pipeline_desc* desc,
    int32_t pushConstantBytes, const void* tileVertex, int32_t tileVertexBytes, const void* tileFragment,
    int32_t tileFragmentBytes, uint64_t programGeneration, const uint32_t* laneRegisters, uint32_t stage) try {
    constexpr const char* operation = "mcdr_dx12CreateOitTileIAPipelineV1";
    if (!desc || !programGeneration || !tileVertex || tileVertexBytes <= 0 || !tileFragment ||
        tileFragmentBytes <= 0 || !laneRegisters || stage > 2 || desc->pipelineFlags ||
        desc->hasDepth != 1 || toDxgiFormat(desc->depth.depthFormat) != DXGI_FORMAT_D32_FLOAT)
        return mcdr::abi::failWithPtr<PipelinePtr>(operation, "Invalid tile IA pipeline");
    d3d12_mc::PipelineDesc::OpaqueSeedShaders route{
        {static_cast<const char*>(tileVertex), static_cast<size_t>(tileVertexBytes)},
        {static_cast<const char*>(tileFragment), static_cast<size_t>(tileFragmentBytes)}, programGeneration, stage};
    std::copy_n(laneRegisters, route.laneRegisters.size(), route.laneRegisters.begin());
    return createGraphicsPipelineErased(desc, pushConstantBytes, nullptr, {}, route);
} catch (const std::exception& failure) {
    return mcdr::abi::failWithPtr<PipelinePtr>("mcdr_dx12CreateOitTileIAPipelineV1", failure.what());
}

int32_t MCDR_CALL mcdr_dx12AttachOitTileIARouteV1(PipelinePtr* source, PipelinePtr* base) {
    constexpr const char* operation = "mcdr_dx12AttachOitTileIARouteV1";
    const auto bindingPrefix = [](const auto& baseBindings, const auto& sourceBindings) {
        return sourceBindings.size() >= baseBindings.size() &&
            std::equal(baseBindings.begin(), baseBindings.end(), sourceBindings.begin());
    };
    if (!source || !base || source == base || !base->tileRoute || source->tileRoute ||
        base->tileRoute->tileStage > 2 || source->isMesh || base->isMesh ||
        source->topology != base->topology || source->vertexStrides != base->vertexStrides ||
        source->pushConstantBytes != base->pushConstantBytes || source->depthWrite != base->depthWrite ||
        source->depthTargetFormat != base->depthTargetFormat ||
        !bindingPrefix(base->graphicsBindingTypes, source->graphicsBindingTypes) ||
        !bindingPrefix(base->graphicsBindingRegisters, source->graphicsBindingRegisters) ||
        !bindingPrefix(base->colorTargetFormats, source->colorTargetFormats))
        return mcdr::abi::failWith(operation, "Tile IA variant differs from its source layout");
    source->tileRoute = base->tileRoute;
    return 1;
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateGraphicsCapturePipelineV1(const mcdr_pipeline_desc* desc, uint32_t byteSize,
    const mcdr_oit_capture_pipeline_v1* capture) {
    if (!mcdr::graphics_capture::valid(capture) || byteSize > 128) return nullptr;
    return createGraphicsPipelineErased(desc, static_cast<int32_t>(byteSize), capture);
}

void MCDR_CALL mcdr_dx12DestroyPipeline(PipelinePtr* pipeline) { d3d12_mc::destroyPipeline(pipeline); }

int32_t MCDR_CALL mcdr_dx12PushGraphicsConstants(mcdr_command_ctx* context, const void* data, int32_t byteSize) {
    if (byteSize < 0 || byteSize > 128 || byteSize % 4 != 0) return -1;
    auto result = d3d12_mc::pushGraphicsConstants(context, data, static_cast<uint32_t>(byteSize));
    if (!result) return mcdr::abi::failResult("mcdr_dx12PushGraphicsConstants", result.error());
    return 1;
}

int32_t MCDR_CALL mcdr_dx12EnsureGraphicsState(mcdr_command_ctx* rawContext) {
    auto* context = static_cast<d3d12_mc::CommandContext*>(rawContext);
    if (!context || !context->listOpen || !context->currentPipeline) return -1;
    if (context->cachedPso) return 1;
    d3d12_mc::restoreDrawHeap(context);
    auto result = d3d12_mc::setPipeline(context, context->currentPipeline, context->graphicsHasDepth);
    if (!result) return mcdr::abi::failResult("mcdr_dx12EnsureGraphicsState", result.error());
    return 2;
}

}  // extern "C"
