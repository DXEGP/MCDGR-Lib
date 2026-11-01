// abi_device.cpp — device, resource and hidden-window entry points.
//
// mcdr_dx12CreateDevice is the one entry point that does not use the last-error channel: it
// returns the failure inside the result string, prefixed with "ERROR: ", and the JVM still
// branches on that prefix. Buffer addresses are handed out raw (the JVM wraps them with
// MemorySegment.ofAddress(...).reinterpret(len)), so no byte-order trap remains.
// 见 docs\device\README.md

module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件
#include "mcdr_rt_tracker.h"
#include "mcdr_nrd_impl.h"

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;

using namespace d3d12_mc;

extern "C" {

int32_t MCDR_CALL mcdr_dx12CreateDevice(char* buf, int32_t cap) {
    using mcdr::abi::writeString;

    const auto r = d3d12_mc::ensureDevice();
    if (!r) {
        return writeString("ERROR: " + r.error().message, buf, cap);
    }

    // The native layer never reads the process environment: the JVM decides verbosity and
    // calls mcdrSetLogVerbose() before this entry point.
    d3d12_mc::DeviceContext& ctx = d3d12_mc::deviceContext();
    // featureLevelName() replaces the old level/10 + level%10 split, and the capability summary
    // trails it so the JVM's "Device probe ..." line already shows the 12_2 baseline.
    std::string result = ctx.adapterName.empty() ? "<unknown adapter>" : ctx.adapterName;
    result += " (";
    result += d3d12_mc::featureLevelName(ctx.featureLevel);
    result += "; ";
    result += d3d12_mc::capabilitiesText(ctx.caps);
    result += "); ";
    // This detail string is what the JVM logs from its four-round createDevice() self-test.
    result += d3d12_mc::runResourceSelfTest();
    return writeString(result, buf, cap);
}

// Capability face for the ray-tracing plan: tiers as the D3D12 enums report them, plus the two
// "was it even asked" bits. A 0 tier is only meaningful when its queried bit is set — OPTIONS21
// is answered by the Agility 619 core, not by every runtime in the wild.
// The JVM mirror (MCDRLayout.RT_CAPS) hard-codes 80 bytes; a field added on one side only would
// silently shift every later field, so the size is pinned at compile time on this side too.
static_assert(sizeof(mcdr_rt_caps) == 80, "mcdr_rt_caps must stay 80 bytes (MCDRLayout.RT_CAPS)");

static_assert(sizeof(mcdr_rt_track_stats) == 92, "mcdr_rt_track_stats must stay 92 bytes (MCDRLayout.RT_TRACK_STATS)");

int32_t MCDR_CALL mcdr_dx12QueryRtTrackStats(mcdr_rt_track_stats* out) {
    if (out == nullptr) {
        return -1;
    }
    *out = mcdr_rt_track_stats{};
    mcdr_rt_track_query(out);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12QueryRtCaps(mcdr_rt_caps* out) {
    if (out == nullptr) {
        return -1;
    }
    const d3d12_mc::DeviceContext& ctx = d3d12_mc::deviceContext();
    if (!ctx.caps.queried) {
        return -1;
    }
    const d3d12_mc::DeviceContext::Capabilities& caps = ctx.caps;
    *out = mcdr_rt_caps{};
    out->raytracingTier = static_cast<int32_t>(caps.raytracingTier);
    out->meshShaderTier = static_cast<int32_t>(caps.meshShaderTier);
    out->workGraphsTier = static_cast<int32_t>(caps.workGraphsTier);
    out->shadingRateTier = static_cast<int32_t>(caps.shadingRateTier);
    out->shaderModel = static_cast<int32_t>(caps.shaderModel);
    out->samplerFeedbackTier = static_cast<int32_t>(caps.samplerFeedbackTier);
    out->options21Queried = caps.options21Queried ? 1 : 0;
    out->options22Queried = caps.options22Queried ? 1 : 0;
    out->serActuallyReorders = caps.serActuallyReorders ? 1 : 0;
    out->enhancedBarriers = caps.enhancedBarriers ? 1 : 0;
    out->dxcAvailable = caps.dxcAvailable ? 1 : 0;
    out->max1DDispatchSize = static_cast<int32_t>(caps.max1DDispatchSize);
    out->max1DDispatchMeshSize = static_cast<int32_t>(caps.max1DDispatchMeshSize);
    // No <cstdio> here on purpose: the note is one of three fixed literals.
    const char* note = caps.options21Queried ? "ok" : "options21-missing";
    int i = 0;
    for (; note[i] != '\0' && i < static_cast<int>(sizeof(out->note)) - 1; ++i) {
        out->note[i] = note[i];
    }
    out->note[i] = '\0';
    return 1;
}

mcdr_object* MCDR_CALL mcdr_dx12CreateTexture(int32_t usage, int32_t format, int32_t width, int32_t height,
                                              int32_t depthOrLayers, int32_t mipLevels) {
    const auto r = d3d12_mc::createTexture(usage, format, width, height, depthOrLayers, mipLevels);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateTexture", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateBuffer(int32_t usage, int64_t size) {
    const auto r = d3d12_mc::createBuffer(usage, size);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateBuffer", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

// ---- 光线追踪加速结构（S3）：七条入口 ------------------------------------------
// 语义全在 device 模块里；这一层只做句柄与错误通道的转换（Result<void> → 1/0）。
static_assert(sizeof(mcdr_rt_geometry) == 64,
              "mcdr_rt_geometry must stay 64 bytes (MCDRLayout.RT_GEOMETRY)");

mcdr_object* MCDR_CALL mcdr_dx12CreateAccelStructBuffer(int64_t size) {
    const auto r = d3d12_mc::createAccelStructBuffer(size);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateAccelStructBuffer",
                                                            r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

int32_t MCDR_CALL mcdr_dx12GetBlasPrebuildInfo(const mcdr_rt_geometry* geometries, int32_t count,
                                               uint64_t* out) {
    const auto r = d3d12_mc::getBlasPrebuildInfo(geometries, count, out);
    if (!r) {
        return mcdr::abi::failWith("mcdr_dx12GetBlasPrebuildInfo", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12GetTlasPrebuildInfo(int32_t instanceCount, uint64_t* out) {
    const auto r = d3d12_mc::getTlasPrebuildInfo(instanceCount, out);
    if (!r) {
        return mcdr::abi::failWith("mcdr_dx12GetTlasPrebuildInfo", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12BuildBlas(mcdr_command_ctx* ctx, mcdr_object* dest, mcdr_object* scratch,
                                     const mcdr_rt_geometry* geometries, int32_t count,
                                     int32_t update) {
    const auto r = d3d12_mc::buildBlas(ctx, dest, scratch, geometries, count, update != 0);
    if (!r) {
        return mcdr::abi::failWith("mcdr_dx12BuildBlas", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12BuildTlas(mcdr_command_ctx* ctx, mcdr_object* dest,
                                     mcdr_object* scratch, mcdr_object* instances,
                                     int32_t instanceCount) {
    const auto r = d3d12_mc::buildTlas(ctx, dest, scratch, instances, instanceCount);
    if (!r) {
        return mcdr::abi::failWith("mcdr_dx12BuildTlas", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12CopyAccelStruct(mcdr_command_ctx* ctx, mcdr_object* dest,
                                           mcdr_object* src) {
    const auto r = d3d12_mc::copyAccelStruct(ctx, dest, src);
    if (!r) {
        return mcdr::abi::failWith("mcdr_dx12CopyAccelStruct", r.error().message);
    }
    return 1;
}

mcdr_object* MCDR_CALL mcdr_dx12CreateAccelStructView(mcdr_object* accel) {
    const auto r = d3d12_mc::createAccelStructView(accel);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateAccelStructView",
                                                            r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}
uint64_t MCDR_CALL mcdr_dx12GetBufferAddress(mcdr_object* buffer) {
    const auto r = d3d12_mc::bufferAddress(buffer);
    if (!r) {
        mcdr::abi::failWith("mcdr_dx12GetBufferAddress", r.error().message);
        return 0;
    }
    return *r;
}


// NRD 的常驻上下文：设备指针在本模块里取，advanced 那个普通 TU 拿不到 gCtx。
int32_t MCDR_CALL mcdr_dx12NrdInit(int32_t width, int32_t height, char* outText, int32_t capacity) {
    const uintptr_t device = d3d12_mc::getDeviceHandle();
    if (device == 0) {
        if (outText != nullptr && capacity > 0) {
            outText[0] = '\0';
        }
        return -1;
    }
    return mcdr_nrd_context_init(reinterpret_cast<void*>(device), width, height, outText, capacity);
}

int32_t MCDR_CALL mcdr_dx12NrdShutdown(void) { return mcdr_nrd_context_shutdown(); }

mcdr_object* MCDR_CALL mcdr_dx12CreateRaytracingPipeline(const void* dxilLib, int64_t dxilSize,
                                                         const char* raygen, const char* miss,
                                                         const mcdr_rt_hit_group* groups,
                                                         int32_t groupCount, int32_t srvCount,
                                                         int32_t uavCount, int32_t maxRecursion) {
    // 结构体布局是 ABI 的一部分：Java 侧按 32 B 写四条指针，宽度对不上会读出野指针。
    static_assert(sizeof(mcdr_rt_hit_group) == 32, "mcdr_rt_hit_group must stay 32 bytes");
    const auto r = d3d12_mc::createRaytracingPipeline(dxilLib, dxilSize, raygen, miss, groups,
                                                      groupCount, srvCount, uavCount, maxRecursion);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateRaytracingPipeline",
                                                            r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

int32_t MCDR_CALL mcdr_dx12GetShaderIdentifier(mcdr_object* pipeline, const char* exportName,
                                               void* out32) {
    return d3d12_mc::getShaderIdentifier(pipeline, exportName, out32);
}

int32_t MCDR_CALL mcdr_dx12DispatchRays(
    mcdr_command_ctx* ctx, mcdr_object* pipeline, mcdr_object* shaderTable, int64_t raygenOffset,
    int64_t missOffset, int64_t missStride, int32_t missCount, int64_t hitOffset, int64_t hitStride,
    int32_t hitCount, mcdr_object* const* srvs, int32_t srvCount, mcdr_object* const* uavs,
    int32_t uavCount, const int32_t* constants, int32_t constantCount, mcdr_object* cbv,
    uint64_t cbvOffset, int32_t width, int32_t height) {
    return d3d12_mc::dispatchRays(ctx, pipeline, shaderTable, raygenOffset, missOffset, missStride,
                                  missCount, hitOffset, hitStride, hitCount, srvs, srvCount, uavs,
                                  uavCount, constants, constantCount, cbv, cbvOffset, width, height);
}
mcdr_object* MCDR_CALL mcdr_dx12CreateSampler(int32_t addressU, int32_t addressV, int32_t minFilter,
                                              int32_t magFilter, int32_t maxAnisotropy, float maxLod) {
    const auto r = d3d12_mc::createSampler(addressU, addressV, minFilter, magFilter, maxAnisotropy, maxLod);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateSampler", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateSamplerWithMipBias(int32_t addressU, int32_t addressV, int32_t minFilter,
                                                         int32_t magFilter, int32_t maxAnisotropy,
                                                         float maxLod, float mipLodBias) {
    const auto r = d3d12_mc::createSamplerWithMipBias(addressU, addressV, minFilter, magFilter, maxAnisotropy,
                                                      maxLod, mipLodBias);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateSamplerWithMipBias",
                                                            r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateTextureView(mcdr_object* texture, int32_t baseMipLevel,
                                                  int32_t mipLevels) {
    const auto r = d3d12_mc::createTextureView(texture, baseMipLevel, mipLevels);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateTextureView", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

void MCDR_CALL mcdr_dx12DestroyResource(mcdr_object* object) { d3d12_mc::destroyObject(object); }

mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineEx(const void* dxil, int32_t dxilSize,
                                                        int32_t srvCount, int32_t uavCount) {
    const auto r = d3d12_mc::createComputePipelineEx(dxil, dxilSize, srvCount, uavCount);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateComputePipelineEx",
                                                            r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipeline(const void* dxil, int32_t dxilSize) {
    const auto r = d3d12_mc::createComputePipeline(dxil, dxilSize);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateComputePipeline",
                                                            r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}


// Shading-rate image compute PSO (2 SRV + 1 UAV; same root signature as the ShadingRate entry).
// mcdr_dx12CreateComputePipelineTexture3 is a plain forwarder to it.
mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineTexture3(const void* dxil, int32_t dxilSize) {
    return mcdr_dx12CreateComputePipelineShadingRate(dxil, dxilSize);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineShadingRate(const void* dxil, int32_t dxilSize) {
    const auto r = d3d12_mc::createComputePipelineShadingRate(dxil, dxilSize);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateComputePipelineShadingRate",
                                                             r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateShadingRateImage(int32_t width, int32_t height) {
    const auto r = d3d12_mc::createShadingRateImage(width, height);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateShadingRateImage",
                                                             r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}
void* MCDR_CALL mcdr_dx12MapBuffer(mcdr_object* buffer, int64_t offset, int64_t length, int32_t read,
                                   int32_t write, int64_t* outLength) {
    // The mapped range is always the caller-requested length; reporting it back lets the JVM size
    // its reinterpret() without a second call.
    if (outLength) {
        *outLength = 0;
    }
    const auto r = d3d12_mc::mapBuffer(buffer, offset, length, read != 0, write != 0);
    if (!r) {
        mcdr::abi::failVoid("mcdr_dx12MapBuffer", r.error().message);
        return nullptr;
    }
    if (outLength) {
        *outLength = length;
    }
    return *r;
}

void MCDR_CALL mcdr_dx12UnmapBuffer(mcdr_object* buffer) { d3d12_mc::unmapBuffer(buffer); }

int32_t MCDR_CALL mcdr_dx12EnumerateAdapters(char* buf, int32_t cap) {
    // Informational only: a machine with no D3D12 adapter legitimately yields "[]", so this
    // never reports an error.
    return mcdr::abi::writeString(d3d12_mc::enumerateAdaptersJson(), buf, cap);
}
int32_t MCDR_CALL mcdr_dx12GetAdapterInfo(char* buf, int32_t cap) {
    // Two-call probe: (NULL,0) returns size+1 including the NUL, then a buffer that big is
    // passed. Empty means "no active device" -> 0 and the caller keeps its fallback. Note the
    // deliberate asymmetry with writeString: here the writing path returns size WITHOUT the NUL.
    // 见 docs\abi\README.md §4.7
    const std::string json = d3d12_mc::adapterInfoJson();
    if (json.empty()) {
        return 0;
    }
    if (buf == nullptr || cap <= 0) {
        return static_cast<int32_t>(json.size()) + 1;
    }
    mcdr::abi::writeString(json, buf, cap);
    return static_cast<int32_t>(json.size());
}

uint64_t MCDR_CALL mcdr_dx12GetQueueHandle(void) { return static_cast<uint64_t>(d3d12_mc::getQueueHandle()); }

uint64_t MCDR_CALL mcdr_dx12GetDeviceHandle(void) {
    return static_cast<uint64_t>(d3d12_mc::getDeviceHandle());
}

void MCDR_CALL mcdr_dx12DestroyDevice(void) { d3d12_mc::destroyDevice(); }

uint64_t MCDR_CALL mcdr_dx12CreateHiddenWindow(int32_t width, int32_t height) {
    return static_cast<uint64_t>(d3d12_mc::createHiddenWindow(width, height));
}

void MCDR_CALL mcdr_dx12DestroyHiddenWindow(uint64_t hwnd) {
    d3d12_mc::destroyHiddenWindow(static_cast<uintptr_t>(hwnd));
}

}  // extern "C"
