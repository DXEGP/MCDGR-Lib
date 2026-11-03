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
#include <vector>

#include "NRD.h"
#include "mcdr_nrd_impl.h"

namespace {

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

    const nrd::DenoiserDesc denoisers[1] = {
        {0, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR},
    };
    nrd::InstanceCreationDesc creation = {};
    creation.denoisers = denoisers;
    creation.denoisersNum = 1;

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

    D3D12_ROOT_PARAMETER params[4]{};
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

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 4;
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

bool createPoolTexture(ID3D12Device* device, const nrd::TextureDesc& desc, uint32_t width,
                       uint32_t height, ID3D12Resource** out, const char** stage) {
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
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(out)))) {
        *stage = "create pool texture";
        return false;
    }
    return true;
}

constexpr uint32_t kDispatchSlots = 8;
constexpr uint32_t kConstantSlotsPerDispatch = 32;

struct NrdDispatchSlot {
    ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;

    bool complete() const {
        return !fence || fence->GetCompletedValue() >= value;
    }
};

struct NrdContext {
    nrd::Instance* instance = nullptr;
    ComPtr<ID3D12RootSignature> rootSignature;
    std::vector<ComPtr<ID3D12PipelineState>> pipelines;
    std::vector<ComPtr<ID3D12Resource>> permanent;
    std::vector<ComPtr<ID3D12Resource>> transient;
    /** 池纹理当前状态：同一张纹理既当 SRV 又当 UAV，状态必须自己记。 */
    std::vector<D3D12_RESOURCE_STATES> permanentState;
    std::vector<D3D12_RESOURCE_STATES> transientState;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12DescriptorHeap> samplers;
    ComPtr<ID3D12Resource> constants;
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

    bool complete() const {
        return std::ranges::all_of(slots, &NrdDispatchSlot::complete);
    }

    void release() {
        pipelines.clear();
        permanent.clear();
        transient.clear();
        permanentState.clear();
        transientState.clear();
        if (constantMapped != nullptr && constants) {
            constants->Unmap(0, nullptr);
            constantMapped = nullptr;
        }
        constants.Reset();
        samplers.Reset();
        descriptors.Reset();
        rootSignature.Reset();
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
                                         char* outText, int32_t capacity) {
    if (outText != nullptr && capacity > 0) {
        outText[0] = '\0';
    }
    if (device == nullptr || width <= 0 || height <= 0) {
        return -1;
    }
    collectContexts();
    if (gNrd.ready && gNrd.device == device && gNrd.width == static_cast<uint32_t>(width)
        && gNrd.height == static_cast<uint32_t>(height)) {
        return 1;
    }
    retireContext();

    int32_t used = 0;
    const char* stage = "create instance";
    const nrd::DenoiserDesc denoisers[1] = {{0, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR}};
    nrd::InstanceCreationDesc creation{};
    creation.denoisers = denoisers;
    creation.denoisersNum = 1;
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

    ID3D12RootSignature* root = nullptr;
    if (!createRootSignature(static_cast<ID3D12Device*>(device), *desc, &root, &stage)) {
        gNrd.release();
        appendLine(outText, capacity, used, "nrd: context failed, %s\n", stage);
        return -1;
    }
    gNrd.rootSignature.Attach(root);

    gNrd.pipelines.resize(desc->pipelinesNum);
    uint32_t created = 0;
    for (uint32_t i = 0; i < desc->pipelinesNum; ++i) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = gNrd.rootSignature.Get();
        pso.CS.pShaderBytecode = desc->pipelines[i].computeShaderDXIL.bytecode;
        pso.CS.BytecodeLength = desc->pipelines[i].computeShaderDXIL.size;
        pso.NodeMask = 0;
        if (FAILED(static_cast<ID3D12Device*>(device)->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&gNrd.pipelines[i])))) {
            stage = "create pipeline state";
            appendLine(outText, capacity, used, "nrd: context failed, %s %u\n", stage, i);
            gNrd.release();
            return -1;
        }
        ++created;
    }

    gNrd.width = static_cast<uint32_t>(width);
    gNrd.height = static_cast<uint32_t>(height);
    gNrd.permanent.resize(desc->permanentPoolSize);
    uint32_t permanent = 0;
    for (uint32_t i = 0; i < desc->permanentPoolSize; ++i) {
        if (!createPoolTexture(static_cast<ID3D12Device*>(device), desc->permanentPool[i],
                               gNrd.width, gNrd.height, &gNrd.permanent[i], &stage)) {
            appendLine(outText, capacity, used, "nrd: context failed, %s %u\n", stage, i);
            gNrd.release();
            return -1;
        }
        ++permanent;
    }
    gNrd.transient.resize(desc->transientPoolSize);
    uint32_t transient = 0;
    for (uint32_t i = 0; i < desc->transientPoolSize; ++i) {
        if (!createPoolTexture(static_cast<ID3D12Device*>(device), desc->transientPool[i],
                               gNrd.width, gNrd.height, &gNrd.transient[i], &stage)) {
            appendLine(outText, capacity, used, "nrd: context failed, %s %u\n", stage, i);
            gNrd.release();
            return -1;
        }
        ++transient;
    }

    const uint32_t perSet = desc->descriptorPoolDesc.perSetTexturesMaxNum
                          + desc->descriptorPoolDesc.perSetStorageTexturesMaxNum;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    gNrd.descriptorsPerDispatch = perSet * desc->descriptorPoolDesc.setsMaxNum;
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
    if (FAILED(static_cast<ID3D12Device*>(device)->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &ringDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&gNrd.constants)))
        || FAILED(gNrd.constants->Map(0, nullptr, reinterpret_cast<void**>(&gNrd.constantMapped)))) {
        appendLine(outText, capacity, used, "nrd: context failed, %s\n", stage);
        gNrd.release();
        return -1;
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

ID3D12Resource* resolveResource(const nrd::ResourceDesc& desc, const mcdr_nrd_frame* frame) {
    switch (desc.type) {
        case nrd::ResourceType::IN_MV:
            return static_cast<ID3D12Resource*>(frame->motion);
        case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
            return static_cast<ID3D12Resource*>(frame->normalRoughness);
        case nrd::ResourceType::IN_VIEWZ:
            return static_cast<ID3D12Resource*>(frame->viewZ);
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
            return index < gNrd.permanent.size() ? gNrd.permanent[index].Get() : nullptr;
        }
        case nrd::ResourceType::TRANSIENT_POOL: {
            const uint32_t index = desc.indexInPool;
            return index < gNrd.transient.size() ? gNrd.transient[index].Get() : nullptr;
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

} // namespace

extern "C" int32_t mcdr_nrd_dispatch(const mcdr_nrd_frame* frame, char* outText, int32_t capacity) {
    if (outText != nullptr && capacity > 0) {
        outText[0] = '\0';
    }
    collectContexts();
    if (!gNrd.ready || gNrd.desc == nullptr || frame == nullptr || frame->list == nullptr
        || frame->completionFence == nullptr || frame->completionValue == 0) {
        return -1;
    }
    ID3D12Device* device = gNrd.device;
    ID3D12GraphicsCommandList* list = static_cast<ID3D12GraphicsCommandList*>(frame->list);
    const nrd::InstanceDesc& desc = *gNrd.desc;
    int32_t used = 0;

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
    // 历史只在需要时丢：每帧 CLEAR_AND_RESTART 等于关掉时间累积，噪点全靠空间模糊。
    common.accumulationMode = frame->resetHistory ? nrd::AccumulationMode::CLEAR_AND_RESTART
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
    if (nrd::SetDenoiserSettings(*gNrd.instance, 0, &settings) != nrd::Result::SUCCESS) {
        appendLine(outText, capacity, used, "nrd: denoiser settings rejected\n");
        return -1;
    }

    // identifier 表不能留空：空表时库回 SUCCESS + 0 条派发，看着成功、其实一个 pass 都没跑。
    const nrd::Identifier identifiers[1] = {0};
    const nrd::DispatchDesc* dispatches = nullptr;
    uint32_t dispatchCount = 0;
    const nrd::Result gathered =
        nrd::GetComputeDispatches(*gNrd.instance, identifiers, 1, dispatches, dispatchCount);
    if (gathered != nrd::Result::SUCCESS || dispatches == nullptr || dispatchCount == 0) {
        appendLine(outText, capacity, used, "nrd: no dispatch (result=%d)\n",
                   static_cast<int32_t>(gathered));
        return -1;
    }

    uint32_t descriptorCount = 0;
    uint32_t constantCount = 0;
    for (const auto& dispatch : std::span(dispatches, dispatchCount)) {
        if (dispatch.pipelineIndex >= gNrd.pipelines.size()
            || dispatch.constantBufferDataSize > gNrd.constantStride) {
            appendLine(outText, capacity, used, "nrd: invalid dispatch layout\n");
            return -1;
        }
        const auto& pipeline = desc.pipelines[dispatch.pipelineIndex];
        uint32_t resourceCount = 0;
        for (const auto& range : std::span(pipeline.resourceRanges, pipeline.resourceRangesNum)) {
            resourceCount += range.descriptorsNum;
        }
        if (resourceCount != dispatch.resourcesNum) {
            appendLine(outText, capacity, used, "nrd: resource layout mismatch\n");
            return -1;
        }
        for (const auto& resource : std::span(dispatch.resources, dispatch.resourcesNum)) {
            if (resolveResource(resource, frame) == nullptr) {
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
    auto availableSlot = std::ranges::find_if(gNrd.slots, &NrdDispatchSlot::complete);
    if (availableSlot == gNrd.slots.end()) {
        appendLine(outText, capacity, used, "nrd: GPU dispatch slots busy\n");
        return -1;
    }
    const auto slotIndex = static_cast<uint32_t>(availableSlot - gNrd.slots.begin());
    availableSlot->fence = static_cast<ID3D12Fence*>(frame->completionFence);
    availableSlot->value = frame->completionValue;
    const uint32_t blockSize = gNrd.descriptorsPerDispatch;
    const UINT blockBase = slotIndex * blockSize;
    const uint32_t constantBase = slotIndex * kConstantSlotsPerDispatch;

    ID3D12DescriptorHeap* heaps[2] = {gNrd.descriptors.Get(), gNrd.samplers.Get()};
    list->SetComputeRootSignature(gNrd.rootSignature.Get());
    list->SetDescriptorHeaps(2, heaps);
    list->SetComputeRootDescriptorTable(3, gNrd.samplers->GetGPUDescriptorHandleForHeapStart());

    const UINT increment = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_GPU_DESCRIPTOR_HANDLE heapGpu = gNrd.descriptors->GetGPUDescriptorHandleForHeapStart();
    uint32_t slotCursor = blockBase;
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
                ID3D12Resource* resource = resolveResource(resourceDesc, frame);
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
                0, gNrd.constants->GetGPUVirtualAddress() + offset);
            ++constantCursor;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = heapGpu;
        srvHandle.ptr += static_cast<UINT64>(srvBase) * increment;
        D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = heapGpu;
        uavHandle.ptr += static_cast<UINT64>(uavBase) * increment;
        list->SetComputeRootDescriptorTable(1, srvHandle);
        list->SetComputeRootDescriptorTable(2, uavHandle);
        list->SetPipelineState(gNrd.pipelines[dispatchDesc.pipelineIndex].Get());
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

    appendLine(outText, capacity, used, "nrd: dispatch passes=%u missing=%u\n", dispatchCount,
               missing);
    return missing == 0 ? static_cast<int32_t>(dispatchCount) : -1;
}

extern "C" int32_t mcdr_nrd_context_shutdown(void) {
    retireContext();
    return 1;
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
                                         char* outText, int32_t capacity) {
    (void)device;
    (void)width;
    (void)height;
    if (outText != nullptr && capacity > 0) {
        std::snprintf(outText, static_cast<size_t>(capacity), "nrd: not built in\n");
    }
    return 0;
}

extern "C" int32_t mcdr_nrd_context_shutdown(void) {
    return 0;
}

#endif
