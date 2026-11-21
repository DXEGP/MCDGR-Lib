module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"

import mcdr.d3d12.device;
import mcdr.error;

using namespace d3d12_mc;

extern "C" {

int32_t MCDR_CALL mcdr_dx12SrPick(int32_t backend) {
    return srPickBackend(backend);
}

int32_t MCDR_CALL mcdr_dx12SrQueryCaps(int32_t backend, mcdr_sr_caps* out) {
    if (out == nullptr) {
        return -1;
    }
    if (backend < MCDR_SR_BACKEND_NONE || backend > MCDR_SR_BACKEND_XESS) {
        return -1;
    }
    srFillCaps(backend, *out);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SrCreate(mcdr_command_ctx* ctx, int32_t backend, int32_t maxRenderWidth,
                                    int32_t maxRenderHeight, int32_t displayWidth,
                                    int32_t displayHeight) {
    return srCreateProvider(ctx, backend, maxRenderWidth, maxRenderHeight, displayWidth,
                            displayHeight);
}

int32_t MCDR_CALL mcdr_dx12SrResize(mcdr_command_ctx* ctx, int32_t renderWidth, int32_t renderHeight,
                                    int32_t displayWidth, int32_t displayHeight) {
    return srResizeProvider(ctx, renderWidth, renderHeight, displayWidth, displayHeight);
}

int32_t MCDR_CALL mcdr_dx12SrPrepare(mcdr_command_ctx* ctx,
                                     const mcdr_sr_dispatch* description,
                                     int32_t dlssQuality, int32_t dlssPreset, uint64_t* token) {
    if (token != nullptr) *token = 0;
    if (description == nullptr || token == nullptr) {
        mcdr::abi::setLastError("mcdr_dx12SrPrepare", "Null description or token output");
        return MCDR_SR_PREPARE_INVALID_ARGUMENT;
    }
    const int32_t result = srPrepareProvider(ctx, *description, dlssQuality, dlssPreset, token);
    if (result < 0) mcdr::abi::setLastError("mcdr_dx12SrPrepare", srLastFailureText());
    return result;
}

int32_t MCDR_CALL mcdr_dx12SrCommit(uint64_t token) {
    return srCommitPreparedProvider(token);
}

int32_t MCDR_CALL mcdr_dx12SrAbort(uint64_t token) {
    return srAbortPreparedProvider(token);
}

int32_t MCDR_CALL mcdr_dx12SrJitter(int32_t frameIndex, float* outX, float* outY) {
    static_cast<void>(frameIndex);
    if (outX == nullptr || outY == nullptr) {
        return -1;
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12SrJitterPhaseCount(int32_t backend, int32_t renderWidth,
                                              int32_t displayWidth) {
    if (backend < MCDR_SR_BACKEND_NONE || backend > MCDR_SR_BACKEND_XESS) {
        return -1;
    }
    return srJitterPhaseCount(backend, renderWidth, displayWidth);
}

void MCDR_CALL mcdr_dx12SrDestroy(void) {
    (void)srReleaseProvider();
}

int32_t MCDR_CALL mcdr_dx12SrQuery(char* note, int32_t noteCap) {
    if (noteCap < 0 || (noteCap > 0 && note == nullptr)) {
        return -1;
    }
    const std::string text = srStatusText();
    if (note == nullptr || noteCap == 0) {
        return static_cast<int32_t>(text.size());
    }
    return mcdr::abi::writeString(text, note, noteCap);
}

}
