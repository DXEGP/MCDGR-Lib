module;

#include "_d3d12_module_prelude.h"
#include "mcdr_graphics_capture.h"
#include "mcdr_terrain_tile_adapter.h"
#include <span>

module mcdr.abi.common;

#include "mcdr_abi.h"

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.workgraph;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;

using namespace d3d12_mc;

namespace {

constexpr int32_t kMaxColorTargets = 8;
constexpr int32_t kMaxPipelineBindings = 64;

constexpr int32_t kMaxSourceBytes = 4 * 1024 * 1024;

constexpr const char* kCompile = "mcdr_dx12CompileShaderDxc";
constexpr const char* kCreateMesh = "mcdr_dx12CreateMeshPipeline";
constexpr const char* kDispatch = "mcdr_dx12DispatchMesh";

using PipelinePtr = d3d12_mc::Dx12Pipeline;

}

extern "C" {

void* MCDR_CALL mcdr_dx12CreateWorkGraph(uint32_t schemaVersion, const void* dxil,
    int32_t dxilSize, const char* programName, const char* entryName, uint32_t recordSize,
    const uint32_t* bindingTriples, uint32_t bindingCount, uint64_t backingBudget) {
    if (schemaVersion != 1 || dxilSize <= 0 || !programName || !entryName ||
        bindingCount > 32 || (bindingCount != 0 && !bindingTriples)) {
        mcdr::abi::failWith("mcdr_dx12CreateWorkGraph", "Invalid schema or description");
        return nullptr;
    }
    const std::string programText(programName);
    const std::string entryText(entryName);
    const std::wstring programWide(programText.begin(), programText.end());
    const std::wstring entryWide(entryText.begin(), entryText.end());
    std::vector<mcdr::workgraph::RootBinding> bindings;
    for (uint32_t index = 0; index < bindingCount; ++index) {
        bindings.push_back({bindingTriples[index * 3], bindingTriples[index * 3 + 1], bindingTriples[index * 3 + 2]});
    }
    const mcdr::workgraph::ProgramDescription description{dxil, static_cast<size_t>(dxilSize),
        programWide.c_str(), entryWide.c_str(), 0, recordSize, bindings.data(), bindingCount, backingBudget};
    auto result = mcdr::workgraph::createProgram(description);
    if (!result) {
        mcdr::abi::failWith("mcdr_dx12CreateWorkGraph", result.error().message);
        return nullptr;
    }
    return *result;
}

void* MCDR_CALL mcdr_dx12CreateTerrainWorkGraph(const void* nodes, int32_t nodesBytes,
    const void* initialize, int32_t initializeBytes, const void* finalize, int32_t finalizeBytes,
    const uint32_t* uniformRegisters, int32_t uniformCount, uint64_t budget) {
    if (!nodes || nodesBytes <= 0 || !initialize || initializeBytes <= 0 || !finalize ||
        finalizeBytes <= 0 || uniformCount < 0 || uniformCount > 28 ||
        (uniformCount != 0 && !uniformRegisters)) {
        mcdr::abi::failWith("mcdr_dx12CreateTerrainWorkGraph", "Invalid program description");
        return nullptr;
    }
    TerrainProgramDescription description{nodes, static_cast<size_t>(nodesBytes),
        initialize, static_cast<size_t>(initializeBytes), finalize, static_cast<size_t>(finalizeBytes),
        uniformRegisters, static_cast<uint32_t>(uniformCount), budget};
    auto result = createTerrainWorkGraph(description);
    if (!result) {
        mcdr::abi::failWith("mcdr_dx12CreateTerrainWorkGraph", result.error().message);
        return nullptr;
    }
    return *result;
}

void MCDR_CALL mcdr_dx12DestroyTerrainWorkGraph(void* program) {
    destroyTerrainWorkGraph(program);
}

void* MCDR_CALL mcdr_dx12PrepareTerrainWorkGraph(void* context, void* program,
    void* const* buffers, const uint64_t* offsets, const uint64_t* lengths,
    int32_t bindingCount, uint32_t sectionCount, uint32_t meshletCapacity) {
    if (bindingCount < 0 || bindingCount > 32) {
        mcdr::abi::failWith("mcdr_dx12PrepareTerrainWorkGraph", "Invalid binding count");
        return nullptr;
    }
    auto result = prepareTerrainWorkGraph(context, program, buffers, offsets, lengths,
        static_cast<uint32_t>(bindingCount), sectionCount, meshletCapacity);
    if (!result) {
        mcdr::abi::failWith("mcdr_dx12PrepareTerrainWorkGraph", result.error().message);
        return nullptr;
    }
    return *result;
}

int32_t MCDR_CALL mcdr_dx12DrawTerrainWorkGraph(void* context, void* batch,
    void* vertexBuffer, uint64_t vertexOffset) {
    auto result = drawTerrainWorkGraph(context, batch, vertexBuffer, vertexOffset);
    if (!result) return mcdr::abi::failWith("mcdr_dx12DrawTerrainWorkGraph", result.error().message);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12DrawTerrainWorkGraphV2(void* context, void* batch,
    void* vertexBuffer, uint64_t vertexOffset, uint64_t vertexBytes) {
    constexpr const char* operation = "mcdr_dx12DrawTerrainWorkGraphV2";
    if (!vertexBytes) return mcdr::abi::failWith(operation, "Missing vertex range");
    auto result = drawTerrainWorkGraph(context, batch, vertexBuffer, vertexOffset, vertexBytes);
    return result ? 1 : mcdr::abi::failWith(operation, result.error().message);
}

void MCDR_CALL mcdr_dx12DestroyTerrainBatch(void* batch) {
    destroyTerrainBatch(batch);
}

void MCDR_CALL mcdr_dx12DestroyWorkGraph(void* program) {
    mcdr::workgraph::destroyProgram(program);
}

int32_t MCDR_CALL mcdr_dx12DispatchWorkGraph(mcdr_command_ctx* ctx, void* program,
    const void* records, uint32_t recordCount, uint32_t recordStride,
    mcdr_object* const* buffers, const uint64_t* offsets, const uint64_t* lengths, uint32_t bindingCount) {
    if (bindingCount > 32 || (bindingCount != 0 && !buffers)) {
        return mcdr::abi::failWith("mcdr_dx12DispatchWorkGraph", "Invalid binding count");
    }
    std::array<void*, 32> handles{};
    for (uint32_t index = 0; index < bindingCount; ++index) handles[index] = buffers[index];
    auto result = mcdr::workgraph::dispatchBuffers(ctx, program, records, recordCount,
        recordStride, handles.data(), offsets, lengths, bindingCount);
    if (!result) return mcdr::abi::failWith("mcdr_dx12DispatchWorkGraph", result.error().message);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12DispatchWorkGraphGpuBuffers(mcdr_command_ctx* ctx, void* program,
    uint32_t inputSchema, mcdr_object* inputBuffer, uint64_t inputOffset, uint64_t inputBytes,
    mcdr_object* const* buffers, const uint64_t* offsets, const uint64_t* lengths, uint32_t bindingCount) {
    if (bindingCount > 32 || (bindingCount != 0 && !buffers)) {
        return mcdr::abi::failWith("mcdr_dx12DispatchWorkGraphGpuBuffers", "Invalid binding count");
    }
    std::array<void*, 32> handles{};
    for (uint32_t index = 0; index < bindingCount; ++index) handles[index] = buffers[index];
    auto result = mcdr::workgraph::dispatchGpuBuffers(ctx, program, inputSchema, inputBuffer,
        inputOffset, inputBytes, handles.data(), offsets, lengths, bindingCount);
    if (!result) return mcdr::abi::failWith("mcdr_dx12DispatchWorkGraphGpuBuffers", result.error().message);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12QueryMeshSupport(char* outReason, int32_t capacity) {
    if (capacity < 0 || (!outReason && capacity != 0))
        return mcdr::abi::failWith("mcdr_dx12QueryMeshSupport", "invalid reason buffer/capacity");
    std::string reason;
    const bool supported = d3d12_mc::meshPipelineSupported(&reason);
    if (outReason && capacity > 0) {
        const size_t count = (std::min)(reason.size(), static_cast<size_t>(capacity - 1));
        std::memcpy(outReason, reason.data(), count);
        outReason[count] = '\0';
    }
    return supported ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12CompileShaderDxc(const char* source, int32_t sourceLen, const char* entry,
                                            const char* profile, void* outDxil, int32_t outCapacity) {
    using mcdr::abi::failWith;
    using mcdr::abi::setLastErrorf;

    if (source == nullptr || sourceLen <= 0) {
        return failWith(kCompile, "source/sourceLen is empty");
    }
    if (sourceLen > kMaxSourceBytes) {
        setLastErrorf(kCompile, "sourceLen %d exceeds the %d byte cap", static_cast<int>(sourceLen),
                      static_cast<int>(kMaxSourceBytes));
        return 0;
    }
    if (profile == nullptr || profile[0] == '\0') {
        return failWith(kCompile, "profile is empty (expected e.g. \"ms_6_6\")");
    }
    if (outCapacity < 0) {
        return failWith(kCompile, "outCapacity is negative");
    }
    if (outDxil == nullptr && outCapacity != 0) {
        return failWith(kCompile, "outDxil is null but outCapacity is not 0");
    }

    std::vector<uint8_t> dxil;
    const std::string entryPoint =
        (entry != nullptr && entry[0] != '\0') ? std::string(entry) : std::string("main");

    if (const auto r = d3d12_mc::compileHlslToDxil(std::string(source, static_cast<size_t>(sourceLen)),
                                                   entryPoint, profile, dxil);
        !r) {
        return failWith(kCompile,
                        r.error().message.empty() ? std::string("DXC produced no bytecode")
                                                  : r.error().message);
    }

    const auto size = static_cast<int32_t>(dxil.size());
    if (outDxil == nullptr || outCapacity < size) {
        return size;
    }
    std::memcpy(outDxil, dxil.data(), static_cast<size_t>(size));
    return size;
}

static PipelinePtr* createMeshPipelineErased(const mcdr_mesh_pipeline_desc* desc, uint64_t rootSignature, int32_t byteSize = 0,
    const mcdr_oit_capture_pipeline_v1* capture = nullptr) {
    using mcdr::abi::failWithPtr;
    using mcdr::abi::setLastErrorf;

    if (!desc) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "descriptor is null");
    }

    d3d12_mc::MeshPipelineDesc d;
    if (byteSize < 0 || byteSize > 128 || byteSize % 4 != 0) {
        return mcdr::abi::failWithPtr<PipelinePtr>(kCreateMesh, "invalid root constant size");
    }
    d.pushConstantBytes = static_cast<uint32_t>(byteSize);
    if (capture) {
        if (!mcdr::graphics_capture::valid(capture)) return failWithPtr<PipelinePtr>(kCreateMesh, "Invalid capture shader contract");
        d.captureHash = capture->shaderContractHash;
        d.captureGeneration = capture->programGeneration;
    }
    if (desc->rasterMotion != 0 && desc->rasterMotion != 1) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "rasterMotion must be 0 or 1");
    }
    d.rasterMotion = desc->rasterMotion != 0;

    if (desc->msLen <= 0 || desc->msData == nullptr) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "msData/msLen is required (mesh shader)");
    }
    {
        const auto* first = static_cast<const uint8_t*>(desc->msData);
        d.msDxil.assign(first, first + desc->msLen);
    }
    if (desc->asLen < 0 || (desc->asLen > 0 && desc->asData == nullptr)) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "asData/asLen mismatch");
    }
    if (desc->asLen > 0) {
        const auto* first = static_cast<const uint8_t*>(desc->asData);
        d.asDxil.assign(first, first + desc->asLen);
    }
    if (desc->psLen < 0 || (desc->psLen > 0 && desc->psData == nullptr)) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "psData/psLen mismatch");
    }
    if (desc->psLen > 0) {
        const auto* first = static_cast<const uint8_t*>(desc->psData);
        d.psDxil.assign(first, first + desc->psLen);
    }

    if (desc->colorCount < 0 || desc->colorCount > kMaxColorTargets) {
        setLastErrorf(kCreateMesh, "bad colorCount %d", static_cast<int>(desc->colorCount));
        return nullptr;
    }
    if (desc->colorCount > 0 && desc->colorTargets == nullptr) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "colorTargets is null");
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

    d.hasDepth = desc->hasDepth != 0;
    if (d.hasDepth) {
        d.depthFormat = desc->depth.depthFormat;
        d.depthWrite = desc->depth.depthWrite != 0;
        d.depthCompareOp = desc->depth.depthCompareOp;
    }

    d.cullEnabled = desc->cullEnabled != 0;
    d.polygonMode = desc->polygonMode;

    if (desc->bindingCount < 0 || desc->bindingCount > kMaxPipelineBindings) {
        setLastErrorf(kCreateMesh, "bad bindingCount %d", static_cast<int>(desc->bindingCount));
        return nullptr;
    }
    if (desc->bindingCount > 0 && desc->bindings == nullptr) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "bindings is null");
    }
    d.bindings.resize(static_cast<size_t>(desc->bindingCount));
    for (int32_t i = 0; i < desc->bindingCount; ++i) {
        d.bindings[static_cast<size_t>(i)].type = desc->bindings[i].type;
        d.bindings[static_cast<size_t>(i)].reg = desc->bindings[i].reg;
    }

    auto r = rootSignature == 0 ? d3d12_mc::createMeshPipeline(d)
                               : d3d12_mc::createMeshPipeline(d, rootSignature);
    if (!r) {
        return mcdr::abi::failResultPtr<PipelinePtr>(kCreateMesh, r.error());
    }
    return static_cast<PipelinePtr*>(*r);
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateMeshCapturePipelineV1(const mcdr_mesh_pipeline_desc* desc, uint64_t rootSignature,
    uint32_t bytes, const mcdr_oit_capture_pipeline_v1* capture) {
    if (!mcdr::graphics_capture::valid(capture) || bytes > 128) return nullptr;
    return createMeshPipelineErased(desc, rootSignature, static_cast<int32_t>(bytes), capture);
}

uint64_t MCDR_CALL mcdr_dx12CreateMeshCaptureRootV1(const mcdr_pipeline_binding_desc* bindings, int32_t count,
    uint32_t meshLayout, uint32_t bytes, const mcdr_oit_capture_pipeline_v1* capture) {
    if (!mcdr::graphics_capture::valid(capture) || count < 0 || count > 64 || (count && !bindings) ||
        meshLayout > 2 || bytes > 128 || bytes % 4) return 0;
    std::vector<d3d12_mc::PipelineDesc::Binding> list;
    for (int32_t index = 0; index < count; ++index) list.push_back({bindings[index].type, bindings[index].reg});
    const auto result = d3d12_mc::createMeshRootSignature(list, meshLayout != 0, meshLayout == 2, bytes, capture->shaderContractHash);
    if (!result) { mcdr::abi::setLastErrorf("oit.root", "%s", result.error().message.c_str()); return 0; }
    return *result;
}

uint64_t MCDR_CALL mcdr_dx12CreateTerrainTileRootV1(const mcdr_pipeline_binding_desc* bindings, int32_t count,
    uint32_t bytes, const mcdr_oit_capture_pipeline_v1* capture) {
    if (!mcdr::graphics_capture::valid(capture) || count < 0 || count > 64 || (count && !bindings) ||
        bytes > 128 || bytes % 4) {
        mcdr::abi::setLastErrorf("terrain.tile.root", "Invalid capture or binding description");
        return 0;
    }
    std::vector<d3d12_mc::PipelineDesc::Binding> list;
    list.reserve(static_cast<size_t>(count));
    for (int32_t index = 0; index < count; ++index)
        list.push_back({bindings[index].type, bindings[index].reg});
    const auto result = d3d12_mc::createMeshRootSignature(list, true, true, bytes, 0, true);
    if (!result) {
        mcdr::abi::setLastErrorf("terrain.tile.root", "%s", result.error().message.c_str());
        return 0;
    }
    return *result;
}

void* MCDR_CALL mcdr_dx12CreateTerrainTileRouteV1(uint64_t rootSignature,
    const mcdr_oit_capture_pipeline_v1* capture) {
    if (!mcdr::graphics_capture::valid(capture)) {
        mcdr::abi::setLastError("terrain.tile.route", "Invalid capture contract");
        return nullptr;
    }
    const auto result = d3d12_mc::createTerrainTileRoute(rootSignature,
        capture->shaderContractHash, capture->programGeneration);
    if (!result) {
        mcdr::abi::setLastErrorf("terrain.tile.route", "%s", result.error().message.c_str());
        return nullptr;
    }
    return *result;
}

int32_t MCDR_CALL mcdr_dx12BindTerrainTileRouteV1(PipelinePtr* capturePipeline, void* route) {
    const auto result = d3d12_mc::bindTerrainTileRoute(capturePipeline, route);
    if (!result) return mcdr::abi::failWith("terrain.tile.bind", result.error().message);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12BindTerrainTilePipelineV1(void* route, PipelinePtr* tilePipeline) {
    const auto result = d3d12_mc::bindTerrainTilePipeline(route, tilePipeline);
    if (!result) return mcdr::abi::failWith("terrain.tile.pipeline", result.error().message);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12BindTerrainTileEnvelopeV1(void* route, const void* dxil, uint32_t bytes) {
    const auto result = d3d12_mc::bindTerrainTileEnvelopeV1(route, dxil, bytes);
    if (!result) return mcdr::abi::failWith("terrain.tile.envelope", result.error().message);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12BindTerrainTileStageV2(PipelinePtr* source, void* route,
    const mcdr_terrain_tile_stage_v2* contract) {
    if (!contract) return mcdr::abi::failWith("terrain.tile.stage", "Tile stage contract is null");
    const auto result = d3d12_mc::bindTerrainTileStageV2(source, route, *contract);
    if (!result) return mcdr::abi::failWith("terrain.tile.stage", result.error().message);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12BindTerrainTileStageV3(PipelinePtr* source, void* route,
    const mcdr_terrain_tile_stage_v2* contract, const uint32_t* laneRegisters) {
    if (!contract || !laneRegisters)
        return mcdr::abi::failWith("terrain.tile.stage", "Missing tile stage contract or lane registers");
    const auto result = d3d12_mc::bindTerrainTileStageV2(source, route, *contract, std::span{laneRegisters, 5});
    if (!result) return mcdr::abi::failWith("terrain.tile.stage", result.error().message);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12BindTerrainOpaqueSeedV1(PipelinePtr* source, void* route,
    uint64_t programGeneration, uint32_t quadsPerMeshlet) {
    const auto result = d3d12_mc::bindTerrainOpaqueSeedV1(source, route, programGeneration, quadsPerMeshlet);
    if (!result) return mcdr::abi::failWith("terrain.seed.route", result.error().message);
    return 1;
}

void MCDR_CALL mcdr_dx12DestroyTerrainTileRouteV1(void* route) {
    d3d12_mc::destroyTerrainTileRoute(route);
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateMeshPipeline(const mcdr_mesh_pipeline_desc* desc) {
    return createMeshPipelineErased(desc, 0);
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateMeshPipelineWithConstants(const mcdr_mesh_pipeline_desc* desc, int32_t byteSize) {
    return createMeshPipelineErased(desc, 0, byteSize);
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateMeshPipelineWithRootConstants(const mcdr_mesh_pipeline_desc* desc, uint64_t rootSignature, int32_t byteSize) {
    return createMeshPipelineErased(desc, rootSignature, byteSize);
}

uint64_t MCDR_CALL mcdr_dx12CreateMeshRootSignature(const mcdr_pipeline_binding_desc* bindings,
                                                    int32_t count, int32_t meshExtras) {
    return mcdr_dx12CreateMeshRootSignatureWithConstants(bindings, count, meshExtras, 0);
}

uint64_t MCDR_CALL mcdr_dx12CreateMeshRootSignatureWithConstants(const mcdr_pipeline_binding_desc* bindings,
                                                    int32_t count, int32_t meshExtras, int32_t byteSize) {
    using mcdr::abi::setLastErrorf;
    if (count < 0 || (count > 0 && bindings == nullptr) || byteSize < 0 || byteSize > 128 || byteSize % 4 != 0) {
        setLastErrorf("mcdr_dx12CreateMeshRootSignature", "bad binding list (count=%d)", static_cast<int>(count));
        return 0;
    }
    std::vector<d3d12_mc::PipelineDesc::Binding> list;
    list.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        d3d12_mc::PipelineDesc::Binding b{};
        b.type = bindings[i].type;
        b.reg = bindings[i].reg;
        list.push_back(b);
    }
    auto r = d3d12_mc::createMeshRootSignature(list, meshExtras != 0, meshExtras == 2, static_cast<uint32_t>(byteSize));
    if (!r) {
        setLastErrorf("mcdr_dx12CreateMeshRootSignature", "%s", r.error().message.c_str());
        return 0;
    }
    return *r;
}

void MCDR_CALL mcdr_dx12ReleaseRootSignature(uint64_t rootSignature) {
    d3d12_mc::releaseMeshRootSignature(rootSignature);
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateMeshPipelineWithRootSignature(
    const mcdr_mesh_pipeline_desc* desc, uint64_t rootSignature) {
    return createMeshPipelineErased(desc, rootSignature);
}

int32_t MCDR_CALL mcdr_dx12DispatchMesh(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t z) {
    using mcdr::abi::failWith;

    if (ctx == nullptr) {
        return failWith(kDispatch, "command context is null");
    }
    if (x <= 0 || y <= 0 || z <= 0) {
        return failWith(kDispatch, "thread group counts must be positive");
    }
    constexpr int32_t kMaxGroups = 65535;
    if (x > kMaxGroups || y > kMaxGroups || z > kMaxGroups) {
        return failWith(kDispatch, "thread group counts exceed 65535");
    }

    if (auto r = d3d12_mc::dispatchMesh(ctx, x, y, z); !r) {
        return mcdr::abi::failResult(kDispatch, r.error());
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12MeshDrawMeshlets(
    mcdr_command_ctx* ctx, mcdr_object* vertexBuffer, int64_t vertexOffset, mcdr_object* sections,
    int64_t sectionsOffset, int64_t sectionsLength, mcdr_object* meshlets, int64_t meshletsOffset,
    int64_t meshletsLength, mcdr_object* globals, int64_t globalsOffset, int32_t meshletCount) {
    using mcdr::abi::failWith;
    constexpr const char* kMeshlets = "mcdr_dx12MeshDrawMeshlets";

    if (ctx == nullptr) {
        return failWith(kMeshlets, "command context is null");
    }
    if (vertexBuffer == nullptr || sections == nullptr || meshlets == nullptr || globals == nullptr) {
        return failWith(kMeshlets, "buffer handle is null");
    }
    if (meshletCount <= 0) {
        return failWith(kMeshlets, "meshletCount must be positive");
    }
    if (auto r = d3d12_mc::drawMeshlets(ctx, vertexBuffer, static_cast<long long>(vertexOffset), sections,
                                        static_cast<long long>(sectionsOffset),
                                        static_cast<long long>(sectionsLength), meshlets,
                                        static_cast<long long>(meshletsOffset),
                                        static_cast<long long>(meshletsLength), globals,
                                        static_cast<long long>(globalsOffset), static_cast<int>(meshletCount));
        !r) {
        return mcdr::abi::failResult(kMeshlets, r.error());
    }
    return 1;
}

}
