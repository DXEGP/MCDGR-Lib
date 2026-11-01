// abi_sr.cpp — SR C ABI adapter, module mcdr.abi.common: validate, fill the POD table, forward to the
// provider registry in src\advanced\d3d12_sr.cpp. 见 docs\advanced\upscalers.md

module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

import mcdr.d3d12.device;
// mcdr.error 是叶子模块，不 import 就取不到 mcdr::Error（import 不传递）。
import mcdr.error;

using namespace d3d12_mc;

extern "C" {

// Records the wanted backend only; the rebuild happens at the next frame start.
int32_t MCDR_CALL mcdr_dx12SrPick(int32_t backend) {
    return srPickBackend(backend);
}

// `available` is the verdict; the return value only means the table was filled.
int32_t MCDR_CALL mcdr_dx12SrQueryCaps(int32_t backend, mcdr_sr_caps* out) {
    if (out == nullptr) {
        return -1;
    }
    // 0..3 之外一律 -1：非法 id 若填成 "unknown backend id" 再返回成功，调用方会当成表有效。
    if (backend < MCDR_SR_BACKEND_NONE || backend > MCDR_SR_BACKEND_XESS) {
        return -1;
    }
    srFillCaps(backend, *out);
    return 1;
}

// Wakes/creates the backend; provider::initialize owns the context and DLSS defers feature creation
// to the first evaluate (NGX CreateFeature records into an open list). Repeating the same backend is
// idempotent and only updates sizes.
int32_t MCDR_CALL mcdr_dx12SrCreate(mcdr_command_ctx* ctx, int32_t backend, int32_t maxRenderWidth,
                                    int32_t maxRenderHeight, int32_t displayWidth,
                                    int32_t displayHeight) {
    return srCreateProvider(ctx, backend, maxRenderWidth, maxRenderHeight, displayWidth,
                            displayHeight);
}

// 0 = no active backend, not an error; DLSS takes its size from the per-frame payload.
int32_t MCDR_CALL mcdr_dx12SrResize(mcdr_command_ctx* ctx, int32_t renderWidth, int32_t renderHeight,
                                    int32_t displayWidth, int32_t displayHeight) {
    static_cast<void>(ctx);
    return srResizeProvider(renderWidth, renderHeight, displayWidth, displayHeight);
}

// Per-frame submit; desc->backend == 0 uses the backend SrPick recorded.
int32_t MCDR_CALL mcdr_dx12SrEvaluate(mcdr_command_ctx* ctx, const mcdr_sr_dispatch* desc) {
    if (ctx == nullptr || desc == nullptr) {
        return -1;
    }
    return srEvaluateFrame(ctx, *desc);
}

// 0 = this backend has no phase table, so the caller keeps D3D12Temporal's Halton(2,3). One jitter
// source only, on purpose: a second one makes the image shake.
int32_t MCDR_CALL mcdr_dx12SrJitter(int32_t frameIndex, float* outX, float* outY) {
    static_cast<void>(frameIndex);
    if (outX == nullptr || outY == nullptr) {
        return -1;
    }
    return 0;
}

// 0 = not applicable (backend NONE), -1 = bad argument (same convention as the rest).
int32_t MCDR_CALL mcdr_dx12SrJitterPhaseCount(int32_t backend, int32_t renderWidth,
                                              int32_t displayWidth) {
    if (backend < MCDR_SR_BACKEND_NONE || backend > MCDR_SR_BACKEND_XESS) {
        return -1;
    }
    return srJitterPhaseCount(backend, renderWidth, displayWidth);
}

// No open command list allowed; repeated calls are safe. Clears the picked backend, else a later
// Evaluate runs against a provider that no longer exists.
void MCDR_CALL mcdr_dx12SrDestroy(void) {
    srReleaseProvider();
    (void)srPickBackend(MCDR_SR_BACKEND_NONE);
}

// One-line availability text via writeString: (NULL,0) probes size+1, a short buffer is not an error.
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

}  // extern "C"