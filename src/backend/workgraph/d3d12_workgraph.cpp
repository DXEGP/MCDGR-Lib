module;

#include "_d3d12_module_prelude.h"

module mcdr.d3d12.workgraph;

import mcdr.d3d12.device;

namespace mcdr::workgraph {
namespace {

using Microsoft::WRL::ComPtr;

struct Program {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12StateObject> state;
    ComPtr<ID3D12RootSignature> root;
    D3D12_PROGRAM_IDENTIFIER identifier{};
    uint64_t backingBytes = 0;
    uint64_t backingBudget = 0;
    uint32_t entryIndex = 0;
    uint32_t recordSize = 0;
    uint32_t recordAlignment = 0;
    std::vector<RootBinding> bindings;
    std::wstring name;
};

enum class Initialization { Required, Recorded, Complete };

struct Instance {
    ComPtr<ID3D12Resource> backing;
    Initialization initialization = Initialization::Required;
    ID3D12Fence* recordingFence = nullptr;
    uint64_t recordingSerial = 0;
};

struct Handle {
    std::shared_ptr<Program> program;
    std::vector<std::shared_ptr<Instance>> instances;
};

struct RecordingLease {
    std::shared_ptr<Program> program;
    std::shared_ptr<Instance> instance;
    ComPtr<ID3D12Fence> fence;
    uint64_t serial;

    ~RecordingLease() {
        if (instance->initialization != Initialization::Recorded) return;
        const UINT64 completed = fence->GetCompletedValue();
        instance->initialization = completed != UINT64_MAX && completed >= serial ?
            Initialization::Complete : Initialization::Required;
    }
};

mcdr::Result<std::shared_ptr<Instance>> retainInstanceForRecording(Handle& handle,
                                                                 d3d12_mc::CommandContext& context) {
    std::shared_ptr<Instance> instance;
    for (const auto& candidate : handle.instances) {
        if (candidate->recordingFence == context.fence.Get() &&
            candidate->recordingSerial == context.fenceValue + 1 && candidate.use_count() > 1)
            return candidate;
        if (!instance && candidate.use_count() == 1)
            instance = candidate;
    }
    if (!instance) {
        if (handle.program->backingBytes != 0 &&
            handle.instances.size() >= handle.program->backingBudget / handle.program->backingBytes) {
            return mcdr::err("workgraph.acquire", "In-flight Work Graph backing memory exceeds program budget: perInstance=" +
                std::to_string(handle.program->backingBytes) + " instances=" +
                std::to_string(handle.instances.size()) + " budget=" + std::to_string(handle.program->backingBudget));
        }
        instance = std::make_shared<Instance>();
        if (handle.program->backingBytes != 0) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC resource{};
            resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resource.Width = handle.program->backingBytes;
            resource.Height = 1;
            resource.DepthOrArraySize = 1;
            resource.MipLevels = 1;
            resource.SampleDesc.Count = 1;
            resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            const HRESULT result = handle.program->device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr, IID_PPV_ARGS(&instance->backing));
            if (FAILED(result)) return mcdr::err("workgraph.acquire", "Backing memory allocation failed");
            instance->backing->SetName((handle.program->name + L" backing").c_str());
        }
        handle.instances.push_back(instance);
    }
    context.recordingLeases[context.currentListSlot].push_back(
        std::make_shared<RecordingLease>(handle.program, instance, context.fence, context.fenceValue + 1));
    instance->recordingFence = context.fence.Get();
    instance->recordingSerial = context.fenceValue + 1;
    return instance;
}

}

mcdr::Result<void*> createProgram(const ProgramDescription& description) {
    auto& device = d3d12_mc::deviceContext();
    if (!device.device || device.caps.workGraphsTier < D3D12_WORK_GRAPHS_TIER_1_0) {
        return mcdr::err("workgraph.create", "Work Graph Tier 1.0 is required");
    }
    if (!description.dxil || description.dxilSize == 0 || !description.programName ||
        !description.entryName || description.recordSize == 0 || description.bindingCount > 32 ||
        (description.bindingCount != 0 && !description.bindings)) {
        return mcdr::err("workgraph.create", "Invalid program description");
    }
    auto program = std::make_shared<Program>();
    program->name = description.programName;
    program->device = device.device;
    std::vector<D3D12_ROOT_PARAMETER> parameters(description.bindingCount);
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges(description.bindingCount);
    for (uint32_t index = 0; index < description.bindingCount; ++index) {
        const auto& binding = description.bindings[index];
        if (binding.kind > 4) return mcdr::err("workgraph.create", "Unsupported root binding kind");
        auto& parameter = parameters[index];
        if (binding.kind >= 3) {
            auto& range = ranges[index];
            range.RangeType = binding.kind == 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            range.NumDescriptors = 1;
            range.BaseShaderRegister = binding.shaderRegister;
            range.RegisterSpace = binding.registerSpace;
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable = {1, &range};
        } else {
            parameter.ParameterType = binding.kind == 0 ? D3D12_ROOT_PARAMETER_TYPE_CBV :
                (binding.kind == 1 ? D3D12_ROOT_PARAMETER_TYPE_SRV : D3D12_ROOT_PARAMETER_TYPE_UAV);
            parameter.Descriptor.ShaderRegister = binding.shaderRegister;
            parameter.Descriptor.RegisterSpace = binding.registerSpace;
        }
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        program->bindings.push_back(binding);
    }
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = description.bindingCount;
    rootDescription.pParameters = parameters.data();
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT result = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors);
    if (FAILED(result)) return mcdr::err("workgraph.create", "Root signature serialization failed");
    result = device.device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&program->root));
    if (FAILED(result)) return mcdr::err("workgraph.create", "Root signature creation failed");
    program->root->SetName((program->name + L" root").c_str());
    D3D12_DXIL_LIBRARY_DESC library{};
    library.DXILLibrary = {description.dxil, description.dxilSize};
    D3D12_GLOBAL_ROOT_SIGNATURE globalRoot{program->root.Get()};
    D3D12_WORK_GRAPH_DESC graph{};
    graph.ProgramName = description.programName;
    graph.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    D3D12_STATE_SUBOBJECT subobjects[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRoot},
        {D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH, &graph},
    };
    D3D12_STATE_OBJECT_DESC stateDescription{};
    stateDescription.Type = D3D12_STATE_OBJECT_TYPE_EXECUTABLE;
    stateDescription.NumSubobjects = static_cast<UINT>(std::size(subobjects));
    stateDescription.pSubobjects = subobjects;
    ComPtr<ID3D12Device5> stateDevice;
    if (FAILED(device.device.As(&stateDevice))) {
        return mcdr::err("workgraph.create", "State object device interface is unavailable");
    }
    ComPtr<ID3D12InfoQueue> infoQueue;
    const bool haveInfoQueue = SUCCEEDED(device.device.As(&infoQueue));
    const UINT64 firstMessage = haveInfoQueue ? infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter() : 0;
    result = stateDevice->CreateStateObject(&stateDescription, IID_PPV_ARGS(&program->state));
    if (FAILED(result)) {
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_9};
        const HRESULT shaderQuery = device.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
            &shaderModel, sizeof(shaderModel));
        char detail[256]{};
        std::snprintf(detail, sizeof(detail),
            "Work Graph state object creation failed (HRESULT=0x%08lX, active shader model=0x%X). "
            "Check the DXIL target and the D3D12Core.dll staged beside the native DLL.",
            static_cast<unsigned long>(result), SUCCEEDED(shaderQuery) ?
                static_cast<unsigned int>(shaderModel.HighestShaderModel) : 0u);
        std::string message(detail);
        if (haveInfoQueue) {
            const UINT64 end = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 index = firstMessage; index < end && index - firstMessage < 8; ++index) {
                SIZE_T bytes = 0;
                if (FAILED(infoQueue->GetMessage(index, nullptr, &bytes))) continue;
                std::vector<uint8_t> storage(bytes);
                auto* diagnostic = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(infoQueue->GetMessage(index, diagnostic, &bytes)) && diagnostic->pDescription) {
                    message.append("\nD3D12: ").append(diagnostic->pDescription);
                }
            }
        }
        return mcdr::err("workgraph.create", message);
    }
    program->state->SetName(program->name.c_str());
    ComPtr<ID3D12WorkGraphProperties> graphProperties;
    ComPtr<ID3D12StateObjectProperties1> stateProperties;
    if (FAILED(program->state.As(&graphProperties)) || FAILED(program->state.As(&stateProperties))) {
        return mcdr::err("workgraph.create", "Work Graph properties are unavailable");
    }
    const UINT graphIndex = graphProperties->GetWorkGraphIndex(description.programName);
    if (graphIndex == UINT_MAX) return mcdr::err("workgraph.create", "Program name was not found");
    const D3D12_NODE_ID entry{description.entryName, description.entryArrayIndex};
    program->entryIndex = graphProperties->GetEntrypointIndex(graphIndex, entry);
    if (program->entryIndex == UINT_MAX) return mcdr::err("workgraph.create", "Entry node was not found");
    program->recordSize = graphProperties->GetEntrypointRecordSizeInBytes(graphIndex, program->entryIndex);
    program->recordAlignment = graphProperties->GetEntrypointRecordAlignmentInBytes(graphIndex, program->entryIndex);
    if (program->recordSize != description.recordSize) {
        return mcdr::err("workgraph.create", "Entry record schema does not match compiled node");
    }
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memory{};
    graphProperties->GetWorkGraphMemoryRequirements(graphIndex, &memory);
    d3d12_mc::dbgLogInfo("Work Graph %ls backing: minimum=%llu maximum=%llu budget=%llu instanceBudget=%llu",
        description.programName, static_cast<unsigned long long>(memory.MinSizeInBytes),
        static_cast<unsigned long long>(memory.MaxSizeInBytes),
        static_cast<unsigned long long>(description.backingBudget),
        static_cast<unsigned long long>(description.backingInstanceBudget));
    if (description.backingInstanceBudget != 0 && memory.MinSizeInBytes > description.backingInstanceBudget) {
        return mcdr::err("workgraph.create", "Required Work Graph backing memory exceeds per-instance budget: required=" +
            std::to_string(memory.MinSizeInBytes) + " instanceBudget=" + std::to_string(description.backingInstanceBudget));
    }
    if (memory.MinSizeInBytes > description.backingBudget) {
        return mcdr::err("workgraph.create", "Required Work Graph backing memory exceeds total program budget: required=" +
            std::to_string(memory.MinSizeInBytes) + " totalBudget=" + std::to_string(description.backingBudget));
    }
    program->backingBytes = memory.MinSizeInBytes;
    program->backingBudget = description.backingBudget;
    program->identifier = stateProperties->GetProgramIdentifier(description.programName);
    auto handle = std::make_unique<Handle>();
    handle->program = std::move(program);
    return static_cast<void*>(handle.release());
}

void destroyProgram(void* opaqueProgram) {
    delete static_cast<Handle*>(opaqueProgram);
}

mcdr::Result<void> dispatchBuffers(void* opaqueContext, void* opaqueProgram, const void* records,
                                  uint32_t recordCount, uint32_t recordStride,
                                  void* const* buffers, const uint64_t* offsets,
                                  const uint64_t* lengths, uint32_t bindingCount) {
    auto* context = static_cast<d3d12_mc::CommandContext*>(opaqueContext);
    auto* handle = static_cast<Handle*>(opaqueProgram);
    if (!context || !context->listOpen || !handle || bindingCount != handle->program->bindings.size() ||
        handle->program->device.Get() != d3d12_mc::deviceContext().device.Get() ||
        (bindingCount != 0 && (!buffers || !offsets || !lengths)) ||
        recordStride < handle->program->recordSize || handle->program->recordAlignment == 0 ||
        recordStride % handle->program->recordAlignment != 0 ||
        (recordCount != 0 && (!records || reinterpret_cast<uintptr_t>(records) % handle->program->recordAlignment != 0))) {
        return mcdr::err("workgraph.dispatchBuffers", "Invalid context or bindings");
    }
    std::array<uint64_t, 32> addresses{};
    struct ResourceState {
        d3d12_mc::Dx12Object* object;
        D3D12_RESOURCE_STATES state;
    };
    std::array<ResourceState, 32> resourceStates{};
    uint32_t resourceStateCount = 0;
    const auto deviceMatches = [&](d3d12_mc::Dx12Object* object) {
        ComPtr<ID3D12Device> owner;
        return object && object->resource &&
            SUCCEEDED(object->resource->GetDevice(IID_PPV_ARGS(&owner))) &&
            owner.Get() == handle->program->device.Get();
    };
    for (uint32_t index = 0; index < bindingCount; ++index) {
        auto* buffer = static_cast<d3d12_mc::Dx12Object*>(buffers[index]);
        const auto kind = handle->program->bindings[index].kind;
        if (kind > 2) return mcdr::err("workgraph.dispatchBuffers", "Descriptor tables require a native descriptor lease");
        if (!buffer || buffer->kind != d3d12_mc::Dx12Object::Kind::Buffer || !buffer->resource ||
            !deviceMatches(buffer) ||
            buffer->size < 0 || offsets[index] > static_cast<uint64_t>(buffer->size) ||
            lengths[index] == 0 || lengths[index] > static_cast<uint64_t>(buffer->size) - offsets[index] ||
            buffer->heapType == D3D12_HEAP_TYPE_READBACK ||
            (kind == 2 && (buffer->heapType != D3D12_HEAP_TYPE_DEFAULT ||
                !(buffer->resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))) {
            return mcdr::err("workgraph.dispatchBuffers", "Invalid buffer range or UAV resource");
        }
        addresses[index] = buffer->resource->GetGPUVirtualAddress() + offsets[index];
        if (addresses[index] % (kind == 0 ? 256 : 4) != 0) {
            return mcdr::err("workgraph.dispatchBuffers", "Misaligned root buffer range");
        }
        const auto requiredState = kind == 0 ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER :
            (kind == 1 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool requiresUav = (requiredState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0;
        auto existing = std::find_if(resourceStates.begin(), resourceStates.begin() + resourceStateCount,
            [resource = buffer->resource.Get()](const ResourceState& entry) {
                return entry.object->resource.Get() == resource;
            });
        if (existing == resourceStates.begin() + resourceStateCount) {
            resourceStates[resourceStateCount++] = {buffer, requiredState};
        } else {
            const bool existingUav = (existing->state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0;
            if (requiresUav != existingUav && (requiresUav || existingUav)) {
                return mcdr::err("workgraph.dispatchBuffers", "A resource cannot alias UAV and read bindings");
            }
            existing->state |= requiredState;
        }
    }
    if (recordCount == 0) return {};
    for (uint32_t index = 0; index < bindingCount; ++index)
        d3d12_mc::retainObjectForRecording(context, buffers[index]);
    for (uint32_t index = 0; index < resourceStateCount; ++index) {
        const auto& resource = resourceStates[index];
        if (resource.object->heapType != D3D12_HEAP_TYPE_UPLOAD)
            d3d12_mc::transitionBufferTo(context, resource.object, resource.state);
    }
    return dispatch(context, handle, records, recordCount, recordStride, addresses.data(), bindingCount);
}

static mcdr::Result<void> dispatchInput(void* opaqueContext, void* opaqueProgram, const void* records,
                           uint32_t recordCount, uint32_t recordStride,
                           const uint64_t* rootAddresses, uint32_t addressCount, uint64_t gpuInput) {
    auto* context = static_cast<d3d12_mc::CommandContext*>(opaqueContext);
    auto* handle = static_cast<Handle*>(opaqueProgram);
    if (!context || !context->listOpen || !context->commandList || !handle) {
        return mcdr::err("workgraph.dispatch", "An open command context and program are required");
    }
    auto& program = *handle->program;
    if (program.device.Get() != d3d12_mc::deviceContext().device.Get() ||
        addressCount != program.bindings.size() || (addressCount != 0 && !rootAddresses) ||
        (gpuInput == 0 && (recordStride < program.recordSize || program.recordAlignment == 0 ||
        recordStride % program.recordAlignment != 0 || (recordCount != 0 && !records) ||
        (reinterpret_cast<uintptr_t>(records) % program.recordAlignment) != 0)) ||
        (gpuInput != 0 && gpuInput % 8 != 0)) {
        return mcdr::err("workgraph.dispatch", "Invalid program generation, bindings, or input records");
    }
    if (gpuInput == 0 && recordCount == 0) return {};
    for (uint32_t index = 0; index < addressCount; ++index) {
        const uint64_t alignment = program.bindings[index].kind == 0 ? 256 :
            (program.bindings[index].kind <= 2 ? 4 : 1);
        if (rootAddresses[index] == 0 || rootAddresses[index] % alignment != 0) {
            return mcdr::err("workgraph.dispatch", "Invalid root buffer address alignment");
        }
    }
    auto* list = context->workGraphCommandList;
    if (!list) {
        return mcdr::err("workgraph.dispatch", "Work Graph command list interface is unavailable");
    }
    auto acquired = retainInstanceForRecording(*handle, *context);
    if (!acquired) return std::unexpected<mcdr::Error>(acquired.error());
    auto instance = *acquired;
    list->SetComputeRootSignature(program.root.Get());
    for (uint32_t index = 0; index < addressCount; ++index) {
        switch (program.bindings[index].kind) {
            case 0: list->SetComputeRootConstantBufferView(index, rootAddresses[index]); break;
            case 1: list->SetComputeRootShaderResourceView(index, rootAddresses[index]); break;
            case 2: list->SetComputeRootUnorderedAccessView(index, rootAddresses[index]); break;
            case 3:
            case 4: list->SetComputeRootDescriptorTable(index, {rootAddresses[index]}); break;
        }
    }
    D3D12_SET_PROGRAM_DESC selected{};
    selected.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    selected.WorkGraph.ProgramIdentifier = program.identifier;
    selected.WorkGraph.Flags = instance->initialization == Initialization::Required ?
        D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE : D3D12_SET_WORK_GRAPH_FLAG_NONE;
    if (instance->backing) {
        selected.WorkGraph.BackingMemory = {instance->backing->GetGPUVirtualAddress(), program.backingBytes};
    }
    list->SetProgram(&selected);
    D3D12_DISPATCH_GRAPH_DESC invocation{};
    if (gpuInput != 0) {
        invocation.Mode = D3D12_DISPATCH_MODE_NODE_GPU_INPUT;
        invocation.NodeGPUInput = gpuInput;
    } else {
        invocation.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        invocation.NodeCPUInput = {program.entryIndex, recordCount, records, recordStride};
    }
    list->BeginEvent(0, program.name.c_str(),
        static_cast<UINT>((program.name.size() + 1) * sizeof(wchar_t)));
    list->DispatchGraph(&invocation);
    list->EndEvent();
    D3D12_RESOURCE_BARRIER completion{};
    completion.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &completion);
    if (instance->initialization == Initialization::Required) instance->initialization = Initialization::Recorded;
    context->cachedPso = nullptr;
    return {};
}

mcdr::Result<void> dispatch(void* context, void* program, const void* records,
                           uint32_t recordCount, uint32_t recordStride,
                           const uint64_t* rootAddresses, uint32_t addressCount) {
    return dispatchInput(context, program, records, recordCount, recordStride, rootAddresses, addressCount, 0);
}

mcdr::Result<void> dispatchGpu(void* context, void* program, uint64_t inputAddress,
                              const uint64_t* rootAddresses, uint32_t addressCount) {
    if (inputAddress == 0) return mcdr::err("workgraph.dispatchGpu", "GPU input descriptor address is required");
    return dispatchInput(context, program, nullptr, 0, 0, rootAddresses, addressCount, inputAddress);
}

mcdr::Result<void> dispatchGpuBuffers(void* opaqueContext, void* opaqueProgram, uint32_t inputSchema,
                                     void* opaqueInput, uint64_t inputOffset, uint64_t inputBytes,
                                     void* const* buffers, const uint64_t* offsets,
                                     const uint64_t* lengths, uint32_t bindingCount) {
    using namespace d3d12_mc;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* handle = static_cast<Handle*>(opaqueProgram);
    auto* input = static_cast<Dx12Object*>(opaqueInput);
    if (!context || !context->listOpen || !context->commandList || !handle || inputSchema != 1 ||
        handle->program->device.Get() != deviceContext().device.Get() ||
        bindingCount != handle->program->bindings.size() ||
        (bindingCount != 0 && (!buffers || !offsets || !lengths))) {
        return mcdr::err("workgraph.dispatchGpuBuffers", "Invalid context, generation, input schema or bindings");
    }
    const auto deviceMatches = [&](Dx12Object* object) {
        ComPtr<ID3D12Device> owner;
        return object && object->resource && SUCCEEDED(object->resource->GetDevice(IID_PPV_ARGS(&owner))) &&
            owner.Get() == handle->program->device.Get();
    };
    if (!input || input->kind != Dx12Object::Kind::Buffer || input->heapType != D3D12_HEAP_TYPE_DEFAULT ||
        input->size < 0 || !deviceMatches(input) || inputOffset % 8 != 0 ||
        inputOffset > static_cast<uint64_t>(input->size) || inputBytes < sizeof(D3D12_NODE_GPU_INPUT) ||
        inputBytes > static_cast<uint64_t>(input->size) - inputOffset) {
        return mcdr::err("workgraph.dispatchGpuBuffers", "Invalid GPU input buffer span or alignment");
    }
    std::array<uint64_t, 32> addresses{};
    for (uint32_t index = 0; index < bindingCount; ++index) {
        auto* buffer = static_cast<Dx12Object*>(buffers[index]);
        const auto kind = handle->program->bindings[index].kind;
        if (kind > 2 || !buffer || buffer->kind != Dx12Object::Kind::Buffer || buffer->size < 0 ||
            !deviceMatches(buffer) || buffer->heapType == D3D12_HEAP_TYPE_READBACK ||
            offsets[index] > static_cast<uint64_t>(buffer->size) || lengths[index] == 0 ||
            lengths[index] > static_cast<uint64_t>(buffer->size) - offsets[index] ||
            (kind == 2 && (buffer->heapType != D3D12_HEAP_TYPE_DEFAULT ||
                !(buffer->resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
                buffer->resource.Get() == input->resource.Get()))) {
            return mcdr::err("workgraph.dispatchGpuBuffers", "Invalid GPU consumer buffer binding or input/UAV alias");
        }
        addresses[index] = buffer->resource->GetGPUVirtualAddress() + offsets[index];
        if (addresses[index] % (kind == 0 ? 256 : 4) != 0) {
            return mcdr::err("workgraph.dispatchGpuBuffers", "Misaligned GPU consumer buffer binding");
        }
    }
    // All host-visible validation precedes pins and state mutations. GPU
    // descriptor entry/count/address/stride are a trusted-producer contract;
    // inspecting them on the CPU would require a synchronization/readback.
    retainObjectForRecording(context, input);
    transitionBufferTo(context, input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (uint32_t index = 0; index < bindingCount; ++index) {
        auto* buffer = static_cast<Dx12Object*>(buffers[index]);
        retainObjectForRecording(context, buffer);
        const auto kind = handle->program->bindings[index].kind;
        if (buffer->heapType != D3D12_HEAP_TYPE_UPLOAD) {
            // Input can also be bound as CBV/SRV; preserve its required SRV
            // access on the shared resource. UAV alias was rejected above.
            auto state = kind == 0 ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER :
                (kind == 1 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (buffer->resource.Get() == input->resource.Get()) state |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            transitionBufferTo(context, buffer, state);
        }
    }
    return dispatchGpu(context, handle, input->resource->GetGPUVirtualAddress() + inputOffset,
        addresses.data(), bindingCount);
}

}
