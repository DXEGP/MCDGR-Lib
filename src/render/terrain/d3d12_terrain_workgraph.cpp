module;

#include "_d3d12_module_prelude.h"
#include "mcdr_terrain_tile.h"
#include "mcdr_graphics_recording.h"
#include "mcdr_oit_budget.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <tuple>

module mcdr.d3d12.mesh;

import mcdr.d3d12.workgraph;
import mcdr.render.vanilla;
import mcdr.d3d12.device;
import mcdr.render.oit;

namespace d3d12_mc {
namespace {

thread_local std::vector<vanilla::PassResourceUse> terrainDrawDeclarationsScratch;

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
    uint32_t meshletCapacity = 0;
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
    bool laneAttachment = false;
    SampledTextureContentCapture sampled;

    bool coversSampledRead(ID3D12Resource* source, uint32_t firstSubresource, uint32_t count) const {
        if (!sampled.lease || resource.Get() != source) return false;
        const auto mipLevels = source->GetDesc().MipLevels;
        const auto layer = firstSubresource / mipLevels;
        const auto mip = firstSubresource % mipLevels;
        return layer < sampled.layerCount && mip >= sampled.firstMip &&
            uint64_t(mip) + count <= uint64_t(sampled.firstMip) + sampled.mipCount;
    }
};

struct TerrainDrawResourceFact {
    ComPtr<ID3D12Resource> resource;
    Dx12Object* object = nullptr;
    std::shared_ptr<void> owner;
    uint32_t firstSubresource = 0;
    uint32_t subresourceCount = 0;
    uint32_t state = 0;
    uint32_t access = 0;
    uint32_t initialState = D3D12_RESOURCE_STATE_COMMON;
};

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
        description.uniformCount > 28 || (description.uniformCount != 0 && !description.uniformRegisters) ||
        description.budget == 0) {
        return mcdr::err("terrain.create", "Invalid terrain program description");
    }
    auto program = std::make_shared<TerrainProgram>();
    program->device = deviceContext().device;
    if (!program->device) return mcdr::err("terrain.create", "No active D3D12 device");
    program->uniformCount = description.uniformCount;
    program->budget = description.budget;
    std::vector<mcdr::workgraph::RootBinding> bindings;
    bindings.reserve(description.uniformCount + 4);
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
    constexpr uint32_t maxTerrainBindings = 28u + 3u;
    constexpr uint32_t maxTerrainAddresses = maxTerrainBindings + 1u;
    constexpr uint32_t maxTerrainDeclarations = maxTerrainBindings + 2u;
    std::array<uint64_t, maxTerrainAddresses> addresses{};
    uint32_t addressCount = 0;
    auto lease = std::make_shared<TerrainRecordingLease>();
    lease->program = handle->program;
    lease->inputs.reserve(bindingCount);
    for (uint32_t index = 0; index < bindingCount; ++index) {
        auto* buffer = static_cast<Dx12Object*>(buffers[index]);
        const bool constant = index <= handle->program->uniformCount;
        if (!buffer || buffer->kind != Dx12Object::Kind::Buffer || !buffer->resource || buffer->size < 0 ||
            buffer->heapType == D3D12_HEAP_TYPE_READBACK || offsets[index] > static_cast<uint64_t>(buffer->size) ||
            lengths[index] == 0 || lengths[index] > static_cast<uint64_t>(buffer->size) - offsets[index] ||
            (buffer->resource->GetGPUVirtualAddress() + offsets[index]) % (constant ? 256 : 4) != 0) {
            return mcdr::err("terrain.prepare", "Invalid terrain raw buffer or uniform range");
        }
        addresses[addressCount++] = buffer->resource->GetGPUVirtualAddress() + offsets[index];
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
    batch->meshletCapacity = meshletCapacity;
    batch->fence = context->fence.Get();
    batch->serial = context->fenceValue + 1;
    batch->resourceVersion = resourceVersion;
    batch->recordingIdentity = context->recordingIdentity;
    batch->globalsObject = static_cast<Dx12Object*>(buffers[globalsIndex]);
    batch->atlasObject = static_cast<Dx12Object*>(buffers[atlasIndex]);
    batch->candidatesObject = static_cast<Dx12Object*>(buffers[candidatesIndex]);
    lease->batch = batch;
    context->recordingLeases[context->currentListSlot].push_back(lease);
    std::array<vanilla::PassResourceUse, maxTerrainDeclarations> declarations{};
    uint32_t declarationCount = 0;
    for (uint32_t index = 0; index < bindingCount; ++index) {
        auto* buffer = static_cast<Dx12Object*>(buffers[index]);
        const auto requested = index <= globalsIndex ?
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER :
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto required = buffer->heapType == D3D12_HEAP_TYPE_UPLOAD
            ? D3D12_RESOURCE_STATE_GENERIC_READ : requested;
        declarations[declarationCount++] = {buffer, resourceVersion, 0, 1,
            vanilla::ResourceAccess::Read, static_cast<uint32_t>(required)};
    }
    declarations[declarationCount++] = {nullptr, resourceVersion, 0, 1, vanilla::ResourceAccess::Uav,
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        vanilla::ExternalResourceState::Tracked, batch->visible.Get(), D3D12_RESOURCE_STATE_COMMON};
    declarations[declarationCount++] = {nullptr, resourceVersion, 0, 1, vanilla::ResourceAccess::Uav,
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        vanilla::ExternalResourceState::Tracked, batch->arguments.Get(), D3D12_RESOURCE_STATE_COMMON};
    vanilla::DeclaredPassToken* token = nullptr;
    if (vanilla::beginDeclaredPass(opaqueContext, 1, declarations.data(),
            declarationCount, 0, &token) != 1) {
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
    addresses[addressCount++] = batch->visible->GetGPUVirtualAddress();
    alignas(16) const uint32_t record[]{(sectionCount + 63u) / 64u, 1, 1, sectionCount};
    auto dispatched = mcdr::workgraph::dispatch(context, handle->program->graph, record, 1, 16,
        addresses.data(), addressCount);
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
    void* opaqueVertexBuffer, uint64_t vertexOffset, uint64_t vertexBytes) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* handle = static_cast<TerrainBatchHandle*>(opaqueBatch);
    auto* vertex = static_cast<Dx12Object*>(opaqueVertexBuffer);
    if (!context || !context->listOpen || !context->inRenderPass || !handle ||
        !context->currentPipeline || !context->currentPipeline->isMesh ||
        !context->currentPipeline->meshVisibleWork || !vertex || vertex->kind != Dx12Object::Kind::Buffer ||
        !vertex->resource || vertex->heapType == D3D12_HEAP_TYPE_READBACK || vertex->size < 4 ||
        vertexOffset > static_cast<uint64_t>(vertex->size) - 4 || vertexOffset % 4 != 0 ||
        vertexBytes > static_cast<uint64_t>(vertex->size) - vertexOffset || vertexBytes % 4 != 0 ||
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
    const bool captured = pipeline->captureHash != 0;
    if (captured && oit::validateTerrainDraw(opaqueContext, vertex, vertexOffset) != 1)
        return failDraw(mcdr::err("terrain.draw", "Terrain capture vertex lease differs from metadata"));
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
    struct DrawLease {
        ComPtr<ID3D12CommandSignature> signature;
        ComPtr<ID3D12Resource> vertex;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12PipelineState> psoDepth;
    };
    {
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
    auto& declarations = terrainDrawDeclarationsScratch;
    declarations.clear();
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
    const auto globalsAddress = batch.globals->GetGPUVirtualAddress() + batch.globalsOffset;
    const auto vertexAddress = vertex->resource->GetGPUVirtualAddress() + vertexOffset;
    const auto visibleAddress = batch.visible->GetGPUVirtualAddress();
    const auto atlasAddress = batch.atlas->GetGPUVirtualAddress() + batch.atlasOffset;
    const UINT segmentParam = pipeline->segmentParam;
    const UINT geometryParam = pipeline->geometryParam;
    if (context->submissionFailed.load() || context->recordingFailed.load()) {
        context->recordingFailed.store(true, std::memory_order_release);
        return failDraw(mcdr::err("terrain.draw", "Terrain recording failed before drawing"));
    }
    context->commandList->SetGraphicsRootConstantBufferView(segmentParam, globalsAddress);
    context->commandList->SetGraphicsRootShaderResourceView(geometryParam, vertexAddress);
    context->commandList->SetGraphicsRootShaderResourceView(geometryParam + 1, visibleAddress);
    context->commandList->SetGraphicsRootShaderResourceView(geometryParam + 2, atlasAddress);
    if (prepareSampledRenderPassWrites(context) != SampledTextureContentStatus::Ready) {
        return failDraw(mcdr::err("terrain.draw", "Sampled texture contents could not be preserved before writing"));
    }
    context->commandList->ExecuteIndirect(signature.Get(), batch.slots, batch.arguments.Get(), 0, nullptr, 0);
    if (pipeline->rasterMotionWritesWorldColor) {
        for (size_t index = 0; index < context->activeColorTargetsTouched.size(); ++index) {
            context->activeColorTargetsTouched[index] = true;
        }
        if (!context->activeColorTargets.empty()) context->colorTargetsWritten = true;
    }
    if (vanilla::finishDeclaredPass(opaqueContext, token) != 1) {
        return failDraw(mcdr::err("terrain.draw", vanilla::lastFailureText()));
    }
    return {};
}

void destroyTerrainBatch(void* batch) {
    delete static_cast<TerrainBatchHandle*>(batch);
}

}
