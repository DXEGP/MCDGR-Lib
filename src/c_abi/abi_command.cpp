module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"

module mcdr.abi.common;

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;
import mcdr.render.vanilla;
import mcdr.render.display;
import mcdr.render.oit;
import mcdr.error;

using namespace d3d12_mc;

namespace {

int vanillaSmaaAdapter(void* context, void* payload) {
    return d3d12_mc::applySmaaToTexture(static_cast<CommandContext*>(context),
                                        static_cast<Dx12Object*>(payload));
}

int reportVanillaPostResult(const char* operation, int result) {
    if (result < 0) {
        std::string detail = d3d12_mc::vanilla::lastFailureText();
        if (std::strcmp(operation, "mcdr_dx12ApplySmaaToTexture") == 0) {
            const std::string provider = d3d12_mc::antiAliasingLastFailureText();
            if (!provider.empty()) detail = provider;
        }
        mcdr::abi::setLastError(operation,
            detail.empty() ? std::string("selected post adapter failed") : detail);
    }
    return result;
}

int reportDepthSeedStatus(const char* operation, vanilla::WorldDepthSeedStatus status) {
    using vanilla::WorldDepthSeedStatus;
    if (status == WorldDepthSeedStatus::Ready) return 1;
    if (status == WorldDepthSeedStatus::NotArmed) return 0;
    mcdr::abi::setLastErrorf(operation, "Seed capture failed (%u)", static_cast<unsigned>(status));
    switch (status) {
        case WorldDepthSeedStatus::LateArm: return -2;
        case WorldDepthSeedStatus::BudgetExceeded: return -3;
        case WorldDepthSeedStatus::Busy: return -4;
        case WorldDepthSeedStatus::StaleRecording: return -5;
        case WorldDepthSeedStatus::Incomplete: return -6;
        case WorldDepthSeedStatus::Cancelled: return -7;
        case WorldDepthSeedStatus::RecordingFailed: return -8;
        default: return -1;
    }
}

}

extern "C" int32_t MCDR_CALL mcdr_dx12PrepareFramePrograms(mcdr_command_ctx* context,
    uint64_t generation, uint32_t flags, void** candidate) {
    constexpr const char* operation = "mcdr_dx12PrepareFramePrograms";
    if (candidate) *candidate = nullptr;
    try {
        const auto result = vanilla::prepareFramePrograms(context, generation, flags, candidate);
        if (result != MCDR_FRAME_PROGRAMS_SUCCESS)
            mcdr::abi::setLastError(operation, vanilla::frameProgramsFailureText());
        return result;
    } catch (const std::exception& failure) {
        mcdr::abi::setLastError(operation, failure.what());
        return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
    } catch (...) {
        mcdr::abi::setLastError(operation, "Frame program preparation failed");
        return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
    }
}

extern "C" int32_t MCDR_CALL mcdr_dx12CommitFramePrograms(mcdr_command_ctx* context, void* candidate) {
    try {
        const auto result = vanilla::commitFramePrograms(context, candidate);
        if (result != MCDR_FRAME_PROGRAMS_SUCCESS)
            mcdr::abi::setLastError("mcdr_dx12CommitFramePrograms", vanilla::frameProgramsFailureText());
        return result;
    } catch (const std::exception& failure) {
        mcdr::abi::setLastError("mcdr_dx12CommitFramePrograms", failure.what());
        return MCDR_FRAME_PROGRAMS_INVALID_REQUEST;
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12CommitFramePrograms", "Frame program validation failed");
        return MCDR_FRAME_PROGRAMS_INVALID_REQUEST;
    }
}

extern "C" void MCDR_CALL mcdr_dx12AbortFramePrograms(void* candidate) {
    vanilla::abortFramePrograms(candidate);
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaBeginFrame(mcdr_command_ctx* context,
                                                          const mcdr_vanilla_frame_input* input) {
    return reportVanillaPostResult("mcdr_dx12VanillaBeginFrame",
        vanilla::beginFrame(context, input, sizeof(*input)));
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaRegisterWorldDepthV1(
    mcdr_command_ctx* context, mcdr_object* depth) {
    return reportVanillaPostResult("mcdr_dx12VanillaRegisterWorldDepthV1",
        vanilla::registerWorldDepthV1(context, depth));
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaArmWorldDepthSeedV1(
    mcdr_command_ctx* context, uint32_t maxEvents, uint64_t totalBudgetBytes) {
    return reportDepthSeedStatus("mcdr_dx12VanillaArmWorldDepthSeedV1",
        vanilla::armWorldDepthSeedCaptureV1(context, maxEvents, totalBudgetBytes));
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaSealWorldDepthSeedV1(mcdr_command_ctx* context) {
    return reportDepthSeedStatus("mcdr_dx12VanillaSealWorldDepthSeedV1",
        vanilla::sealWorldDepthSeedCaptureV1(context, nullptr));
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaWorldReady(mcdr_command_ctx* context,
                                                          const mcdr_frame_identity* input) {
    return reportVanillaPostResult("mcdr_dx12VanillaWorldReady",
        vanilla::markWorldReady(context, input, sizeof(*input)));
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaGuiReady(mcdr_command_ctx* context,
                                                        const mcdr_frame_identity* input) {
    return reportVanillaPostResult("mcdr_dx12VanillaGuiReady",
        vanilla::markGuiReady(context, input, sizeof(*input)));
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaPublishPreparedFrame(mcdr_command_ctx* context,
                                                                    const mcdr_prepared_frame* input) {
    return reportVanillaPostResult("mcdr_dx12VanillaPublishPreparedFrame",
        vanilla::publishPreparedFrame(context, input, sizeof(*input)));
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaEvaluateReconstruction(
    mcdr_command_ctx* context, const mcdr_reconstruction_request* input) {
    return reportVanillaPostResult("mcdr_dx12VanillaEvaluateReconstruction",
        vanilla::executePreparedReconstruction(context, input, sizeof(*input),
            d3d12_mc::evaluatePreparedReconstruction));
}

extern "C" int32_t MCDR_CALL mcdr_dx12SettleDisplayFrameV1(
    mcdr_command_ctx* context, uint64_t timeoutMs, mcdr_present_receipt_v1* receipt) {
    if (!receipt || receipt->schemaVersion != MCDR_PRESENT_RECEIPT_SCHEMA_VERSION ||
        receipt->structSize != sizeof(mcdr_present_receipt_v1)) {
        mcdr::abi::setLastError("mcdr_dx12SettleDisplayFrameV1", "Invalid display receipt schema or size");
        return MCDR_DISPLAY_SETTLE_INVALID_REQUEST;
    }
    const auto result = settleDisplayFrame(context, timeoutMs, receipt, sizeof(*receipt));
    if (!result) {
        mcdr::abi::setLastError("mcdr_dx12SettleDisplayFrameV1", result.error().message);
        return MCDR_DISPLAY_SETTLE_INVALID_REQUEST;
    }
    return *result;
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaResetFrame(mcdr_command_ctx* context) {
    if (context == nullptr) return -1;
    if (display::retire(context) != 1) {
        mcdr::abi::setLastError("mcdr_dx12VanillaResetFrame", display::lastFailureText());
        return -1;
    }
    oit::reset(context);
    vanilla::reset(context);
    return 1;
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaPrepareWorldOutput(
    mcdr_command_ctx* context, const mcdr_world_output_input* input) {
    return reportVanillaPostResult("mcdr_dx12VanillaPrepareWorldOutput",
        vanilla::prepareWorldOutput(context, input, sizeof(*input)));
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaBeginRasterPass(
    mcdr_command_ctx* context, void** token) {
    if (token != nullptr) *token = nullptr;
    if (context == nullptr || token == nullptr) {
        mcdr::abi::setLastError("mcdr_dx12VanillaBeginRasterPass", "Null context or token output");
        return -1;
    }
    const auto result = vanilla::beginRasterAttachmentPass(context, token);
    if (result != 1) mcdr::abi::setLastError("mcdr_dx12VanillaBeginRasterPass", vanilla::lastFailureText());
    return result;
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaBeginDeclaredPass(
    mcdr_command_ctx* context, uint32_t pass, const mcdr_vanilla_resource_use* uses,
    uint32_t useCount, uint32_t graphicsEffects, void** token) {
    if (context == nullptr || uses == nullptr || token == nullptr || useCount == 0) return -1;
    std::vector<d3d12_mc::vanilla::PassResourceUse> declarations;
    declarations.reserve(useCount);
    for (uint32_t index = 0; index < useCount; ++index) {
        const auto& use = uses[index];
        declarations.push_back({use.object, use.version, use.firstSubresource, use.subresourceCount,
            static_cast<d3d12_mc::vanilla::ResourceAccess>(use.access), use.requiredState,
            use.finalState, static_cast<d3d12_mc::vanilla::ExternalResourceState>(use.externalState)});
    }
    d3d12_mc::vanilla::DeclaredPassToken* opaque = nullptr;
    const auto result = d3d12_mc::vanilla::beginDeclaredPass(context, pass, declarations.data(),
        useCount, graphicsEffects, &opaque);
    *token = opaque;
    if (result != 1) mcdr::abi::setLastError("mcdr_dx12VanillaBeginDeclaredPass", vanilla::lastFailureText());
    return result;
}

extern "C" int32_t MCDR_CALL mcdr_dx12VanillaFinishDeclaredPass(
    mcdr_command_ctx* context, void* token) {
    const auto result = d3d12_mc::vanilla::finishDeclaredPass(context,
        static_cast<d3d12_mc::vanilla::DeclaredPassToken*>(token));
    if (result != 1) mcdr::abi::setLastError("mcdr_dx12VanillaFinishDeclaredPass", vanilla::lastFailureText());
    return result;
}

extern "C" int32_t MCDR_CALL mcdr_dx12InvalidateReconstructionInputs(mcdr_command_ctx* context) {
    const auto result = d3d12_mc::invalidateReconstructionInputs(context);
    if (result != 1) return mcdr::abi::failWith("mcdr_dx12InvalidateReconstructionInputs", "Input invalidation rejected");
    return 1;
}

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

int32_t MCDR_CALL mcdr_dx12DestroyCommandEncoderCheckedV1(mcdr_command_ctx* ctx) {
    return d3d12_mc::destroyCommandEncoderChecked(ctx);
}

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
    if (!r && !r.error().message.empty()) {
        mcdr::abi::failVoid("mcdr_dx12Submit", r.error().message);
    }
    return static_cast<uint64_t>(r.value_or(0));
}

int32_t MCDR_CALL mcdr_dx12WaitForFence(mcdr_command_ctx* ctx, uint64_t value, int64_t timeoutNs) {
    const auto r = d3d12_mc::waitForContextFenceValue(ctx, static_cast<UINT64>(value),
                                                     static_cast<UINT64>(timeoutNs));
    if (!r && r.error().message.find("timed out") == std::string::npos) {
        mcdr::abi::failVoid("mcdr_dx12WaitForFence", r.error().message);
    }
    return r ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12CommandRecordingStatusV1(mcdr_command_ctx* ctx) {
    return d3d12_mc::commandRecordingStatus(ctx);
}

int32_t MCDR_CALL mcdr_dx12SubmitV1(mcdr_command_ctx* ctx, uint64_t* submittedFence) {
    return d3d12_mc::submitCommandListV1(ctx, submittedFence);
}

int32_t MCDR_CALL mcdr_dx12SettleCommandSubmissionV1(mcdr_command_ctx* ctx,
    uint64_t timeoutMs, uint64_t* submittedFence) {
    return d3d12_mc::settleCommandSubmissionV1(ctx, timeoutMs, submittedFence);
}

uint64_t MCDR_CALL mcdr_dx12GetFenceValue(mcdr_command_ctx* ctx) {
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

int32_t MCDR_CALL mcdr_dx12CopyBufferChecked(mcdr_command_ctx* ctx, mcdr_object* src,
    int64_t srcOffset, mcdr_object* dst, int64_t dstOffset, int64_t size) {
    const auto result = d3d12_mc::copyBufferToBuffer(ctx, src, srcOffset, dst, dstOffset, size);
    if (!result) {
        mcdr::abi::failVoid("mcdr_dx12CopyBufferChecked", result.error().message);
        return 0;
    }
    return 1;
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

int32_t MCDR_CALL mcdr_dx12WriteToTextureChecked(mcdr_command_ctx* ctx, mcdr_object* stagingBuffer,
                                               int64_t stagingOffset, int32_t width, int32_t height,
                                               mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                               int32_t dstX, int32_t dstY) {
    const auto result = d3d12_mc::copyBufferToTexture(ctx, stagingBuffer, stagingOffset, width, height,
                                                    dstTexture, mip, layer, dstX, dstY, width, height);
    if (!result) return mcdr::abi::failWith("mcdr_dx12WriteToTextureChecked", result.error().message);
    return 1;
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

int32_t MCDR_CALL mcdr_dx12CopyBufferToTextureChecked(mcdr_command_ctx* ctx, mcdr_object* srcBuffer,
                                                    int64_t srcOffset, int32_t srcWidth, int32_t srcHeight,
                                                    mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                    int32_t dstX, int32_t dstY, int32_t width, int32_t height) {
    const auto result = d3d12_mc::copyBufferToTexture(ctx, srcBuffer, srcOffset, srcWidth, srcHeight,
                                                    dstTexture, mip, layer, dstX, dstY, width, height);
    if (!result) return mcdr::abi::failWith("mcdr_dx12CopyBufferToTextureChecked", result.error().message);
    return 1;
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

int32_t MCDR_CALL mcdr_dx12CopyTextureToTextureChecked(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                                     mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                     int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY,
                                                     int32_t width, int32_t height) {
    const auto result = d3d12_mc::copyTextureToTexture(ctx, srcTexture, dstTexture, mip, layer,
                                                     srcX, srcY, dstX, dstY, width, height);
    if (!result) return mcdr::abi::failWith("mcdr_dx12CopyTextureToTextureChecked", result.error().message);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12ApplySmaaToTexture(mcdr_command_ctx* ctx, mcdr_object* texture) {
    if (d3d12_mc::deviceContext().aaMode != 1) {
        return 0;
    }
    return reportVanillaPostResult("mcdr_dx12ApplySmaaToTexture",
        vanilla::executeSmaaWorldPost(ctx, vanillaSmaaAdapter, texture));
}

int32_t MCDR_CALL mcdr_dx12BlitTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                mcdr_object* dst) {
    const auto r = d3d12_mc::blitTextureToTexture(ctx, src, dst);
    if (!r) {
        mcdr::abi::failVoid("mcdr_dx12BlitTextureToTexture", r.error().message);
        return 0;
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SharpenTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                   mcdr_object* dst, float amount) {
    const auto r = d3d12_mc::sharpenTextureToTexture(ctx, src, dst, amount);
    if (!r) {
        mcdr::abi::failVoid("mcdr_dx12SharpenTextureToTexture", r.error().message);
        return 0;
    }
    return 1;
}

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

void MCDR_CALL mcdr_dx12InitAsyncRenderer(int32_t workerCount) {
    if (!d3d12_mc::initAsyncRenderer(static_cast<UINT>(workerCount))) {
        mcdr::abi::failVoid("mcdr_dx12InitAsyncRenderer", std::string());
    }
}

int32_t MCDR_CALL mcdr_dx12DestroyAsyncRenderer(void) { return d3d12_mc::destroyAsyncRenderer() ? 1 : 0; }

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

int32_t MCDR_CALL mcdr_dx12DispatchCompute(mcdr_command_ctx* ctx, mcdr_object* pipeline,
                                           mcdr_object* uavTexture, const int32_t* constants,
                                           int32_t constantCount, int32_t x, int32_t y, int32_t z) {
    return d3d12_mc::dispatchCompute(ctx, pipeline, uavTexture, constants, constantCount, x, y, z);
}

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
}
