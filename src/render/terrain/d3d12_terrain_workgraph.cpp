module;

#include "_d3d12_module_prelude.h"
#include "mcdr_terrain_tile.h"
#include "mcdr_graphics_recording.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

module mcdr.d3d12.mesh;

import mcdr.d3d12.workgraph;
import mcdr.render.vanilla;
import mcdr.d3d12.device;
import mcdr.render.oit;

namespace d3d12_mc {
namespace {

struct TerrainBatch {
    ComPtr<ID3D12Resource> visible;
    ComPtr<ID3D12Resource> arguments;
    ComPtr<ID3D12Resource> atlas;
    ComPtr<ID3D12Resource> globals;
    uint64_t visibleBytes = 0;
    uint64_t argumentBytes = 0;
    uint64_t atlasOffset = 0;
    uint64_t globalsOffset = 0;
    uint64_t atlasBytes = 0;
    uint64_t globalsBytes = 0;
    uint64_t candidatesOffset = 0;
    uint64_t candidatesBytes = 0;
    uint32_t slots = 0;
    uint32_t sectionCount = 0;
    ID3D12Fence* fence = nullptr;
    uint64_t serial = 0;
    uint64_t resourceVersion = 0;
    std::shared_ptr<const void> recordingIdentity;
    Dx12Object* globalsObject = nullptr;
    Dx12Object* atlasObject = nullptr;
    Dx12Object* candidatesObject = nullptr;
};

struct TerrainProgram {
    struct IndirectSignature {
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12CommandSignature> signature;
        UINT baseParameter;
    };
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> initialize;
    ComPtr<ID3D12PipelineState> finalize;
    void* graph = nullptr;
    uint32_t uniformCount = 0;
    uint64_t budget = 0;
    uint64_t allocated = 0;
    std::vector<std::shared_ptr<TerrainBatch>> batches;
    std::vector<IndirectSignature> signatures;

    ~TerrainProgram() { mcdr::workgraph::destroyProgram(graph); }
};

struct TerrainProgramHandle { std::shared_ptr<TerrainProgram> program; };
struct TerrainBatchHandle {
    std::shared_ptr<TerrainProgram> program;
    std::shared_ptr<TerrainBatch> batch;
};
struct TerrainRecordingLease {
    std::shared_ptr<TerrainProgram> program;
    std::shared_ptr<TerrainBatch> batch;
    std::vector<ComPtr<ID3D12Resource>> inputs;
};

struct TerrainDrawSlice {
    ComPtr<ID3D12Resource> resource;
    uint64_t offset = 0;
    uint64_t bytes = 0;
    uint32_t initialState = D3D12_RESOURCE_STATE_COMMON;
};

struct TerrainDrawBindingFact {
    DrawBinding binding;
    ComPtr<ID3D12Resource> resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    bool hasSrv = false;
    std::shared_ptr<void> objectLease;
};

struct TerrainDrawResourceFact {
    ComPtr<ID3D12Resource> resource;
    uint32_t firstSubresource = 0;
    uint32_t subresourceCount = 0;
    uint32_t state = 0;
    uint32_t access = 0;
    uint32_t initialState = D3D12_RESOURCE_STATE_COMMON;
};

struct TerrainDrawPacket {
    uint32_t schemaVersion = 1;
    oit::TerrainDrawContent content = oit::TerrainDrawContent::OriginalDrawOnly;
    oit::TerrainDrawIdentity identity;
    std::shared_ptr<const void> contentLease;
    std::shared_ptr<void> tileRouteLease;
    mcdr::terrain_tile::StageAdapterDescription tileAdapter;
    GraphicsDrawState state;
    std::shared_ptr<TerrainProgram> programLease;
    std::shared_ptr<const TerrainBatch> batchLease;
    std::shared_ptr<void> vertexLease;
    ComPtr<ID3D12CommandSignature> signature;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12PipelineState> psoDepth;
    ComPtr<ID3D12PipelineState> activePso;
    uintptr_t pipelineIdentity = 0;
    uint64_t captureHash = 0;
    uint64_t captureGeneration = 0;
    UINT segmentParam = 0;
    UINT geometryParam = 0;
    UINT pushConstantParam = UINT_MAX;
    uint32_t pushConstantBytes = 0;
    uint32_t slots = 0;
    TerrainDrawSlice vertex, globals, atlas, visible, arguments;
    std::vector<DrawBinding> bindings;
    std::vector<TerrainDrawBindingFact> bindingFacts;
    std::vector<TerrainDrawResourceFact> resources;
    D3D12_GPU_VIRTUAL_ADDRESS captureConstants = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE captureDescriptors{};
    TerrainTileGeometryV2 tileGeometry{};
};

struct TerrainTileBatchOwner {
    std::shared_ptr<void> route;
    std::vector<std::shared_ptr<void>> resources;
    std::vector<std::shared_ptr<void>> pipelines;
    uint64_t frameSerial = 0;
    uint64_t worldEpoch = 0;
    uint64_t programGeneration = 0;
    uint64_t resourceVersion = 0;
    uint32_t laneIndex = 0;
    uint32_t residentTileCount = 0;
    uint32_t primitiveCount = 0;
    uint32_t refCapacity = 0;
    ComPtr<ID3D12Resource> drawConstants;
    D3D12_GPU_VIRTUAL_ADDRESS drawConstantsAddress = 0;
    ComPtr<ID3D12Resource> batchConstants;
    D3D12_GPU_VIRTUAL_ADDRESS batchConstantsAddress = 0;
};

mcdr::terrain_tile::Result prepareTerrainTilePacketImpl(void* context,
    const std::shared_ptr<const void>& packet, const mcdr::terrain_tile::Batch& batch);
mcdr::terrain_tile::Result prepareTerrainTilePacketV2Impl(void* context,
    const std::shared_ptr<const void>& packet, const mcdr::terrain_tile::BatchV2& batch);

mcdr::Result<ComPtr<ID3D12Resource>> terrainBuffer(ID3D12Device* device, uint64_t bytes) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource;
    const HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(result)) return mcdr::err("terrain.allocate", "Terrain output allocation failed");
    return resource;
}

void terrainUavBarrier(CommandContext& context) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    context.commandList->ResourceBarrier(1, &barrier);
}

mcdr::Result<std::shared_ptr<TerrainBatch>> acquireTerrainBatch(TerrainProgram& program,
    uint64_t visibleBytes, uint64_t argumentBytes) {
    for (const auto& batch : program.batches) {
        if (batch.use_count() == 1 && batch->visibleBytes >= visibleBytes &&
            batch->argumentBytes >= argumentBytes) return batch;
    }
    for (auto iterator = program.batches.begin(); iterator != program.batches.end();) {
        if (iterator->use_count() == 1) {
            program.allocated -= (*iterator)->visibleBytes + (*iterator)->argumentBytes;
            iterator = program.batches.erase(iterator);
        } else ++iterator;
    }
    const uint64_t required = visibleBytes + argumentBytes;
    if (required > program.budget || program.allocated > program.budget - required) {
        return mcdr::err("terrain.allocate", "In-flight terrain outputs exceed the configured budget");
    }
    auto visible = terrainBuffer(program.device.Get(), visibleBytes);
    if (!visible) return std::unexpected<mcdr::Error>(visible.error());
    auto arguments = terrainBuffer(program.device.Get(), argumentBytes);
    if (!arguments) return std::unexpected<mcdr::Error>(arguments.error());
    auto batch = std::make_shared<TerrainBatch>();
    batch->visible = *visible;
    batch->arguments = *arguments;
    batch->visibleBytes = visibleBytes;
    batch->argumentBytes = argumentBytes;
    program.allocated += required;
    program.batches.push_back(batch);
    return batch;
}

}

mcdr::Result<void*> createTerrainWorkGraph(const TerrainProgramDescription& description) {
    if (!description.nodes || description.nodesBytes == 0 || !description.initialize ||
        description.initializeBytes == 0 || !description.finalize || description.finalizeBytes == 0 ||
        description.uniformCount > 24 || (description.uniformCount != 0 && !description.uniformRegisters) ||
        description.budget == 0) {
        return mcdr::err("terrain.create", "Invalid terrain program description");
    }
    auto program = std::make_shared<TerrainProgram>();
    program->device = deviceContext().device;
    if (!program->device) return mcdr::err("terrain.create", "No active D3D12 device");
    program->uniformCount = description.uniformCount;
    program->budget = description.budget;
    std::vector<mcdr::workgraph::RootBinding> bindings;
    for (uint32_t index = 0; index < description.uniformCount; ++index) {
        const uint32_t shaderRegister = description.uniformRegisters[index];
        if (shaderRegister == 8 || shaderRegister == 9 || shaderRegister > 63 ||
            std::any_of(bindings.begin(), bindings.end(), [shaderRegister](const auto& binding) {
                return binding.shaderRegister == shaderRegister;
            })) return mcdr::err("terrain.create", "Invalid or duplicate terrain uniform register");
        bindings.push_back({0, shaderRegister, 0});
    }
    bindings.push_back({0, 8, 0});
    bindings.push_back({1, 15, 0});
    bindings.push_back({1, 16, 0});
    bindings.push_back({2, 0, 0});
    mcdr::workgraph::ProgramDescription graphDescription{description.nodes, description.nodesBytes,
        L"Terrain", L"SectionEntry", 0, 16, bindings.data(), static_cast<uint32_t>(bindings.size()),
        description.budget};
    auto graph = mcdr::workgraph::createProgram(graphDescription);
    if (!graph) return std::unexpected<mcdr::Error>(graph.error());
    program->graph = *graph;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 8;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.ShaderRegister = 0;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[2].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 3;
    rootDescription.pParameters = parameters;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT result = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors);
    if (FAILED(result)) return mcdr::err("terrain.create", "Argument root signature serialization failed");
    result = program->device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&program->root));
    if (FAILED(result)) return mcdr::err("terrain.create", "Argument root signature creation failed");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = program->root.Get();
    pipeline.CS = {description.initialize, description.initializeBytes};
    result = program->device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&program->initialize));
    if (FAILED(result)) return mcdr::err("terrain.create", "Terrain initialization PSO creation failed");
    pipeline.CS = {description.finalize, description.finalizeBytes};
    result = program->device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&program->finalize));
    if (FAILED(result)) return mcdr::err("terrain.create", "Terrain argument PSO creation failed");
    return static_cast<void*>(new TerrainProgramHandle{std::move(program)});
}

void destroyTerrainWorkGraph(void* program) {
    delete static_cast<TerrainProgramHandle*>(program);
}

mcdr::Result<void*> prepareTerrainWorkGraph(void* opaqueContext, void* opaqueProgram,
    void* const* buffers, const uint64_t* offsets, const uint64_t* lengths,
    uint32_t bindingCount, uint32_t sectionCount, uint32_t meshletCapacity) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* handle = static_cast<TerrainProgramHandle*>(opaqueProgram);
    if (!context || !context->listOpen || !context->commandList || !handle || !buffers || !offsets ||
        !lengths || bindingCount != handle->program->uniformCount + 3 || sectionCount == 0 ||
        sectionCount > 65535u * 64u || meshletCapacity == 0 ||
        meshletCapacity > UINT32_MAX - 4194303u ||
        handle->program->device.Get() != deviceContext().device.Get()) {
        return mcdr::err("terrain.prepare", "Invalid terrain context, bindings or candidate counts");
    }
    const uint64_t resourceVersion = vanilla::frozenResourceVersion(opaqueContext);
    if (resourceVersion == 0) return mcdr::err("terrain.prepare", "No frozen vanilla resource version");
    const uint64_t visibleBytes = 32ull + sectionCount * 32ull + meshletCapacity * 8ull;
    if (visibleBytes > INT_MAX) return mcdr::err("terrain.prepare", "Terrain visible table exceeds 2 GB");
    std::vector<uint64_t> addresses;
    auto lease = std::make_shared<TerrainRecordingLease>();
    lease->program = handle->program;
    for (uint32_t index = 0; index < bindingCount; ++index) {
        auto* buffer = static_cast<Dx12Object*>(buffers[index]);
        const bool constant = index <= handle->program->uniformCount;
        if (!buffer || buffer->kind != Dx12Object::Kind::Buffer || !buffer->resource || buffer->size < 0 ||
            buffer->heapType == D3D12_HEAP_TYPE_READBACK || offsets[index] > static_cast<uint64_t>(buffer->size) ||
            lengths[index] == 0 || lengths[index] > static_cast<uint64_t>(buffer->size) - offsets[index] ||
            (buffer->resource->GetGPUVirtualAddress() + offsets[index]) % (constant ? 256 : 4) != 0) {
            return mcdr::err("terrain.prepare", "Invalid terrain raw buffer or uniform range");
        }
        addresses.push_back(buffer->resource->GetGPUVirtualAddress() + offsets[index]);
        lease->inputs.push_back(buffer->resource);
    }
    const uint32_t globalsIndex = handle->program->uniformCount;
    const uint32_t candidatesIndex = globalsIndex + 1;
    const uint32_t atlasIndex = globalsIndex + 2;
    if (lengths[globalsIndex] < 32 || lengths[candidatesIndex] < sectionCount * 40ull ||
        lengths[atlasIndex] < 16 || lengths[atlasIndex] % 16 != 0) {
        return mcdr::err("terrain.prepare", "Terrain input schema does not match buffer lengths");
    }
    const uint32_t slots = (meshletCapacity + 4194303u) / 4194304u;
    auto acquired = acquireTerrainBatch(*handle->program, visibleBytes, slots * 16ull);
    if (!acquired) return std::unexpected<mcdr::Error>(acquired.error());
    auto batch = *acquired;
    batch->atlas = lease->inputs[atlasIndex];
    batch->globals = lease->inputs[globalsIndex];
    batch->atlasOffset = offsets[atlasIndex];
    batch->globalsOffset = offsets[globalsIndex];
    batch->atlasBytes = lengths[atlasIndex];
    batch->globalsBytes = lengths[globalsIndex];
    batch->candidatesOffset = offsets[candidatesIndex];
    batch->candidatesBytes = lengths[candidatesIndex];
    batch->slots = slots;
    batch->sectionCount = sectionCount;
    batch->fence = context->fence.Get();
    batch->serial = context->fenceValue + 1;
    batch->resourceVersion = resourceVersion;
    batch->recordingIdentity = context->recordingIdentity;
    batch->globalsObject = static_cast<Dx12Object*>(buffers[globalsIndex]);
    batch->atlasObject = static_cast<Dx12Object*>(buffers[atlasIndex]);
    batch->candidatesObject = static_cast<Dx12Object*>(buffers[candidatesIndex]);
    lease->batch = batch;
    context->recordingLeases[context->currentListSlot].push_back(lease);
    std::vector<vanilla::PassResourceUse> declarations;
    declarations.reserve(bindingCount + 2);
    for (uint32_t index = 0; index < bindingCount; ++index) {
        auto* buffer = static_cast<Dx12Object*>(buffers[index]);
        const auto requested = index <= globalsIndex ?
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER :
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto required = buffer->heapType == D3D12_HEAP_TYPE_UPLOAD
            ? D3D12_RESOURCE_STATE_GENERIC_READ : requested;
        declarations.push_back({buffer, resourceVersion, 0, 1,
            vanilla::ResourceAccess::Read, static_cast<uint32_t>(required)});
    }
    declarations.push_back({nullptr, resourceVersion, 0, 1, vanilla::ResourceAccess::Uav,
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        vanilla::ExternalResourceState::Tracked, batch->visible.Get(), D3D12_RESOURCE_STATE_COMMON});
    declarations.push_back({nullptr, resourceVersion, 0, 1, vanilla::ResourceAccess::Uav,
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        vanilla::ExternalResourceState::Tracked, batch->arguments.Get(), D3D12_RESOURCE_STATE_COMMON});
    vanilla::DeclaredPassToken* token = nullptr;
    if (vanilla::beginDeclaredPass(opaqueContext, 1, declarations.data(),
            static_cast<uint32_t>(declarations.size()), 0, &token) != 1) {
        vanilla::failRasterInputPass(opaqueContext);
        return mcdr::err("terrain.prepare", vanilla::lastFailureText());
    }
    const auto bindArguments = [&]() {
        context->commandList->SetComputeRootSignature(handle->program->root.Get());
        context->commandList->SetComputeRootConstantBufferView(0, addresses[globalsIndex]);
        context->commandList->SetComputeRootUnorderedAccessView(1, batch->visible->GetGPUVirtualAddress());
        context->commandList->SetComputeRootUnorderedAccessView(2, batch->arguments->GetGPUVirtualAddress());
    };
    bindArguments();
    context->commandList->SetPipelineState(handle->program->initialize.Get());
    context->commandList->Dispatch(1, 1, 1);
    terrainUavBarrier(*context);
    addresses.push_back(batch->visible->GetGPUVirtualAddress());
    alignas(16) const uint32_t record[]{(sectionCount + 63u) / 64u, 1, 1, sectionCount};
    auto dispatched = mcdr::workgraph::dispatch(context, handle->program->graph, record, 1, 16,
        addresses.data(), static_cast<uint32_t>(addresses.size()));
    context->cachedPso = nullptr;
    if (!dispatched) {
        vanilla::failRasterInputPass(opaqueContext);
        return std::unexpected<mcdr::Error>(dispatched.error());
    }
    bindArguments();
    context->commandList->SetPipelineState(handle->program->finalize.Get());
    context->commandList->Dispatch((slots + 63u) / 64u, 1, 1);
    terrainUavBarrier(*context);
    if (auto transitioned = transitionResourceRange(context, batch->visible.Get(), 0, 1,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); !transitioned) {
        vanilla::failRasterInputPass(opaqueContext);
        return std::unexpected<mcdr::Error>(transitioned.error());
    }
    if (auto transitioned = transitionResourceRange(context, batch->arguments.Get(), 0, 1,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT); !transitioned) {
        vanilla::failRasterInputPass(opaqueContext);
        return std::unexpected<mcdr::Error>(transitioned.error());
    }
    context->cachedPso = nullptr;
    if (vanilla::finishDeclaredPass(opaqueContext, token) != 1) {
        vanilla::failRasterInputPass(opaqueContext);
        return mcdr::err("terrain.prepare", vanilla::lastFailureText());
    }
    return static_cast<void*>(new TerrainBatchHandle{handle->program, std::move(batch)});
}

mcdr::Result<void> drawTerrainWorkGraph(void* opaqueContext, void* opaqueBatch,
    void* opaqueVertexBuffer, uint64_t vertexOffset) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* handle = static_cast<TerrainBatchHandle*>(opaqueBatch);
    auto* vertex = static_cast<Dx12Object*>(opaqueVertexBuffer);
    if (!context || !context->listOpen || !context->inRenderPass || !handle ||
        !context->currentPipeline || !context->currentPipeline->isMesh ||
        !context->currentPipeline->meshVisibleWork || !vertex || vertex->kind != Dx12Object::Kind::Buffer ||
        !vertex->resource || vertex->heapType == D3D12_HEAP_TYPE_READBACK || vertex->size < 4 ||
        vertexOffset > static_cast<uint64_t>(vertex->size) - 4 || vertexOffset % 4 != 0 ||
        handle->batch->fence != context->fence.Get() || handle->batch->serial != context->fenceValue + 1 ||
        !context->recordingIdentity || handle->batch->recordingIdentity != context->recordingIdentity) {
        return mcdr::err("terrain.draw", "Invalid terrain draw context, visible MS pipeline or vertex range");
    }
    const auto failDraw = [&](std::unexpected<mcdr::Error> error) -> mcdr::Result<void> {
        completeGraphicsCommandRecording(context, mcdr::graphics_recording::Completion::Failed);
        vanilla::failRasterInputPass(opaqueContext);
        return error;
    };
    if (handle->batch->resourceVersion == 0 ||
        vanilla::frozenResourceVersion(opaqueContext) != handle->batch->resourceVersion) {
        return failDraw(mcdr::err("terrain.draw", "Terrain batch resource version is stale"));
    }
    auto& batch = *handle->batch;
    auto* pipeline = context->currentPipeline;
    oit::TerrainDrawIdentity captureIdentity;
    const bool captured = pipeline->captureHash != 0;
    if (captured && oit::validateTerrainDraw(opaqueContext,vertex,vertexOffset,&captureIdentity)!=1)
        return failDraw(mcdr::err("terrain.draw", "Terrain capture vertex lease differs from metadata"));
    if (oit::prepareTerrainStageDrawReceipt(opaqueContext, vertex, vertexOffset) != 1)
        return failDraw(mcdr::err("terrain.draw", "Terrain stage draw registration differs from native draw"));
    ComPtr<ID3D12CommandSignature> signature;
    const UINT baseParameter = pipeline->geometryParam + 3;
    for (const auto& cached : handle->program->signatures) {
        if (cached.root.Get() == pipeline->rootSignature.Get() && cached.baseParameter == baseParameter) {
            signature = cached.signature;
            break;
        }
    }
    if (!signature) {
        D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
        arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        arguments[0].Constant.RootParameterIndex = baseParameter;
        arguments[0].Constant.Num32BitValuesToSet = 1;
        arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
        D3D12_COMMAND_SIGNATURE_DESC description{};
        description.ByteStride = 16;
        description.NumArgumentDescs = 2;
        description.pArgumentDescs = arguments;
        const HRESULT result = handle->program->device->CreateCommandSignature(&description,
            pipeline->rootSignature.Get(), IID_PPV_ARGS(&signature));
        if (FAILED(result)) return failDraw(mcdr::err("terrain.draw", "Terrain indirect command signature creation failed"));
        handle->program->signatures.push_back({pipeline->rootSignature, signature, baseParameter});
    }
    if (const auto collected = collectGraphicsInputs(opaqueContext, false, nullptr); !collected) {
        return failDraw(std::unexpected(collected.error()));
    }
    if (oit::validateTerrainStageReads(opaqueContext, opaqueBatch, vertex, vertexOffset,
            batch.atlasObject, batch.atlasOffset, batch.atlasBytes,
            batch.sectionCount, batch.globalsObject, batch.globalsOffset, batch.globalsBytes,
            batch.candidatesObject, batch.candidatesOffset, batch.candidatesBytes,
            std::static_pointer_cast<const void>(handle->batch)) != 1)
        return failDraw(mcdr::err("terrain.draw", "Terrain replay read-set differs from original draw resources"));
    struct DrawLease {
        ComPtr<ID3D12CommandSignature> signature;
        ComPtr<ID3D12Resource> vertex;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12PipelineState> psoDepth;
    };
    std::shared_ptr<const TerrainDrawPacket> packet;
    if (captured) {
        auto facts = std::make_shared<TerrainDrawPacket>();
        facts->identity = std::move(captureIdentity);
        facts->state = context->graphicsDrawState;
        facts->programLease = handle->program;
        facts->batchLease = handle->batch;
        facts->vertexLease = retainObjectLease(vertex);
        facts->signature = signature;
        facts->root = pipeline->rootSignature;
        facts->pso = pipeline->withoutDepth;
        facts->psoDepth = pipeline->withDepth;
        facts->activePso = context->cachedPso;
        facts->pipelineIdentity = reinterpret_cast<uintptr_t>(pipeline);
        facts->captureHash = pipeline->captureHash;
        facts->captureGeneration = pipeline->captureGeneration;
        facts->tileRouteLease = pipeline->terrainTileRoute;
        facts->tileAdapter = describeTerrainTileStageAdapter(
            std::static_pointer_cast<const void>(pipeline->terrainTileRoute),
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pipeline)), pipeline->shaderGeneration);
        facts->segmentParam = pipeline->segmentParam;
        facts->geometryParam = pipeline->geometryParam;
        facts->pushConstantParam = pipeline->pushConstantParam;
        facts->pushConstantBytes = pipeline->pushConstantBytes;
        facts->slots = batch.slots;
        facts->vertex = {vertex->resource, vertexOffset, facts->identity.vertexBytes,
            static_cast<uint32_t>(vertex->heapType == D3D12_HEAP_TYPE_UPLOAD
                ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON)};
        facts->globals = {batch.globals, batch.globalsOffset, batch.globalsBytes,
            static_cast<uint32_t>(batch.globalsObject->heapType == D3D12_HEAP_TYPE_UPLOAD
                ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON)};
        facts->atlas = {batch.atlas, batch.atlasOffset, batch.atlasBytes,
            static_cast<uint32_t>(batch.atlasObject->heapType == D3D12_HEAP_TYPE_UPLOAD
                ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON)};
        facts->visible = {batch.visible, 0, batch.visibleBytes};
        facts->arguments = {batch.arguments, 0, batch.argumentBytes};
        facts->bindings = context->graphicsBindings;
        facts->bindingFacts.reserve(facts->bindings.size());
        for (const auto& binding : facts->bindings) {
            auto* object = binding.type == 1 ? binding.view : binding.buffer;
            auto* resource = object && object->sourceTexture ? object->sourceTexture : object;
            facts->bindingFacts.push_back({binding, resource ? resource->resource : nullptr,
                object ? object->srvDesc : D3D12_SHADER_RESOURCE_VIEW_DESC{}, object && object->hasSrvDesc,
                retainObjectLease(object)});
        }
        facts->resources.reserve(context->graphicsInputRanges.size());
        for (const auto& range : context->graphicsInputRanges) {
            facts->resources.push_back({range.object ? range.object->resource : nullptr, range.firstSubresource,
                range.subresourceCount, range.state, range.access,
                static_cast<uint32_t>(range.object && range.object->heapType == D3D12_HEAP_TYPE_UPLOAD
                    ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON)});
        }
        facts->captureConstants = context->graphicsCapture.constants;
        facts->captureDescriptors = context->graphicsCapture.descriptors;
        facts->tileGeometry.segmentAddress = facts->globals.resource->GetGPUVirtualAddress() + facts->globals.offset;
        facts->tileGeometry.vertexAddress = facts->vertex.resource->GetGPUVirtualAddress() + facts->vertex.offset;
        facts->tileGeometry.visibleAddress = facts->visible.resource->GetGPUVirtualAddress();
        facts->tileGeometry.atlasAddress = facts->atlas.resource->GetGPUVirtualAddress() + facts->atlas.offset;
        auto content = std::make_shared<PacketContentLease>();
        content->status = PacketContentStatus::UnsupportedContent;
        content->recordingIdentity = context->recordingIdentity;
        content->fence = context->fence;
        content->submission = context->fenceValue + 1;
        facts->contentLease = std::move(content);
        context->recordingLeases[context->currentListSlot].push_back(facts);
        packet = std::move(facts);
    } else {
        auto lease = std::make_shared<DrawLease>();
        lease->signature = signature;
        lease->vertex = vertex->resource;
        lease->root = pipeline->rootSignature;
        lease->pso = pipeline->withoutDepth;
        lease->psoDepth = pipeline->withDepth;
        context->recordingLeases[context->currentListSlot].push_back(lease);
    }
    const auto vertexState = vertex->heapType == D3D12_HEAP_TYPE_UPLOAD
        ? D3D12_RESOURCE_STATE_GENERIC_READ
        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto globalsState = batch.globalsObject->heapType == D3D12_HEAP_TYPE_UPLOAD
        ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    const auto atlasState = batch.atlasObject->heapType == D3D12_HEAP_TYPE_UPLOAD
        ? D3D12_RESOURCE_STATE_GENERIC_READ
        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    std::vector<vanilla::PassResourceUse> declarations;
    declarations.reserve(5 + context->graphicsInputRanges.size());
    declarations.push_back({vertex, batch.resourceVersion, 0, 1,
        vanilla::ResourceAccess::Read, static_cast<uint32_t>(vertexState)});
    declarations.push_back({batch.globalsObject, batch.resourceVersion, 0, 1,
        vanilla::ResourceAccess::Read, static_cast<uint32_t>(globalsState)});
    declarations.push_back({batch.atlasObject, batch.resourceVersion, 0, 1,
        vanilla::ResourceAccess::Read, static_cast<uint32_t>(atlasState)});
    declarations.push_back({nullptr, batch.resourceVersion, 0, 1, vanilla::ResourceAccess::Read,
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        UINT32_MAX, vanilla::ExternalResourceState::Tracked, batch.visible.Get(), D3D12_RESOURCE_STATE_COMMON});
    declarations.push_back({nullptr, batch.resourceVersion, 0, 1, vanilla::ResourceAccess::Read,
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        UINT32_MAX, vanilla::ExternalResourceState::Tracked, batch.arguments.Get(), D3D12_RESOURCE_STATE_COMMON});
    for (const auto& range : context->graphicsInputRanges) {
        declarations.push_back({range.owner ? nullptr : range.object, batch.resourceVersion, range.firstSubresource,
            range.subresourceCount, static_cast<vanilla::ResourceAccess>(range.access), range.state,
            UINT32_MAX,vanilla::ExternalResourceState::Tracked,
            range.owner ? range.object->resource.Get() : nullptr,
            static_cast<uint32_t>(range.object->heapType==D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON)});
    }
    vanilla::DeclaredPassToken* token = nullptr;
    if (vanilla::beginDeclaredPass(opaqueContext, 2, declarations.data(),
            static_cast<uint32_t>(declarations.size()), 3, &token) != 1) {
        return failDraw(mcdr::err("terrain.draw", vanilla::lastFailureText()));
    }
    auto* graphicsPso = context->graphicsHasDepth ? pipeline->withDepth.Get() : pipeline->withoutDepth.Get();
    if (!graphicsPso) graphicsPso = pipeline->withDepth.Get();
    if (!graphicsPso) {
        return failDraw(mcdr::err("terrain.draw", "Terrain graphics pipeline state is unavailable after Work Graph preparation"));
    }
    context->commandList->SetPipelineState(graphicsPso);
    context->cachedPso = graphicsPso;
    if (const auto applied = applyGraphicsCapture(context); !applied) {
        return failDraw(std::unexpected(applied.error()));
    }
    if (packet && oit::retainTerrainDrawPacket(opaqueContext, packet->identity,
            packet->content, std::static_pointer_cast<const void>(packet), &prepareTerrainTilePacketImpl,
            &prepareTerrainTilePacketV2Impl) != 1) {
        return failDraw(mcdr::err("terrain.draw", "Terrain draw packet lease could not be retained by OIT frame"));
    }
    const auto globalsAddress = packet
        ? packet->globals.resource->GetGPUVirtualAddress() + packet->globals.offset
        : batch.globals->GetGPUVirtualAddress() + batch.globalsOffset;
    const auto vertexAddress = packet
        ? packet->vertex.resource->GetGPUVirtualAddress() + packet->vertex.offset
        : vertex->resource->GetGPUVirtualAddress() + vertexOffset;
    const auto visibleAddress = packet ? packet->visible.resource->GetGPUVirtualAddress()
        : batch.visible->GetGPUVirtualAddress();
    const auto atlasAddress = packet ? packet->atlas.resource->GetGPUVirtualAddress() + packet->atlas.offset
        : batch.atlas->GetGPUVirtualAddress() + batch.atlasOffset;
    const UINT segmentParam = packet ? packet->segmentParam : pipeline->segmentParam;
    const UINT geometryParam = packet ? packet->geometryParam : pipeline->geometryParam;
    if (context->submissionFailed.load() || context->recordingFailed.load() || (packet &&
        packetContentStatus(opaqueContext, packet->contentLease) == PacketContentStatus::StaleRecording)) {
        context->recordingFailed.store(true, std::memory_order_release);
        return failDraw(mcdr::err("terrain.draw", "Terrain packet recording became stale before drawing"));
    }
    context->commandList->SetGraphicsRootConstantBufferView(segmentParam, globalsAddress);
    context->commandList->SetGraphicsRootShaderResourceView(geometryParam, vertexAddress);
    context->commandList->SetGraphicsRootShaderResourceView(geometryParam + 1, visibleAddress);
    context->commandList->SetGraphicsRootShaderResourceView(geometryParam + 2, atlasAddress);
    if (prepareSampledRenderPassWrites(context) != SampledTextureContentStatus::Ready) {
        return failDraw(mcdr::err("terrain.draw", "Sampled texture contents could not be preserved before writing"));
    }
    std::shared_ptr<const void> graphicsPacket;
    if (context->graphicsCommandObserver.prepare) {
        mcdr::graphics_recording::Command command;
        command.family = mcdr::graphics_recording::CommandFamily::Mesh;
        command.count = packet ? packet->slots : batch.slots;
        command.meshGroups[0] = command.count;
        command.meshGroups[1] = 1;
        command.meshGroups[2] = 1;
        const auto vertexBytes = packet ? packet->vertex.bytes : static_cast<uint64_t>(vertex->size) - vertexOffset;
        const auto vertexOffsetValue = packet ? packet->vertex.offset : vertexOffset;
        const auto globalsBytes = packet ? packet->globals.bytes : batch.globalsBytes;
        const auto globalsOffsetValue = packet ? packet->globals.offset : batch.globalsOffset;
        const auto atlasBytes = packet ? packet->atlas.bytes : batch.atlasBytes;
        const auto atlasOffsetValue = packet ? packet->atlas.offset : batch.atlasOffset;
        const DrawBinding terrainBindings[] = {
            {2, vertex, static_cast<long long>(vertexOffsetValue), static_cast<long long>(vertexBytes)},
            {2, batch.globalsObject, static_cast<long long>(globalsOffsetValue), static_cast<long long>(globalsBytes)},
            {2, batch.atlasObject, static_cast<long long>(atlasOffsetValue), static_cast<long long>(atlasBytes)},
            {2, batch.candidatesObject, static_cast<long long>(batch.candidatesOffset), static_cast<long long>(batch.candidatesBytes)}
        };
        const auto tileAdapter = packet ? packet->tileAdapter : describeTerrainTileStageAdapter(
            std::static_pointer_cast<const void>(pipeline->terrainTileRoute),
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pipeline)), pipeline->shaderGeneration);
        mcdr::graphics_recording::AdapterFacts adapter;
        switch (tileAdapter.status) {
            case mcdr::terrain_tile::StageAdapterStatus::Unavailable:
                adapter.registration = mcdr::graphics_recording::AdapterRegistration::Unavailable;
                break;
            case mcdr::terrain_tile::StageAdapterStatus::Registered:
                adapter.registration = mcdr::graphics_recording::AdapterRegistration::Registered;
                break;
            case mcdr::terrain_tile::StageAdapterStatus::Stale:
                adapter.registration = mcdr::graphics_recording::AdapterRegistration::Stale;
                break;
        }
        adapter.sourceIdentity = tileAdapter.sourceIdentity;
        adapter.sourceGeneration = tileAdapter.sourceGeneration;
        adapter.tileIdentity = tileAdapter.tileIdentity;
        adapter.tileGeneration = tileAdapter.tileGeneration;
        adapter.contractHash = tileAdapter.contract.shaderContractHash;
        adapter.programGeneration = tileAdapter.contract.programGeneration;
        adapter.schemaVersion = tileAdapter.contract.schemaVersion;
        adapter.structSize = tileAdapter.contract.structSize;
        adapter.shaderAbiVersion = tileAdapter.contract.shaderAbiVersion;
        adapter.addressingVersion = tileAdapter.contract.addressingVersion;
        adapter.stage = tileAdapter.contract.stage;
        adapter.quadsPerMeshlet = tileAdapter.contract.quadsPerMeshlet;
        adapter.routeLease = tileAdapter.routeLease;
        if (prepareGraphicsCommandCaptureWithInputs(context, command, terrainBindings, 4, {},
                globalsAddress, adapter, &graphicsPacket) != 1) {
            return failDraw(mcdr::err("terrain.draw", "Graphics command capture preparation failed"));
        }
    }
    context->commandList->ExecuteIndirect(packet ? packet->signature.Get() : signature.Get(),
        packet ? packet->slots : batch.slots, packet ? packet->arguments.resource.Get() : batch.arguments.Get(),
        packet ? packet->arguments.offset : 0, nullptr, 0);
    if (graphicsPacket && confirmGraphicsCommandCapture(context, graphicsPacket) != 1) {
        context->recordingFailed.store(true, std::memory_order_release);
        return failDraw(mcdr::err("terrain.draw", "Graphics command capture confirmation failed"));
    }
    if (context->vanillaDepthSeedDraw && context->activeDepthTarget)
        context->vanillaDepthSeedDraw(context->vanillaDepthSeedState, context, context->activeDepthTarget,
            pipeline, static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::Mesh),
            ++context->vanillaDepthSeedOrdinal, graphicsPacket);
    if (pipeline->rasterMotionWritesWorldColor) {
        for (size_t index = 0; index < context->activeColorTargetsTouched.size(); ++index) {
            context->activeColorTargetsTouched[index] = true;
        }
        if (!context->activeColorTargets.empty()) context->colorTargetsWritten = true;
    }
    if (vanilla::finishDeclaredPass(opaqueContext, token) != 1) {
        return failDraw(mcdr::err("terrain.draw", vanilla::lastFailureText()));
    }
    if (graphicsPacket && completeGraphicsCommandRecording(context, mcdr::graphics_recording::Completion::Succeeded) != 1) {
        context->recordingFailed.store(true, std::memory_order_release);
        vanilla::failRasterInputPass(opaqueContext);
        return mcdr::err("terrain.draw", "Graphics command capture completion failed");
    }
    if (packet && oit::confirmTerrainDrawReceipt(opaqueContext) != 1) {
        context->recordingFailed.store(true, std::memory_order_release);
        vanilla::failRasterInputPass(opaqueContext);
        return mcdr::err("terrain.draw", "Terrain draw receipt could not be confirmed after recording");
    }
    if (oit::confirmTerrainStageDrawReceipt(opaqueContext) != 1) {
        context->recordingFailed.store(true, std::memory_order_release);
        vanilla::failRasterInputPass(opaqueContext);
        return mcdr::err("terrain.draw", "Terrain stage receipt could not be confirmed after recording");
    }
    return {};
}

namespace {

mcdr::terrain_tile::Result prepareTerrainTilePacketImpl(void* opaqueContext,
    const std::shared_ptr<const void>& opaquePacket, const mcdr::terrain_tile::Batch& batch) {
    using mcdr::terrain_tile::Result;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    const auto packet = std::static_pointer_cast<const TerrainDrawPacket>(opaquePacket);
    if (!context || !packet || !context->listOpen || !context->commandList || context->submissionFailed.load())
        return Result::StaleRecording;
    if (context->recordingFailed.load()) return Result::RecordingFailed;
    if (batch.schemaVersion != mcdr::terrain_tile::schemaVersion || batch.flags != 0 ||
        batch.residentTileCount == 0 || batch.residentTileCount > mcdr::terrain_tile::maximumResidentTiles ||
        batch.laneIndex >= mcdr::terrain_tile::laneCount) return Result::InvalidBatch;
    for (const auto value : batch.sourceViewport)
        if (!std::isfinite(value)) return Result::InvalidBatch;
    if (batch.sourceViewport[2] <= 0 || batch.sourceViewport[3] <= 0) return Result::InvalidBatch;
    for (uint32_t index = 0; index < batch.residentTileCount; ++index) {
        const auto& tile = batch.tiles[index];
        if (tile.originX % 2 || tile.originY % 2 || tile.effectiveWidth == 0 || tile.effectiveHeight == 0 ||
            tile.effectiveWidth > mcdr::terrain_tile::tileSize || tile.effectiveHeight > mcdr::terrain_tile::tileSize)
            return Result::InvalidBatch;
    }
    if (packet->schemaVersion != 1 || !packet->signature || !packet->root || !packet->batchLease ||
        packet->slots == 0 || packet->vertex.bytes == 0 || packet->globals.bytes == 0 || packet->atlas.bytes == 0 ||
        packet->visible.bytes == 0 || packet->arguments.bytes == 0 || !packet->vertex.resource ||
        !packet->globals.resource || !packet->atlas.resource || !packet->visible.resource ||
        !packet->arguments.resource || !packet->contentLease)
        return Result::UnsupportedContent;
    const auto content = packetContentStatus(opaqueContext, packet->contentLease);
    if (content == PacketContentStatus::StaleRecording) return Result::StaleRecording;
    if (content != PacketContentStatus::Frozen) return Result::UnsupportedContent;
    if (!packet->tileRouteLease || packet->tileAdapter.status != mcdr::terrain_tile::StageAdapterStatus::Registered ||
        !packet->tileAdapter.routeLease || packet->tileAdapter.routeLease.get() != packet->tileRouteLease.get() ||
        packet->tileAdapter.sourceIdentity != packet->pipelineIdentity ||
        packet->tileAdapter.sourceGeneration == 0 || packet->tileAdapter.tileIdentity == 0 ||
        packet->tileAdapter.tileGeneration == 0 ||
        packet->tileAdapter.contract.programGeneration != packet->captureGeneration)
        return Result::UnsupportedRoute;
    return Result::UnsupportedContent;
}

namespace {

bool validTileBuffer(const mcdr::terrain_tile::ResourceRef& reference, uint64_t minimumBytes) {
    auto* object = static_cast<Dx12Object*>(reference.object);
    if (!object || object->kind != Dx12Object::Kind::Buffer || !object->resource || reference.bytes < minimumBytes ||
        reference.offset > static_cast<uint64_t>(object->size) ||
        reference.bytes > static_cast<uint64_t>(object->size) - reference.offset)
        return false;
    return true;
}

bool validTileTexture(const mcdr::terrain_tile::ResourceRef& reference) {
    auto* object = static_cast<Dx12Object*>(reference.object);
    if (!object || object->kind != Dx12Object::Kind::Texture || !object->resource || reference.offset != 0)
        return false;
    const auto description = object->resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
}

bool validTileScratchTexture(const mcdr::terrain_tile::ResourceRef& reference, uint32_t residentCount) {
    if (!validTileTexture(reference)) return false;
    const auto* object = static_cast<const Dx12Object*>(reference.object);
    const auto description = object->resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.Width == mcdr::terrain_tile::scratchSize &&
        description.Height == mcdr::terrain_tile::scratchSize &&
        description.DepthOrArraySize >= residentCount && description.MipLevels == 1 &&
        description.SampleDesc.Count == 1;
}

bool validTileDepthTexture(const mcdr::terrain_tile::ResourceRef& reference, uint32_t residentCount) {
    if (!validTileScratchTexture(reference, residentCount)) return false;
    const auto* object = static_cast<const Dx12Object*>(reference.object);
    const auto description = object->resource->GetDesc();
    return object->dxgiFormat == DXGI_FORMAT_D32_FLOAT &&
        (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 &&
        (description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
}

bool validTileEnvelopes(const mcdr::terrain_tile::ResourceRef& reference,
    const std::shared_ptr<const TerrainDrawPacket>& packet, uint32_t primitiveCount) {
    auto* object = static_cast<Dx12Object*>(reference.object);
    if (!object || object->heapType != D3D12_HEAP_TYPE_UPLOAD || !object->mappedPtr ||
        !packet || packet->identity.primitiveCount == 0 || packet->identity.sectionCount == 0 ||
        packet->atlas.bytes < 16)
        return false;
    const auto required = static_cast<uint64_t>(primitiveCount) * 64;
    if (reference.offset > static_cast<uint64_t>(object->size) ||
        required > static_cast<uint64_t>(object->size) - reference.offset ||
        reference.bytes < required)
        return false;
    const auto* bytes = static_cast<const std::byte*>(object->mappedPtr) + reference.offset;
    for (uint32_t primitive = 0; primitive < primitiveCount; ++primitive) {
        uint32_t identity[4]{};
        std::memcpy(identity, bytes + static_cast<uint64_t>(primitive) * 64 + 16, sizeof(identity));
        if (identity[0] >= packet->identity.primitiveCount || identity[1] >= packet->identity.sectionCount ||
            identity[2] > (packet->atlas.bytes - 16) / 16)
            return false;
    }
    return true;
}

bool validBinningPipeline(void* opaque) {
    auto* pipeline = static_cast<Dx12Object*>(opaque);
    return pipeline && pipeline->kind == Dx12Object::Kind::ComputePipeline &&
        pipeline->terrainTileBinning && pipeline->computeRootSignature && pipeline->computePipelineState;
}

mcdr::Result<void> createTileBatchConstants(const mcdr::terrain_tile::BatchV2& batch,
    ComPtr<ID3D12Resource>& resource, D3D12_GPU_VIRTUAL_ADDRESS& address) {
    auto* device = deviceContext().device.Get();
    if (!device) return mcdr::err("terrain.tile.v2", "No active device for tile batch constants");
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = mcdr::terrain_tile::batchConstantAllocationBytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource))))
        return mcdr::err("terrain.tile.v2", "Tile batch constant allocation failed");
    struct BatchConstants {
        uint32_t schema, resident, lane, flags;
        float viewport[4];
        uint32_t regions[mcdr::terrain_tile::maximumResidentTiles][4];
    } constants{};
    static_assert(sizeof(BatchConstants) == 288);
    constants.schema = 1;
    constants.resident = batch.residentTileCount;
    constants.lane = batch.laneIndex;
    constants.flags = batch.flags;
    std::memcpy(constants.viewport, batch.sourceViewport, sizeof(constants.viewport));
    for (uint32_t index = 0; index < batch.residentTileCount; ++index) {
        constants.regions[index][0] = batch.tiles[index].originX;
        constants.regions[index][1] = batch.tiles[index].originY;
        constants.regions[index][2] = batch.tiles[index].effectiveWidth;
        constants.regions[index][3] = batch.tiles[index].effectiveHeight;
    }
    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, 0};
    if (FAILED(resource->Map(0, &readRange, &mapped)))
        return mcdr::err("terrain.tile.v2", "Tile batch constant mapping failed");
    std::memcpy(mapped, &constants, sizeof(constants));
    resource->Unmap(0, nullptr);
    address = resource->GetGPUVirtualAddress();
    if (address % 256 != 0) return mcdr::err("terrain.tile.v2", "Tile batch CBV is misaligned");
    return {};
}

mcdr::Result<void> createTileDrawConstants(ComPtr<ID3D12Resource>& resource,
    D3D12_GPU_VIRTUAL_ADDRESS& address) {
    auto* device = deviceContext().device.Get();
    if (!device) return mcdr::err("terrain.tile.v2", "No active device for tile draw constants");
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = 256;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource))))
        return mcdr::err("terrain.tile.v2", "Tile draw constant allocation failed");
    struct DrawConstants {
        uint32_t tileSlot;
        uint32_t sourcePrimitive;
        uint32_t sourceInstance;
        uint32_t visibleMeshlet;
    } constants{};
    static_assert(sizeof(DrawConstants) == 16);
    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, 0};
    if (FAILED(resource->Map(0, &readRange, &mapped)))
        return mcdr::err("terrain.tile.v2", "Tile draw constant mapping failed");
    std::memcpy(mapped, &constants, sizeof(constants));
    resource->Unmap(0, nullptr);
    address = resource->GetGPUVirtualAddress();
    if (address % 256 != 0) return mcdr::err("terrain.tile.v2", "Tile draw CBV is misaligned");
    return {};
}

mcdr::Result<std::shared_ptr<TerrainTileBatchOwner>> retainTerrainTileBatch(
    const std::shared_ptr<const TerrainDrawPacket>& packet, const mcdr::terrain_tile::BatchV2& batch) {
    using mcdr::terrain_tile::ReplayStage;
    if (batch.schemaVersion != 2 || batch.structSize != sizeof(mcdr::terrain_tile::BatchV2) || batch.flags != 0 ||
        batch.residentTileCount == 0 || batch.residentTileCount > mcdr::terrain_tile::maximumResidentTiles ||
        batch.laneIndex >= mcdr::terrain_tile::laneCount || batch.frameSerial == 0 || batch.worldEpoch == 0 ||
        batch.programGeneration == 0 || batch.resourceVersion == 0 || batch.primitiveCount == 0 ||
        batch.primitiveCount > 4096 || batch.refCapacity < batch.primitiveCount * batch.residentTileCount ||
        (batch.commandKind != 0 && batch.commandKind != 1) ||
        (batch.commandKind == 0 && batch.commandStride != 36) ||
        (batch.commandKind == 1 && batch.commandStride != 28))
        return mcdr::err("terrain.tile.v2", "Invalid tile batch contract");
    if (!packet || packet->schemaVersion != 1 || packet->content != oit::TerrainDrawContent::OriginalDrawOnly ||
        !packet->tileRouteLease || !packet->contentLease || packet->captureHash == 0 ||
        packet->tileAdapter.status != mcdr::terrain_tile::StageAdapterStatus::Registered ||
        !packet->tileAdapter.routeLease || packet->tileAdapter.routeLease.get() != packet->tileRouteLease.get() ||
        packet->tileAdapter.sourceIdentity != packet->pipelineIdentity ||
        packet->tileAdapter.sourceGeneration == 0 || packet->tileAdapter.tileIdentity == 0 ||
        packet->tileAdapter.tileGeneration == 0 ||
        packet->tileAdapter.contract.programGeneration != batch.programGeneration ||
        packet->captureGeneration != batch.programGeneration || packet->identity.frameSerial != batch.frameSerial ||
        packet->identity.frame.worldEpoch != batch.worldEpoch ||
        packet->identity.frame.resourceVersion != batch.resourceVersion)
        return mcdr::err("terrain.tile.v2", "Tile batch identity does not match frozen terrain packet");
    const auto& adapter = packet->tileAdapter;
    if (adapter.contract.stage > 2)
        return mcdr::err("terrain.tile.v2", "Terrain tile adapter stage is invalid");
    const auto* adaptedPipeline = static_cast<const Dx12Pipeline*>(batch.stages[adapter.contract.stage].pipeline);
    if (!adaptedPipeline || static_cast<uint64_t>(reinterpret_cast<uintptr_t>(adaptedPipeline)) != adapter.tileIdentity ||
        adaptedPipeline->shaderGeneration != adapter.tileGeneration)
        return mcdr::err("terrain.tile.v2", "Tile stage pipeline differs from the registered terrain adapter");
    for (const float value : batch.sourceViewport)
        if (!std::isfinite(value)) return mcdr::err("terrain.tile.v2", "Tile viewport is not finite");
    if (batch.sourceViewport[2] <= 0 || batch.sourceViewport[3] <= 0)
        return mcdr::err("terrain.tile.v2", "Tile viewport has non-positive extent");
    for (uint32_t index = 0; index < batch.residentTileCount; ++index) {
        const auto& tile = batch.tiles[index];
        if ((tile.originX & 1u) != 0 || (tile.originY & 1u) != 0 || tile.effectiveWidth == 0 ||
            tile.effectiveHeight == 0 || tile.effectiveWidth > mcdr::terrain_tile::tileSize ||
            tile.effectiveHeight > mcdr::terrain_tile::tileSize)
            return mcdr::err("terrain.tile.v2", "Tile region is outside the fixed tile contract");
    }
    if (!validTileBuffer(batch.envelopes, static_cast<uint64_t>(batch.primitiveCount) * 64) ||
        !validTileBuffer(batch.masks, static_cast<uint64_t>(batch.primitiveCount) * 4) ||
        !validTileBuffer(batch.prefixes, static_cast<uint64_t>(batch.primitiveCount) * 4) ||
        !validTileBuffer(batch.commands, static_cast<uint64_t>(batch.refCapacity) * batch.commandStride) ||
        !validTileBuffer(batch.control, 16) || !validTileBuffer(batch.binningConstants, 16) ||
        !batch.binningWorkGraph || !validBinningPipeline(batch.binningInitializePipeline) ||
        !validBinningPipeline(batch.binningPrefixPipeline) || !validBinningPipeline(batch.binningScatterPipeline))
        return mcdr::err("terrain.tile.v2", "Tile binning resources are incomplete");
    if (!validTileEnvelopes(batch.envelopes, packet, batch.primitiveCount))
        return mcdr::err("terrain.tile.v2", "Tile envelopes are not mapped or reference invalid terrain data");
    if (!validTileScratchTexture(batch.bounds, batch.residentTileCount) ||
        !validTileScratchTexture(batch.culledBounds, batch.residentTileCount) ||
        !validTileDepthTexture(batch.depthTerrain, batch.residentTileCount) ||
        !validTileDepthTexture(batch.depthCloud, batch.residentTileCount) ||
        !validTileScratchTexture(batch.q, batch.residentTileCount) ||
        !validTileScratchTexture(batch.scratchColor, batch.residentTileCount) ||
        !validTileScratchTexture(batch.scratchDepth, batch.residentTileCount))
        return mcdr::err("terrain.tile.v2", "Tile depth or scratch resources are incomplete");
    for (const auto& coefficient : batch.coefficients)
        if (!validTileTexture(coefficient)) return mcdr::err("terrain.tile.v2", "Tile coefficient resource is invalid");
    auto owner = std::make_shared<TerrainTileBatchOwner>();
    owner->route = packet->tileRouteLease;
    owner->frameSerial = batch.frameSerial;
    owner->worldEpoch = batch.worldEpoch;
    owner->programGeneration = batch.programGeneration;
    owner->resourceVersion = batch.resourceVersion;
    owner->laneIndex = batch.laneIndex;
    owner->residentTileCount = batch.residentTileCount;
    owner->primitiveCount = batch.primitiveCount;
    owner->refCapacity = batch.refCapacity;
    if (auto constants = createTileDrawConstants(owner->drawConstants, owner->drawConstantsAddress); !constants)
        return std::unexpected<mcdr::Error>(constants.error());
    if (auto constants = createTileBatchConstants(batch, owner->batchConstants, owner->batchConstantsAddress); !constants)
        return std::unexpected<mcdr::Error>(constants.error());
    const mcdr::terrain_tile::ResourceRef resources[] = {
        batch.envelopes, batch.masks, batch.prefixes, batch.commands, batch.control,
        batch.bounds, batch.culledBounds, batch.depthTerrain, batch.depthCloud,
        batch.coefficients[0], batch.coefficients[1], batch.coefficients[2], batch.coefficients[3],
        batch.q, batch.scratchColor, batch.scratchDepth, batch.binningConstants,
    };
    for (const auto& resource : resources) owner->resources.push_back(retainObjectLease(resource.object));
    for (void* pipeline : {batch.binningInitializePipeline, batch.binningPrefixPipeline,
                           batch.binningScatterPipeline})
        owner->resources.push_back(retainObjectLease(pipeline));
    for (const auto& stage : batch.stages) {
        if (stage.stage > static_cast<uint32_t>(ReplayStage::Accumulate) || stage.flags != 0 ||
            stage.renderTargetCount == 0 || stage.renderTargetCount > 5 || !stage.pipeline ||
            !validTileDepthTexture(stage.depth, batch.residentTileCount))
            return mcdr::err("terrain.tile.v2", "Tile stage binding is incomplete");
        const auto expected = static_cast<uint32_t>(owner->pipelines.size());
        if (stage.stage != expected) return mcdr::err("terrain.tile.v2", "Tile stages are not ordered");
        auto* pipeline = static_cast<Dx12Pipeline*>(stage.pipeline);
        if (!pipeline->isMesh || !pipeline->rootSignature || !pipeline->meshVisibleWork)
            return mcdr::err("terrain.tile.v2", "Tile stage pipeline is not a visible mesh pipeline");
        if (pipeline->colorTargetCount != stage.renderTargetCount ||
            pipeline->colorTargetFormats.size() != stage.renderTargetCount ||
            pipeline->depthTargetFormat != DXGI_FORMAT_D32_FLOAT)
            return mcdr::err("terrain.tile.v2", "Tile stage attachment count does not match its PSO");
        for (uint32_t target = 0; target < stage.renderTargetCount; ++target) {
            const auto* color = static_cast<const Dx12Object*>(stage.renderTargets[target].object);
            if (!color || color->dxgiFormat != pipeline->colorTargetFormats[target])
                return mcdr::err("terrain.tile.v2", "Tile stage render target format does not match its PSO");
        }
        if ((stage.stage == 0 && stage.renderTargets[0].object != batch.bounds.object) ||
            (stage.stage == 1 && (stage.renderTargetCount != batch.stages[1].renderTargetCount ||
                std::any_of(stage.renderTargets, stage.renderTargets + stage.renderTargetCount,
                    [&](const auto& target) {
                        const auto index = static_cast<size_t>(&target - stage.renderTargets);
                        return target.object != batch.coefficients[index].object;
                    }))) ||
            (stage.stage == 2 && stage.renderTargets[0].object != batch.q.object))
            return mcdr::err("terrain.tile.v2", "Tile stage output mapping is unavailable");
        for (uint32_t prior = 0; prior < owner->pipelines.size(); ++prior)
            if (owner->pipelines[prior].get() == pipeline)
                return mcdr::err("terrain.tile.v2", "Tile replay stages must use distinct PSOs");
        owner->pipelines.push_back(std::shared_ptr<void>(pipeline, [](void* value) {
            destroyPipeline(static_cast<Dx12Pipeline*>(value));
        }));
        pipeline->lifetimeReferences.fetch_add(1, std::memory_order_relaxed);
        if (!validTileScratchTexture(stage.depth, batch.residentTileCount))
            return mcdr::err("terrain.tile.v2", "Tile stage depth attachment is invalid");
        owner->resources.push_back(retainObjectLease(stage.depth.object));
        for (uint32_t target = 0; target < stage.renderTargetCount; ++target) {
            if (!validTileScratchTexture(stage.renderTargets[target], batch.residentTileCount) ||
                !(static_cast<Dx12Object*>(stage.renderTargets[target].object)->usage & 8))
                return mcdr::err("terrain.tile.v2", "Tile stage render target is invalid");
            owner->resources.push_back(retainObjectLease(stage.renderTargets[target].object));
        }
    }
    return owner;
}

uint64_t tileBufferAddress(const mcdr::terrain_tile::ResourceRef& reference) {
    auto* object = static_cast<Dx12Object*>(reference.object);
    return object->resource->GetGPUVirtualAddress() + reference.offset;
}

void retainTileBufferForRecording(CommandContext* context, const mcdr::terrain_tile::ResourceRef& reference) {
    auto* object = static_cast<Dx12Object*>(reference.object);
    retainObjectForRecording(context, object);
}

void setTileBinningRoots(CommandContext* context, Dx12Object* pipeline,
    const std::array<uint64_t, 8>& addresses) {
    context->commandList->SetComputeRootSignature(pipeline->computeRootSignature.Get());
    context->commandList->SetComputeRootConstantBufferView(0, addresses[0]);
    context->commandList->SetComputeRootConstantBufferView(1, addresses[1]);
    context->commandList->SetComputeRootConstantBufferView(2, addresses[2]);
    context->commandList->SetComputeRootShaderResourceView(3, addresses[3]);
    context->commandList->SetComputeRootUnorderedAccessView(4, addresses[4]);
    context->commandList->SetComputeRootUnorderedAccessView(5, addresses[5]);
    context->commandList->SetComputeRootUnorderedAccessView(6, addresses[6]);
    context->commandList->SetComputeRootUnorderedAccessView(7, addresses[7]);
}

mcdr::Result<void> recordTerrainTileBinning(CommandContext* context,
    const mcdr::terrain_tile::BatchV2& batch, const std::shared_ptr<TerrainTileBatchOwner>& owner) {
    auto* initializePipeline = static_cast<Dx12Object*>(batch.binningInitializePipeline);
    auto* prefixPipeline = static_cast<Dx12Object*>(batch.binningPrefixPipeline);
    auto* scatterPipeline = static_cast<Dx12Object*>(batch.binningScatterPipeline);
    const std::array<uint64_t, 8> addresses = {
        owner->drawConstantsAddress,
        owner->batchConstantsAddress,
        tileBufferAddress(batch.binningConstants),
        tileBufferAddress(batch.envelopes),
        tileBufferAddress(batch.masks),
        tileBufferAddress(batch.prefixes),
        tileBufferAddress(batch.commands),
        tileBufferAddress(batch.control),
    };
    if (addresses[0] % 256 != 0 || addresses[1] % 256 != 0 || addresses[2] % 256 != 0 ||
        std::any_of(addresses.begin() + 3, addresses.end(), [](uint64_t address) { return address % 4 != 0; }))
        return mcdr::err("terrain.tile.v2", "Tile binning root address alignment is invalid");
    const mcdr::terrain_tile::ResourceRef resources[] = {
        batch.binningConstants, batch.envelopes, batch.masks, batch.prefixes, batch.commands, batch.control,
    };
    for (const auto& resource : resources) {
        retainTileBufferForRecording(context, resource);
    }
    context->recordingLeases[context->currentListSlot].push_back(owner);
    const auto declare = [&](std::initializer_list<std::tuple<const mcdr::terrain_tile::ResourceRef*,
            vanilla::ResourceAccess, uint32_t, uint32_t>> entries, uint32_t pass,
            vanilla::DeclaredPassToken*& token) -> bool {
        std::vector<vanilla::PassResourceUse> uses;
        uses.reserve(entries.size());
        for (const auto& [reference, access, required, finalState] : entries)
            uses.push_back({reference->object, batch.resourceVersion, UINT32_MAX, 0,
                access, required, finalState});
        return vanilla::beginDeclaredPass(context, pass, uses.data(),
            static_cast<uint32_t>(uses.size()), 0, &token) == 1;
    };
    const auto finish = [&](vanilla::DeclaredPassToken* token) -> bool {
        if (vanilla::finishDeclaredPass(context, token) == 1) return true;
        vanilla::failRasterInputPass(context);
        return false;
    };
    vanilla::DeclaredPassToken* token = nullptr;
    if (!declare({
            {&batch.binningConstants, vanilla::ResourceAccess::Read,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                D3D12_RESOURCE_STATE_GENERIC_READ},
            {&batch.masks, vanilla::ResourceAccess::Uav,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
            {&batch.control, vanilla::ResourceAccess::Uav,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        }, 0x220, token)) {
        return mcdr::err("terrain.tile.v2", vanilla::lastFailureText());
    }
    setTileBinningRoots(context, initializePipeline, addresses);
    context->commandList->SetPipelineState(initializePipeline->computePipelineState.Get());
    context->commandList->Dispatch(1, 1, 1);
    if (!finish(token)) return mcdr::err("terrain.tile.v2", vanilla::lastFailureText());
    if (!declare({
            {&batch.binningConstants, vanilla::ResourceAccess::Read,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                D3D12_RESOURCE_STATE_GENERIC_READ},
            {&batch.envelopes, vanilla::ResourceAccess::Read,
                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_GENERIC_READ},
            {&batch.masks, vanilla::ResourceAccess::Uav,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
            {&batch.control, vanilla::ResourceAccess::ReadWrite,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        }, 0x221, token)) {
        return mcdr::err("terrain.tile.v2", vanilla::lastFailureText());
    }
    alignas(16) const uint32_t entry[4] = {
        (batch.primitiveCount + 63u) / 64u, 1u, 1u, batch.primitiveCount,
    };
    const auto graph = mcdr::workgraph::dispatch(context, batch.binningWorkGraph, entry, 1, sizeof(entry),
        addresses.data(), static_cast<uint32_t>(addresses.size()));
    if (!graph) {
        vanilla::failRasterInputPass(context);
        return std::unexpected<mcdr::Error>(graph.error());
    }
    if (!finish(token)) return mcdr::err("terrain.tile.v2", vanilla::lastFailureText());
    if (!declare({
            {&batch.binningConstants, vanilla::ResourceAccess::Read,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                D3D12_RESOURCE_STATE_GENERIC_READ},
            {&batch.masks, vanilla::ResourceAccess::Read,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
            {&batch.prefixes, vanilla::ResourceAccess::Uav,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
            {&batch.control, vanilla::ResourceAccess::ReadWrite,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        }, 0x222, token)) {
        return mcdr::err("terrain.tile.v2", vanilla::lastFailureText());
    }
    setTileBinningRoots(context, prefixPipeline, addresses);
    context->commandList->SetPipelineState(prefixPipeline->computePipelineState.Get());
    context->commandList->Dispatch(1, 1, 1);
    if (!finish(token)) return mcdr::err("terrain.tile.v2", vanilla::lastFailureText());
    if (!declare({
            {&batch.binningConstants, vanilla::ResourceAccess::Read,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                D3D12_RESOURCE_STATE_GENERIC_READ},
            {&batch.masks, vanilla::ResourceAccess::Read,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
            {&batch.prefixes, vanilla::ResourceAccess::Read,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
            {&batch.commands, vanilla::ResourceAccess::Uav,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},
            {&batch.control, vanilla::ResourceAccess::ReadWrite,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},
        }, 0x223, token)) {
        return mcdr::err("terrain.tile.v2", vanilla::lastFailureText());
    }
    setTileBinningRoots(context, scatterPipeline, addresses);
    context->commandList->SetPipelineState(scatterPipeline->computePipelineState.Get());
    context->commandList->Dispatch((batch.primitiveCount + 63u) / 64u, 1, 1);
    for (const auto* reference : {&batch.commands, &batch.control}) {
        auto* object = static_cast<Dx12Object*>(reference->object);
        const auto before = resourceSubresourceState(context, object->resource.Get(), 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (const auto transitioned = transitionResourceRange(context, object->resource.Get(), 0, 1,
                before, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT); !transitioned) {
            vanilla::failRasterInputPass(context);
            return std::unexpected<mcdr::Error>(transitioned.error());
        }
    }
    if (!finish(token)) return mcdr::err("terrain.tile.v2", vanilla::lastFailureText());
    context->cachedPso = nullptr;
    return {};
}

}

mcdr::terrain_tile::Result prepareTerrainTilePacketV2Impl(void* opaqueContext,
    const std::shared_ptr<const void>& opaquePacket, const mcdr::terrain_tile::BatchV2& batch) {
    using mcdr::terrain_tile::Result;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    const auto packet = std::static_pointer_cast<const TerrainDrawPacket>(opaquePacket);
    if (!context || !packet || !context->listOpen || !context->commandList || context->submissionFailed.load())
        return Result::StaleRecording;
    if (context->recordingFailed.load()) return Result::RecordingFailed;
    const auto content = packetContentStatus(opaqueContext, packet->contentLease);
    if (content == PacketContentStatus::StaleRecording) return Result::StaleRecording;
    if (content != PacketContentStatus::Frozen) return Result::UnsupportedContent;
    const auto contentLease = std::static_pointer_cast<const PacketContentLease>(packet->contentLease);
    const auto findFrozen = [&](ID3D12Resource* source, uint64_t offset, uint64_t bytes,
        uint32_t firstSubresource, uint32_t subresourceCount) -> const PacketContentRange* {
        const PacketContentRange* match = nullptr;
        for (const auto& range : contentLease->frozenReads) {
            if (range.sourceResource.Get() != source || range.firstSubresource != firstSubresource ||
                range.subresourceCount != subresourceCount) continue;
            if (bytes != 0 && (range.byteLength != bytes || range.sourceByteOffset != offset))
                continue;
            if (!match) {
                match = &range;
                continue;
            }
            if (match->sourceByteOffset != range.sourceByteOffset ||
                match->byteLength != range.byteLength || match->resource.Get() != range.resource.Get() ||
                match->byteOffset != range.byteOffset)
                return nullptr;
        }
        return match;
    };
    const auto* frozenVertex = findFrozen(packet->vertex.resource.Get(), packet->vertex.offset,
        packet->vertex.bytes, 0, 1);
    const auto* frozenGlobals = findFrozen(packet->globals.resource.Get(), packet->globals.offset,
        packet->globals.bytes, 0, 1);
    const auto* frozenAtlas = findFrozen(packet->atlas.resource.Get(), packet->atlas.offset,
        packet->atlas.bytes, 0, 1);
    const auto* frozenVisible = findFrozen(packet->visible.resource.Get(), packet->visible.offset,
        packet->visible.bytes, 0, 1);
    if (!frozenVertex || !frozenGlobals || !frozenAtlas || !frozenVisible)
        return Result::UnsupportedContent;
    auto ownerResult = retainTerrainTileBatch(packet, batch);
    if (!ownerResult) return Result::UnsupportedContent;
    auto owner = std::move(*ownerResult);
    TerrainTileReplayInputsV2 inputs;
    inputs.bindings.reserve(packet->bindings.size());
    for (const auto& binding : packet->bindings) {
        auto frozenBinding = binding;
        if (binding.type == 1) {
            const auto view = std::find_if(contentLease->frozenViews.begin(), contentLease->frozenViews.end(),
                [&](const auto& frozen) { return frozen.source == binding.view; });
            if (view == contentLease->frozenViews.end() || !view->frozen) return Result::UnsupportedContent;
            frozenBinding.view = view->frozen;
        } else if (binding.buffer && binding.offset >= 0 && binding.length >= 0) {
            const auto* frozen = findFrozen(binding.buffer->resource.Get(),
                static_cast<uint64_t>(binding.offset), static_cast<uint64_t>(binding.length), 0, 1);
            if (!frozen || !frozen->object) return Result::UnsupportedContent;
            frozenBinding.buffer = frozen->object;
            frozenBinding.offset = static_cast<long long>(frozen->byteOffset);
        }
        inputs.bindings.push_back({frozenBinding.type, frozenBinding.buffer, frozenBinding.offset,
            frozenBinding.length, frozenBinding.texelFormat, frozenBinding.view});
    }
    inputs.pushConstants = packet->state.pushConstants;
    inputs.pushConstantBytes = packet->pushConstantBytes;
    inputs.pushConstantsValid = packet->pushConstantBytes == 0 ||
        (packet->state.pushConstantsValid && packet->state.pushRoot == packet->root.Get() &&
            packet->state.pushConstantBytes == packet->pushConstantBytes);
    for (const auto& stage : batch.stages) {
        const auto* pipeline = static_cast<const Dx12Pipeline*>(stage.pipeline);
        if (!pipeline->graphicsBindingLayoutKnown ||
            pipeline->graphicsBindingTypes.size() != inputs.bindings.size() ||
            pipeline->pushConstantBytes != inputs.pushConstantBytes || !inputs.pushConstantsValid ||
            inputs.pushConstantBytes > inputs.pushConstants.size() ||
            (inputs.pushConstantBytes != 0 && pipeline->pushConstantParam == UINT_MAX))
            return Result::UnsupportedContent;
        for (uint32_t index = 0; index < inputs.bindings.size(); ++index)
            if (pipeline->graphicsBindingTypes[index] != inputs.bindings[index].type)
                return Result::UnsupportedContent;
    }
    inputs.reads.reserve(packet->resources.size() + 4);
    for (const auto& read : packet->resources) {
        if (read.access != static_cast<uint32_t>(vanilla::ResourceAccess::Read)) continue;
        bool matched = false;
        for (const auto& frozen : contentLease->frozenReads) {
            if (frozen.sourceResource != read.resource || frozen.firstSubresource != read.firstSubresource ||
                frozen.subresourceCount != read.subresourceCount) continue;
            inputs.reads.push_back({frozen.resource.Get(), frozen.firstSubresource,
                frozen.subresourceCount, read.state, frozen.initialState});
            matched = true;
        }
        if (!matched) return Result::UnsupportedContent;
    }
    const uint32_t shaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for (const auto* read : {frozenVertex, frozenAtlas, frozenVisible})
        inputs.reads.push_back({read->resource.Get(), 0, 1,
            read->initialState == D3D12_RESOURCE_STATE_GENERIC_READ
                ? D3D12_RESOURCE_STATE_GENERIC_READ : shaderRead, read->initialState});
    inputs.reads.push_back({frozenGlobals->resource.Get(), 0, 1,
        static_cast<uint32_t>(frozenGlobals->initialState == D3D12_RESOURCE_STATE_GENERIC_READ
            ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        frozenGlobals->initialState});
    return Result::UnsupportedContent;
}

}

void destroyTerrainBatch(void* batch) {
    delete static_cast<TerrainBatchHandle*>(batch);
}

}
