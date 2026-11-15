// abi_pipeline.cpp — graphics pipeline creation/destruction.
//
// The descriptor is a real struct (mcdr_pipeline_desc) whose layout the JVM mirrors with
// MemoryLayout and verifies through mcdrABILayout(), so there is nothing to unpack here.
// 见 docs\c_abi\README.md §3.2

module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;

using namespace d3d12_mc;

namespace {

// Contract with createGraphicsPipeline(), not incidental limits.
constexpr int32_t kMaxColorTargets = 8;
constexpr int32_t kMaxInputElements = 64;
constexpr int32_t kMaxPipelineBindings = 64;

constexpr const char* kCreate = "mcdr_dx12CreateGraphicsPipeline";

using PipelinePtr = d3d12_mc::Dx12Pipeline;

}  // namespace

extern "C" {

PipelinePtr* MCDR_CALL mcdr_dx12CreateGraphicsPipeline(const mcdr_pipeline_desc* desc) {
    using mcdr::abi::failWithPtr;
    using mcdr::abi::setLastErrorf;

    if (!desc) {
        return failWithPtr<PipelinePtr>(kCreate, "descriptor is null");
    }

    d3d12_mc::PipelineDesc d;

    // --- shader bytecode ---------------------------------------------------
    // A null pointer with a non-zero length is an explicit, reportable error.
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
        // An empty semanticName stays empty, which createGraphicsPipeline() reads as
        // "fall back to TEXCOORD".
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
        // The message crosses the boundary; Java used to see only a null handle.
        return failWithPtr<PipelinePtr>(kCreate, r.error().message);
    }
    return static_cast<PipelinePtr*>(*r);
}

void MCDR_CALL mcdr_dx12DestroyPipeline(PipelinePtr* pipeline) { d3d12_mc::destroyPipeline(pipeline); }

}  // extern "C"
