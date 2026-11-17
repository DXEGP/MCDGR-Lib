module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include "mcdr_dlss_impl.h"

module mcdr.d3d12.device;

namespace d3d12_mc {
namespace {
struct ReconstructionReceipt {
    std::mutex mutex;
    mcdr_frame_identity identity{};
    uint64_t historyGeneration = 0;
    uint64_t invalidationGeneration = 0;
    uint32_t consumer = 0;
    uint32_t backend = 0;
    bool valid = false;
};

ReconstructionReceipt& reconstructionReceipt() {
    static ReconstructionReceipt receipt;
    return receipt;
}

void multiplyCamera(const float* left, const float* right, float* output) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) {
            float value = 0;
            for (int inner = 0; inner < 4; ++inner)
                value += left[inner * 4 + row] * right[column * 4 + inner];
            output[column * 4 + row] = value;
        }
}

bool prepareDlssCamera(const mcdr_vanilla_frame_input& frame, bool reset) {
    if ((frame.flags & 1u) == 0 || (frame.current.flags & 1u) == 0) {
        mcdr_dlss_clear_camera_constants();
        return false;
    }
    mcdr_dlss_constants constants{};
    float previousView[16];
    std::memcpy(previousView, frame.previous.viewRotation, sizeof(previousView));
    const double delta[3] = {
        frame.current.origin[0] - frame.previous.origin[0],
        frame.current.origin[1] - frame.previous.origin[1],
        frame.current.origin[2] - frame.previous.origin[2]};
    for (int row = 0; row < 3; ++row)
        previousView[12 + row] = frame.previous.viewRotation[row] * static_cast<float>(delta[0]) +
            frame.previous.viewRotation[4 + row] * static_cast<float>(delta[1]) +
            frame.previous.viewRotation[8 + row] * static_cast<float>(delta[2]);
    multiplyCamera(frame.current.projection, frame.current.viewRotation, constants.viewProjection);
    multiplyCamera(frame.previous.projection, previousView, constants.prevViewProjection);
    std::memcpy(constants.projection, frame.current.projection, sizeof(constants.projection));
    constants.jitterOffsetX = static_cast<float>(frame.current.jitterX);
    constants.jitterOffsetY = -static_cast<float>(frame.current.jitterY);
    constants.mvecScaleX = frame.motionScaleX / frame.renderWidth;
    constants.mvecScaleY = frame.motionScaleY / frame.renderHeight;
    constants.cameraRightX = frame.current.viewRotation[0];
    constants.cameraRightY = frame.current.viewRotation[4];
    constants.cameraRightZ = frame.current.viewRotation[8];
    constants.cameraUpX = frame.current.viewRotation[1];
    constants.cameraUpY = frame.current.viewRotation[5];
    constants.cameraUpZ = frame.current.viewRotation[9];
    constants.cameraFwdX = frame.current.viewRotation[2];
    constants.cameraFwdY = frame.current.viewRotation[6];
    constants.cameraFwdZ = frame.current.viewRotation[10];
    constants.cameraNear = frame.current.nearPlane;
    constants.cameraFar = frame.current.farPlane;
    constants.cameraFovRadians = frame.current.verticalFovRadians;
    constants.cameraAspectRatio = static_cast<float>(frame.displayWidth) / frame.displayHeight;
    constants.depthInverted = 1;
    constants.cameraMotionIncluded = 1;
    constants.reset = reset ? 1u : 0u;
    constants.renderingGameFrames = 1;
    return mcdr_dlss_set_constants(&constants) == 1;
}
}

void invalidatePreparedReconstructionHistory() {
    auto& receipt = reconstructionReceipt();
    {
        std::lock_guard<std::mutex> lock(receipt.mutex);
        receipt.valid = false;
        ++receipt.invalidationGeneration;
    }
    mcdr_dlss_clear_camera_constants();
    invalidateFrameGenerationHistory(1);
}

int evaluatePreparedReconstruction(void* rawContext, const void* frameCopy,
    const void* preparedCopy, const void* requestCopy) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context || !frameCopy || !preparedCopy || !requestCopy) return -1;
    const auto& frame = *static_cast<const mcdr_vanilla_frame_input*>(frameCopy);
    const auto& prepared = *static_cast<const mcdr_prepared_frame*>(preparedCopy);
    const auto& request = *static_cast<const mcdr_reconstruction_request*>(requestCopy);
    if ((frame.flags & 1u) == 0) mcdr_dlss_clear_camera_constants();
    auto& receipt = reconstructionReceipt();
    uint64_t invalidationGeneration;
    bool compatible;
    {
        std::lock_guard<std::mutex> lock(receipt.mutex);
        invalidationGeneration = receipt.invalidationGeneration;
        compatible = receipt.valid && (frame.flags & 1u) != 0 &&
            receipt.identity.realFrameId == frame.previousRealFrameId &&
            receipt.identity.worldEpoch == frame.identity.worldEpoch &&
            receipt.identity.configVersion == frame.identity.configVersion &&
            receipt.identity.resourceVersion == frame.identity.resourceVersion &&
            receipt.historyGeneration == frame.historyGeneration &&
            receipt.consumer == prepared.consumer &&
            receipt.backend == frame.settings.reconstructionBackend;
        receipt.valid = false;
    }
    double cameraDistanceSquared = 0;
    float cameraDirectionDot = 0;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        const double displacement = frame.current.origin[axis] - frame.previous.origin[axis];
        cameraDistanceSquared += displacement * displacement;
        cameraDirectionDot += frame.current.viewRotation[2 + axis * 4] * frame.previous.viewRotation[2 + axis * 4];
    }
    const bool cameraCut = cameraDistanceSquared > 64.0 * 64.0 || cameraDirectionDot < 0.0f;
    const bool reset = !compatible || cameraCut || prepared.resetReasons != 0 || (request.flags & 1u) != 0;
    const auto finish = [&](int result) {
        std::lock_guard<std::mutex> lock(receipt.mutex);
        if (receipt.invalidationGeneration == invalidationGeneration) {
            receipt.valid = result == 1;
            if (receipt.valid) {
                receipt.identity = frame.identity;
                receipt.historyGeneration = frame.historyGeneration;
                receipt.consumer = prepared.consumer;
                receipt.backend = frame.settings.reconstructionBackend;
            }
        }
        return result;
    };
    const auto texture = [](const mcdr_prepared_binding& binding) {
        return reinterpret_cast<Dx12Object*>(binding.object);
    };
    for (const auto* binding : {&prepared.color, &prepared.depth, &prepared.motion,
        &prepared.exposure, &prepared.reactive, &prepared.composition, &prepared.responsive, &request.output})
        if (binding->object) retainObjectForRecording(context, texture(*binding));
    if (prepared.consumer == 1) {
        if ((frame.flags & 1u) == 0) return -1;
        return finish(applyTaaToTexture(context, texture(prepared.color), texture(prepared.motion),
            static_cast<float>(frame.current.jitterX), static_cast<float>(frame.current.jitterY),
            static_cast<float>(frame.previous.jitterX), static_cast<float>(frame.previous.jitterY),
            frame.settings.taaBlend, reset ? 1 : 0, frame.settings.taaSharpness));
    }
    if (prepared.consumer != 2 && prepared.consumer != 3) return -1;
    if (frame.settings.reconstructionBackend == MCDR_SR_BACKEND_NONE) return -1;
    if ((frame.current.flags & 1u) == 0) return -1;
    if (frame.settings.reconstructionBackend == MCDR_SR_BACKEND_DLSS) {
        if (!prepareDlssCamera(frame, reset)) return -1;
        const auto quality = frame.settings.antiAliasing == 3 ? 4 : static_cast<int32_t>(frame.settings.dlssQuality);
        if (mcdr_dlss_request_quality(quality) < 0 ||
            mcdr_dlss_request_preset(static_cast<int32_t>(frame.settings.dlssPreset)) < 0) return -1;
    }
    if (prepared.consumer == 3) {
        mcdr_dlss_input input{};
        input.renderWidth = frame.renderWidth; input.renderHeight = frame.renderHeight;
        input.targetWidth = frame.displayWidth; input.targetHeight = frame.displayHeight;
        input.jitterOffsetX = static_cast<float>(frame.current.jitterX);
        input.jitterOffsetY = -static_cast<float>(frame.current.jitterY);
        input.mvScaleX = frame.motionScaleX; input.mvScaleY = frame.motionScaleY;
        input.preExposure = frame.preExposure; input.exposureScale = 1;
        input.exposureMode = prepared.exposure.object ? MCDR_DLSS_EXPOSURE_AUTO : MCDR_DLSS_EXPOSURE_NONE;
        input.nearZ = frame.current.nearPlane; input.farZ = frame.current.farPlane;
        input.depthInverted = 1; input.reset = reset ? 1u : 0u;
        input.pInColor = prepared.color.object; input.pInDepth = prepared.depth.object;
        input.pInMotionVectors = prepared.motion.object; input.pInOutput = request.output.object;
        input.pInExposure = prepared.exposure.object;
        return finish(applyDlssToTexture(context, input));
    }
    mcdr_sr_dispatch dispatch{};
    dispatch.renderWidth = frame.renderWidth; dispatch.renderHeight = frame.renderHeight;
    dispatch.displayWidth = frame.displayWidth; dispatch.displayHeight = frame.displayHeight;
    dispatch.jitterOffsetX = static_cast<float>(frame.current.jitterX);
    dispatch.jitterOffsetY = -static_cast<float>(frame.current.jitterY);
    dispatch.motionVectorScaleX = frame.motionScaleX; dispatch.motionVectorScaleY = frame.motionScaleY;
    dispatch.preExposure = frame.preExposure; dispatch.frameTimeDeltaMs = frame.frameDeltaMs;
    dispatch.cameraNear = frame.current.nearPlane; dispatch.cameraFar = frame.current.farPlane;
    dispatch.cameraVerticalFov = frame.current.verticalFovRadians;
    dispatch.sharpness = frame.settings.providerSharpness;
    dispatch.reset = reset ? 1 : 0; dispatch.hdrInput = frame.colorEncoding == 3 ? 1 : 0;
    dispatch.backend = static_cast<int32_t>(frame.settings.reconstructionBackend);
    dispatch.pColorInput = prepared.color.object; dispatch.pColorOutput = request.output.object;
    dispatch.pDepth = prepared.depth.object; dispatch.pMotionVectors = prepared.motion.object;
    dispatch.pExposure = prepared.exposure.object;
    dispatch.pReactiveMask = dispatch.backend == MCDR_SR_BACKEND_XESS
        ? prepared.responsive.object : prepared.reactive.object;
    dispatch.pTransparencyAndComposition = prepared.composition.object;
    dispatch.cameraAspectRatio = static_cast<float>(frame.displayWidth) / frame.displayHeight;
    dispatch.viewSpaceToMetersFactor = frame.viewSpaceToMeters;
    return finish(srEvaluateFrame(context, dispatch));
}
}
