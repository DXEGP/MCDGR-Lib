module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include "mcdr_oit_budget.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <span>

module mcdr.render.oit;

import mcdr.d3d12.device;
import mcdr.render.vanilla;

namespace d3d12_mc::oit {
namespace {
constexpr uint32_t normalizationLaneCount = 4;
constexpr uint32_t normalizationConstantsStride = 512;
constexpr uint32_t normalizationDescriptorStride = 8;
constexpr uint32_t normalizationScratchSize = 68;
constexpr uint32_t normalizationDispatchCount = normalizationLaneCount + 1;
constexpr uint32_t normalizationBatchBytes = normalizationConstantsStride * normalizationDispatchCount;
constexpr uint32_t normalizationPageBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
constexpr uint32_t normalizationPageDescriptors = 512;
constexpr uint32_t normalizationResidentCapacity = 16;
constexpr uint32_t normalizationSliceCapacity = normalizationLaneCount * normalizationResidentCapacity;
constexpr DXGI_FORMAT normalizationFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
static_assert(normalizationConstantsStride % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);

struct NormalizationUploadPage {
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    ComPtr<ID3D12Resource> resource;
    std::byte* mapped = nullptr;
    uint32_t usedBytes = 0;
};

struct NormalizationDescriptorPage {
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    ComPtr<ID3D12DescriptorHeap> heap;
    uint32_t usedDescriptors = 0;
};

struct NormalizationSourceView {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = normalizationFormat;
    uint32_t firstSlice = 0;
    uint32_t sliceCount = 0;

    bool operator==(const NormalizationSourceView&) const = default;
};

struct NormalizationDescriptorTable {
    std::array<NormalizationSourceView, 5> sources{};
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
};

struct NormalizationFrame {
    std::shared_ptr<const void> recordingIdentity;
    mcdr_oit_normalization_batch_v1 input{};
    ComPtr<ID3D12Resource> normalized, corrected;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::vector<mcdr_oit_normalization_region_v1> regions;
    std::vector<NormalizationUploadPage> uploadPages;
    std::vector<NormalizationDescriptorPage> descriptorPages;
    std::array<NormalizationDescriptorTable, normalizationDispatchCount> recentTables;
    uint64_t batchSequence = 0;
    uint64_t averagedPixels = 0;
    uint32_t targetCount = 0;
    bool batchOpen = false;
    bool recordingFailed = false;
};

struct NormalizationPrograms {
    void* context = nullptr;
    DWORD ownerThread = 0;
    ComPtr<ID3D12Fence> contextFence;
    uint64_t generation = 0;
    uint32_t legacyTargetCount = 0;
    ComPtr<ID3D12Device> device;
    ComPtr<IUnknown> deviceIdentity;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 5> lane;
    ComPtr<ID3D12PipelineState> average;
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
    D3D12_GPU_VIRTUAL_ADDRESS constants = 0;
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

int writeNormalizationBatchConstants(NormalizationBatch& batch) {
    auto& pages = batch.frame->uploadPages;
    if (pages.empty() || pages.back().usedBytes > normalizationPageBytes - normalizationBatchBytes) {
        NormalizationUploadPage page{};
        page.budget = mcdr::oit_budget::reserve(normalizationPageBytes, batch.input.totalBudgetBytes);
        if (!page.budget) return MCDR_OIT_NORMALIZATION_BUDGET_EXCEEDED;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = normalizationPageBytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        requireNormalizationSuccess(batch.programs->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &description, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&page.resource)),
            "CreateNormalizationUploadPage");
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        requireNormalizationSuccess(page.resource->Map(0, &noRead, &mapped), "MapNormalizationUploadPage");
        page.mapped = static_cast<std::byte*>(mapped);
        pages.push_back(std::move(page));
    }
    struct Constants {
        uint32_t schema, resident, lane, flags;
        uint32_t width, height, reserved[2];
        mcdr_oit_normalization_region_v1 regions[normalizationResidentCapacity];
    } constants{};
    static_assert(sizeof(constants) == 288);
    constants.schema = 1;
    constants.resident = batch.input.residentCount;
    constants.width = batch.input.outputWidth;
    constants.height = batch.input.outputHeight;
    std::memcpy(constants.regions, batch.input.regions, sizeof(constants.regions));

    auto& page = pages.back();
    auto* bytes = page.mapped + page.usedBytes;
    std::memset(bytes, 0, normalizationBatchBytes);
    for (uint32_t group = 0; group < normalizationDispatchCount; ++group) {
        constants.lane = group == normalizationLaneCount ? 0 : group;
        std::memcpy(bytes + group * normalizationConstantsStride, &constants, sizeof(constants));
    }
    batch.constants = page.resource->GetGPUVirtualAddress() + page.usedBytes;
    return MCDR_OIT_NORMALIZATION_SUCCESS;
}

int ensureNormalizationDescriptorSpace(NormalizationBatch& batch) {
    auto& pages = batch.frame->descriptorPages;
    constexpr uint32_t maxBatchDescriptors = normalizationDispatchCount * normalizationDescriptorStride;
    if (!pages.empty() && pages.back().usedDescriptors <= normalizationPageDescriptors - maxBatchDescriptors)
        return MCDR_OIT_NORMALIZATION_SUCCESS;

    NormalizationDescriptorPage page{};
    page.budget = mcdr::oit_budget::reserve(normalizationPageBytes, batch.input.totalBudgetBytes);
    if (!page.budget) return MCDR_OIT_NORMALIZATION_BUDGET_EXCEEDED;
    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    description.NumDescriptors = normalizationPageDescriptors;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    requireNormalizationSuccess(batch.programs->device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&page.heap)),
        "CreateNormalizationDescriptorPage");
    pages.push_back(std::move(page));
    return MCDR_OIT_NORMALIZATION_SUCCESS;
}

NormalizationDescriptorTable normalizationTable(NormalizationBatch& batch, uint32_t group,
    std::span<const mcdr_prepared_binding> inputs) {
    auto& frame = *batch.frame;
    const bool average = group == normalizationLaneCount;
    std::array<NormalizationSourceView, 5> sources{};
    for (auto& source : sources)
        source.sliceCount = batch.input.residentCount;
    for (size_t index = 0; index < inputs.size(); ++index) {
        const auto& binding = inputs[index];
        sources[index] = {binding.object->resource.Get(), binding.object->dxgiFormat,
            binding.firstSubresource, binding.subresourceCount};
    }

    // A lane table and the average table have different SRV/UAV layouts.
    auto& recent = frame.recentTables[group];
    auto candidates = std::span(frame.recentTables);
    const auto matchingLayout = average ? candidates.last(1) : candidates.first(normalizationLaneCount);
    for (const auto& table : matchingLayout) {
        if (table.heap && table.sources == sources) {
            recent = table;
            return recent;
        }
    }

    // Batch creation leaves space for all five dispatches before recording can begin.
    auto& page = frame.descriptorPages.back();
    auto* device = batch.programs->device.Get();
    const auto increment = deviceContext().srvInc;
    const auto first = page.usedDescriptors;
    const auto cpu = page.heap->GetCPUDescriptorHandleForHeapStart();
    const auto cpuDescriptor = [&](uint32_t index) {
        return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + static_cast<SIZE_T>(first + index) * increment};
    };
    for (uint32_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = source.format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2DArray.MipLevels = 1;
        view.Texture2DArray.FirstArraySlice = source.firstSlice;
        view.Texture2DArray.ArraySize = source.sliceCount;
        device->CreateShaderResourceView(source.resource, &view, cpuDescriptor(index));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC normalizedView{};
    normalizedView.Format = normalizationFormat;
    normalizedView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    normalizedView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    normalizedView.Texture2DArray.MipLevels = 1;
    normalizedView.Texture2DArray.ArraySize = normalizationSliceCapacity;
    device->CreateShaderResourceView(average ? frame.normalized.Get() : nullptr,
        &normalizedView, cpuDescriptor(5));

    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = normalizationFormat;
    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.ArraySize = normalizationSliceCapacity;
    device->CreateUnorderedAccessView(average ? nullptr : frame.normalized.Get(), nullptr, &view, cpuDescriptor(6));
    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    view.Texture2D = {};
    device->CreateUnorderedAccessView(average ? frame.corrected.Get() : nullptr, nullptr, &view, cpuDescriptor(7));

    page.usedDescriptors += normalizationDescriptorStride;
    auto gpu = page.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(first) * increment;
    recent = {sources, page.heap.Get(), gpu};
    return recent;
}

bool validNormalizationSource(const NormalizationBatch& batch, const mcdr_prepared_binding& binding) {
    const auto* object = binding.object;
    if (!object || object->kind != Dx12Object::Kind::Texture || !object->resource ||
        binding.subresourceCount != batch.input.residentCount ||
        (object->dxgiFormat != DXGI_FORMAT_R16G16B16A16_FLOAT && object->dxgiFormat != normalizationFormat))
        return false;
    const auto description = object->resource->GetDesc();
    ComPtr<ID3D12Device> device;
    ComPtr<IUnknown> deviceIdentity;
    if (FAILED(object->resource->GetDevice(IID_PPV_ARGS(&device))) || FAILED(device.As(&deviceIdentity)) ||
        deviceIdentity.Get() != batch.programs->deviceIdentity.Get())
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

void bindNormalizationCompute(CommandContext* context, const NormalizationBatch& batch, uint32_t group,
    const NormalizationDescriptorTable& table) {
    ID3D12DescriptorHeap* heaps[] = {table.heap};
    context->commandList->SetDescriptorHeaps(1, heaps);
    context->commandList->SetComputeRootSignature(batch.programs->root.Get());
    context->commandList->SetComputeRootConstantBufferView(0,
        batch.constants + group * normalizationConstantsStride);
    context->commandList->SetComputeRootDescriptorTable(1, table.gpu);
    context->commandList->SetComputeRootDescriptorTable(2,
        {table.gpu.ptr + 6ull * deviceContext().srvInc});
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

int buildNormalizationPrograms(CommandContext* context, uint64_t generation,
    std::span<const mcdr_oit_normalization_program_v1> inputs, std::shared_ptr<NormalizationPrograms>& output) {
    auto* device = deviceContext().device.Get();
    const auto& first = inputs.front();
    if (!context || !context->fence || !device || !generation || !first.averageDxil ||
        !first.averageDxilBytes || first.averageDxilBytes > mcdr::oit_budget::limit)
        return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    uint64_t programBytes = (inputs.size() + 1) * (2ull * 1024 * 1024) + first.averageDxilBytes;
    uint32_t targetMask = 0;
    for (const auto& input : inputs) {
        if (input.schemaVersion != 1 || input.structSize != sizeof(input) || input.reserved ||
            input.programGeneration != generation || (input.targetCount != 1 && input.targetCount != 2 && input.targetCount != 4) ||
            !input.laneDxil || !input.laneDxilBytes || input.laneDxilBytes > mcdr::oit_budget::limit ||
            input.averageDxil != first.averageDxil || input.averageDxilBytes != first.averageDxilBytes ||
            (targetMask & input.targetCount)) return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
        targetMask |= input.targetCount;
        programBytes += input.laneDxilBytes;
    }
    if (context->submissionFailed.load()) return MCDR_OIT_NORMALIZATION_SUBMISSION_UNKNOWN;
    auto programs = std::make_shared<NormalizationPrograms>();
    programs->context = context;
    programs->ownerThread = GetCurrentThreadId();
    programs->contextFence = context->fence;
    programs->device = device;
    programs->generation = generation;
    programs->budget = mcdr::oit_budget::reserve(programBytes);
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
    for (const auto& input : inputs) {
        pipeline.CS = {input.laneDxil, static_cast<SIZE_T>(input.laneDxilBytes)};
        requireNormalizationSuccess(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&programs->lane[input.targetCount])),
            "CreateNormalizationLanePipeline");
    }
    pipeline.CS = {first.averageDxil, static_cast<SIZE_T>(first.averageDxilBytes)};
    requireNormalizationSuccess(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&programs->average)),
        "CreateNormalizationAveragePipeline");
    // Device children can report the underlying device when creation uses an interposer.
    ComPtr<ID3D12Device> pipelineDevice;
    requireNormalizationSuccess(programs->average->GetDevice(IID_PPV_ARGS(&pipelineDevice)), "QueryNormalizationPipelineDevice");
    requireNormalizationSuccess(pipelineDevice.As(&programs->deviceIdentity), "QueryNormalizationDeviceIdentity");
    output = std::move(programs);
    return MCDR_OIT_NORMALIZATION_SUCCESS;
}

}

int prepareNormalizationPrograms(void* opaqueContext, uint64_t generation,
    const mcdr_oit_normalization_program_v1 (&inputs)[3], std::shared_ptr<void>* output) try {
    if (!output) return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    output->reset();
    std::shared_ptr<NormalizationPrograms> programs;
    const auto status = buildNormalizationPrograms(static_cast<CommandContext*>(opaqueContext), generation, inputs, programs);
    if (status == MCDR_OIT_NORMALIZATION_SUCCESS) *output = std::move(programs);
    return status;
} catch (const NormalizationResourceFailure& failure) {
    return reportNormalizationFailure(static_cast<CommandContext*>(opaqueContext), failure);
} catch (const std::bad_alloc&) {
    return MCDR_OIT_NORMALIZATION_BUDGET_EXCEEDED;
}

int createNormalizationPrograms(void* opaqueContext, const mcdr_oit_normalization_program_v1& input,
    void** output) try {
    if (!output) return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    *output = nullptr;
    std::shared_ptr<NormalizationPrograms> programs;
    const auto status = buildNormalizationPrograms(static_cast<CommandContext*>(opaqueContext), input.programGeneration,
        std::span{&input, 1}, programs);
    if (status != MCDR_OIT_NORMALIZATION_SUCCESS) return status;
    programs->legacyTargetCount = input.targetCount;
    const uintptr_t handle = nextNormalizationHandle.fetch_add(1);
    if (handle == 0) return MCDR_OIT_NORMALIZATION_UNSUPPORTED;
    std::lock_guard lock(normalizationRegistryMutex);
    normalizationPrograms.emplace(handle, std::move(programs));
    *output = reinterpret_cast<void*>(handle);
    return MCDR_OIT_NORMALIZATION_SUCCESS;
} catch (const NormalizationResourceFailure& failure) {
    return reportNormalizationFailure(static_cast<CommandContext*>(opaqueContext), failure);
} catch (const std::bad_alloc&) {
    return MCDR_OIT_NORMALIZATION_BUDGET_EXCEEDED;
}

void destroyNormalizationPrograms(void* handle) noexcept {
    std::lock_guard lock(normalizationRegistryMutex);
    normalizationPrograms.erase(reinterpret_cast<uintptr_t>(handle));
}

namespace {
int createNormalizationBatch(void* opaqueContext, const std::shared_ptr<NormalizationPrograms>& programs,
    uint32_t targetCount, const mcdr_oit_normalization_batch_v1& input, void** output) try {
    if (output) *output = nullptr;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (const int status = normalizationRecordingStatus(context); status != MCDR_OIT_NORMALIZATION_SUCCESS)
        return status;
    if (!programs || programs->context != context || programs->contextFence.Get() != context->fence.Get() ||
        programs->ownerThread != GetCurrentThreadId() || programs->device.Get() != deviceContext().device.Get())
        return MCDR_OIT_NORMALIZATION_STALE;
    if (targetCount >= programs->lane.size() || !programs->lane[targetCount])
        return MCDR_OIT_NORMALIZATION_UNSUPPORTED;
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
            prior.outputHeight != input.outputHeight || prior.totalBudgetBytes != input.totalBudgetBytes ||
            frame->targetCount != targetCount)
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
        frame->targetCount = targetCount;
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
    if (const auto status = ensureNormalizationDescriptorSpace(*batch); status != MCDR_OIT_NORMALIZATION_SUCCESS)
        return status;
    if (const auto status = writeNormalizationBatchConstants(*batch); status != MCDR_OIT_NORMALIZATION_SUCCESS)
        return status;
    batch->inputLeases.reserve(normalizationLaneCount * (1 + targetCount));
    const size_t regionCount = frame->regions.size() + input.residentCount;
    if (regionCount > frame->regions.capacity())
        frame->regions.reserve(std::max(regionCount, frame->regions.capacity() * 2));
    const uintptr_t handle = nextNormalizationHandle.fetch_add(1);
    if (handle == 0) return MCDR_OIT_NORMALIZATION_UNSUPPORTED;
    context->recordingLeases[context->currentListSlot].push_back(batch);
    std::lock_guard lock(normalizationRegistryMutex);
    normalizationBatches.emplace(handle, batch);
    frame->regions.insert(frame->regions.end(), input.regions, input.regions + input.residentCount);
    frame->uploadPages.back().usedBytes += normalizationBatchBytes;
    batch->sequence = ++frame->batchSequence;
    frame->batchOpen = true;
    programs->frame = frame;
    *output = reinterpret_cast<void*>(handle);
    return MCDR_OIT_NORMALIZATION_SUCCESS;
} catch (const NormalizationResourceFailure& failure) {
    return reportNormalizationFailure(static_cast<CommandContext*>(opaqueContext), failure);
} catch (const std::bad_alloc&) {
    return MCDR_OIT_NORMALIZATION_BUDGET_EXCEEDED;
}
}

int createPreparedNormalizationBatch(void* opaqueContext, const std::shared_ptr<void>& programs,
    uint32_t targetCount, const mcdr_oit_normalization_batch_v1& input, void** output) {
    return createNormalizationBatch(opaqueContext, std::static_pointer_cast<NormalizationPrograms>(programs),
        targetCount, input, output);
}

int createNormalizationBatch(void* opaqueContext, void* programsHandle,
    const mcdr_oit_normalization_batch_v1& input, void** output) {
    const auto programs = findNormalizationPrograms(programsHandle);
    return createNormalizationBatch(opaqueContext, programs, programs ? programs->legacyTargetCount : 0, input, output);
}

int recordNormalizationLane(void* opaqueContext, void* handle, const mcdr_oit_normalization_lane_v1& input) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto batch = findNormalizationBatch(handle);
    if (const int status = normalizationBatchStatus(context, batch); status != MCDR_OIT_NORMALIZATION_SUCCESS)
        return status;
    if (input.schemaVersion != 1 || input.structSize != sizeof(input) || input.reserved != 0 ||
        input.laneIndex >= normalizationLaneCount || input.laneIndex != batch->nextLane || batch->averageRecorded ||
        !batch->frame->batchOpen || batch->sequence != batch->frame->batchSequence)
        return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    if (!validNormalizationSource(*batch, input.q)) {
        dbgLogError("OIT normalization: invalid Q texture");
        return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    }
    std::vector<mcdr_prepared_binding> inputs{input.q};
    for (uint32_t index = 0; index < 4; ++index) {
        if (index < batch->frame->targetCount) {
            if (!validNormalizationSource(*batch, input.coefficients[index])) {
                dbgLogError("OIT normalization: invalid coefficient texture (%u)", index);
                return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
            }
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
    const auto table = normalizationTable(*batch, input.laneIndex, inputs);
    NormalizationRecording recording{context, *batch};
    try {
        vanilla::DeclaredPassToken* token = nullptr;
        if (vanilla::beginDeclaredPass(context, 0x240 + input.laneIndex, uses.data(),
                static_cast<uint32_t>(uses.size()), 3, &token) != 1)
            return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
        bindNormalizationCompute(context, *batch, input.laneIndex, table);
        context->commandList->SetPipelineState(batch->programs->lane[batch->frame->targetCount].Get());
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
    const auto table = normalizationTable(*batch, normalizationLaneCount, {});
    NormalizationRecording recording{context, *batch};
    try {
        vanilla::DeclaredPassToken* token = nullptr;
        if (vanilla::beginDeclaredPass(context, 0x244, uses, 2, 3, &token) != 1)
            return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
        bindNormalizationCompute(context, *batch, normalizationLaneCount, table);
        context->commandList->SetPipelineState(batch->programs->average.Get());
        context->commandList->Dispatch(8, 8, batch->input.residentCount);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = batch->frame->corrected.Get();
        context->commandList->ResourceBarrier(1, &barrier);
        if (vanilla::finishDeclaredPass(context, token) != 1)
            return MCDR_OIT_NORMALIZATION_RECORDING_FAILED;
        for (uint32_t index = 0; index < batch->input.residentCount; ++index) {
            const auto& region = batch->input.regions[index];
            batch->frame->averagedPixels += static_cast<uint64_t>(region.width) * region.height;
        }
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

int resolveNormalizationOutput(void* opaqueContext, void* handle, ID3D12Resource** output) {
    if (!output) return MCDR_OIT_NORMALIZATION_INVALID_REQUEST;
    *output = nullptr;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto batch = findNormalizationBatch(handle);
    if (const int status = normalizationBatchStatus(context, batch); status != MCDR_OIT_NORMALIZATION_SUCCESS)
        return status;
    const auto& frame = *batch->frame;
    if (!batch->averageRecorded || frame.batchOpen || batch->sequence != frame.batchSequence ||
        frame.averagedPixels != static_cast<uint64_t>(frame.input.outputWidth) * frame.input.outputHeight)
        return MCDR_OIT_NORMALIZATION_UNSUPPORTED;
    *output = frame.corrected.Get();
    return MCDR_OIT_NORMALIZATION_SUCCESS;
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
    output->targetCount = batch->frame->targetCount;
    output->frameSerial = batch->input.frameSerial;
    output->programGeneration = batch->programs->generation;
    return MCDR_OIT_NORMALIZATION_SUCCESS;
}

void destroyNormalizationBatch(void* handle) noexcept {
    std::lock_guard lock(normalizationRegistryMutex);
    normalizationBatches.erase(reinterpret_cast<uintptr_t>(handle));
}

}
