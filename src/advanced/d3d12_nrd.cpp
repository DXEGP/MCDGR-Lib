// d3d12_nrd.cpp — NRD 实例清单（S4 4-c 第一步）。普通翻译单元：NRD 的 InstanceDesc 是纯 CPU 结构，
// 不需要 D3D12 设备，也不参与 C++23 模块。
//
// 清单里的数字（pipeline 数、CB 上限、纹理池、寄存器空间）是库自己算出来的，手填对不上就是整段降噪
// 不出图。这里只把它们读出来交给 JVM 打日志。
// 见 docs\rt\README.md §20

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "mcdr_abi.h"

#if defined(MCDR_HAS_NRD)

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "NRD.h"
#include "mcdr_nrd_impl.h"
#include "../../build/nrd-dependencies.h"

namespace {

constexpr std::array<nrd::DenoiserDesc, 2> kDenoisers{{
    {0, nrd::Denoiser::REBLUR_DIFFUSE},
    {1, nrd::Denoiser::REBLUR_SPECULAR},
}};

// 追加一行；写满就停，绝不越界（capacity 由调用方给）。
void appendLine(char* out, int32_t capacity, int32_t& used, const char* fmt, ...) {
    if (used >= capacity - 1) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    const int32_t written =
        std::vsnprintf(out + used, static_cast<size_t>(capacity - used), fmt, args);
    va_end(args);
    if (written <= 0) {
        return;
    }
    used += written < capacity - used ? written : capacity - used - 1;
}

} // namespace

extern "C" int32_t MCDR_CALL mcdr_dx12NrdInspect(char* outText, int32_t capacity) {
    if (outText == nullptr || capacity <= 0) {
        return -1;
    }
    outText[0] = '\0';

    nrd::InstanceCreationDesc creation = {};
    creation.denoisers = kDenoisers.data();
    creation.denoisersNum = static_cast<uint32_t>(kDenoisers.size());

    nrd::Instance* instance = nullptr;
    const nrd::Result created = nrd::CreateInstance(creation, instance);
    if (created != nrd::Result::SUCCESS || instance == nullptr) {
        std::snprintf(outText, static_cast<size_t>(capacity), "nrd: create failed, result=%d\n",
                      static_cast<int32_t>(created));
        return -1;
    }

    const nrd::InstanceDesc* desc = nrd::GetInstanceDesc(*instance);
    int32_t used = 0;
    if (desc == nullptr) {
        std::snprintf(outText, static_cast<size_t>(capacity), "nrd: no instance desc\n");
    } else {
        // 编码是库编译期定的，我们不能选：normal-roughness 的打包方式必须照着它来，
        // 否则降噪读到的是解错的法线与粗糙度，画面只会变糊而不会报错。
        const nrd::LibraryDesc* library = nrd::GetLibraryDesc();
        if (library != nullptr) {
            appendLine(outText, capacity, used, "nrd: version=%u.%u.%u denoisers=%u\n",
                       library->versionMajor, library->versionMinor, library->versionBuild,
                       library->supportedDenoisersNum);
            appendLine(outText, capacity, used, "nrd: normal encoding=%d roughness=%d\n",
                       static_cast<int32_t>(library->normalEncoding),
                       static_cast<int32_t>(library->roughnessEncoding));
        }
        appendLine(outText, capacity, used, "nrd: pipelines=%u\n", desc->pipelinesNum);
        appendLine(outText, capacity, used, "nrd: cb max=%u bytes\n", desc->constantBufferMaxDataSize);
        appendLine(outText, capacity, used, "nrd: spaces cb=%u res=%u\n",
                   desc->constantBufferAndSamplersSpaceIndex, desc->resourcesSpaceIndex);
        appendLine(outText, capacity, used, "nrd: registers cb=%u srv=%u smp=%u\n",
                   desc->constantBufferRegisterIndex, desc->resourcesBaseRegisterIndex,
                   desc->samplersBaseRegisterIndex);
        appendLine(outText, capacity, used, "nrd: samplers=%u\n", desc->samplersNum);
        appendLine(outText, capacity, used, "nrd: pool permanent=%u transient=%u\n",
                   desc->permanentPoolSize, desc->transientPoolSize);
        appendLine(outText, capacity, used, "nrd: desc set srv=%u uav=%u sets=%u\n",
                   desc->descriptorPoolDesc.perSetTexturesMaxNum,
                   desc->descriptorPoolDesc.perSetStorageTexturesMaxNum,
                   desc->descriptorPoolDesc.setsMaxNum);
        if (desc->pipelinesNum > 0) {
            appendLine(outText, capacity, used, "nrd: pipeline0 dxil=%llu cb=%d\n",
                       static_cast<unsigned long long>(desc->pipelines[0].computeShaderDXIL.size),
                       desc->pipelines[0].hasConstantData ? 1 : 0);
        }
        if (desc->shaderEntryPoint != nullptr) {
            appendLine(outText, capacity, used, "nrd: entry=%s\n", desc->shaderEntryPoint);
        }
    }

    nrd::DestroyInstance(*instance);
    return desc == nullptr ? -1 : 1;
}

// ---------------------------------------------------------------------------
// 常驻上下文：实例 + 每条着色器一条 PSO + 两套纹理池 + 一张着色器可见的描述符堆。
// 这三样东西都依赖设备，而且尺寸一变就要重来，所以放在一个可重建的结构里。
// ---------------------------------------------------------------------------

namespace {

using Microsoft::WRL::ComPtr;

// NRD 的 Format 是它自己的枚举，必须逐项映射；漏一个就会在池纹理上拿 E_INVALIDARG，
// 而且只在降噪第一次跑到那条管线时才炸。
DXGI_FORMAT toDxgi(nrd::Format format) {
    switch (format) {
    case nrd::Format::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case nrd::Format::R8_SNORM: return DXGI_FORMAT_R8_SNORM;
    case nrd::Format::R8_UINT: return DXGI_FORMAT_R8_UINT;
    case nrd::Format::R8_SINT: return DXGI_FORMAT_R8_SINT;
    case nrd::Format::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case nrd::Format::RG8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
    case nrd::Format::RG8_UINT: return DXGI_FORMAT_R8G8_UINT;
    case nrd::Format::RG8_SINT: return DXGI_FORMAT_R8G8_SINT;
    case nrd::Format::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case nrd::Format::RGBA8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case nrd::Format::RGBA8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
    case nrd::Format::RGBA8_SINT: return DXGI_FORMAT_R8G8B8A8_SINT;
    case nrd::Format::RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case nrd::Format::R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case nrd::Format::R16_SNORM: return DXGI_FORMAT_R16_SNORM;
    case nrd::Format::R16_UINT: return DXGI_FORMAT_R16_UINT;
    case nrd::Format::R16_SINT: return DXGI_FORMAT_R16_SINT;
    case nrd::Format::R16_SFLOAT: return DXGI_FORMAT_R16_FLOAT;
    case nrd::Format::RG16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
    case nrd::Format::RG16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
    case nrd::Format::RG16_UINT: return DXGI_FORMAT_R16G16_UINT;
    case nrd::Format::RG16_SINT: return DXGI_FORMAT_R16G16_SINT;
    case nrd::Format::RG16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case nrd::Format::RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case nrd::Format::RGBA16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case nrd::Format::RGBA16_UINT: return DXGI_FORMAT_R16G16B16A16_UINT;
    case nrd::Format::RGBA16_SINT: return DXGI_FORMAT_R16G16B16A16_SINT;
    case nrd::Format::RGBA16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case nrd::Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
    case nrd::Format::R32_SINT: return DXGI_FORMAT_R32_SINT;
    case nrd::Format::R32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
    case nrd::Format::RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
    case nrd::Format::RG32_SINT: return DXGI_FORMAT_R32G32_SINT;
    case nrd::Format::RG32_SFLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case nrd::Format::RGB32_UINT: return DXGI_FORMAT_R32G32B32_UINT;
    case nrd::Format::RGB32_SINT: return DXGI_FORMAT_R32G32B32_SINT;
    case nrd::Format::RGB32_SFLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
    case nrd::Format::RGBA32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
    case nrd::Format::RGBA32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
    case nrd::Format::RGBA32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case nrd::Format::R10_G10_B10_A2_UINT: return DXGI_FORMAT_R10G10B10A2_UINT;
    case nrd::Format::R11_G11_B10_UFLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case nrd::Format::R9_G9_B9_E5_UFLOAT: return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// NRD 的共享布局（见 docs/rt/README.md §22）：b0 与采样器在同一个空间，SRV/UAV 在另一个空间，
// 寄存器基址与空间号都由 InstanceDesc 给出，不能写死。
bool createRootSignature(ID3D12Device* device, const nrd::InstanceDesc& desc,
                         ID3D12RootSignature** out, const char** stage) {
    D3D12_DESCRIPTOR_RANGE srv{};
    srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.NumDescriptors = desc.descriptorPoolDesc.perSetTexturesMaxNum;
    srv.BaseShaderRegister = desc.resourcesBaseRegisterIndex;
    srv.RegisterSpace = desc.resourcesSpaceIndex;
    srv.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uav{};
    uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.NumDescriptors = desc.descriptorPoolDesc.perSetStorageTexturesMaxNum;
    uav.BaseShaderRegister = desc.resourcesBaseRegisterIndex;
    uav.RegisterSpace = desc.resourcesSpaceIndex;
    uav.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE smp{};
    smp.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    smp.NumDescriptors = desc.samplersNum;
    smp.BaseShaderRegister = desc.samplersBaseRegisterIndex;
    smp.RegisterSpace = desc.constantBufferAndSamplersSpaceIndex;
    smp.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[7]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = desc.constantBufferRegisterIndex;
    params[0].Descriptor.RegisterSpace = desc.constantBufferAndSamplersSpaceIndex;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uav;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &smp;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[4].Descriptor = {0, 3};
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[5].Descriptor = {0, 3};
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[6].Descriptor = {0, 3};

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 7;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob,
                                           &error))) {
        *stage = "serialize root signature";
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(out)))) {
        *stage = "create root signature";
        return false;
    }
    return true;
}

struct NrdResource {
    std::shared_ptr<void> allocationCharge;
    ComPtr<ID3D12Resource> resource;
};

int32_t createPoolTexture(ID3D12Device* device, const nrd::TextureDesc& desc, uint32_t width,
                       uint32_t height, McdrNrdReserveMemory reserveMemory, NrdResource& output,
                       const char** stage) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = width / (desc.downsampleFactor == 0 ? 1u : desc.downsampleFactor);
    resource.Height = height / (desc.downsampleFactor == 0 ? 1u : desc.downsampleFactor);
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.Format = toDxgi(desc.format);
    resource.SampleDesc.Count = 1;
    // 池里的纹理在两个方向都要能读写：同一张图这帧是输入、下帧是输出。
    resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (resource.Width == 0 || resource.Height == 0 || resource.Format == DXGI_FORMAT_UNKNOWN) {
        *stage = "invalid pool texture";
        return -1;
    }
    output.allocationCharge = reserveMemory(&resource, false);
    if (!output.allocationCharge) {
        *stage = "managed memory budget exhausted";
        return MCDR_NRD_MEMORY_PRESSURE;
    }
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&output.resource)))) {
        *stage = "create pool texture";
        return -1;
    }
    return 1;
}

constexpr uint32_t kDispatchSlots = 8;
constexpr uint32_t kConstantSlotsPerDispatch = 64;
constexpr uint32_t kHistoryRejectionDescriptors = 7;
constexpr uint32_t kDependencyConstantBytes = 512;
using NrdDependencyBinding = std::array<uint32_t, 4>;

struct NrdDispatchSlot {
    ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;
    std::shared_ptr<void> recording;
    McdrNrdRecordingState (*recordingState)(const void*) = nullptr;

    bool cancelled() const {
        if (!recording || !recordingState) return false;
        const auto state = recordingState(recording.get());
        return state == McdrNrdRecordingState::Cancelled || state == McdrNrdRecordingState::SubmissionFailed;
    }

    bool complete() const {
        if (recording) {
            if (!recordingState) return false;
            const auto state = recordingState(recording.get());
            if (state == McdrNrdRecordingState::Cancelled) return true;
            if (state != McdrNrdRecordingState::Submitted && state != McdrNrdRecordingState::SubmissionFailed) return false;
        }
        if (!fence) return !recording;
        const auto completed = fence->GetCompletedValue();
        return completed != UINT64_MAX && completed >= value;
    }
};

struct NrdContext {
    nrd::Instance* instance = nullptr;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12RootSignature> historyRejectionRoot;
    ComPtr<ID3D12PipelineState> historyRejectionPipeline;
    ComPtr<ID3D12RootSignature> dependencyRoot;
    ComPtr<ID3D12PipelineState> dependencyUpdate;
    std::vector<ComPtr<ID3D12PipelineState>> dependencyPipelines;
    NrdResource dependencyRead;
    NrdResource dependencyWrite;
    NrdResource dependencyMask;
    NrdResource dependencyConstants;
    uint8_t* dependencyMapped = nullptr;
    std::vector<NrdDependencyBinding> permanentDependencies;
    std::vector<NrdDependencyBinding> transientDependencies;
    std::array<NrdDependencyBinding, 2> outputDependencies{};
    std::array<NrdDependencyBinding, 3> rawDependencies{};
    McdrNrdReserveMemory reserveMemory = nullptr;
    uint32_t dependencyEntries = 0;
    uint32_t dependencyMaskOffset = 0;
    uint32_t dependencyTiles = 0;
    uint32_t dependencyDispatches = 0;
    uint64_t dependencyEpoch = 0;
    bool dependencyActive = false;
    D3D12_RESOURCE_STATES dependencyReadState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES dependencyWriteState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES dependencyMaskState = D3D12_RESOURCE_STATE_COMMON;
    std::vector<ComPtr<ID3D12PipelineState>> pipelines;
    std::vector<NrdResource> permanent;
    std::vector<NrdResource> transient;
    /** 池纹理当前状态：同一张纹理既当 SRV 又当 UAV，状态必须自己记。 */
    std::vector<D3D12_RESOURCE_STATES> permanentState;
    std::vector<D3D12_RESOURCE_STATES> transientState;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12DescriptorHeap> samplers;
    NrdResource constants;
    uint8_t* constantMapped = nullptr;
    uint32_t constantStride = 0;
    uint32_t descriptorsPerDispatch = 0;
    std::array<NrdDispatchSlot, kDispatchSlots> slots;
    const nrd::InstanceDesc* desc = nullptr;
    ID3D12Device* device = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool ready = false;
    bool traced = false;
    bool resetPending = true;

    bool complete() const {
        return std::ranges::all_of(slots, &NrdDispatchSlot::complete);
    }

    bool cancelled() const {
        return std::ranges::any_of(slots, &NrdDispatchSlot::cancelled);
    }

    void release() {
        dependencyPipelines.clear();
        dependencyUpdate.Reset();
        dependencyRoot.Reset();
        dependencyRead = {};
        dependencyWrite = {};
        dependencyMask = {};
        if (dependencyMapped && dependencyConstants.resource) dependencyConstants.resource->Unmap(0, nullptr);
        dependencyMapped = nullptr;
        dependencyConstants = {};
        permanentDependencies.clear();
        transientDependencies.clear();
        dependencyActive = false;
        dependencyEpoch = 0;
        dependencyEntries = 0;
        dependencyMaskOffset = 0;
        dependencyTiles = 0;
        dependencyReadState = D3D12_RESOURCE_STATE_COMMON;
        dependencyWriteState = D3D12_RESOURCE_STATE_COMMON;
        dependencyMaskState = D3D12_RESOURCE_STATE_COMMON;
        reserveMemory = nullptr;
        pipelines.clear();
        permanent.clear();
        transient.clear();
        permanentState.clear();
        transientState.clear();
        if (constantMapped != nullptr && constants.resource) {
            constants.resource->Unmap(0, nullptr);
            constantMapped = nullptr;
        }
        constants.resource.Reset();
        constants.allocationCharge.reset();
        samplers.Reset();
        descriptors.Reset();
        rootSignature.Reset();
        historyRejectionPipeline.Reset();
        historyRejectionRoot.Reset();
        slots = {};
        if (instance != nullptr) {
            nrd::DestroyInstance(*instance);
            instance = nullptr;
        }
        device = nullptr;
        width = 0;
        height = 0;
        ready = false;
        traced = false;
        resetPending = true;
    }
};

NrdContext gNrd;
std::vector<std::unique_ptr<NrdContext>> gRetiredContexts;

void collectContexts() {
    std::erase_if(gRetiredContexts, [](const auto& context) {
        if (!context->complete()) return false;
        context->release();
        return true;
    });
}

void retireContext() {
    if (gNrd.complete()) {
        gNrd.release();
    } else {
        gRetiredContexts.push_back(std::make_unique<NrdContext>(std::move(gNrd)));
        gNrd = NrdContext{};
    }
    collectContexts();
}

} // namespace

extern "C" int32_t mcdr_nrd_context_init(void* device, int32_t width, int32_t height,
                                         McdrNrdReserveMemory reserveMemory, char* outText, int32_t capacity) {
    if (outText != nullptr && capacity > 0) {
        outText[0] = '\0';
    }
    if (device == nullptr || width <= 0 || height <= 0 || reserveMemory == nullptr) {
        return -1;
    }
    collectContexts();
    if (gNrd.cancelled()) retireContext();
    if (gNrd.ready && gNrd.device == device && gNrd.width == static_cast<uint32_t>(width)
        && gNrd.height == static_cast<uint32_t>(height)) {
        return 1;
    }
    retireContext();

    int32_t used = 0;
    const char* stage = "create instance";
    nrd::InstanceCreationDesc creation{};
    creation.denoisers = kDenoisers.data();
    creation.denoisersNum = static_cast<uint32_t>(kDenoisers.size());
    if (nrd::CreateInstance(creation, gNrd.instance) != nrd::Result::SUCCESS
        || gNrd.instance == nullptr) {
        gNrd.release();
        appendLine(outText, capacity, used, "nrd: context failed, %s\n", stage);
        return -1;
    }
    const nrd::InstanceDesc* desc = nrd::GetInstanceDesc(*gNrd.instance);
    gNrd.desc = desc;
    if (desc == nullptr) {
        gNrd.release();
        appendLine(outText, capacity, used, "nrd: context failed, no instance desc\n");
        return -1;
    }

    gNrd.width = static_cast<uint32_t>(width);
    gNrd.height = static_cast<uint32_t>(height);
    gNrd.reserveMemory = reserveMemory;
    gNrd.permanent.resize(desc->permanentPoolSize);
    uint32_t permanent = 0;
    for (uint32_t i = 0; i < desc->permanentPoolSize; ++i) {
        const auto result = createPoolTexture(static_cast<ID3D12Device*>(device), desc->permanentPool[i],
                               gNrd.width, gNrd.height, reserveMemory, gNrd.permanent[i], &stage);
        if (result <= 0) {
            appendLine(outText, capacity, used, "nrd: context failed, %s %u\n", stage, i);
            gNrd.release();
            return result;
        }
        ++permanent;
    }
    gNrd.transient.resize(desc->transientPoolSize);
    uint32_t transient = 0;
    for (uint32_t i = 0; i < desc->transientPoolSize; ++i) {
        const auto result = createPoolTexture(static_cast<ID3D12Device*>(device), desc->transientPool[i],
                               gNrd.width, gNrd.height, reserveMemory, gNrd.transient[i], &stage);
        if (result <= 0) {
            appendLine(outText, capacity, used, "nrd: context failed, %s %u\n", stage, i);
            gNrd.release();
            return result;
        }
        ++transient;
    }

    const uint32_t perSet = desc->descriptorPoolDesc.perSetTexturesMaxNum
                          + desc->descriptorPoolDesc.perSetStorageTexturesMaxNum;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    gNrd.descriptorsPerDispatch = perSet * desc->descriptorPoolDesc.setsMaxNum
        + kHistoryRejectionDescriptors * static_cast<uint32_t>(kDenoisers.size()) + 1;
    heapDesc.NumDescriptors = gNrd.descriptorsPerDispatch * kDispatchSlots;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    stage = "create descriptor heap";
    if (FAILED(static_cast<ID3D12Device*>(device)->CreateDescriptorHeap(
            &heapDesc, IID_PPV_ARGS(&gNrd.descriptors)))) {
        appendLine(outText, capacity, used, "nrd: context failed, %s\n", stage);
        gNrd.release();
        return -1;
    }

    D3D12_DESCRIPTOR_HEAP_DESC samplerDesc{};
    samplerDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samplerDesc.NumDescriptors = desc->samplersNum;
    samplerDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    stage = "create sampler heap";
    if (FAILED(static_cast<ID3D12Device*>(device)->CreateDescriptorHeap(
            &samplerDesc, IID_PPV_ARGS(&gNrd.samplers)))) {
        appendLine(outText, capacity, used, "nrd: context failed, %s\n", stage);
        gNrd.release();
        return -1;
    }
    const UINT samplerIncrement = static_cast<ID3D12Device*>(device)->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for (uint32_t i = 0; i < desc->samplersNum; ++i) {
        D3D12_SAMPLER_DESC samplerInfo{};
        const bool linear = desc->samplers[i] == nrd::Sampler::LINEAR_CLAMP;
        samplerInfo.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplerInfo.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplerInfo.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplerInfo.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplerInfo.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = gNrd.samplers->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(i) * samplerIncrement;
        static_cast<ID3D12Device*>(device)->CreateSampler(&samplerInfo, handle);
    }

    // 每段常量的步长必须是对齐后的段长（本库 896 -> 1024）：写 256 会让后一段的 memcpy 盖掉
    // 前一段，所有 pass 读到别的 pass 的常量，输出只剩一条 tile 行有值或全 0，且不报错。
    gNrd.constantStride = ((desc->constantBufferMaxDataSize + 255u) / 256u) * 256u;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ringDesc{};
    ringDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ringDesc.Width = static_cast<uint64_t>(gNrd.constantStride) * kDispatchSlots
                   * kConstantSlotsPerDispatch;
    ringDesc.Height = 1;
    ringDesc.DepthOrArraySize = 1;
    ringDesc.MipLevels = 1;
    ringDesc.SampleDesc.Count = 1;
    ringDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    stage = "create constant ring";
    gNrd.constants.allocationCharge = reserveMemory(&ringDesc, true);
    if (!gNrd.constants.allocationCharge) {
        appendLine(outText, capacity, used, "nrd: context failed, managed memory budget exhausted\n");
        gNrd.release();
        return MCDR_NRD_MEMORY_PRESSURE;
    }
    if (FAILED(static_cast<ID3D12Device*>(device)->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &ringDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&gNrd.constants.resource)))
        || FAILED(gNrd.constants.resource->Map(0, nullptr, reinterpret_cast<void**>(&gNrd.constantMapped)))) {
        appendLine(outText, capacity, used, "nrd: context failed, %s\n", stage);
        gNrd.release();
        return -1;
    }

    ID3D12RootSignature* root = nullptr;
    if (!createRootSignature(static_cast<ID3D12Device*>(device), *desc, &root, &stage)) {
        gNrd.release();
        appendLine(outText, capacity, used, "nrd: context failed, %s\n", stage);
        return -1;
    }
    gNrd.rootSignature.Attach(root);
    gNrd.pipelines.resize(desc->pipelinesNum);
    uint32_t created = 0;
    for (uint32_t pipelineIndex = 0; pipelineIndex < desc->pipelinesNum; ++pipelineIndex) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = gNrd.rootSignature.Get();
        pipeline.CS.pShaderBytecode = desc->pipelines[pipelineIndex].computeShaderDXIL.bytecode;
        pipeline.CS.BytecodeLength = desc->pipelines[pipelineIndex].computeShaderDXIL.size;
        if (FAILED(static_cast<ID3D12Device*>(device)->CreateComputePipelineState(
                &pipeline, IID_PPV_ARGS(&gNrd.pipelines[pipelineIndex])))) {
            appendLine(outText, capacity, used, "nrd: context failed, create pipeline state %u\n", pipelineIndex);
            gNrd.release();
            return -1;
        }
        ++created;
    }

    gNrd.device = static_cast<ID3D12Device*>(device);
    // 池纹理跨 pass 既被当 SRV 读也被当 UAV 写：状态由派发循环逐次显式过渡，
    // 这里只记初始值（见 docs/rt/README.md §23.4）。
    gNrd.permanentState.assign(gNrd.permanent.size(), D3D12_RESOURCE_STATE_COMMON);
    gNrd.transientState.assign(gNrd.transient.size(), D3D12_RESOURCE_STATE_COMMON);
    gNrd.ready = true;
    appendLine(outText, capacity, used, "nrd: context ready, psos=%u pool=%u/%u heap=%u\n",
               created, permanent, transient, heapDesc.NumDescriptors);
    appendLine(outText, capacity, used, "nrd: cb max=%u samplers=%u\n",
               desc->constantBufferMaxDataSize, desc->samplersNum);
    return 1;
}

// ---- 逐帧派发 ----
//
namespace {

ID3D12Resource* resolveResource(const nrd::ResourceDesc& desc, const mcdr_nrd_frame* frame,
                               nrd::Identifier identifier) {
    switch (desc.type) {
        case nrd::ResourceType::IN_MV:
            return static_cast<ID3D12Resource*>(frame->motion);
        case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
            return static_cast<ID3D12Resource*>(frame->normalRoughness);
        case nrd::ResourceType::IN_VIEWZ:
            return static_cast<ID3D12Resource*>(identifier == 1 && frame->specularViewZ
                ? frame->specularViewZ : frame->viewZ);
        case nrd::ResourceType::IN_DIFF_CONFIDENCE:
            return static_cast<ID3D12Resource*>(frame->diffuseConfidence);
        case nrd::ResourceType::IN_SPEC_CONFIDENCE:
            return static_cast<ID3D12Resource*>(frame->specularConfidence);
        case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
            return static_cast<ID3D12Resource*>(frame->radianceIn);
        case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
            return static_cast<ID3D12Resource*>(frame->radianceOut);
        case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
            return static_cast<ID3D12Resource*>(frame->specularIn);
        case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
            return static_cast<ID3D12Resource*>(frame->specularOut);
        case nrd::ResourceType::OUT_VALIDATION:
            return static_cast<ID3D12Resource*>(frame->validationOutput);
        case nrd::ResourceType::PERMANENT_POOL: {
            const uint32_t index = desc.indexInPool;
            return index < gNrd.permanent.size() ? gNrd.permanent[index].resource.Get() : nullptr;
        }
        case nrd::ResourceType::TRANSIENT_POOL: {
            const uint32_t index = desc.indexInPool;
            return index < gNrd.transient.size() ? gNrd.transient[index].resource.Get() : nullptr;
        }
        default:
            return nullptr;
    }
}

void writeView(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT slot, ID3D12Resource* resource,
               DXGI_FORMAT format, bool storage) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot)
                * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
    if (storage) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource, nullptr, &view, handle);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = resourceDesc.MipLevels;
        device->CreateShaderResourceView(resource, &view, handle);
    }
}

constexpr auto kHistoryRejectionShader = R"hlsl(
Texture2D<uint> rejection : register(t0);
RWTexture2D<float> previousDepth : register(u0);
RWTexture2D<uint> previousInternal : register(u1);
RWTexture2D<float4> previousHistory : register(u2);
RWTexture2D<float> previousFastHistory : register(u3);
RWTexture2D<float> previousTracking : register(u4);
RWTexture2D<float> previousTrackingAlternate : register(u5);
cbuffer Parameters : register(b0) {
    uint width;
    uint height;
    uint lobeBit;
};
[numthreads(8, 8, 1)]
void RejectHistory(uint3 pixel : SV_DispatchThreadID) {
    if (pixel.x >= width || pixel.y >= height) return;
    uint mask = 0u;
    for (int offsetY = -2; offsetY <= 2; ++offsetY) {
        for (int offsetX = -2; offsetX <= 2; ++offsetX) {
            int2 source = int2(pixel.xy) + int2(offsetX, offsetY);
            if (all(source >= 0) && all(source < int2(width, height)))
                mask |= rejection.Load(int3(source, 0));
        }
    }
    if ((mask & lobeBit) == 0u) return;
    previousDepth[pixel.xy] = 1000000.0;
    previousInternal[pixel.xy] = 0u;
    previousHistory[pixel.xy] = 0.0;
    previousFastHistory[pixel.xy] = 0.0;
    if (lobeBit == 2u) {
        previousTracking[pixel.xy] = 0.0;
        previousTrackingAlternate[pixel.xy] = 0.0;
    }
}
)hlsl";

bool prepareHistoryRejection(const mcdr_nrd_frame& frame) {
    if (gNrd.historyRejectionPipeline) return true;
    if (!frame.createHistoryRejectionPipeline) return false;
    std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 6;
    std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 3;
    for (uint32_t index = 0; index < ranges.size(); ++index) {
        parameters[index + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[index + 1].DescriptorTable = {1, &ranges[index]};
    }
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<uint32_t>(parameters.size());
    description.pParameters = parameters.data();
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
        &bytecode, &errors)) || FAILED(gNrd.device->CreateRootSignature(0,
            bytecode->GetBufferPointer(), bytecode->GetBufferSize(), IID_PPV_ARGS(&root))) ||
        !frame.createHistoryRejectionPipeline(kHistoryRejectionShader, root.Get(),
            reinterpret_cast<void**>(pipeline.GetAddressOf())) || !pipeline) return false;
    gNrd.historyRejectionRoot = std::move(root);
    gNrd.historyRejectionPipeline = std::move(pipeline);
    return true;
}

bool isTemporalAccumulation(const nrd::PipelineDesc& pipeline) {
    return std::string_view(pipeline.shaderIdentifier).find("REBLUR_TemporalAccumulation") != std::string_view::npos;
}

bool historyRejectionLayout(const nrd::DispatchDesc& dispatch) {
    if (dispatch.identifier > 1 || dispatch.resourcesNum != (dispatch.identifier == 1 ? 19u : 16u) ||
        dispatch.constantBufferDataSize == 0) return false;
    for (const auto index : {4u, 6u, 10u, 11u}) {
        const auto& resource = dispatch.resources[index];
        if (resource.type != nrd::ResourceType::PERMANENT_POOL ||
            resource.descriptorType != nrd::DescriptorType::TEXTURE ||
            resource.indexInPool >= gNrd.permanent.size()) return false;
    }
    if (dispatch.identifier == 1 && (dispatch.resources[12].type != nrd::ResourceType::PERMANENT_POOL ||
        dispatch.resources[12].descriptorType != nrd::DescriptorType::TEXTURE ||
        dispatch.resources[12].indexInPool >= gNrd.permanent.size() ||
        dispatch.resources[17].type != nrd::ResourceType::PERMANENT_POOL ||
        dispatch.resources[17].descriptorType != nrd::DescriptorType::STORAGE_TEXTURE ||
        dispatch.resources[17].indexInPool >= gNrd.permanent.size())) return false;
    return gNrd.permanent[dispatch.resources[4].indexInPool].resource->GetDesc().Format == DXGI_FORMAT_R32_FLOAT &&
        gNrd.permanent[dispatch.resources[6].indexInPool].resource->GetDesc().Format == DXGI_FORMAT_R16_UINT &&
        gNrd.permanent[dispatch.resources[10].indexInPool].resource->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        gNrd.permanent[dispatch.resources[11].indexInPool].resource->GetDesc().Format == DXGI_FORMAT_R16_FLOAT &&
        (dispatch.identifier == 0 || (
            gNrd.permanent[dispatch.resources[12].indexInPool].resource->GetDesc().Format == DXGI_FORMAT_R16_FLOAT &&
            gNrd.permanent[dispatch.resources[17].indexInPool].resource->GetDesc().Format == DXGI_FORMAT_R16_FLOAT));
}

void rejectHistory(const mcdr_nrd_frame& frame, const nrd::DispatchDesc& dispatch, UINT descriptorBase) {
    auto* list = static_cast<ID3D12GraphicsCommandList*>(frame.list);
    writeView(gNrd.device, gNrd.descriptors.Get(), descriptorBase,
        static_cast<ID3D12Resource*>(frame.historyRejection),
        static_cast<ID3D12Resource*>(frame.historyRejection)->GetDesc().Format, false);
    const std::array<uint32_t, 6> indices{4, 6, 10, 11, 12, 17};
    for (uint32_t index = 0; index < indices.size(); ++index) {
        if (index >= 4 && dispatch.identifier == 0) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
            description.Format = DXGI_FORMAT_R16_FLOAT;
            description.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            auto handle = gNrd.descriptors->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(descriptorBase + index + 1) *
                gNrd.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            gNrd.device->CreateUnorderedAccessView(nullptr, nullptr, &description, handle);
            continue;
        }
        const auto poolIndex = dispatch.resources[indices[index]].indexInPool;
        auto* resource = gNrd.permanent[poolIndex].resource.Get();
        auto& state = gNrd.permanentState[poolIndex];
        if (state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            const D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                D3D12_RESOURCE_BARRIER_FLAG_NONE,
                {{resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}}};
            list->ResourceBarrier(1, &barrier);
            state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        writeView(gNrd.device, gNrd.descriptors.Get(), descriptorBase + index + 1,
            resource, resource->GetDesc().Format, true);
    }
    list->SetComputeRootSignature(gNrd.historyRejectionRoot.Get());
    list->SetPipelineState(gNrd.historyRejectionPipeline.Get());
    const std::array<uint32_t, 3> constants{gNrd.width, gNrd.height, 1u << dispatch.identifier};
    list->SetComputeRoot32BitConstants(0, static_cast<uint32_t>(constants.size()), constants.data(), 0);
    const auto increment = gNrd.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto handle = gNrd.descriptors->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(descriptorBase) * increment;
    list->SetComputeRootDescriptorTable(1, handle);
    handle.ptr += increment;
    list->SetComputeRootDescriptorTable(2, handle);
    list->Dispatch((gNrd.width + 7) / 8, (gNrd.height + 7) / 8, 1);
    const D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV,
        D3D12_RESOURCE_BARRIER_FLAG_NONE, {}};
    list->ResourceBarrier(1, &barrier);
    list->SetComputeRootSignature(gNrd.rootSignature.Get());
    list->SetComputeRootDescriptorTable(3, gNrd.samplers->GetGPUDescriptorHandleForHeapStart());
}

NrdDependencyBinding dependencyBinding(const nrd::ResourceDesc& resource) {
    if (resource.type == nrd::ResourceType::PERMANENT_POOL) return gNrd.permanentDependencies[resource.indexInPool];
    if (resource.type == nrd::ResourceType::TRANSIENT_POOL) return gNrd.transientDependencies[resource.indexInPool];
    if (resource.type == nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST) return gNrd.outputDependencies[0];
    if (resource.type == nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST) return gNrd.outputDependencies[1];
    if (resource.type == nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST) return gNrd.rawDependencies[0];
    if (resource.type == nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST) return gNrd.rawDependencies[1];
    return gNrd.rawDependencies[2];
}

int32_t prepareDependencies() {
    if (gNrd.dependencyWrite.resource) return 1;
    const auto* library = nrd::GetLibraryDesc();
    if (!library || library->versionMajor != 4 || library->versionMinor != 18 ||
        library->normalEncoding != nrd::NormalEncoding::R10_G10_B10_A2_UNORM ||
        library->roughnessEncoding != nrd::RoughnessEncoding::LINEAR) return -1;
    uint64_t bytes = 0;
    const auto binding = [&bytes](ID3D12Resource* resource, uint32_t factor) {
        const auto description = resource->GetDesc();
        const auto tileSize = std::max(1u, 64u / std::max(1u, factor));
        NrdDependencyBinding result{static_cast<uint32_t>(bytes), static_cast<uint32_t>(description.Width),
            description.Height, tileSize};
        bytes += uint64_t{(result[1] + tileSize - 1u) / tileSize} * ((result[2] + tileSize - 1u) / tileSize) * 32u;
        return result;
    };
    gNrd.permanentDependencies.clear();
    gNrd.transientDependencies.clear();
    for (uint32_t index = 0; index < gNrd.permanent.size(); ++index)
        gNrd.permanentDependencies.push_back(binding(gNrd.permanent[index].resource.Get(), gNrd.desc->permanentPool[index].downsampleFactor));
    for (uint32_t index = 0; index < gNrd.transient.size(); ++index)
        gNrd.transientDependencies.push_back(binding(gNrd.transient[index].resource.Get(), gNrd.desc->transientPool[index].downsampleFactor));
    for (auto& output : gNrd.outputDependencies) output = binding(gNrd.permanent[0].resource.Get(), 1);
    for (auto& raw : gNrd.rawDependencies) raw = binding(gNrd.permanent[0].resource.Get(), 1);
    if (bytes > UINT32_MAX / 2) return -1;
    gNrd.dependencyEntries = static_cast<uint32_t>(bytes / 32u);
    gNrd.dependencyMaskOffset = static_cast<uint32_t>(bytes);
    gNrd.dependencyTiles = ((gNrd.width + 63u) / 64u) * ((gNrd.height + 63u) / 64u);
    bytes += uint64_t{gNrd.dependencyTiles} * 4u;
    std::array<NrdResource, 3> buffers;
    for (uint32_t index = 0; index < buffers.size(); ++index) {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = index == 2 ? uint64_t{kDependencyConstantBytes} * kConstantSlotsPerDispatch * kDispatchSlots : bytes;
        description.Height = description.DepthOrArraySize = description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = index == 1 ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        buffers[index].allocationCharge = gNrd.reserveMemory(&description, index == 2);
        if (!buffers[index].allocationCharge) return MCDR_NRD_MEMORY_PRESSURE;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = index == 2 ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(gNrd.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            index == 2 ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&buffers[index].resource)))) return -1;
    }
    NrdResource mask;
    const char* stage = nullptr;
    const auto createdMask = createPoolTexture(gNrd.device, {nrd::Format::R32_UINT, 1},
        gNrd.width, gNrd.height, gNrd.reserveMemory, mask, &stage);
    if (createdMask <= 0) return createdMask;
    D3D12_DESCRIPTOR_RANGE maskRange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 3, 0};
    std::array<D3D12_ROOT_PARAMETER, 6> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 3, 16};
    for (uint32_t index = 1; index <= 3; ++index) {
        parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[index].Descriptor = {index - 1u, 3};
    }
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[4].Descriptor = {0, 3};
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[5].DescriptorTable = {1, &maskRange};
    const D3D12_ROOT_SIGNATURE_DESC rootDescription{static_cast<UINT>(parameters.size()), parameters.data(), 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors)) || FAILED(gNrd.device->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&root)))) return -1;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root.Get();
    pipeline.CS = {kMcdrNrdDependencyUpdate, sizeof(kMcdrNrdDependencyUpdate)};
    ComPtr<ID3D12PipelineState> update;
    if (FAILED(gNrd.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&update)))) return -1;
    std::vector<ComPtr<ID3D12PipelineState>> pipelines(gNrd.desc->pipelinesNum);
    for (uint32_t index = 0; index < pipelines.size(); ++index) {
        const auto& original = gNrd.desc->pipelines[index];
        const auto found = std::ranges::find(kMcdrNrdTrackedShaders, std::string_view(original.shaderIdentifier),
            &McdrNrdTrackedShader::identifier);
        if (found == std::end(kMcdrNrdTrackedShaders)) continue;
        pipeline.pRootSignature = gNrd.rootSignature.Get();
        pipeline.CS = {found->bytecode, found->size};
        if (FAILED(gNrd.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipelines[index])))) return -1;
    }
    uint8_t* mapped = nullptr;
    if (FAILED(buffers[2].resource->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return -1;
    gNrd.dependencyRead = std::move(buffers[0]);
    gNrd.dependencyWrite = std::move(buffers[1]);
    gNrd.dependencyConstants = std::move(buffers[2]);
    gNrd.dependencyMask = std::move(mask);
    gNrd.dependencyMapped = mapped;
    gNrd.dependencyRoot = std::move(root);
    gNrd.dependencyUpdate = std::move(update);
    gNrd.dependencyPipelines = std::move(pipelines);
    return 1;
}

void dependencyTransition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES next) {
    if (state == next) return;
    const D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        D3D12_RESOURCE_BARRIER_FLAG_NONE, {{resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, next}}};
    list->ResourceBarrier(1, &barrier);
    state = next;
}

void copyDependencies(const mcdr_nrd_frame& frame) {
    auto* list = static_cast<ID3D12GraphicsCommandList*>(frame.list);
    dependencyTransition(list, gNrd.dependencyWrite.resource.Get(), gNrd.dependencyWriteState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    dependencyTransition(list, gNrd.dependencyRead.resource.Get(), gNrd.dependencyReadState, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(gNrd.dependencyRead.resource.Get(), gNrd.dependencyWrite.resource.Get());
    dependencyTransition(list, gNrd.dependencyRead.resource.Get(), gNrd.dependencyReadState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    dependencyTransition(list, gNrd.dependencyWrite.resource.Get(), gNrd.dependencyWriteState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void updateDependencies(const mcdr_nrd_frame& frame, UINT descriptor, uint32_t mode,
    uint32_t count, NrdDependencyBinding binding = {}, uint32_t readOffset = 0, uint32_t lobe = 0) {
    auto* list = static_cast<ID3D12GraphicsCommandList*>(frame.list);
    dependencyTransition(list, gNrd.dependencyRead.resource.Get(), gNrd.dependencyReadState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    dependencyTransition(list, gNrd.dependencyWrite.resource.Get(), gNrd.dependencyWriteState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dependencyTransition(list, gNrd.dependencyMask.resource.Get(), gNrd.dependencyMaskState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto& input = *frame.dependencies;
    const std::array<uint32_t, 16> constants{mode, count, readOffset, binding[0], binding[1], binding[2], binding[3], lobe,
        input.pathOffset, input.changeOffset, input.changeCount, input.fullDomain, gNrd.dependencyMaskOffset,
        gNrd.width, gNrd.height, 0};
    list->SetComputeRootSignature(gNrd.dependencyRoot.Get());
    list->SetPipelineState(gNrd.dependencyUpdate.Get());
    list->SetComputeRoot32BitConstants(0, static_cast<uint32_t>(constants.size()), constants.data(), 0);
    list->SetComputeRootShaderResourceView(1, gNrd.dependencyRead.resource->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, static_cast<ID3D12Resource*>(input.paths)->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, static_cast<ID3D12Resource*>(input.changes)->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, gNrd.dependencyWrite.resource->GetGPUVirtualAddress());
    auto mask = gNrd.descriptors->GetGPUDescriptorHandleForHeapStart();
    mask.ptr += static_cast<uint64_t>(descriptor) * gNrd.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    list->SetComputeRootDescriptorTable(5, mask);
    const auto groups = (count + 63u) / 64u;
    list->Dispatch(std::min(groups, 65535u), (groups + 65534u) / 65535u, 1);
    ++gNrd.dependencyDispatches;
    const D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {}};
    list->ResourceBarrier(1, &barrier);
}

uint32_t dependencyTileCount(const NrdDependencyBinding& binding) {
    return ((binding[1] + binding[3] - 1u) / binding[3]) * ((binding[2] + binding[3] - 1u) / binding[3]);
}

void beginDependencyFrame(const mcdr_nrd_frame& frame, std::span<const nrd::DispatchDesc> dispatches,
    UINT descriptor, bool reset) {
    writeView(gNrd.device, gNrd.descriptors.Get(), descriptor, gNrd.dependencyMask.resource.Get(), DXGI_FORMAT_R32_UINT, true);
    if (reset) updateDependencies(frame, descriptor, 0, gNrd.dependencyEntries);
    copyDependencies(frame);
    updateDependencies(frame, descriptor, 4, gNrd.dependencyTiles);
    for (const auto& dispatch : dispatches) {
        if (!isTemporalAccumulation(gNrd.desc->pipelines[dispatch.pipelineIndex])) continue;
        if (frame.dependencies->changeCount == 0 && frame.dependencies->fullDomain == 0) continue;
        for (const auto& resource : std::span(dispatch.resources, dispatch.resourcesNum)) {
            if (resource.type != nrd::ResourceType::PERMANENT_POOL) continue;
            const auto binding = dependencyBinding(resource);
            updateDependencies(frame, descriptor, 3, dependencyTileCount(binding), binding, binding[0], dispatch.identifier);
        }
    }
    copyDependencies(frame);
    updateDependencies(frame, descriptor, 5, gNrd.width * gNrd.height);
    for (const auto& dispatch : dispatches) {
        if (!isTemporalAccumulation(gNrd.desc->pipelines[dispatch.pipelineIndex])) continue;
        for (const auto& resource : std::span(dispatch.resources, dispatch.resourcesNum)) {
            if (resource.type != nrd::ResourceType::PERMANENT_POOL) continue;
            const auto binding = dependencyBinding(resource);
            updateDependencies(frame, descriptor, 6, dependencyTileCount(binding), binding, 0, dispatch.identifier);
        }
    }
    for (uint32_t lobe = 0; lobe < gNrd.rawDependencies.size(); ++lobe)
        updateDependencies(frame, descriptor, 1, gNrd.dependencyTiles, gNrd.rawDependencies[lobe], 0, lobe);
}

void beginDependencyDispatch(const mcdr_nrd_frame& frame, const nrd::DispatchDesc& dispatch,
    UINT descriptor, uint32_t constantSlot) {
    copyDependencies(frame);
    std::array<NrdDependencyBinding, 32> layout;
    layout.fill({UINT32_MAX, 1, 1, 1});
    for (uint32_t index = 0; index < dispatch.resourcesNum; ++index) {
        const auto& resource = dispatch.resources[index];
        const auto binding = dependencyBinding(resource);
        layout[index] = binding;
        if (resource.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE)
            updateDependencies(frame, descriptor, 0, dependencyTileCount(binding), binding);
    }
    const uint64_t offset = uint64_t{constantSlot} * kDependencyConstantBytes;
    std::memcpy(gNrd.dependencyMapped + offset, layout.data(), sizeof(layout));
    auto* list = static_cast<ID3D12GraphicsCommandList*>(frame.list);
    list->SetComputeRootSignature(gNrd.rootSignature.Get());
    list->SetComputeRootDescriptorTable(3, gNrd.samplers->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootShaderResourceView(4, gNrd.dependencyRead.resource->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(5, gNrd.dependencyWrite.resource->GetGPUVirtualAddress());
    list->SetComputeRootConstantBufferView(6, gNrd.dependencyConstants.resource->GetGPUVirtualAddress() + offset);
}

void endDependencyDispatch(const mcdr_nrd_frame& frame, const nrd::DispatchDesc& dispatch, UINT descriptor) {
    for (const auto& resource : std::span(dispatch.resources, dispatch.resourcesNum)) {
        if (resource.descriptorType != nrd::DescriptorType::STORAGE_TEXTURE) continue;
        const auto binding = dependencyBinding(resource);
        updateDependencies(frame, descriptor, 2, dependencyTileCount(binding), binding, binding[0]);
    }
}

} // namespace

extern "C" int32_t mcdr_nrd_dispatch(const mcdr_nrd_frame* frame, char* outText, int32_t capacity) {
    if (outText != nullptr && capacity > 0) {
        outText[0] = '\0';
    }
    collectContexts();
    if (!gNrd.ready || gNrd.desc == nullptr || frame == nullptr || frame->list == nullptr
        || frame->completionFence == nullptr || frame->completionValue == 0 || gNrd.cancelled()
        || (frame->diffuseConfidence == nullptr) != (frame->specularConfidence == nullptr)
        || (frame->recording && (!frame->recordingState ||
            frame->recordingState(frame->recording.get()) != McdrNrdRecordingState::Pending))) {
        return -1;
    }
    ID3D12Device* device = gNrd.device;
    ID3D12GraphicsCommandList* list = static_cast<ID3D12GraphicsCommandList*>(frame->list);
    const nrd::InstanceDesc& desc = *gNrd.desc;
    int32_t used = 0;
    if (frame->historyRejection) {
        const auto description = static_cast<ID3D12Resource*>(frame->historyRejection)->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            (description.Format != DXGI_FORMAT_R8_UINT && description.Format != DXGI_FORMAT_R32_UINT) ||
            description.Width != gNrd.width || description.Height != gNrd.height ||
            description.MipLevels != 1 || description.DepthOrArraySize != 1 || description.SampleDesc.Count != 1 ||
            (description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
            appendLine(outText, capacity, used, "nrd: history rejection mask invalid\n");
            return -1;
        }
    }
    if (std::ranges::any_of(gNrd.slots, [frame](const auto& slot) {
        return slot.recording && slot.recording != frame->recording &&
            (!slot.recordingState || slot.recordingState(slot.recording.get()) != McdrNrdRecordingState::Submitted);
    })) {
        appendLine(outText, capacity, used, "nrd: another recording is pending\n");
        return -1;
    }
    auto availableSlot = std::ranges::find_if(gNrd.slots, &NrdDispatchSlot::complete);
    if (availableSlot == gNrd.slots.end()) {
        appendLine(outText, capacity, used, "nrd: GPU dispatch slots busy\n");
        return -1;
    }

    mcdr_nrd_frame dependencyFrame = *frame;
    const bool trackDependencies = frame->dependencies != nullptr;
    if (trackDependencies) {
        const auto& dependencies = *frame->dependencies;
        if (!dependencies.paths || !dependencies.changes || dependencies.worldEpoch == 0 ||
            dependencies.changeCount > 256 || dependencies.fullDomain > 1) return -1;
        const auto paths = static_cast<ID3D12Resource*>(dependencies.paths)->GetDesc();
        const auto changes = static_cast<ID3D12Resource*>(dependencies.changes)->GetDesc();
        const uint64_t tiles = uint64_t{(gNrd.width + 63u) / 64u} * ((gNrd.height + 63u) / 64u);
        if (paths.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || changes.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
            uint64_t{dependencies.pathOffset} + 64u + tiles * 128u > paths.Width ||
            uint64_t{dependencies.changeOffset} + 64u + uint64_t{dependencies.changeCount} * 64u > changes.Width) return -1;
        const auto prepared = prepareDependencies();
        if (prepared <= 0) {
            appendLine(outText, capacity, used, "nrd: dependencies unavailable (%d)\n", prepared);
            return prepared;
        }
        dependencyFrame.historyRejection = gNrd.dependencyMask.resource.Get();
        frame = &dependencyFrame;
    }
    const bool dependencyReset = trackDependencies != gNrd.dependencyActive ||
        (trackDependencies && gNrd.dependencyEpoch != frame->dependencies->worldEpoch);

    nrd::CommonSettings common{};
    for (int i = 0; i < 16; ++i) {
        common.viewToClipMatrix[i] = frame->viewToClip[i];
        common.viewToClipMatrixPrev[i] = frame->viewToClipPrev[i];
        common.worldToViewMatrix[i] = frame->worldToView[i];
        common.worldToViewMatrixPrev[i] = frame->worldToViewPrev[i];
    }
    common.motionVectorScale[0] = 1.0f / static_cast<float>(gNrd.width);
    common.motionVectorScale[1] = 1.0f / static_cast<float>(gNrd.height);
    common.motionVectorScale[2] = 0.0f;
    common.resourceSize[0] = static_cast<uint16_t>(gNrd.width);
    common.resourceSize[1] = static_cast<uint16_t>(gNrd.height);
    common.resourceSizePrev[0] = common.resourceSize[0];
    common.resourceSizePrev[1] = common.resourceSize[1];
    common.rectSize[0] = common.resourceSize[0];
    common.rectSize[1] = common.resourceSize[1];
    common.rectSizePrev[0] = common.rectSize[0];
    common.rectSizePrev[1] = common.rectSize[1];
    common.cameraJitter[0] = frame->cameraJitter[0];
    common.cameraJitter[1] = frame->cameraJitter[1];
    common.cameraJitterPrev[0] = frame->cameraJitterPrev[0];
    common.cameraJitterPrev[1] = frame->cameraJitterPrev[1];
    common.viewZScale = 1.0f;
    common.denoisingRange = 500000.0f;
    common.disocclusionThreshold = 0.01f;
    // 备用阈值与三个材质 ID 必须显式给：留 0 会被库判成非法参数，函数返回
    // INVALID_ARGUMENT 而设置照旧生效 —— 只看返回值不看内容根本发现不了。
    common.disocclusionThresholdAlternate = 0.01f;
    common.strandMaterialID = 999.0f;
    common.cameraAttachedReflectionMaterialID = 999.0f;
    common.historyFixAlternatePixelStrideMaterialID = 999.0f;
    common.frameIndex = frame->frameIndex;
    common.enableValidation = frame->validationOutput != nullptr;
    common.isHistoryConfidenceAvailable = frame->diffuseConfidence != nullptr;
    // 历史只在需要时丢：每帧 CLEAR_AND_RESTART 等于关掉时间累积，噪点全靠空间模糊。
    common.accumulationMode = frame->resetHistory || gNrd.resetPending || dependencyReset ? nrd::AccumulationMode::CLEAR_AND_RESTART
                                                  : nrd::AccumulationMode::CONTINUE;
    common.isMotionVectorInWorldSpace = false;
    if (nrd::SetCommonSettings(*gNrd.instance, common) != nrd::Result::SUCCESS) {
        appendLine(outText, capacity, used, "nrd: common settings rejected\n");
        return -1;
    }

    nrd::ReblurSettings settings{};
    settings.maxAccumulatedFrameNum = 30;
    settings.maxFastAccumulatedFrameNum = 6;
    settings.diffusePrepassBlurRadius = 30.0f;
    settings.minBlurRadius = 1.0f;
    settings.maxBlurRadius = 30.0f;
    settings.lobeAngleFraction = 0.15f;
    settings.roughnessFraction = 0.5f;
    settings.planeDistanceSensitivity = 0.01f;
    settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
    settings.maxStabilizedFrameNum = 0;
    for (const auto& denoiser : kDenoisers) {
        if (nrd::SetDenoiserSettings(*gNrd.instance, denoiser.identifier, &settings) != nrd::Result::SUCCESS) {
            appendLine(outText, capacity, used, "nrd: denoiser settings rejected\n");
            return -1;
        }
    }

    // identifier 表不能留空：空表时库回 SUCCESS + 0 条派发，看着成功、其实一个 pass 都没跑。
    const std::array<nrd::Identifier, 2> identifiers{0, 1};
    const nrd::DispatchDesc* dispatches = nullptr;
    uint32_t dispatchCount = 0;
    const nrd::Result gathered =
        nrd::GetComputeDispatches(*gNrd.instance, identifiers.data(),
            static_cast<uint32_t>(identifiers.size()), dispatches, dispatchCount);
    if (gathered != nrd::Result::SUCCESS || dispatches == nullptr || dispatchCount == 0) {
        appendLine(outText, capacity, used, "nrd: no dispatch (result=%d)\n",
                   static_cast<int32_t>(gathered));
        return -1;
    }

    uint32_t descriptorCount = trackDependencies ? 1u : 0u;
    uint32_t constantCount = 0;
    uint32_t rejectionCount = 0;
    for (const auto& dispatch : std::span(dispatches, dispatchCount)) {
        if (dispatch.pipelineIndex >= gNrd.pipelines.size()
            || dispatch.constantBufferDataSize > gNrd.constantStride) {
            appendLine(outText, capacity, used, "nrd: invalid dispatch layout\n");
            return -1;
        }
        const auto& pipeline = desc.pipelines[dispatch.pipelineIndex];
        if (trackDependencies && (dispatch.resourcesNum > 32 ||
            (!gNrd.dependencyPipelines[dispatch.pipelineIndex] &&
                !std::string_view(pipeline.shaderIdentifier).starts_with("Clear.cs.hlsl")))) {
            gNrd.resetPending = true;
            appendLine(outText, capacity, used, "nrd: dependency shader unsupported\n");
            return -1;
        }
        if (frame->historyRejection && isTemporalAccumulation(pipeline)) {
            if (!historyRejectionLayout(dispatch)) {
                gNrd.resetPending = true;
                appendLine(outText, capacity, used, "nrd: history rejection layout unsupported\n");
                return -1;
            }
            descriptorCount += kHistoryRejectionDescriptors;
            ++rejectionCount;
        }
        uint32_t resourceCount = 0;
        for (const auto& range : std::span(pipeline.resourceRanges, pipeline.resourceRangesNum)) {
            resourceCount += range.descriptorsNum;
        }
        if (resourceCount != dispatch.resourcesNum) {
            appendLine(outText, capacity, used, "nrd: resource layout mismatch\n");
            return -1;
        }
        for (const auto& resource : std::span(dispatch.resources, dispatch.resourcesNum)) {
            if (resolveResource(resource, frame, dispatch.identifier) == nullptr) {
                appendLine(outText, capacity, used, "nrd: dispatch resource missing\n");
                return -1;
            }
        }
        descriptorCount += resourceCount;
        constantCount += dispatch.constantBufferDataSize != 0;
    }
    if (descriptorCount > gNrd.descriptorsPerDispatch || constantCount > kConstantSlotsPerDispatch) {
        appendLine(outText, capacity, used, "nrd: dispatch capacity exceeded\n");
        return -1;
    }
    if (frame->historyRejection && (rejectionCount != kDenoisers.size() || !prepareHistoryRejection(*frame))) {
        gNrd.resetPending = true;
        appendLine(outText, capacity, used, "nrd: history rejection unavailable\n");
        return -1;
    }
    const auto slotIndex = static_cast<uint32_t>(availableSlot - gNrd.slots.begin());
    availableSlot->fence = static_cast<ID3D12Fence*>(frame->completionFence);
    availableSlot->value = frame->completionValue;
    availableSlot->recording = frame->recording;
    availableSlot->recordingState = frame->recordingState;
    const uint32_t blockSize = gNrd.descriptorsPerDispatch;
    const UINT blockBase = slotIndex * blockSize;
    const uint32_t constantBase = slotIndex * kConstantSlotsPerDispatch;

    ID3D12DescriptorHeap* heaps[2] = {gNrd.descriptors.Get(), gNrd.samplers.Get()};
    list->SetComputeRootSignature(gNrd.rootSignature.Get());
    list->SetDescriptorHeaps(2, heaps);
    list->SetComputeRootDescriptorTable(3, gNrd.samplers->GetGPUDescriptorHandleForHeapStart());
    gNrd.dependencyDispatches = 0;
    if (trackDependencies) beginDependencyFrame(*frame, {dispatches, dispatchCount}, blockBase,
        common.accumulationMode == nrd::AccumulationMode::CLEAR_AND_RESTART);

    const UINT increment = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_GPU_DESCRIPTOR_HANDLE heapGpu = gNrd.descriptors->GetGPUDescriptorHandleForHeapStart();
    uint32_t slotCursor = blockBase + (trackDependencies ? 1u : 0u);
    uint32_t constantCursor = constantBase;
    uint32_t missing = 0;
    // 首帧把每个 pass 的管线、线程组、描述符数量报一次：线程组与描述符数量对不上时派发链照跑不报错，
    // 只有挂死这一种症状，这一行表是唯一线索（见 docs/rt/README.md §23.3）。
    const bool trace = !gNrd.traced;
    for (uint32_t i = 0; i < dispatchCount; ++i) {
        const nrd::DispatchDesc& dispatchDesc = dispatches[i];
        if (dispatchDesc.pipelineIndex >= gNrd.pipelines.size()) {
            ++missing;
            continue;
        }
        const nrd::PipelineDesc& pipelineDesc = desc.pipelines[dispatchDesc.pipelineIndex];
        if (frame->historyRejection && isTemporalAccumulation(pipelineDesc)) {
            if (trackDependencies) dependencyTransition(list, gNrd.dependencyMask.resource.Get(),
                gNrd.dependencyMaskState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            rejectHistory(*frame, dispatchDesc, slotCursor);
            slotCursor += kHistoryRejectionDescriptors;
        }
        const bool trackedPipeline = trackDependencies && gNrd.dependencyPipelines[dispatchDesc.pipelineIndex];
        if (trackedPipeline) beginDependencyDispatch(*frame, dispatchDesc, blockBase, constantCursor);
        else {
            list->SetComputeRootSignature(gNrd.rootSignature.Get());
            list->SetComputeRootDescriptorTable(3, gNrd.samplers->GetGPUDescriptorHandleForHeapStart());
        }
        UINT srvCount = 0;
        UINT uavCount = 0;
        for (uint32_t r = 0; r < pipelineDesc.resourceRangesNum; ++r) {
            const nrd::ResourceRangeDesc& range = pipelineDesc.resourceRanges[r];
            if (range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE) {
                uavCount += range.descriptorsNum;
            } else {
                srvCount += range.descriptorsNum;
            }
        }
        if (slotCursor + srvCount + uavCount > blockBase + blockSize) {
            ++missing;
            break;
        }
        const UINT srvBase = slotCursor;
        const UINT uavBase = slotCursor + srvCount;
        slotCursor += srvCount + uavCount;

        UINT srvCursor = srvBase;
        UINT uavCursor = uavBase;
        UINT read = 0;
        for (uint32_t r = 0; r < pipelineDesc.resourceRangesNum; ++r) {
            const nrd::ResourceRangeDesc& range = pipelineDesc.resourceRanges[r];
            const bool storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            for (uint32_t j = 0; j < range.descriptorsNum; ++j) {
                if (read >= dispatchDesc.resourcesNum) {
                    break;
                }
                const nrd::ResourceDesc& resourceDesc = dispatchDesc.resources[read++];
                ID3D12Resource* resource = resolveResource(resourceDesc, frame, dispatchDesc.identifier);
                if (resource == nullptr) {
                    ++missing;
                    ++(storage ? uavCursor : srvCursor);
                    continue;
                }
                // 池纹理在同一帧里会被不同 pass 当 SRV 读、当 UAV 写：状态逐次显式切换。
                // 只靠 COMMON 的隐式提升时，同一张纹理在同一个命令列表里换身份访问就是未定义行为，
                // 表现是 GPU 偶发挂死而调试层一声不响。
                D3D12_RESOURCE_STATES* poolState = nullptr;
                if (resourceDesc.type == nrd::ResourceType::PERMANENT_POOL &&
                    resourceDesc.indexInPool < gNrd.permanentState.size()) {
                    poolState = &gNrd.permanentState[resourceDesc.indexInPool];
                } else if (resourceDesc.type == nrd::ResourceType::TRANSIENT_POOL &&
                           resourceDesc.indexInPool < gNrd.transientState.size()) {
                    poolState = &gNrd.transientState[resourceDesc.indexInPool];
                }
                if (poolState != nullptr) {
                    const D3D12_RESOURCE_STATES want = storage
                        ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    if (*poolState != want) {
                        const D3D12_RESOURCE_BARRIER transition = {
                            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                            D3D12_RESOURCE_BARRIER_FLAG_NONE,
                            {{resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, *poolState, want}}};
                        list->ResourceBarrier(1, &transition);
                        *poolState = want;
                    }
                }
                // 视图格式必须与资源一致：类型化格式不符时调试层拒建视图、描述符留空，
                // 该 pass 读到垃圾而不报错。池纹理与输入纹理都按自己的描述给。
                const DXGI_FORMAT format = resource->GetDesc().Format;
                writeView(device, gNrd.descriptors.Get(), storage ? uavCursor++ : srvCursor++,
                          resource, format, storage);
            }
        }

        if (dispatchDesc.constantBufferDataSize > 0
            && constantCursor < constantBase + kConstantSlotsPerDispatch) {
            const uint64_t offset = static_cast<uint64_t>(constantCursor) * gNrd.constantStride;
            std::memcpy(gNrd.constantMapped + offset, dispatchDesc.constantBufferData,
                        dispatchDesc.constantBufferDataSize);
            list->SetComputeRootConstantBufferView(
                0, gNrd.constants.resource->GetGPUVirtualAddress() + offset);
            ++constantCursor;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = heapGpu;
        srvHandle.ptr += static_cast<UINT64>(srvBase) * increment;
        D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = heapGpu;
        uavHandle.ptr += static_cast<UINT64>(uavBase) * increment;
        list->SetComputeRootDescriptorTable(1, srvHandle);
        list->SetComputeRootDescriptorTable(2, uavHandle);
        list->SetPipelineState(trackedPipeline ? gNrd.dependencyPipelines[dispatchDesc.pipelineIndex].Get()
            : gNrd.pipelines[dispatchDesc.pipelineIndex].Get());
        list->Dispatch(dispatchDesc.gridWidth, dispatchDesc.gridHeight, 1);
        if (trace) {
            appendLine(outText, capacity, used, "nrd: p%u %s pipe=%u grid=%ux%u srv=%u uav=%u\n", i,
                       dispatchDesc.name != nullptr ? dispatchDesc.name : "-",
                       static_cast<uint32_t>(dispatchDesc.pipelineIndex), dispatchDesc.gridWidth,
                       dispatchDesc.gridHeight, srvCount, uavCount);
        }
        // 同一帧里连续派发、纹理状态不变，D3D12 不会自动给 UAV 之间排序：
        // 少了这道全局屏障，后一个 pass 可能读到前一个 pass 写之前的像素。
        const D3D12_RESOURCE_BARRIER hazard = {
            D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {}};
        list->ResourceBarrier(1, &hazard);
        if (trackedPipeline) endDependencyDispatch(*frame, dispatchDesc, blockBase);
    }

    if (trace) {

        struct Named {
            const char* name;
            void* handle;
        };
        const Named inputs[7] = {{"mv", frame->motion},
                                 {"nr", frame->normalRoughness},
                                 {"viewz", frame->viewZ},
                                 {"in", frame->radianceIn},
                                 {"out", frame->radianceOut},
                                 {"spec-in", frame->specularIn},
                                 {"spec-out", frame->specularOut}};
        for (const Named& input : inputs) {
            const D3D12_RESOURCE_DESC texture =
                static_cast<ID3D12Resource*>(input.handle)->GetDesc();
            appendLine(outText, capacity, used, "nrd: %s %llux%llu fmt=%u\n", input.name,
                       static_cast<unsigned long long>(texture.Width),
                       static_cast<unsigned long long>(texture.Height),
                       static_cast<uint32_t>(texture.Format));
        }
        gNrd.traced = true;
    }

    if (trackDependencies) {
        dependencyTransition(list, gNrd.dependencyRead.resource.Get(), gNrd.dependencyReadState,
            D3D12_RESOURCE_STATE_COMMON);
        dependencyTransition(list, gNrd.dependencyWrite.resource.Get(), gNrd.dependencyWriteState,
            D3D12_RESOURCE_STATE_COMMON);
    }
    const auto totalDispatches = dispatchCount + rejectionCount + gNrd.dependencyDispatches;
    appendLine(outText, capacity, used, "nrd: dispatch passes=%u missing=%u\n", totalDispatches,
               missing);
    if (missing == 0) {
        gNrd.resetPending = false;
        gNrd.dependencyActive = trackDependencies;
        gNrd.dependencyEpoch = trackDependencies ? frame->dependencies->worldEpoch : 0;
    }
    return missing == 0 ? static_cast<int32_t>(totalDispatches) : -1;
}

extern "C" int32_t mcdr_nrd_context_shutdown(void) {
    retireContext();
    return 1;
}

extern "C" void mcdr_nrd_context_collect(void) {
    collectContexts();
}

#else

extern "C" int32_t MCDR_CALL mcdr_dx12NrdInspect(char* outText, int32_t capacity) {
    if (outText != nullptr && capacity > 0) {
        std::snprintf(outText, static_cast<size_t>(capacity), "nrd: not built in\n");
    }
    return 0;
}

extern "C" int32_t mcdr_nrd_dispatch(const mcdr_nrd_frame* frame, char* outText,
                                     int32_t capacity) {
    (void)frame;
    if (outText != nullptr && capacity > 0) {
        std::snprintf(outText, static_cast<size_t>(capacity), "nrd: not built in\n");
    }
    return 0;
}

extern "C" int32_t mcdr_nrd_context_init(void* device, int32_t width, int32_t height,
                                         McdrNrdReserveMemory reserveMemory, char* outText, int32_t capacity) {
    (void)device;
    (void)width;
    (void)height;
    (void)reserveMemory;
    if (outText != nullptr && capacity > 0) {
        std::snprintf(outText, static_cast<size_t>(capacity), "nrd: not built in\n");
    }
    return 0;
}

extern "C" int32_t mcdr_nrd_context_shutdown(void) {
    return 0;
}

extern "C" void mcdr_nrd_context_collect(void) {}

#endif
