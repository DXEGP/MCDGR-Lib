module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include "mcdr_oit_budget.h"
#include <algorithm>
#include <array>
#include <cstring>

module mcdr.render.oit;

import mcdr.d3d12.device;
import mcdr.render.vanilla;

namespace d3d12_mc::oit {
namespace {
constexpr uint32_t normalizationLaneCount = 4;
constexpr uint32_t normalizationConstantsStride = 512;
constexpr uint32_t normalizationDescriptorStride = 8;
constexpr uint32_t normalizationScratchSize = 68;
constexpr uint32_t normalizationDescriptorCount = normalizationDescriptorStride * 5;
constexpr uint32_t normalizationResidentCapacity = 16;
constexpr uint32_t normalizationSliceCapacity = normalizationLaneCount * normalizationResidentCapacity;
constexpr DXGI_FORMAT normalizationFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

struct NormalizationFrame {
    std::shared_ptr<const void> recordingIdentity;
    mcdr_oit_normalization_batch_v1 input{};
    ComPtr<ID3D12Resource> normalized, corrected;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::vector<mcdr_oit_normalization_region_v1> regions;
    uint64_t batchSequence = 0;
    bool batchOpen = false;
    bool recordingFailed = false;
};

struct NormalizationPrograms {
    void* context = nullptr;
    DWORD ownerThread = 0;
    ComPtr<ID3D12Fence> contextFence;
    uint64_t generation = 0;
    uint32_t targetCount = 0;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> lane, average;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::weak_ptr<NormalizationFrame> frame;
};

struct NormalizationBatch {
    void* context = nullptr;
    DWORD ownerThread = 0;
    std::shared_ptr<const void> recordingIdentity;
    mcdr_oit_normalization_batch_v1 input{};
    std::shared_ptr<NormalizationPrograms> programs;
    std::shared_ptr<NormalizationFrame> frame;
    uint64_t sequence = 0;
    ComPtr<ID3D12Resource> constants;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::vector<std::shared_ptr<void>> inputLeases;
    uint32_t nextLane = 0;
    bool averageRecorded = false;
    bool recordingFailed = false;
};

std::mutex normalizationRegistryMutex;
std::atomic<uintptr_t> nextNormalizationHandle{1};
std::unordered_map<uintptr_t, std::shared_ptr<NormalizationPrograms>> normalizationPrograms;
std::unordered_map<uintptr_t, std::shared_ptr<NormalizationBatch>> normalizationBatches;

struct NormalizationResourceFailure {
    const char* operation;
    HRESULT result;
};

void requireNormalizationSuccess(HRESULT result, const char* operation) {
    if (FAILED(result)) throw NormalizationResourceFailure{operation, result};
}

int reportNormalizationFailure(CommandContext* context, const NormalizationResourceFailure& failure) {
    dbgLogError("OIT normalization operation=%s HRESULT=0x%08X", failure.operation,
        static_cast<unsigned int>(failure.result));
    const HRESULT deviceStatus = deviceContext().device
        ? deviceContext().device->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(deviceStatus) || failure.result == DXGI_ERROR_DEVICE_REMOVED ||
        failure.result == DXGI_ERROR_DEVICE_RESET || failure.result == DXGI_ERROR_DEVICE_HUNG ||
        failure.result == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        if (context) {
            context->submissionFailed.store(true, std::memory_order_release);
            vanilla::failRasterInputPass(context);
        }
        return MCDR_OIT_NORMALIZATION_SUBMISSION_UNKNOWN;
    }
    if (failure.result == E_OUTOFMEMORY) return MCDR_OIT_NORMALIZATION_BUDGET_EXCEEDED;
    if (failure.result == E_INVALIDARG) return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    if (failure.result == E_NOINTERFACE || failure.result == E_NOTIMPL || failure.result == DXGI_ERROR_UNSUPPORTED)
        return MCDR_OIT_NORMALIZATION_UNSUPPORTED;
    return MCDR_OIT_NORMALIZATION_BACKEND_FAILURE;
}

int normalizationRecordingStatus(CommandContext* context) {
    if (!context) return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    if (context->submissionFailed.load()) return MCDR_OIT_NORMALIZATION_SUBMISSION_UNKNOWN;
    if (context->recordingFailed.load()) return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
    if (!context->listOpen || !context->commandList || !context->recordingIdentity || context->inRenderPass)
        return MCDR_OIT_NORMALIZATION_BUSY;
    return MCDR_OIT_NORMALIZATION_SUCCESS;
}

std::shared_ptr<NormalizationPrograms> findNormalizationPrograms(void* handle) {
    std::lock_guard lock(normalizationRegistryMutex);
    const auto found = normalizationPrograms.find(reinterpret_cast<uintptr_t>(handle));
    return found == normalizationPrograms.end() ? nullptr : found->second;
}

std::shared_ptr<NormalizationBatch> findNormalizationBatch(void* handle) {
    std::lock_guard lock(normalizationRegistryMutex);
    const auto found = normalizationBatches.find(reinterpret_cast<uintptr_t>(handle));
    return found == normalizationBatches.end() ? nullptr : found->second;
}

int normalizationBatchStatus(CommandContext* context, const std::shared_ptr<NormalizationBatch>& batch) {
    if (const int status = normalizationRecordingStatus(context); status != MCDR_OIT_NORMALIZATION_SUCCESS)
        return status;
    if (!batch || batch->context != context || batch->ownerThread != GetCurrentThreadId() ||
        batch->recordingIdentity != context->recordingIdentity ||
        batch->input.frameSerial != context->fenceValue + 1 ||
        batch->programs->contextFence.Get() != context->fence.Get() ||
        batch->programs->device.Get() != deviceContext().device.Get() ||
        !vanilla::matchesFrozenFrame(context, &batch->input.identity, batch->input.historyGeneration))
        return MCDR_OIT_NORMALIZATION_STALE;
    return batch->recordingFailed || batch->frame->recordingFailed
        ? MCDR_OIT_NORMALIZATION_RECORDING_FAILED : MCDR_OIT_NORMALIZATION_SUCCESS;
}

D3D12_RESOURCE_DESC normalizationTextureDescription(uint32_t width, uint32_t height, uint32_t slices) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = static_cast<UINT16>(slices);
    description.MipLevels = 1;
    description.Format = normalizationFormat;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return description;
}

ComPtr<ID3D12Resource> createNormalizationTexture(ID3D12Device* device,
    const D3D12_RESOURCE_DESC& description) {
    ComPtr<ID3D12Resource> resource;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    requireNormalizationSuccess(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)), "CreateNormalizationTexture");
    return resource;
}

D3D12_CPU_DESCRIPTOR_HANDLE normalizationCpuDescriptor(const NormalizationBatch& batch, uint32_t index) {
    auto handle = batch.descriptors->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * deviceContext().srvInc;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE normalizationGpuDescriptor(const NormalizationBatch& batch, uint32_t index) {
    auto handle = batch.descriptors->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * deviceContext().srvInc;
    return handle;
}

void createNormalizationSourceDescriptor(NormalizationBatch& batch, uint32_t index,
    const mcdr_prepared_binding& binding) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = binding.object ? binding.object->dxgiFormat : normalizationFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MostDetailedMip = 0;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.FirstArraySlice = binding.firstSubresource;
    view.Texture2DArray.ArraySize = binding.object ? binding.subresourceCount : batch.input.residentCount;
    batch.programs->device->CreateShaderResourceView(binding.object ? binding.object->resource.Get() : nullptr,
        &view, normalizationCpuDescriptor(batch, index));
}

void createNormalizationBatchDescriptors(NormalizationBatch& batch) {
    auto* device = batch.programs->device.Get();
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = normalizationDescriptorCount;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    requireNormalizationSuccess(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&batch.descriptors)),
        "CreateNormalizationDescriptors");
    for (uint32_t group = 0; group <= normalizationLaneCount; ++group) {
        const uint32_t base = group * normalizationDescriptorStride;
        for (uint32_t source = 0; source < 5; ++source)
            createNormalizationSourceDescriptor(batch, base + source, {});
        D3D12_SHADER_RESOURCE_VIEW_DESC normalizedView{};
        normalizedView.Format = normalizationFormat;
        normalizedView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        normalizedView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        normalizedView.Texture2DArray.MipLevels = 1;
        normalizedView.Texture2DArray.ArraySize = normalizationSliceCapacity;
        device->CreateShaderResourceView(group == normalizationLaneCount ? batch.frame->normalized.Get() : nullptr,
            &normalizedView, normalizationCpuDescriptor(batch, base + 5));
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = normalizationFormat;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.ArraySize = normalizationSliceCapacity;
        device->CreateUnorderedAccessView(group < normalizationLaneCount ? batch.frame->normalized.Get() : nullptr,
            nullptr, &view,
            normalizationCpuDescriptor(batch, base + 6));
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D = {};
        device->CreateUnorderedAccessView(group == normalizationLaneCount ? batch.frame->corrected.Get() : nullptr,
            nullptr, &view,
            normalizationCpuDescriptor(batch, base + 7));
    }
}

void createNormalizationConstants(NormalizationBatch& batch) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = normalizationConstantsStride * 5;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    requireNormalizationSuccess(batch.programs->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&batch.constants)),
        "CreateNormalizationConstants");
    struct Constants {
        uint32_t schema, resident, lane, flags;
        uint32_t width, height, reserved[2];
        mcdr_oit_normalization_region_v1 regions[16];
    } constants{};
    static_assert(sizeof(constants) == 288);
    constants.schema = 1;
    constants.resident = batch.input.residentCount;
    constants.width = batch.input.outputWidth;
    constants.height = batch.input.outputHeight;
    std::memcpy(constants.regions, batch.input.regions, sizeof(constants.regions));
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    requireNormalizationSuccess(batch.constants->Map(0, &noRead, &mapped), "MapNormalizationConstants");
    auto* bytes = static_cast<std::byte*>(mapped);
    std::memset(bytes, 0, static_cast<size_t>(description.Width));
    for (uint32_t lane = 0; lane <= normalizationLaneCount; ++lane) {
        constants.lane = lane == normalizationLaneCount ? 0 : lane;
        std::memcpy(bytes + lane * normalizationConstantsStride, &constants, sizeof(constants));
    }
    const D3D12_RANGE written{0, static_cast<SIZE_T>(description.Width)};
    batch.constants->Unmap(0, &written);
}

bool validNormalizationSource(const NormalizationBatch& batch, const mcdr_prepared_binding& binding) {
    const auto* object = binding.object;
    if (!object || object->kind != Dx12Object::Kind::Texture || !object->resource ||
        binding.subresourceCount != batch.input.residentCount ||
        (object->dxgiFormat != DXGI_FORMAT_R16G16B16A16_FLOAT && object->dxgiFormat != normalizationFormat))
        return false;
    const auto description = object->resource->GetDesc();
    ComPtr<ID3D12Device> device;
    if (FAILED(object->resource->GetDevice(IID_PPV_ARGS(&device))) || device.Get() != batch.programs->device.Get())
        return false;
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.Width == normalizationScratchSize && description.Height == normalizationScratchSize &&
        description.MipLevels == 1 && description.SampleDesc.Count == 1 &&
        !(description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
        binding.firstSubresource <= description.DepthOrArraySize &&
        binding.subresourceCount <= description.DepthOrArraySize - binding.firstSubresource &&
        object->resource.Get() != batch.frame->normalized.Get() &&
        object->resource.Get() != batch.frame->corrected.Get();
}

void bindNormalizationCompute(CommandContext* context, NormalizationBatch& batch, uint32_t group) {
    ID3D12DescriptorHeap* heaps[] = {batch.descriptors.Get()};
    context->commandList->SetDescriptorHeaps(1, heaps);
    context->commandList->SetComputeRootSignature(batch.programs->root.Get());
    context->commandList->SetComputeRootConstantBufferView(0,
        batch.constants->GetGPUVirtualAddress() + group * normalizationConstantsStride);
    context->commandList->SetComputeRootDescriptorTable(1,
        normalizationGpuDescriptor(batch, group * normalizationDescriptorStride));
    context->commandList->SetComputeRootDescriptorTable(2,
        normalizationGpuDescriptor(batch, group * normalizationDescriptorStride + 6));
}

struct NormalizationRecording {
    CommandContext* context;
    NormalizationBatch& batch;
    bool completed = false;
    ~NormalizationRecording() {
        if (!completed) {
            batch.recordingFailed = true;
            batch.frame->recordingFailed = true;
            context->recordingFailed.store(true, std::memory_order_release);
            vanilla::failRasterInputPass(context);
        }
        context->cachedPso = nullptr;
        restoreDrawHeap(context);
    }
};

}

int createNormalizationPrograms(void* opaqueContext, const mcdr_oit_normalization_program_v1& input,
    void** output) try {
    if (output) *output = nullptr;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* device = deviceContext().device.Get();
    if (!output || !context || !context->fence || !device || input.schemaVersion != 1 ||
        input.structSize != sizeof(input) || input.reserved != 0 || input.programGeneration == 0 ||
        (input.targetCount != 1 && input.targetCount != 2 && input.targetCount != 4) ||
        !input.laneDxil || !input.averageDxil || input.laneDxilBytes == 0 || input.averageDxilBytes == 0 ||
        input.laneDxilBytes > mcdr::oit_budget::limit || input.averageDxilBytes > mcdr::oit_budget::limit)
        return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    if (context->submissionFailed.load()) return MCDR_OIT_NORMALIZATION_SUBMISSION_UNKNOWN;
    auto programs = std::make_shared<NormalizationPrograms>();
    programs->context = context;
    programs->ownerThread = GetCurrentThreadId();
    programs->contextFence = context->fence;
    programs->device = device;
    programs->generation = input.programGeneration;
    programs->targetCount = input.targetCount;
    programs->budget = mcdr::oit_budget::reserve(4ull * 1024 * 1024 + input.laneDxilBytes + input.averageDxilBytes);
    if (!programs->budget) return MCDR_OIT_NORMALIZATION_BUDGET_EXCEEDED;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format{normalizationFormat};
    requireNormalizationSuccess(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format)),
        "CheckNormalizationFormat");
    if (!(format.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
        !(format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return MCDR_OIT_NORMALIZATION_UNSUPPORTED;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 6;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    D3D12_ROOT_PARAMETER roots[3]{};
    roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (uint32_t index = 0; index < 2; ++index) {
        roots[index + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[index + 1].DescriptorTable = {1, &ranges[index]};
    }
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 3;
    description.pParameters = roots;
    ComPtr<ID3DBlob> serialized, diagnostic;
    requireNormalizationSuccess(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &diagnostic), "SerializeNormalizationRoot");
    requireNormalizationSuccess(device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&programs->root)), "CreateNormalizationRoot");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = programs->root.Get();
    pipeline.CS = {input.laneDxil, static_cast<SIZE_T>(input.laneDxilBytes)};
    requireNormalizationSuccess(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&programs->lane)),
        "CreateNormalizationLanePipeline");
    pipeline.CS = {input.averageDxil, static_cast<SIZE_T>(input.averageDxilBytes)};
    requireNormalizationSuccess(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&programs->average)),
        "CreateNormalizationAveragePipeline");
    const uintptr_t handle = nextNormalizationHandle.fetch_add(1);
    if (handle == 0) return MCDR_OIT_NORMALIZATION_UNSUPPORTED;
    std::lock_guard lock(normalizationRegistryMutex);
    normalizationPrograms.emplace(handle, std::move(programs));
    *output = reinterpret_cast<void*>(handle);
    return MCDR_OIT_NORMALIZATION_SUCCESS;
} catch (const NormalizationResourceFailure& failure) {
    return reportNormalizationFailure(static_cast<CommandContext*>(opaqueContext), failure);
}

void destroyNormalizationPrograms(void* handle) noexcept {
    std::lock_guard lock(normalizationRegistryMutex);
    normalizationPrograms.erase(reinterpret_cast<uintptr_t>(handle));
}

int createNormalizationBatch(void* opaqueContext, void* programsHandle,
    const mcdr_oit_normalization_batch_v1& input, void** output) try {
    if (output) *output = nullptr;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (const int status = normalizationRecordingStatus(context); status != MCDR_OIT_NORMALIZATION_SUCCESS)
        return status;
    auto programs = findNormalizationPrograms(programsHandle);
    if (!programs || programs->context != context || programs->contextFence.Get() != context->fence.Get() ||
        programs->ownerThread != GetCurrentThreadId() || programs->device.Get() != deviceContext().device.Get())
        return MCDR_OIT_NORMALIZATION_STALE;
    if (!output || input.schemaVersion != 1 || input.structSize != sizeof(input) || input.reserved != 0 ||
        input.residentCount == 0 || input.residentCount > 16 || input.outputWidth == 0 || input.outputHeight == 0 ||
        input.outputWidth > 16384 || input.outputHeight > 16384 || input.identity.worldEpoch == 0 ||
        input.identity.realFrameId == 0 || input.identity.configVersion == 0 || input.identity.resourceVersion == 0 ||
        input.historyGeneration == 0 || input.programGeneration != programs->generation ||
        input.totalBudgetBytes == 0 || input.totalBudgetBytes > mcdr::oit_budget::limit)
        return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    if (input.frameSerial != context->fenceValue + 1 ||
        !vanilla::matchesFrozenFrame(context, &input.identity, input.historyGeneration))
        return MCDR_OIT_NORMALIZATION_STALE;
    for (uint32_t index = 0; index < input.residentCount; ++index) {
        const auto& region = input.regions[index];
        if (region.width == 0 || region.height == 0 || region.width > 64 || region.height > 64 ||
            region.width > input.outputWidth || region.height > input.outputHeight ||
            region.originX > input.outputWidth - region.width || region.originY > input.outputHeight - region.height ||
            (region.originX & 1u) != 0 || (region.originY & 1u) != 0)
            return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
        for (uint32_t priorIndex = 0; priorIndex < index; ++priorIndex) {
            const auto& prior = input.regions[priorIndex];
            if (region.originX < prior.originX + prior.width && prior.originX < region.originX + region.width &&
                region.originY < prior.originY + prior.height && prior.originY < region.originY + region.height)
                return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
        }
    }
    auto frame = programs->frame.lock();
    if (frame && frame->recordingIdentity == context->recordingIdentity) {
        const auto& prior = frame->input;
        if (frame->recordingFailed) return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
        if (frame->batchOpen) return MCDR_OIT_NORMALIZATION_BUSY;
        if (prior.identity.worldEpoch != input.identity.worldEpoch ||
            prior.identity.realFrameId != input.identity.realFrameId ||
            prior.identity.configVersion != input.identity.configVersion ||
            prior.identity.resourceVersion != input.identity.resourceVersion ||
            prior.frameSerial != input.frameSerial || prior.historyGeneration != input.historyGeneration ||
            prior.programGeneration != input.programGeneration || prior.outputWidth != input.outputWidth ||
            prior.outputHeight != input.outputHeight || prior.totalBudgetBytes != input.totalBudgetBytes)
            return MCDR_OIT_NORMALIZATION_STALE;
        for (uint32_t index = 0; index < input.residentCount; ++index) {
            const auto& region = input.regions[index];
            for (const auto& priorRegion : frame->regions) {
                if (region.originX < priorRegion.originX + priorRegion.width &&
                    priorRegion.originX < region.originX + region.width &&
                    region.originY < priorRegion.originY + priorRegion.height &&
                    priorRegion.originY < region.originY + region.height)
                    return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
            }
        }
    } else {
        frame = std::make_shared<NormalizationFrame>();
        frame->recordingIdentity = context->recordingIdentity;
        frame->input = input;
        const auto normalized = normalizationTextureDescription(normalizationScratchSize, normalizationScratchSize,
            normalizationSliceCapacity);
        const auto corrected = normalizationTextureDescription(input.outputWidth, input.outputHeight, 1);
        const auto normalizedAllocation = programs->device->GetResourceAllocationInfo(0, 1, &normalized);
        const auto correctedAllocation = programs->device->GetResourceAllocationInfo(0, 1, &corrected);
        if (normalizedAllocation.SizeInBytes == UINT64_MAX || correctedAllocation.SizeInBytes == UINT64_MAX ||
            normalizedAllocation.SizeInBytes > mcdr::oit_budget::limit ||
            correctedAllocation.SizeInBytes > mcdr::oit_budget::limit)
            return MCDR_OIT_NORMALIZATION_UNSUPPORTED;
        frame->budget = mcdr::oit_budget::reserve(normalizedAllocation.SizeInBytes + correctedAllocation.SizeInBytes,
            input.totalBudgetBytes);
        if (!frame->budget) return MCDR_OIT_NORMALIZATION_BUDGET_EXCEEDED;
        frame->normalized = createNormalizationTexture(programs->device.Get(), normalized);
        frame->corrected = createNormalizationTexture(programs->device.Get(), corrected);
    }
    auto batch = std::make_shared<NormalizationBatch>();
    batch->context = context;
    batch->ownerThread = GetCurrentThreadId();
    batch->recordingIdentity = context->recordingIdentity;
    batch->input = input;
    batch->programs = programs;
    batch->frame = frame;
    batch->budget = mcdr::oit_budget::reserve(131072, input.totalBudgetBytes);
    if (!batch->budget) return MCDR_OIT_NORMALIZATION_BUDGET_EXCEEDED;
    createNormalizationConstants(*batch);
    createNormalizationBatchDescriptors(*batch);
    batch->inputLeases.reserve(normalizationLaneCount * (1 + batch->programs->targetCount));
    const size_t regionCount = frame->regions.size() + input.residentCount;
    if (regionCount > frame->regions.capacity())
        frame->regions.reserve(std::max(regionCount, frame->regions.capacity() * 2));
    const uintptr_t handle = nextNormalizationHandle.fetch_add(1);
    if (handle == 0) return MCDR_OIT_NORMALIZATION_UNSUPPORTED;
    context->recordingLeases[context->currentListSlot].push_back(batch);
    std::lock_guard lock(normalizationRegistryMutex);
    normalizationBatches.emplace(handle, batch);
    frame->regions.insert(frame->regions.end(), input.regions, input.regions + input.residentCount);
    batch->sequence = ++frame->batchSequence;
    frame->batchOpen = true;
    programs->frame = frame;
    *output = reinterpret_cast<void*>(handle);
    return MCDR_OIT_NORMALIZATION_SUCCESS;
} catch (const NormalizationResourceFailure& failure) {
    return reportNormalizationFailure(static_cast<CommandContext*>(opaqueContext), failure);
}

int recordNormalizationLane(void* opaqueContext, void* handle, const mcdr_oit_normalization_lane_v1& input) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto batch = findNormalizationBatch(handle);
    if (const int status = normalizationBatchStatus(context, batch); status != MCDR_OIT_NORMALIZATION_SUCCESS)
        return status;
    if (input.schemaVersion != 1 || input.structSize != sizeof(input) || input.reserved != 0 ||
        input.laneIndex >= normalizationLaneCount || input.laneIndex != batch->nextLane || batch->averageRecorded ||
        !batch->frame->batchOpen || batch->sequence != batch->frame->batchSequence ||
        !validNormalizationSource(*batch, input.q))
        return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    std::vector<mcdr_prepared_binding> inputs{input.q};
    for (uint32_t index = 0; index < 4; ++index) {
        if (index < batch->programs->targetCount) {
            if (!validNormalizationSource(*batch, input.coefficients[index]))
                return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
            inputs.push_back(input.coefficients[index]);
        } else if (input.coefficients[index].object || input.coefficients[index].firstSubresource ||
            input.coefficients[index].subresourceCount) {
            return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
        }
    }
    std::vector<vanilla::PassResourceUse> uses;
    uses.reserve(inputs.size() + 1);
    std::vector<std::shared_ptr<void>> leases;
    leases.reserve(inputs.size());
    for (const auto& source : inputs) {
        auto lease = retainObjectLease(source.object);
        if (!lease) return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
        leases.push_back(std::move(lease));
        uses.push_back({source.object, batch->input.identity.resourceVersion, source.firstSubresource,
            source.subresourceCount, vanilla::ResourceAccess::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
    }
    uses.push_back({nullptr, batch->input.identity.resourceVersion, 0, normalizationSliceCapacity,
        vanilla::ResourceAccess::Uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, UINT32_MAX,
        vanilla::ExternalResourceState::Tracked, batch->frame->normalized.Get(), D3D12_RESOURCE_STATE_COMMON});
    for (auto& lease : leases) batch->inputLeases.push_back(std::move(lease));
    for (uint32_t index = 0; index < inputs.size(); ++index)
        createNormalizationSourceDescriptor(*batch, input.laneIndex * normalizationDescriptorStride + index,
            inputs[index]);
    NormalizationRecording recording{context, *batch};
    try {
        vanilla::DeclaredPassToken* token = nullptr;
        if (vanilla::beginDeclaredPass(context, 0x240 + input.laneIndex, uses.data(),
                static_cast<uint32_t>(uses.size()), 3, &token) != 1)
            return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
        bindNormalizationCompute(context, *batch, input.laneIndex);
        context->commandList->SetPipelineState(batch->programs->lane.Get());
        context->commandList->Dispatch(9, 9, batch->input.residentCount);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = batch->frame->normalized.Get();
        context->commandList->ResourceBarrier(1, &barrier);
        if (vanilla::finishDeclaredPass(context, token) != 1)
            return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
        ++batch->nextLane;
        recording.completed = true;
        return MCDR_OIT_NORMALIZATION_RECORDED_UNVERIFIED;
    } catch (const std::exception& failure) {
        dbgLogError("OIT normalization lane=%u recording failed: %s", input.laneIndex, failure.what());
        return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
    } catch (...) {
        dbgLogError("OIT normalization lane=%u recording failed with an unknown exception", input.laneIndex);
        return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
    }
}

int recordNormalizationAverage(void* opaqueContext, void* handle) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto batch = findNormalizationBatch(handle);
    if (const int status = normalizationBatchStatus(context, batch); status != MCDR_OIT_NORMALIZATION_SUCCESS)
        return status;
    if (batch->nextLane != normalizationLaneCount || batch->averageRecorded || !batch->frame->batchOpen ||
        batch->sequence != batch->frame->batchSequence)
        return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    const vanilla::PassResourceUse uses[] = {
        {nullptr, batch->input.identity.resourceVersion, 0, normalizationSliceCapacity, vanilla::ResourceAccess::Read,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, UINT32_MAX, vanilla::ExternalResourceState::Tracked,
            batch->frame->normalized.Get(), D3D12_RESOURCE_STATE_COMMON},
        {nullptr, batch->input.identity.resourceVersion, 0, 1, vanilla::ResourceAccess::Uav,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, UINT32_MAX, vanilla::ExternalResourceState::Tracked,
            batch->frame->corrected.Get(), D3D12_RESOURCE_STATE_COMMON},
    };
    NormalizationRecording recording{context, *batch};
    try {
        vanilla::DeclaredPassToken* token = nullptr;
        if (vanilla::beginDeclaredPass(context, 0x244, uses, 2, 3, &token) != 1)
            return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
        bindNormalizationCompute(context, *batch, normalizationLaneCount);
        context->commandList->SetPipelineState(batch->programs->average.Get());
        context->commandList->Dispatch(8, 8, batch->input.residentCount);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = batch->frame->corrected.Get();
        context->commandList->ResourceBarrier(1, &barrier);
        if (vanilla::finishDeclaredPass(context, token) != 1)
            return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
        batch->averageRecorded = true;
        batch->frame->batchOpen = false;
        recording.completed = true;
        return MCDR_OIT_NORMALIZATION_RECORDED_UNVERIFIED;
    } catch (const std::exception& failure) {
        dbgLogError("OIT normalization average recording failed: %s", failure.what());
        return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
    } catch (...) {
        dbgLogError("OIT normalization average recording failed with an unknown exception");
        return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
    }
}

int queryNormalizationBatch(void* opaqueContext, void* handle, mcdr_oit_normalization_info_v1* output) {
    if (output) *output = {};
    if (!output) return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto batch = findNormalizationBatch(handle);
    if (const int status = normalizationBatchStatus(context, batch); status != MCDR_OIT_NORMALIZATION_SUCCESS)
        return status;
    output->schemaVersion = 1;
    output->structSize = sizeof(*output);
    output->recordedLaneMask = (1u << batch->nextLane) - 1u;
    output->averageRecorded = batch->averageRecorded ? 1u : 0u;
    output->availability = MCDR_OIT_NORMALIZATION_UNAVAILABLE;
    output->unavailableReason = MCDR_OIT_NORMALIZATION_UNAVAILABLE_STAGE_EXECUTION;
    output->residentCount = batch->input.residentCount;
    output->targetCount = batch->programs->targetCount;
    output->frameSerial = batch->input.frameSerial;
    output->programGeneration = batch->programs->generation;
    return MCDR_OIT_NORMALIZATION_SUCCESS;
}

void destroyNormalizationBatch(void* handle) noexcept {
    std::lock_guard lock(normalizationRegistryMutex);
    normalizationBatches.erase(reinterpret_cast<uintptr_t>(handle));
}

}
