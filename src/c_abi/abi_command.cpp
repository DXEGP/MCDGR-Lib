// abi_command.cpp — command layer: encoder lifecycle, copies, clears, render pass, timestamp
// queries and the async-renderer control channel. 见 docs\abi\README.md
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
// mcdr.error 是叶子模块，不 import 就取不到 mcdr::Error（import 不传递）。
import mcdr.error;

using namespace d3d12_mc;

// Handles are the native objects themselves (mcdr_object IS d3d12_mc::Dx12Object, see
// include\mcdr_abi.h), so nothing is converted here; render-pass inputs arrive as native pointers.
extern "C" {

int64_t MCDR_CALL MCDR_D3D12_get_timestamp_frequency(void) {
    return static_cast<int64_t>(d3d12_mc::getTimestampFrequency());
}

mcdr_command_ctx* MCDR_CALL MCDR_D3D12_create_command_encoder(void) {
    const auto r = d3d12_mc::createCommandEncoder();
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::CommandContext>("MCDR_D3D12_create_command_encoder",
                                                               r.error().message);
    }
    return static_cast<d3d12_mc::CommandContext*>(*r);
}

void MCDR_CALL mcdr_dx12DestroyCommandEncoder(mcdr_command_ctx* ctx) { d3d12_mc::destroyCommandEncoder(ctx); }

void MCDR_CALL mcdr_dx12BeginCommandList(mcdr_command_ctx* ctx) {
    if (const auto r = d3d12_mc::beginCommandList(ctx); !r) {
        mcdr::abi::failVoid("mcdr_dx12BeginCommandList", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12BeginCommandListWithWait(mcdr_command_ctx* ctx, uint64_t waitForValue) {
    if (const auto r = d3d12_mc::beginCommandListWithWait(ctx, waitForValue); !r) {
        mcdr::abi::failVoid("mcdr_dx12BeginCommandListWithWait", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12EndCommandList(mcdr_command_ctx* ctx) {
    if (const auto r = d3d12_mc::endCommandList(ctx); !r) {
        mcdr::abi::failVoid("mcdr_dx12EndCommandList", r.error().message);
    }
}

uint64_t MCDR_CALL mcdr_dx12Submit(mcdr_command_ctx* ctx) {
    d3d12_mc::dbgLogDebug("submit: enter ctx=%p", static_cast<void*>(ctx));
    const auto r = d3d12_mc::submitCommandList(ctx);
    // Returns this ctx's per-command-list sequence number, which mcdr_dx12WaitForFence accepts;
    // 0 means "nothing to submit" and an empty error message means the same.
    if (!r && !r.error().message.empty()) {
        mcdr::abi::failVoid("mcdr_dx12Submit", r.error().message);
    }
    return static_cast<uint64_t>(r.value_or(0));
}

int32_t MCDR_CALL mcdr_dx12WaitForFence(mcdr_command_ctx* ctx, uint64_t value, int64_t timeoutNs) {
    // Waits on THIS ctx's own per-command-list fence: value comes from
    // mcdr_dx12GetFenceValue / mcdr_dx12Submit, all three on one timeline, so a one-shot upload
    // encoder can never advance a frame encoder's token.
    const auto r = d3d12_mc::waitForContextFenceValue(ctx, static_cast<UINT64>(value),
                                                     static_cast<UINT64>(timeoutNs));
    // A timeout is success-path semantics (GpuFence.awaitCompletion returns false, it does not
    // throw, and StagedVertexBuffer reclamation depends on that): only a real failure sets lastError.
    if (!r && r.error().message.find("timed out") == std::string::npos) {
        mcdr::abi::failVoid("mcdr_dx12WaitForFence", r.error().message);
    }
    return r ? 1 : 0;
}

uint64_t MCDR_CALL mcdr_dx12GetFenceValue(mcdr_command_ctx* ctx) {
    // createFence() 捕获的是本 ctx 的当前 per-command-list 序号，target = 该值 + 1；
    // 等待对象是同一个 ctx 的 fence（BUG-03）。
    return static_cast<uint64_t>(d3d12_mc::currentFenceValue(ctx));
}

int64_t MCDR_CALL mcdr_dx12GetTimestampNow(mcdr_command_ctx* ctx) {
    const auto r = d3d12_mc::getTimestampNow(ctx);
    if (!r && !r.error().message.empty()) {
        mcdr::abi::failVoid("mcdr_dx12GetTimestampNow", r.error().message);
    }
    return static_cast<int64_t>(r.value_or(0));
}

int32_t MCDR_CALL mcdr_dx12IsListOpen(mcdr_command_ctx* ctx) { return d3d12_mc::isListOpen(ctx) ? 1 : 0; }

void MCDR_CALL mcdr_dx12CopyBuffer(mcdr_command_ctx* ctx, mcdr_object* src, int64_t srcOffset,
                                   mcdr_object* dst, int64_t dstOffset, int64_t size) {
    DBG_LOG_DEBUG("copyBuffer: src=%p(%lld) dst=%p(%lld) size=%lld", static_cast<void*>(src),
                  (long long)srcOffset, static_cast<void*>(dst), (long long)dstOffset, (long long)size);
    if (const auto r = d3d12_mc::copyBufferToBuffer(ctx, src, srcOffset, dst, dstOffset, size); !r) {
        mcdr::abi::failVoid("mcdr_dx12CopyBuffer", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12WriteToTexture(mcdr_command_ctx* ctx, mcdr_object* stagingBuffer,
                                       int64_t stagingOffset, int32_t width, int32_t height,
                                       mcdr_object* dstTexture, int32_t mip, int32_t layer, int32_t dstX,
                                       int32_t dstY) {
    if (const auto r = d3d12_mc::copyBufferToTexture(ctx, stagingBuffer, stagingOffset, width, height, dstTexture,
                                                     mip, layer, dstX, dstY, width, height);
        !r) {
        mcdr::abi::failVoid("mcdr_dx12WriteToTexture", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12CopyBufferToTexture(mcdr_command_ctx* ctx, mcdr_object* srcBuffer, int64_t srcOffset,
                                            int32_t srcWidth, int32_t srcHeight, mcdr_object* dstTexture,
                                            int32_t mip, int32_t layer, int32_t dstX, int32_t dstY,
                                            int32_t width, int32_t height) {
    if (const auto r =
            d3d12_mc::copyBufferToTexture(ctx, srcBuffer, srcOffset, srcWidth, srcHeight, dstTexture, mip, layer,
                                          dstX, dstY, width, height);
        !r) {
        mcdr::abi::failVoid("mcdr_dx12CopyBufferToTexture", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12CopyTextureToBuffer(mcdr_command_ctx* ctx, mcdr_object* srcTexture, int32_t mip,
                                            int32_t layer, int32_t srcX, int32_t srcY, int32_t width,
                                            int32_t height, mcdr_object* dstBuffer, int64_t dstOffset) {
    if (const auto r = d3d12_mc::copyTextureToBuffer(ctx, srcTexture, mip, layer, srcX, srcY, width, height,
                                                    dstBuffer, dstOffset);
        !r) {
        mcdr::abi::failVoid("mcdr_dx12CopyTextureToBuffer", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12CopyTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                             mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                             int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY,
                                             int32_t width, int32_t height) {
    if (const auto r = d3d12_mc::copyTextureToTexture(ctx, srcTexture, dstTexture, mip, layer, srcX, srcY, dstX,
                                                      dstY, width, height);
        !r) {
        mcdr::abi::failVoid("mcdr_dx12CopyTextureToTexture", r.error().message);
    }
}

// In-place SMAA on the world layer, called after the world has rendered and before the GUI is
// drawn, so the anti-aliasing never touches UI. mode != SMAA is a silent no-op (returns 0).
int32_t MCDR_CALL mcdr_dx12ApplySmaaToTexture(mcdr_command_ctx* ctx, mcdr_object* texture) {
    return d3d12_mc::applySmaaToTexture(ctx, texture) ? 1 : 0;
}

// Hands the finished world layer to the GUI's own window-sized render target, once per frame.
int32_t MCDR_CALL mcdr_dx12BlitTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                mcdr_object* dst) {
    const auto r = d3d12_mc::blitTextureToTexture(ctx, src, dst);
    if (!r) {
        mcdr::abi::failVoid("mcdr_dx12BlitTextureToTexture", r.error().message);
        return 0;
    }
    return 1;
}

// Post sharpening shared by TAA / DLAA / DLSS: a 5-tap unsharp from src to dst with
// amount in [0,1]. src == dst is illegal (in-place sharpen returns 0).
int32_t MCDR_CALL mcdr_dx12SharpenTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                   mcdr_object* dst, float amount) {
    const auto r = d3d12_mc::sharpenTextureToTexture(ctx, src, dst, amount);
    if (!r) {
        mcdr::abi::failVoid("mcdr_dx12SharpenTextureToTexture", r.error().message);
        return 0;
    }
    return 1;
}

// TAA resolve, native side. Same call point as SMAA, but history textures, the resolve pipeline
// and jitter bookkeeping live in the device context; the JVM only pushes per-frame values.
// 1 = applied, 0 = skipped (mode is not TAA / unsuitable target / already disabled),
// -1 = failed (one ERROR log, TAA disabled).
int32_t MCDR_CALL mcdr_dx12ApplyTaaToTexture(mcdr_command_ctx* ctx, mcdr_object* scene,
                                             mcdr_object* motion, float jitterX, float jitterY,
                                             float prevJitterX, float prevJitterY, float blend,
                                             int32_t reset, float sharpenAmount) {
    return d3d12_mc::applyTaaToTexture(ctx, scene, motion, jitterX, jitterY, prevJitterX, prevJitterY,
                                       blend, reset, sharpenAmount);
}

// DLSS upscale, same call point as SMAA: the world layer is upscaled from render size into the
// window-sized target. Silenced by the independent DLSS switch and by any earlier failure (which
// was already logged exactly once). 1 = applied, 0 = skipped, -1 = bad args.
int32_t MCDR_CALL mcdr_dx12CreateDlssFeature(mcdr_command_ctx* ctx, const mcdr_dlss_input* in) {
    if (ctx == nullptr || in == nullptr) {
        return -1;
    }
    // Create and Evaluate are one entry point on purpose: NGX may only create the feature while a
    // command list is open, and the JVM has exactly one such window per frame.
    return d3d12_mc::applyDlssToTexture(ctx, *in) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12EvaluateDlss(mcdr_command_ctx* ctx, const mcdr_dlss_input* in) {
    if (ctx == nullptr || in == nullptr) {
        return -1;
    }
    return d3d12_mc::applyDlssToTexture(ctx, *in) ? 1 : 0;
}

// One-line DLSS availability/state query for the UI and the log.
int32_t MCDR_CALL mcdr_dx12QueryDlss(char* note, int32_t noteCap) {
    if (noteCap < 0 || (noteCap > 0 && note == nullptr)) {
        return -1;
    }
    return d3d12_mc::queryDlss(note, static_cast<int>(noteCap));
}

void MCDR_CALL mcdr_dx12ClearColorTexture(mcdr_command_ctx* ctx, mcdr_object* texture, float r, float g,
                                          float b, float a) {
    const auto res = d3d12_mc::clearColorTexture(ctx, texture, r, g, b, a);
    if (!res) {
        mcdr::abi::failVoid("mcdr_dx12ClearColorTexture", res.error().message);
    }
}

void MCDR_CALL mcdr_dx12ClearDepthTexture(mcdr_command_ctx* ctx, mcdr_object* texture, double depth) {
    if (const auto r = d3d12_mc::clearDepthTexture(ctx, texture, depth); !r) {
        mcdr::abi::failVoid("mcdr_dx12ClearDepthTexture", r.error().message);
    }
}

int32_t MCDR_CALL mcdr_dx12ClearStencilTexture(mcdr_command_ctx* ctx, mcdr_object* texture, int32_t value) {
    if (const auto r = d3d12_mc::clearStencilTexture(ctx, texture, static_cast<int>(value)); !r) {
        return mcdr::abi::failWith("mcdr_dx12ClearStencilTexture", r.error().message);
    }
    return 1;
}

// Region clear: the GUI item atlas clears one slot before a STALE redraw, and a whole-texture
// clear would wipe already-baked icons that never re-render after paging back.
void MCDR_CALL mcdr_dx12ClearColorTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture, float r, float g,
                                                float b, float a, int32_t x, int32_t y, int32_t width,
                                                int32_t height) {
    const auto res =
        d3d12_mc::clearColorTextureRegion(ctx, texture, r, g, b, a, x, y, width, height);
    if (!res) {
        mcdr::abi::failVoid("mcdr_dx12ClearColorTextureRegion", res.error().message);
    }
}

void MCDR_CALL mcdr_dx12ClearDepthTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture, double depth,
                                                int32_t x, int32_t y, int32_t width, int32_t height) {
    if (const auto r = d3d12_mc::clearDepthTextureRegion(ctx, texture, depth, x, y, width, height); !r) {
        mcdr::abi::failVoid("mcdr_dx12ClearDepthTextureRegion", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12BeginRenderPass(mcdr_command_ctx* ctx, mcdr_object* const* colorTextures,
                                        int32_t colorCount, const int32_t* colorMips,
                                        const int32_t* colorClearFlags, const float* clearColors,
                                        mcdr_object* depthTexture, int32_t depthMip, int32_t depthClearFlag,
                                        double depthClearValue, int32_t x, int32_t y, int32_t width,
                                        int32_t height) {
    if (const auto r = d3d12_mc::beginRenderPass(ctx, colorTextures, colorCount, colorMips, colorClearFlags,
                                                 clearColors, depthTexture, depthMip, depthClearFlag,
                                                 depthClearValue, x, y, width, height);
        !r) {
        mcdr::abi::failVoid("mcdr_dx12BeginRenderPass", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12EndRenderPass(mcdr_command_ctx* ctx) {
    if (const auto r = d3d12_mc::endRenderPass(ctx); !r) {
        mcdr::abi::failVoid("mcdr_dx12EndRenderPass", r.error().message);
    }
}

mcdr_object* MCDR_CALL mcdr_dx12GetActiveColorTexture(mcdr_command_ctx* ctx) {
    return reinterpret_cast<mcdr_object*>(d3d12_mc::getActiveColorTextureHandle(ctx));
}

mcdr_query_pool* MCDR_CALL mcdr_dx12CreateQueryPool(int32_t size) {
    const auto r = d3d12_mc::createQueryPool(size);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::QueryPool>("mcdr_dx12CreateQueryPool", r.error().message);
    }
    return static_cast<d3d12_mc::QueryPool*>(*r);
}

void MCDR_CALL mcdr_dx12DestroyQueryPool(mcdr_query_pool* pool) { d3d12_mc::destroyQueryPool(pool); }

void MCDR_CALL mcdr_dx12WriteTimestamp(mcdr_command_ctx* ctx, mcdr_query_pool* pool, int32_t index) {
    if (const auto r = d3d12_mc::writeTimestampToPool(ctx, pool, index); !r) {
        mcdr::abi::failVoid("mcdr_dx12WriteTimestamp", r.error().message);
    }
}

uint64_t MCDR_CALL mcdr_dx12ReadQueryValue(mcdr_query_pool* pool, int32_t index) {
    long long out = 0;
    if (const auto r = d3d12_mc::readQueryValue(pool, index, out); !r) {
        mcdr::abi::failVoid("mcdr_dx12ReadQueryValue", r.error().message);
        return 0;
    }
    return static_cast<uint64_t>(out);
}

int32_t MCDR_CALL mcdr_dx12ReadQueryValues(mcdr_query_pool* pool, int32_t start, int32_t count,
                                           int64_t* out) {
    if (count <= 0) {
        return 1;
    }
    if (!out) {
        return mcdr::abi::failWith("mcdr_dx12ReadQueryValues", "out buffer is null");
    }
    if (const auto r =
            d3d12_mc::readQueryValues(pool, start, count, reinterpret_cast<long long*>(out));
        !r) {
        return mcdr::abi::failWith("mcdr_dx12ReadQueryValues", r.error().message);
    }
    return 1;
}

// Async renderer control channel.

void MCDR_CALL mcdr_dx12InitAsyncRenderer(int32_t workerCount) {
    // initAsyncRenderer still returns bool (the device side treats it as "cannot fail").
    if (!d3d12_mc::initAsyncRenderer(static_cast<UINT>(workerCount))) {
        mcdr::abi::failVoid("mcdr_dx12InitAsyncRenderer", std::string());
    }
}

void MCDR_CALL mcdr_dx12DestroyAsyncRenderer(void) { d3d12_mc::destroyAsyncRenderer(); }

int32_t MCDR_CALL mcdr_dx12AsyncRenderBeginFrame(mcdr_command_ctx* ctx) {
    const auto r = d3d12_mc::asyncRenderBeginFrame(ctx);
    if (!r && !r.error().message.empty()) {
        mcdr::abi::failVoid("mcdr_dx12AsyncRenderBeginFrame", r.error().message);
    }
    return r ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitComplete(mcdr_command_ctx* ctx, int64_t timeoutMs) {
    const auto r = d3d12_mc::asyncRenderWaitComplete(ctx, static_cast<UINT64>(timeoutMs));
    if (!r && !r.error().message.empty()) {
        mcdr::abi::failVoid("mcdr_dx12AsyncRenderWaitComplete", r.error().message);
    }
    return r ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncRenderIsRecordingReady(mcdr_command_ctx* ctx) {
    return d3d12_mc::asyncRenderIsRecordingReady(ctx) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitRecordingReady(mcdr_command_ctx* ctx, int64_t timeoutMs) {
    return d3d12_mc::asyncRenderWaitRecordingReady(ctx, static_cast<UINT64>(timeoutMs)) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncSendCommandsReady(mcdr_command_ctx* ctx) {
    d3d12_mc::asyncSendCommandsReady(ctx);
    return 1;
}

void MCDR_CALL mcdr_dx12ClearAsyncRenderCtx(void) { d3d12_mc::clearAsyncRenderCtx(); }

// Compute dispatch; the 1/0/-2 codes come from native (-2 = no open command list).
int32_t MCDR_CALL mcdr_dx12DispatchComputeEx(mcdr_command_ctx* ctx, mcdr_object* pipeline,
                                             mcdr_object* const* srvs, int32_t srvCount,
                                             mcdr_object* const* uavs, int32_t uavCount,
                                             const int32_t* constants, int32_t constantCount,
                                             mcdr_object* cbv, uint64_t cbvOffset, int32_t x,
                                             int32_t y, int32_t z) {
    return d3d12_mc::dispatchComputeEx(ctx, pipeline, srvs, srvCount, uavs, uavCount, constants,
                                       constantCount, cbv, cbvOffset, x, y, z);
}

int32_t MCDR_CALL mcdr_dx12NrdDenoise(mcdr_command_ctx* ctx, mcdr_object* const* textures,
                                      int32_t textureCount, const float* matrices,
                                      uint32_t frameIndex, uint32_t resetHistory, char* outText,
                                      int32_t capacity) {
    return d3d12_mc::nrdDenoise(ctx, textures, textureCount, matrices, frameIndex, resetHistory,
                                outText, capacity);
}

int32_t MCDR_CALL mcdr_dx12DlssdEval(mcdr_command_ctx* ctx, mcdr_object* const* textures,
                                   int32_t textureCount, uint32_t width, uint32_t height,
                                   uint32_t outputWidth, uint32_t outputHeight,
                                   uint32_t frameIndex, int32_t reset, const float* constants,
                                   char* outText, int32_t capacity) {
    return d3d12_mc::dlssdEval(ctx, textures, textureCount, width, height, outputWidth,
                               outputHeight, frameIndex, reset, constants, outText, capacity);
}

int32_t MCDR_CALL mcdr_dx12Barrier(mcdr_command_ctx* ctx, const mcdr_barrier_desc* descs,
                                  int32_t count) {
    return d3d12_mc::submitBarriers(ctx, descs, count);
}

int32_t MCDR_CALL mcdr_dx12DispatchCompute(mcdr_command_ctx* ctx, mcdr_object* pipeline,
                                           mcdr_object* uavTexture, const int32_t* constants,
                                           int32_t constantCount, int32_t x, int32_t y, int32_t z) {
    return d3d12_mc::dispatchCompute(ctx, pipeline, uavTexture, constants, constantCount, x, y, z);
}


// Shading-rate image dispatch and bind (1=ok 0=arg/device problem -1=bad handle -2=list not open).
// The generic compute-texture pass forwards to the same implementation: one root signature.
int32_t MCDR_CALL mcdr_dx12DispatchComputeTexture3(mcdr_command_ctx* ctx, mcdr_object* pipeline,
                                              mcdr_object* srv0, mcdr_object* srv1,
                                              mcdr_object* srv2, mcdr_object* image,
                                              const int32_t* constants, int32_t constantCount,
                                              int32_t x, int32_t y, int32_t z) {
    return d3d12_mc::dispatchComputeShadingRate(ctx, pipeline, srv0, srv1, srv2, image, constants,
                                                constantCount, x, y, z);
}

int32_t MCDR_CALL mcdr_dx12DispatchComputeShadingRate(mcdr_command_ctx* ctx, mcdr_object* pipeline,
                                                      mcdr_object* srvDepth, mcdr_object* srvMotion,
                                                      mcdr_object* srvPrevious,
                                                      mcdr_object* image, const int32_t* constants,
                                                      int32_t constantCount, int32_t x, int32_t y,
                                                      int32_t z) {
    return d3d12_mc::dispatchComputeShadingRate(ctx, pipeline, srvDepth, srvMotion, srvPrevious,
                                                image, constants, constantCount, x, y, z);
}

int32_t MCDR_CALL mcdr_dx12SetShadingRateImage(mcdr_command_ctx* ctx, mcdr_object* image) {
    return d3d12_mc::setShadingRateImage(ctx, image);
}
}  // extern "C"
