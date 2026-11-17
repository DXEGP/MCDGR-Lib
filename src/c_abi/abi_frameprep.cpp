module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"

import mcdr.render.frameprep;

extern "C" int32_t MCDR_CALL mcdr_dx12PrewarmFrameprep(
    mcdr_command_ctx* context, uint64_t generation, uint32_t flags) {
    try {
        const int32_t result = d3d12_mc::frameprep::prewarm(context, generation, flags);
        if (result <= 0) {
            mcdr::abi::setLastErrorf("mcdr_dx12PrewarmFrameprep",
                result == 0 ? "Invalid context, generation or graph selection" : "Frameprep Work Graph creation failed");
        }
        return result;
    } catch (...) {
        mcdr::abi::setLastErrorf("mcdr_dx12PrewarmFrameprep", "Frameprep Work Graph prewarm failed");
        return -1;
    }
}

extern "C" int32_t MCDR_CALL mcdr_dx12ProduceReactiveMask(
    mcdr_command_ctx* context, mcdr_object* source0, mcdr_object* source1, mcdr_object* output,
    int32_t width, int32_t height, uint64_t worldEpoch, uint64_t realFrameId,
    uint64_t sourceVersion, uint64_t viewportVersion) {
    if (context == nullptr || width <= 0 || height <= 0) return -1;
    try {
        return d3d12_mc::frameprep::produceReactiveMask({context, source0, source1, output,
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), worldEpoch, realFrameId,
            sourceVersion, viewportVersion, nullptr, 1});
    } catch (...) {
        return -1;
    }
}

extern "C" int32_t MCDR_CALL mcdr_dx12ProduceProviderMasks(
    mcdr_command_ctx* context, mcdr_object* source0, mcdr_object* source1, mcdr_object* output,
    mcdr_object* compositionOutput, int32_t width, int32_t height, uint32_t outputFlags,
    uint64_t worldEpoch, uint64_t realFrameId, uint64_t sourceVersion, uint64_t viewportVersion) {
    if (context == nullptr || width <= 0 || height <= 0) return -1;
    try {
        return d3d12_mc::frameprep::produceReactiveMask({context, source0, source1, output,
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), worldEpoch, realFrameId,
            sourceVersion, viewportVersion, compositionOutput, outputFlags});
    } catch (...) {
        return -1;
    }
}

extern "C" int32_t MCDR_CALL mcdr_dx12ProduceProviderMasksV2(
    mcdr_command_ctx* context, const mcdr_provider_mask_input* input) {
    if (!context || !input) return -1;
    try {
        const int result = d3d12_mc::frameprep::produceProviderMasks(context, input, sizeof(*input));
        if (result <= 0) mcdr::abi::setLastErrorf("mcdr_dx12ProduceProviderMasksV2",
            result == 0 ? "Invalid provider-mask classification input" : "Provider-mask classification failed");
        return result;
    } catch (...) {
        mcdr::abi::setLastErrorf("mcdr_dx12ProduceProviderMasksV2", "Provider-mask classification failed");
        return -1;
    }
}

extern "C" int32_t MCDR_CALL mcdr_dx12ProduceVrs(
    mcdr_command_ctx* context, mcdr_object* depth, mcdr_object* motion, mcdr_object* previous,
    mcdr_object* output, int32_t width, int32_t height, uint32_t tileSize, uint32_t edgePermille,
    uint32_t motionPixels, uint32_t flags, uint64_t worldEpoch, uint64_t realFrameId,
    uint64_t sourceVersion, uint64_t viewportVersion, mcdr_object* candidates,
    const float* previousToCurrent, const float* currentToPrevious) {
    if (context == nullptr || width <= 0 || height <= 0) {
        return mcdr::abi::failWith("mcdr_dx12ProduceVrs", "Invalid context or source dimensions");
    }
    try {
        const int result = d3d12_mc::frameprep::produceVrs({context, depth, motion, previous, output,
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), tileSize, edgePermille,
            motionPixels, flags, worldEpoch, realFrameId, sourceVersion, viewportVersion,
            candidates, previousToCurrent, currentToPrevious});
        if (result <= 0) {
            mcdr::abi::setLastErrorf("mcdr_dx12ProduceVrs", result == 0 ?
                "Invalid or incompatible VRS inputs" : "VRS Work Graph creation or dispatch failed");
        }
        return result;
    } catch (...) {
        return mcdr::abi::failWith("mcdr_dx12ProduceVrs", "VRS production threw an exception");
    }
}

extern "C" int32_t MCDR_CALL mcdr_dx12ProduceExposure(
    mcdr_command_ctx* context, mcdr_object* color, mcdr_object* output,
    int32_t width, int32_t height, uint32_t colorEncoding, float preExposure) {
    if (context == nullptr || width <= 0 || height <= 0) {
        return mcdr::abi::failWith("mcdr_dx12ProduceExposure", "Invalid context or source dimensions");
    }
    try {
        const int result = d3d12_mc::frameprep::produceExposure({context, color, output,
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), colorEncoding, preExposure});
        if (result <= 0) {
            mcdr::abi::setLastErrorf("mcdr_dx12ProduceExposure", "Exposure metering failed");
        }
        return result;
    } catch (...) {
        return mcdr::abi::failWith("mcdr_dx12ProduceExposure", "Exposure metering threw an exception");
    }
}

extern "C" void MCDR_CALL mcdr_dx12ResetFrameprep(mcdr_command_ctx* context) {
    d3d12_mc::frameprep::reset(context);
}

