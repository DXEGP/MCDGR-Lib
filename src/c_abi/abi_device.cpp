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
    const auto result = mcdr_nrd_context_init(reinterpret_cast<void*>(device), width, height,
        d3d12_mc::reserveRtNrdMemory, outText, capacity);
    return result == MCDR_NRD_MEMORY_PRESSURE ? -1 : result;
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
uint64_t MCDR_CALL mcdr_dx12CreateRtFrame(const mcdr_rt_frame_program* program) {
    try {
        const auto result = d3d12_mc::createRtFrame(program);
        if (result) return *result;
        mcdr::abi::failWith("mcdr_dx12CreateRtFrame", result.error().message);
    } catch (...) {
        mcdr::abi::failWith("mcdr_dx12CreateRtFrame", "rt frame: creation failed");
    }
    return 0;
}

uint64_t MCDR_CALL mcdr_dx12CreateRtWorkGraphFrame(const mcdr_rt_frame_program* program) {
    try {
        const auto result = d3d12_mc::createRtFrame(program, true);
        if (result) return *result;
        mcdr::abi::failWith("mcdr_dx12CreateRtWorkGraphFrame", result.error().message);
    } catch (...) {
        mcdr::abi::failWith("mcdr_dx12CreateRtWorkGraphFrame", "rt WG: creation failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12ReleaseRtFrame(uint64_t frame) {
    try {
        const auto result = d3d12_mc::releaseRtFrame(frame);
        if (result) return 1;
        return mcdr::abi::failWith("mcdr_dx12ReleaseRtFrame", result.error().message);
    } catch (...) {
        return mcdr::abi::failWith("mcdr_dx12ReleaseRtFrame", "rt frame: release failed");
    }
}

uint64_t MCDR_CALL mcdr_dx12CreateRtGBufferProgram(const mcdr_rt_gbuffer_program* program) {
    try {
        const auto result = d3d12_mc::createRtGBufferProgram(program);
        if (result) return *result;
        mcdr::abi::setLastError("mcdr_dx12CreateRtGBufferProgram", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12CreateRtGBufferProgram", "rt gbuffer: program creation failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12CreateRtGeometrySnapshot(mcdr_command_ctx* ctx,
    const mcdr_rt_geometry_desc* desc, mcdr_rt_geometry_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::createRtGeometrySnapshot(ctx, desc, output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12CreateRtGeometrySnapshot", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12CreateRtGeometrySnapshot", "rt geometry: creation failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12ReleaseRtGeometrySnapshot(uint64_t snapshot) {
    try {
        const auto result = d3d12_mc::releaseRtGeometrySnapshot(snapshot);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12ReleaseRtGeometrySnapshot", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12ReleaseRtGeometrySnapshot", "rt geometry: release failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12CreateRtGeometryDelta(mcdr_command_ctx* ctx,
    const mcdr_rt_geometry_delta_desc* desc, mcdr_rt_geometry_delta_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::createRtGeometryDelta(ctx, desc, output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12CreateRtGeometryDelta", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12CreateRtGeometryDelta", "rt geometry: creation failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12GetRtGeometryMemory(mcdr_rt_geometry_memory* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::getRtGeometryMemory(output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12GetRtGeometryMemory", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12GetRtGeometryMemory", "rt geometry: memory query failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12GetRtAccelerationBudget(uint64_t* values, int32_t valueCount) {
    try {
        const auto result = d3d12_mc::getRtAccelerationBudget(values, valueCount);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12GetRtAccelerationBudget", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12GetRtAccelerationBudget", "rt accel: budget query failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12SetRtManagedMemoryBudget(uint64_t bytes) {
    try {
        const auto result = d3d12_mc::setRtManagedMemoryBudget(bytes);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12SetRtManagedMemoryBudget", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12SetRtManagedMemoryBudget", "rt memory: budget update failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12GetRtManagedMemoryBudget(uint64_t* values, int32_t valueCount) {
    try {
        const auto result = d3d12_mc::getRtManagedMemoryBudget(values, valueCount);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12GetRtManagedMemoryBudget", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12GetRtManagedMemoryBudget", "rt memory: budget query failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12ReleaseRtGBufferProgram(uint64_t program) {
    try {
        const auto result = d3d12_mc::releaseRtGBufferProgram(program);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12ReleaseRtGBufferProgram", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12ReleaseRtGBufferProgram", "rt gbuffer: program release failed");
    }
    return 0;
}

uint64_t MCDR_CALL mcdr_dx12CreateRtAcceleration(uint64_t snapshot, uint64_t reuse) {
    try {
        const auto result = d3d12_mc::createRtAcceleration(snapshot, reuse);
        if (result) return *result;
        mcdr::abi::setLastError("mcdr_dx12CreateRtAcceleration", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12CreateRtAcceleration", "rt accel: creation failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12ReleaseRtAcceleration(uint64_t acceleration) {
    try {
        const auto result = d3d12_mc::releaseRtAcceleration(acceleration);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12ReleaseRtAcceleration", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12ReleaseRtAcceleration", "rt accel: release failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12PrepareRtAcceleration(mcdr_command_ctx* ctx,
    uint64_t acceleration, mcdr_rt_acceleration_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::prepareRtAcceleration(ctx, acceleration, output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12PrepareRtAcceleration", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12PrepareRtAcceleration", "rt accel: build failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12FilterRtAcceleration(mcdr_command_ctx* ctx,
    uint64_t acceleration, const uint64_t* invalidSections, uint32_t count, mcdr_rt_acceleration_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::filterRtAcceleration(ctx, acceleration, invalidSections, count, output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12FilterRtAcceleration", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12FilterRtAcceleration", "rt accel: filtering failed");
    }
    return 0;
}

uint64_t MCDR_CALL mcdr_dx12RtAccelerationMemory(uint64_t first, uint64_t second) {
    try {
        const auto result = d3d12_mc::rtAccelerationMemory(first, second);
        if (result) return *result;
        mcdr::abi::setLastError("mcdr_dx12RtAccelerationMemory", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtAccelerationMemory", "rt accel: accounting failed");
    }
    return 0;
}

uint64_t MCDR_CALL mcdr_dx12CreateRtScene(void) {
    try {
        const auto result = d3d12_mc::createRtScene();
        if (result) return *result;
        mcdr::abi::setLastError("mcdr_dx12CreateRtScene", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12CreateRtScene", "rt scene: creation failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12ReleaseRtScene(uint64_t scene) {
    try {
        const auto result = d3d12_mc::releaseRtScene(scene);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12ReleaseRtScene", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12ReleaseRtScene", "rt scene: release failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12PrepareRtScene(mcdr_command_ctx* ctx, uint64_t scene,
    const mcdr_rt_scene_desc* desc, mcdr_rt_scene_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::prepareRtScene(ctx, scene, desc, output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12PrepareRtScene", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12PrepareRtScene", "rt scene: preparation failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12PrepareRtSceneTargets(mcdr_command_ctx* ctx, uint64_t scene,
    const mcdr_rt_scene_desc* desc, const uint64_t* targets, uint32_t targetCount, mcdr_rt_scene_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::prepareRtSceneTargets(ctx, scene, desc, targets, targetCount, output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12PrepareRtSceneTargets", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12PrepareRtSceneTargets", "rt scene: preparation failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12SetRtSceneWorkBudget(uint64_t scene, uint64_t cpuMicros,
                                            uint64_t gpuMicros, uint64_t triangles) {
    try {
        const auto result = d3d12_mc::setRtSceneWorkBudget(scene, cpuMicros, gpuMicros, triangles);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12SetRtSceneWorkBudget", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12SetRtSceneWorkBudget", "rt scene: work configuration failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12GetRtSceneWorkBudget(uint64_t scene, uint64_t* values, int32_t valueCount) {
    try {
        const auto result = d3d12_mc::getRtSceneWorkBudget(scene, values, valueCount);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12GetRtSceneWorkBudget", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12GetRtSceneWorkBudget", "rt scene: work query failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12GetRtSceneCaptureDemands(uint64_t scene, uint64_t* values, int32_t valueCount) {
    try {
        const auto result = d3d12_mc::getRtSceneCaptureDemands(scene, values, valueCount);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12GetRtSceneCaptureDemands", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12GetRtSceneCaptureDemands", "rt scene: demand query failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12GetRtSceneStats(uint64_t scene, uint64_t* values, int32_t valueCount) {
    try {
        const auto result = d3d12_mc::getRtSceneStats(scene, values, valueCount);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12GetRtSceneStats", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12GetRtSceneStats", "rt scene: statistics failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12RtFrameBeginGBuffer(mcdr_command_ctx* ctx, uint64_t frame,
    int64_t frameNanos, int32_t width, int32_t height, mcdr_rt_gbuffer_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::beginRtGBuffer(ctx, frame, frameNanos, width, height, output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12RtFrameBeginGBuffer", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtFrameBeginGBuffer", "rt gbuffer: begin failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12RtFrameEndGBuffer(mcdr_command_ctx* ctx, uint64_t frame,
    uint64_t revision, int32_t success) {
    try {
        if (success != 0 && success != 1) {
            mcdr::abi::setLastError("mcdr_dx12RtFrameEndGBuffer", "rt gbuffer: invalid completion");
            return 0;
        }
        const auto result = d3d12_mc::endRtGBuffer(ctx, frame, revision, success != 0);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12RtFrameEndGBuffer", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtFrameEndGBuffer", "rt gbuffer: end failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12RtFrameReplayGBuffer(mcdr_command_ctx* ctx, uint64_t frame,
    const mcdr_rt_gbuffer_replay* replay) {
    try {
        const auto result = d3d12_mc::replayRtGBuffer(ctx, frame, replay);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12RtFrameReplayGBuffer", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtFrameReplayGBuffer", "rt gbuffer: replay failed");
    }
    return 0;
}

uint64_t MCDR_CALL mcdr_dx12RtCaptureGBuffer(mcdr_command_ctx* ctx, uint64_t capture,
    int64_t frameNanos, const mcdr_rt_gbuffer_batch* batch) {
    try {
        const auto result = d3d12_mc::captureRtGBuffer(ctx, capture, frameNanos, batch);
        if (result) return *result;
        mcdr::abi::setLastError("mcdr_dx12RtCaptureGBuffer", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtCaptureGBuffer", "rt capture: capture failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12RtReleaseGBufferCapture(mcdr_command_ctx* ctx, uint64_t capture) {
    try {
        const auto result = d3d12_mc::releaseRtGBufferCapture(ctx, capture);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12RtReleaseGBufferCapture", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtReleaseGBufferCapture", "rt capture: release failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12RtFrameReplayCapturedGBuffer(mcdr_command_ctx* ctx, uint64_t frame,
    uint64_t capture, const mcdr_rt_gbuffer_replay* replay) {
    try {
        const auto result = d3d12_mc::replayCapturedRtGBuffer(ctx, frame, capture, replay);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12RtFrameReplayCapturedGBuffer", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtFrameReplayCapturedGBuffer", "rt capture: replay failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12RtFrameReblur(mcdr_command_ctx* ctx, uint64_t frame, uint64_t frameId) {
    try {
        const auto result = d3d12_mc::reblurRtFrame(ctx, frame, frameId);
        if (result) return 1;
        return mcdr::abi::failWith("mcdr_dx12RtFrameReblur", result.error().message);
    } catch (...) {
        return mcdr::abi::failWith("mcdr_dx12RtFrameReblur", "rt frame: signal encoding failed");
    }
}

int32_t MCDR_CALL mcdr_dx12RtFrameDenoise(mcdr_command_ctx* ctx, uint64_t frame,
    const mcdr_rt_denoise_desc* desc, mcdr_rt_denoise_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::denoiseRtFrame(ctx, frame, desc, output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12RtFrameDenoise", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtFrameDenoise", "rt frame: denoising failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12RtFrameUpscale(mcdr_command_ctx* ctx, uint64_t frame,
    uint64_t frameId, int32_t backend, mcdr_rt_denoise_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::upscaleRtFrame(ctx, frame, frameId, backend, output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12RtFrameUpscale", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtFrameUpscale", "rt sr: reconstruction failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12RtFrameComposite(mcdr_command_ctx* ctx, uint64_t frame,
    uint64_t frameId, mcdr_object* source, int32_t debugMode, mcdr_rt_denoise_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::compositeRtFrame(ctx, frame, frameId, source, debugMode, output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12RtFrameComposite", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtFrameComposite", "rt composite: recording failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12RtFrameTransaction(mcdr_command_ctx* ctx, uint64_t frame,
    const mcdr_rt_transaction_desc* desc, mcdr_rt_transaction_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::recordRtFrameTransaction(ctx, frame, desc, output);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12RtFrameTransaction", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtFrameTransaction", "rt frame: transaction failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12RtFrameTransactionLighting(mcdr_command_ctx* ctx, uint64_t frame,
    const mcdr_rt_transaction_desc* desc, const void* lighting, int64_t lightingBytes,
    mcdr_rt_transaction_output* output) {
    if (output) *output = {};
    try {
        if (!lighting || lightingBytes == 0) {
            mcdr::abi::setLastError("mcdr_dx12RtFrameTransactionLighting", "rt lighting: payload required");
            return 0;
        }
        const auto result = d3d12_mc::recordRtFrameTransaction(ctx, frame, desc, output, lighting, lightingBytes);
        if (result) return 1;
        mcdr::abi::setLastError("mcdr_dx12RtFrameTransactionLighting", result.error().message);
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12RtFrameTransactionLighting", "rt lighting: transaction failed");
    }
    return 0;
}

int32_t MCDR_CALL mcdr_dx12RtFrameSetSpatial(mcdr_command_ctx* ctx, uint64_t frame,
    uint64_t frameId, uint64_t worldEpoch, uint64_t liveRevision, const void* data, int64_t bytes) {
    try {
        const auto result = d3d12_mc::setRtFrameSpatial(ctx, frame, frameId, worldEpoch, liveRevision, data, bytes);
        if (result) return 1;
        return mcdr::abi::failWith("mcdr_dx12RtFrameSetSpatial", result.error().message);
    } catch (...) {
        return mcdr::abi::failWith("mcdr_dx12RtFrameSetSpatial", "rt spatial: capture failed");
    }
}

int32_t MCDR_CALL mcdr_dx12RtFrame(mcdr_command_ctx* ctx, uint64_t frame,
    const mcdr_rt_frame_desc* desc, mcdr_rt_frame_output* output) {
    if (output) *output = {};
    try {
        const auto result = d3d12_mc::recordRtFrame(ctx, frame, desc, output);
        if (result) return 1;
        return mcdr::abi::failWith("mcdr_dx12RtFrame", result.error().message);
    } catch (...) {
        return mcdr::abi::failWith("mcdr_dx12RtFrame", "rt frame: recording failed");
    }
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
