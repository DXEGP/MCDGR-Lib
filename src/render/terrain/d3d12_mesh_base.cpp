#include "_d3d12_module_prelude.h"
#include "mcdr_graphics_capture.h"
#include "mcdr_graphics_recording.h"
#include "mcdr_oit_budget.h"
#include "mcdr_terrain_tile.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

#include "mcdr_abi.h"

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.render.vanilla;
import mcdr.error;

namespace {

std::string hrHex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return std::string(buf);
}

D3D12_COMPARISON_FUNC toCompare(int op) {
    switch (op) {
        case 0: return D3D12_COMPARISON_FUNC_ALWAYS;
        case 1: return D3D12_COMPARISON_FUNC_LESS;
        case 2: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case 3: return D3D12_COMPARISON_FUNC_EQUAL;
        case 4: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case 5: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case 6: return D3D12_COMPARISON_FUNC_GREATER;
        case 7: return D3D12_COMPARISON_FUNC_NEVER;
        default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

D3D12_BLEND meshBlendFactor(uint8_t f) {
    constexpr D3D12_BLEND factors[] = {
        D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_DEST_ALPHA,
        D3D12_BLEND_DEST_COLOR, D3D12_BLEND_ONE, D3D12_BLEND_INV_BLEND_FACTOR,
        D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_SRC_ALPHA,
        D3D12_BLEND_SRC_ALPHA_SAT, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_ZERO};
    return f < 15 ? factors[f] : D3D12_BLEND_ONE;
}

D3D12_BLEND_OP meshBlendOp(uint8_t op) {
    return op < 5 ? static_cast<D3D12_BLEND_OP>(D3D12_BLEND_OP_ADD + op) : D3D12_BLEND_OP_ADD;
}

D3D12_STATIC_SAMPLER_DESC makeStaticSampler(uint8_t reg) {
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    s.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.MipLODBias = 0.0f;
    s.MaxAnisotropy = 1;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    s.MinLOD = 0.0f;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = reg;
    s.RegisterSpace = 0;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return s;
}

#pragma warning(push)
#pragma warning(disable : 4324)
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Payload>
struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    Payload payload{};
};
#pragma warning(pop)

using StreamRenderTargets = D3D12_RT_FORMAT_ARRAY;

inline void appendSubobjects(std::vector<unsigned char>&) {}

template <typename T, typename... Rest>
void appendSubobjects(std::vector<unsigned char>& bytes, const T& first, const Rest&... rest) {
    const size_t at = bytes.size();
    bytes.resize(at + sizeof(T));
    std::memcpy(bytes.data() + at, &first, sizeof(T));
    appendSubobjects(bytes, rest...);
}

DXGI_FORMAT depthStencilFormat(const d3d12_mc::MeshPipelineDesc& desc) {
    return desc.hasDepth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
}

}

namespace d3d12_mc {

bool meshPipelineSupported(std::string* why) {
    auto reject = [why](const char* reason) {
        if (why != nullptr) *why = reason;
        return false;
    };

    DeviceContext& ctx = deviceContext();
    if (!ctx.device) return reject("device not initialized");

    if (ctx.caps.featureLevel < D3D_FEATURE_LEVEL_12_2)
        return reject("feature level below 12_2 (project support policy; mesh API starts at SM 6.5)");
    if (!ctx.caps.meshShadersSupported())
        return reject("D3D12 mesh shader tier is 0 (not supported by this adapter)");
    if (!ctx.caps.dxcAvailable) return reject("dxcompiler.dll not available");

    ComPtr<ID3D12Device2> device2;
    if (FAILED(ctx.device.As(&device2)) || !device2)
        return reject("ID3D12Device2 is not available (CreatePipelineState required)");

    if (why != nullptr) why->clear();
    return true;
}

struct MeshRootSigInfo {
    std::vector<PipelineDesc::Binding> bindings;
    UINT segmentParam = 0;
    UINT geometryParam = 0;
    bool visibleWork = false;
    UINT constantBytes = 0;
    UINT constantParam = UINT_MAX;
    UINT captureParam = UINT_MAX;
    uint64_t captureHash = 0;
    std::shared_ptr<void> captureBudget;
    bool tileExtras = false;
    UINT tileRootParam = UINT_MAX;
    UINT tileBatchParam = UINT_MAX;
    UINT tileDescriptorParam = UINT_MAX;
    bool registered = false;
};

std::mutex gMeshRootSigMutex;
std::unordered_map<uint64_t, MeshRootSigInfo> gMeshRootSigs;

void recordMeshRootSig(uint64_t handle, const std::vector<PipelineDesc::Binding>& bindings,
                       UINT segmentParam, UINT geometryParam, bool visibleWork,
                       UINT constantBytes, UINT constantParam, UINT captureParam, uint64_t captureHash,
                       std::shared_ptr<void> captureBudget, bool tileExtras, UINT tileRootParam,
                       UINT tileBatchParam, UINT tileDescriptorParam) {
    std::lock_guard<std::mutex> lock(gMeshRootSigMutex);
    auto& info = gMeshRootSigs[handle];
    info = MeshRootSigInfo{bindings, segmentParam, geometryParam, visibleWork, constantBytes,
        constantParam, captureParam, captureHash, std::move(captureBudget), tileExtras, tileRootParam,
        tileBatchParam, tileDescriptorParam};
    info.registered = true;
}

MeshRootSigInfo lookupMeshRootSig(uint64_t handle) {
    std::lock_guard<std::mutex> lock(gMeshRootSigMutex);
    const auto it = gMeshRootSigs.find(handle);
    return it == gMeshRootSigs.end() ? MeshRootSigInfo{} : it->second;
}

void forgetMeshRootSig(uint64_t handle) {
    std::lock_guard<std::mutex> lock(gMeshRootSigMutex);
    gMeshRootSigs.erase(handle);
}

namespace {
mcdr::Result<void*> createMeshPipelineImpl(const MeshPipelineDesc& desc, uint64_t externalRootSignature) {
    if (const auto r = ensureDevice(); !r)
        return mcdr::err("createMeshPipeline", r.error().message);
    std::string why;
    if (!meshPipelineSupported(&why)) return mcdr::err("createMeshPipeline", why);

    if (desc.msDxil.empty()) {
        return mcdr::err("createMeshPipeline", "msDxil is empty (mesh shader bytecode is required)");
    }
    if (desc.captureHash) {
        if (const auto supported = requireCaptureBarycentrics(); !supported)
            return std::unexpected(supported.error());
        if (const auto capture=validateCapturePixelShader(desc.psDxil.data(),static_cast<uint32_t>(desc.psDxil.size())); !capture)
            return std::unexpected(capture.error());
    }
    if (desc.colorCount < 0 || desc.colorCount > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ||
        desc.colorTargets.size() != static_cast<size_t>(desc.colorCount)) {
        return mcdr::err("createMeshPipeline", "color target count does not match its descriptors");
    }
    if (desc.rasterMotion && (!desc.hasDepth || desc.psDxil.empty() || desc.colorCount < 2 ||
        toDxgiFormat(desc.colorTargets.back().format) != DXGI_FORMAT_R32G32B32A32_FLOAT ||
        desc.colorTargets.back().writeMask != D3D12_COLOR_WRITE_ENABLE_ALL)) {
        return mcdr::err("createMeshPipeline", "raster motion requires depth, a pixel shader and a final full RGBA32_FLOAT raw target");
    }

    ComPtr<ID3D12Device2> device2;
    HRESULT hr = deviceContext().device.As(&device2);
    if (FAILED(hr) || !device2) {
        return mcdr::err("createMeshPipeline", "ID3D12Device2 unavailable hr=" + hrHex(hr));
    }

    std::vector<uint8_t> cbvRegs;
    std::vector<D3D12_DESCRIPTOR_RANGE> srvRanges;
    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
    cbvRegs.reserve(desc.bindings.size());
    srvRanges.reserve(desc.bindings.size());
    staticSamplers.reserve(desc.bindings.size());
    for (const PipelineDesc::Binding& b : desc.bindings) {
        if (b.type == 0) {
            cbvRegs.push_back(b.reg);
            continue;
        }
        D3D12_DESCRIPTOR_RANGE r{};
        r.NumDescriptors = 1;
        r.RegisterSpace = 0;
        r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        r.BaseShaderRegister = b.reg;
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges.push_back(r);
        if (b.type == 1) staticSamplers.push_back(makeStaticSampler(b.reg));
    }

    std::vector<D3D12_ROOT_PARAMETER> params;
    params.reserve(cbvRegs.size() + 1);
    for (uint8_t reg : cbvRegs) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = reg;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    if (!srvRanges.empty()) {
        D3D12_ROOT_DESCRIPTOR_TABLE table{};
        table.NumDescriptorRanges = static_cast<UINT>(srvRanges.size());
        table.pDescriptorRanges = srvRanges.data();
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable = table;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(params.size());
    rsDesc.pParameters = params.empty() ? nullptr : params.data();
    rsDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
    rsDesc.pStaticSamplers = staticSamplers.empty() ? nullptr : staticSamplers.data();
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3D12RootSignature> rootSig;
    UINT meshSegmentParam = 0;
    UINT meshGeometryParam = 0;
    bool meshVisibleWork = false;
    UINT meshConstantParam = static_cast<UINT>(params.size());
    UINT captureParam = UINT_MAX;
    D3D12_DESCRIPTOR_RANGE captureRanges[2]{};
    if (desc.captureHash && (desc.captureHash != mcdr::oit_shader::contractHash || !desc.hasDepth ||
        desc.depthWrite || desc.psDxil.empty() || desc.rasterMotion || desc.captureGeneration == 0))
        return mcdr::err("createMeshPipeline", "Invalid capture shader contract or depth state");
    if (desc.pushConstantBytes > 128 || desc.pushConstantBytes % 4 != 0 ||
        cbvRegs.size() * 2 + (srvRanges.empty() ? 0 : 1) + desc.pushConstantBytes / 4 + (desc.captureHash ? 4 : 0) > 64) {
        return mcdr::err("createMeshPipeline", "root constant size or root DWORD budget exceeded");
    }
    if (externalRootSignature != 0) {
        const MeshRootSigInfo info = lookupMeshRootSig(externalRootSignature);
        if (!info.registered)
            return mcdr::err("createMeshPipeline", "External root signature metadata is unavailable");
        if (info.constantBytes != desc.pushConstantBytes || info.captureHash != desc.captureHash)
            return mcdr::err("createMeshPipeline", "External root layout does not match capture pipeline");
        if (info.bindings.size() != desc.bindings.size() || !std::equal(info.bindings.begin(), info.bindings.end(),
                desc.bindings.begin(), [](const PipelineDesc::Binding& expected, const PipelineDesc::Binding& actual) {
                    return expected.type == actual.type && expected.reg == actual.reg;
                }))
            return mcdr::err("createMeshPipeline", "External root binding type/register layout differs");
        ID3D12RootSignature* external = reinterpret_cast<ID3D12RootSignature*>(
            static_cast<uintptr_t>(externalRootSignature));
        external->AddRef();
        rootSig.Attach(external);
        meshConstantParam = info.constantParam;
        captureParam = info.captureParam;
        meshSegmentParam = info.segmentParam;
        meshGeometryParam = info.geometryParam;
        meshVisibleWork = info.visibleWork;
        if (info.tileExtras) {
            if (info.tileRootParam == UINT_MAX || info.tileBatchParam == UINT_MAX ||
                info.tileDescriptorParam == UINT_MAX)
                return mcdr::err("createMeshPipeline", "External root layout has incomplete terrain tile parameters");
        }
        dbgLogInfo("createMeshPipeline: rs=0x%llx segParam=%u geoParam=%u as=%d\n",
                   static_cast<unsigned long long>(externalRootSignature), info.segmentParam,
                   info.geometryParam, desc.asDxil.empty() ? 0 : 1);
    } else {
    if (desc.pushConstantBytes != 0) {
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = {0, 1, desc.pushConstantBytes / 4};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(parameter);
        rsDesc.NumParameters = static_cast<UINT>(params.size());
        rsDesc.pParameters = params.data();
    }
    if (desc.captureHash) {
        captureParam = mcdr::graphics_capture::appendParameters(params, captureRanges);
        rsDesc.NumParameters = static_cast<UINT>(params.size());
        rsDesc.pParameters = params.data();
    }
    ComPtr<ID3DBlob> rsBlob, rsErr;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    versioned.Desc_1_0 = rsDesc;
    hr = D3D12SerializeVersionedRootSignature(&versioned, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        std::string msg = "D3D12SerializeVersionedRootSignature hr=" + hrHex(hr);
        if (rsErr && rsErr->GetBufferSize() > 0) {
            msg += " ";
            msg.append(static_cast<const char*>(rsErr->GetBufferPointer()), rsErr->GetBufferSize());
        }
        return mcdr::err("createMeshPipeline", std::move(msg));
    }
    hr = deviceContext().device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                                     IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        return mcdr::err("createMeshPipeline", "CreateRootSignature hr=" + hrHex(hr));
    }
    }

    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = TRUE;
    for (D3D12_RENDER_TARGET_BLEND_DESC& slot : blend.RenderTarget) {
        slot.BlendEnable = FALSE;
        slot.LogicOpEnable = FALSE;
        slot.SrcBlend = D3D12_BLEND_ONE;
        slot.DestBlend = D3D12_BLEND_ZERO;
        slot.BlendOp = D3D12_BLEND_OP_ADD;
        slot.SrcBlendAlpha = D3D12_BLEND_ONE;
        slot.DestBlendAlpha = D3D12_BLEND_ZERO;
        slot.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        slot.LogicOp = D3D12_LOGIC_OP_NOOP;
        slot.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    int numRT = 0;
    for (int i = 0; i < desc.colorCount; ++i) {
        const PipelineDesc::ColorTarget& ct = desc.colorTargets[static_cast<size_t>(i)];
        if (ct.format < 0) continue;
        numRT = i + 1;
        D3D12_RENDER_TARGET_BLEND_DESC& rt = blend.RenderTarget[i];
        rt.BlendEnable = ct.blendEnabled;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = ct.blendEnabled ? meshBlendFactor(ct.srcColor) : D3D12_BLEND_ONE;
        rt.DestBlend = ct.blendEnabled ? meshBlendFactor(ct.dstColor) : D3D12_BLEND_ZERO;
        rt.BlendOp = meshBlendOp(ct.colorOp);
        rt.SrcBlendAlpha = ct.blendEnabled ? meshBlendFactor(ct.srcAlpha) : D3D12_BLEND_ONE;
        rt.DestBlendAlpha = ct.blendEnabled ? meshBlendFactor(ct.dstAlpha) : D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = meshBlendOp(ct.alphaOp);
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = ct.writeMask;
    }

    D3D12_RASTERIZER_DESC rs{};
    rs.FillMode = (desc.polygonMode == 1) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    rs.CullMode = desc.cullEnabled ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    rs.FrontCounterClockwise = TRUE;
    rs.DepthBias = 0;
    rs.DepthBiasClamp = 0.0f;
    rs.SlopeScaledDepthBias = 0.0f;
    rs.DepthClipEnable = TRUE;
    rs.MultisampleEnable = FALSE;
    rs.AntialiasedLineEnable = FALSE;
    rs.ForcedSampleCount = 0;
    rs.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = desc.hasDepth ? TRUE : FALSE;
    ds.DepthWriteMask = desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = desc.hasDepth ? toCompare(desc.depthCompareOp) : D3D12_COMPARISON_FUNC_ALWAYS;
    ds.StencilEnable = FALSE;
    ds.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    ds.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

    D3D12_SHADER_BYTECODE ms{};
    ms.pShaderBytecode = desc.msDxil.data();
    ms.BytecodeLength = desc.msDxil.size();
    D3D12_SHADER_BYTECODE as{};
    if (!desc.asDxil.empty()) {
        as.pShaderBytecode = desc.asDxil.data();
        as.BytecodeLength = desc.asDxil.size();
    }
    D3D12_SHADER_BYTECODE ps{};
    if (!desc.psDxil.empty()) {
        ps.pShaderBytecode = desc.psDxil.data();
        ps.BytecodeLength = desc.psDxil.size();
    }

    StreamRenderTargets rtFormats{};
    for (int i = 0; i < desc.colorCount; ++i) {
        const PipelineDesc::ColorTarget& ct = desc.colorTargets[static_cast<size_t>(i)];
        rtFormats.RTFormats[i] = ct.format < 0 ? DXGI_FORMAT_UNKNOWN : toDxgiFormat(ct.format);
    }
    rtFormats.NumRenderTargets = static_cast<UINT>(numRT);

    using StreamRootSig = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>;
    using StreamMs = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE>;
    using StreamAs = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE>;
    using StreamPs = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>;
    using StreamBlend = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>;
    using StreamRaster = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>;
    using StreamDepth = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC>;
    using StreamRtv = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,
                                StreamRenderTargets>;
    using StreamDsv = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>;
    using StreamSampleDesc = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC>;
    using StreamSampleMask = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT>;
    using StreamNodeMask = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, UINT>;

    StreamRootSig sRoot;
    sRoot.payload = rootSig.Get();
    StreamMs sMs;
    sMs.payload = ms;
    StreamAs sAs;
    sAs.payload = as;
    StreamPs sPs;
    sPs.payload = ps;
    StreamBlend sBlend;
    sBlend.payload = blend;
    StreamRaster sRaster;
    sRaster.payload = rs;
    StreamDepth sDepth;
    sDepth.payload = ds;
    StreamRtv sRtv;
    sRtv.payload = rtFormats;
    StreamDsv sDsv;
    sDsv.payload = depthStencilFormat(desc);
    StreamSampleDesc sSample;
    sSample.payload = DXGI_SAMPLE_DESC{1, 0};
    StreamSampleMask sMask;
    sMask.payload = UINT_MAX;
    StreamNodeMask sNode;
    sNode.payload = 0;

    auto pipeline = std::make_unique<Dx12Pipeline>();
    pipeline->shaderGeneration = allocateGraphicsPipelineGeneration();
    if (!pipeline->shaderGeneration)
        return mcdr::err("createMeshPipeline", "Graphics shader generation exhausted");
    pipeline->rootSignature = rootSig;
    pipeline->graphicsBindingLayoutKnown = true;
    for (const auto& binding : desc.bindings) {
        pipeline->graphicsBindingTypes.push_back(binding.type);
        pipeline->graphicsBindingRegisters.push_back(binding.reg);
    }
    pipeline->pushConstantBytes = desc.pushConstantBytes;
    pipeline->pushConstantParam = desc.pushConstantBytes ? meshConstantParam : UINT_MAX;
    pipeline->captureParam = captureParam;
    pipeline->captureHash = desc.captureHash;
    pipeline->captureGeneration = desc.captureGeneration;
    if (desc.captureHash) {
        pipeline->captureBudget = mcdr::oit_budget::reserve(1024 * 1024);
        if (!pipeline->captureBudget) return mcdr::err("createMeshPipeline", "Capture pipeline budget exceeded");
    }
    pipeline->isMesh = true;
    pipeline->rasterMotion = desc.rasterMotion;
    pipeline->depthWrite = desc.hasDepth && desc.depthWrite;
    pipeline->rasterMotionWritesWorldColor = false;
    const int worldColorCount = desc.colorCount - (desc.rasterMotion ? 1 : 0);
    pipeline->colorTargetFormats.reserve(static_cast<size_t>(numRT));
    for (int targetIndex = 0; targetIndex < worldColorCount; ++targetIndex) {
        const auto& target = desc.colorTargets[static_cast<size_t>(targetIndex)];
        if (target.format >= 0 && target.writeMask != 0) pipeline->rasterMotionWritesWorldColor = true;
    }
    pipeline->colorTargetCount = static_cast<UINT>(numRT);
    for (int targetIndex = 0; targetIndex < numRT; ++targetIndex) {
        pipeline->colorTargetFormats.push_back(toDxgiFormat(
            desc.colorTargets[static_cast<size_t>(targetIndex)].format));
    }
    pipeline->depthTargetFormat = desc.hasDepth ? toDxgiFormat(desc.depthFormat) : DXGI_FORMAT_UNKNOWN;
    pipeline->segmentParam = meshSegmentParam;
    pipeline->geometryParam = meshGeometryParam;
    pipeline->meshVisibleWork = meshVisibleWork;

    auto buildPso = [&](bool withAs, bool withPs, ComPtr<ID3D12PipelineState>& out) -> mcdr::Result<void> {
        std::vector<unsigned char> stream;
        stream.reserve(512);
        appendSubobjects(stream, sRoot, sMs);
        if (withAs) appendSubobjects(stream, sAs);
        if (withPs) appendSubobjects(stream, sPs);
        appendSubobjects(stream, sBlend, sRaster, sDepth, sRtv, sDsv, sSample, sMask);

        D3D12_PIPELINE_STATE_STREAM_DESC sd{};
        sd.SizeInBytes = static_cast<SIZE_T>(stream.size());
        sd.pPipelineStateSubobjectStream = stream.data();

        dbgLogInfo("createMeshPipeline: stream=%zuB as=%d ps=%d rt=%d dsv=%d cull=%d wire=%d\n",
               static_cast<size_t>(sd.SizeInBytes), withAs ? 1 : 0, withPs ? 1 : 0, numRT,
               static_cast<int>(sDsv.payload), desc.cullEnabled ? 1 : 0, desc.polygonMode == 1 ? 1 : 0);
        const HRESULT h = device2->CreatePipelineState(&sd, IID_PPV_ARGS(&out));
        if (FAILED(h)) {
            ComPtr<ID3D12InfoQueue> iq;
            if (SUCCEEDED(device2->QueryInterface(IID_PPV_ARGS(&iq))) && iq) {
                const UINT64 stored = iq->GetNumStoredMessages();
                dbgLogWarn("PSO infoqueue: %llu stored message(s)", (unsigned long long)stored);
                for (UINT64 k = 0; k < stored && k < 8; ++k) {
                    SIZE_T len = 0;
                    if (FAILED(iq->GetMessage(k, nullptr, &len)) || len == 0) continue;
                    std::vector<char> buf(len);
                    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                    if (SUCCEEDED(iq->GetMessage(k, msg, &len))) {
                        dbgLogWarn("PSO infoqueue[%d]: %s", static_cast<int>(msg->Severity),
                                   msg->pDescription ? msg->pDescription : "");
                    }
                }
            }
            return mcdr::err("createMeshPipeline", "CreatePipelineState hr=" + hrHex(h));
        }
        return {};
    };

    if (auto r = buildPso(!desc.asDxil.empty(), !desc.psDxil.empty(), pipeline->withDepth); !r)
        return std::unexpected(r.error());
    if (!desc.hasDepth) {
        const DXGI_FORMAT saved = sDsv.payload;
        sDsv.payload = DXGI_FORMAT_UNKNOWN;
        ds.DepthEnable = FALSE;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sDepth.payload = ds;
        auto r = buildPso(!desc.asDxil.empty(), !desc.psDxil.empty(), pipeline->withoutDepth);
        sDsv.payload = saved;
        if (!r) return std::unexpected(r.error());
    }

    return static_cast<void*>(pipeline.release());
}

}

mcdr::Result<void> dispatchMesh(void* ctxHandle, int x, int y, int z) {
    auto* ctx = static_cast<CommandContext*>(ctxHandle);
    if (ctx == nullptr) {
        return mcdr::err("dispatchMesh", "command context is null");
    }
    if (ctx->commandList == nullptr || !ctx->listOpen) {
        return mcdr::err("dispatchMesh", "command list is not open");
    }
    if (!ctx->inRenderPass) return mcdr::err("dispatchMesh", "render pass is not open");
    if (!ctx->currentPipeline || !ctx->currentPipeline->isMesh || !ctx->cachedPso)
        return mcdr::err("dispatchMesh", "mesh pipeline is not bound");
    if (x <= 0 || y <= 0 || z <= 0 || x > 65535 || y > 65535 || z > 65535
        || static_cast<uint64_t>(x) * y * z > (1ull << 22))
        return mcdr::err("dispatchMesh", "invalid group counts (axis <=65535, product <=4194304)");

    ComPtr<ID3D12GraphicsCommandList6> list6;
    HRESULT hr = ctx->commandList->QueryInterface(IID_PPV_ARGS(&list6));
    if (FAILED(hr) || !list6) {
        return mcdr::err("dispatchMesh", "ID3D12GraphicsCommandList6 unavailable hr=" + hrHex(hr));
    }

    if (prepareSampledRenderPassWrites(ctx) != SampledTextureContentStatus::Ready) {
        if (ctx->graphicsCommandObserver.prepare)
            completeGraphicsCommandRecording(ctx, mcdr::graphics_recording::Completion::Failed);
        return mcdr::err("dispatchMesh", "Sampled texture contents could not be preserved before writing");
    }
    std::shared_ptr<const void> graphicsPacket;
    if (ctx->graphicsCommandObserver.prepare) {
        mcdr::graphics_recording::Command command;
        command.family = mcdr::graphics_recording::CommandFamily::Mesh;
        command.count = static_cast<uint32_t>(static_cast<uint64_t>(x) * y * z);
        command.meshGroups[0] = static_cast<uint32_t>(x);
        command.meshGroups[1] = static_cast<uint32_t>(y);
        command.meshGroups[2] = static_cast<uint32_t>(z);
        if (prepareGraphicsCommandCapture(ctx, command, &graphicsPacket) != 1) {
            completeGraphicsCommandRecording(ctx, mcdr::graphics_recording::Completion::Failed);
            return mcdr::err("dispatchMesh", "Graphics command capture preparation failed");
        }
    }
    list6->DispatchMesh(static_cast<UINT>(x), static_cast<UINT>(y), static_cast<UINT>(z));
    if (graphicsPacket && confirmGraphicsCommandCapture(ctx, graphicsPacket) != 1) {
        completeGraphicsCommandRecording(ctx, mcdr::graphics_recording::Completion::Failed);
        return mcdr::err("dispatchMesh", "Graphics command capture confirmation failed");
    }
    if (graphicsPacket && completeGraphicsCommandRecording(ctx, mcdr::graphics_recording::Completion::Succeeded) != 1)
        return mcdr::err("dispatchMesh", "Graphics command capture completion failed");
    if (ctx->vanillaDepthSeedDraw && ctx->activeDepthTarget)
        ctx->vanillaDepthSeedDraw(ctx->vanillaDepthSeedState, ctx, ctx->activeDepthTarget,
            ctx->currentPipeline, static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::Mesh),
            ++ctx->vanillaDepthSeedOrdinal, graphicsPacket);
    ctx->colorTargetsWritten = true;
    for (size_t i = 0; i < ctx->activeColorTargetsTouched.size(); ++i)
        ctx->activeColorTargetsTouched[i] = true;
    return {};
}

}

namespace d3d12_mc {

mcdr::Result<uint64_t> createMeshRootSignature(const std::vector<PipelineDesc::Binding>& bindings,
                                               bool meshExtras, bool visibleWork, uint32_t pushConstantBytes,
                                               uint64_t captureHash, bool tileExtras) {
    if (const auto r = ensureDevice(); !r)
        return mcdr::err("createMeshRootSignature", r.error().message);
    if (captureHash) {
        if (const auto supported = requireCaptureBarycentrics(); !supported)
            return std::unexpected(supported.error());
    }
    auto captureBudget=captureHash ? mcdr::oit_budget::reserve(65536) : std::shared_ptr<mcdr::oit_budget::Reservation>{};
    if (captureHash && (!captureBudget || captureHash!=mcdr::oit_shader::contractHash))
        return mcdr::err("createMeshRootSignature", "Capture root budget or shader contract is invalid");

    std::vector<uint8_t> cbvRegs;
    std::vector<D3D12_DESCRIPTOR_RANGE> srvRanges;
    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
    for (const PipelineDesc::Binding& b : bindings) {
        if (b.type == 0) {
            cbvRegs.push_back(b.reg);
            continue;
        }
        D3D12_DESCRIPTOR_RANGE r{};
        r.NumDescriptors = 1;
        r.RegisterSpace = 0;
        r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        r.BaseShaderRegister = b.reg;
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges.push_back(r);
        if (b.type == 1) staticSamplers.push_back(makeStaticSampler(b.reg));
    }

    D3D12_ROOT_PARAMETER segmentCbv{};
    segmentCbv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    segmentCbv.Descriptor.ShaderRegister = 8;
    segmentCbv.Descriptor.RegisterSpace = 0;
    segmentCbv.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE vbRange{};
    vbRange.NumDescriptors = 1;
    vbRange.RegisterSpace = 0;
    vbRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    vbRange.BaseShaderRegister = 14;
    vbRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    D3D12_DESCRIPTOR_RANGE ibRange = vbRange;
    ibRange.BaseShaderRegister = 15;
    D3D12_DESCRIPTOR_RANGE sectionRange = vbRange;
    sectionRange.BaseShaderRegister = 16;
    const D3D12_DESCRIPTOR_RANGE geometryRanges[3] = {vbRange, ibRange, sectionRange};

    D3D12_DESCRIPTOR_RANGE tileRanges{};
    tileRanges.NumDescriptors = 5;
    tileRanges.RegisterSpace = 3;
    tileRanges.BaseShaderRegister = 0;
    tileRanges.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    tileRanges.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

    std::vector<D3D12_ROOT_PARAMETER> params;
    params.reserve(cbvRegs.size() + 3 + (tileExtras ? 3 : 0));
    for (uint8_t reg : cbvRegs) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = reg;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    if (!srvRanges.empty()) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(srvRanges.size());
        p.DescriptorTable.pDescriptorRanges = srvRanges.data();
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    if (meshExtras) {
        params.push_back(segmentCbv);
        if (visibleWork) {
            for (UINT shaderRegister = 14; shaderRegister <= 16; ++shaderRegister) {
                D3D12_ROOT_PARAMETER parameter{};
                parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                parameter.Descriptor.ShaderRegister = shaderRegister;
                parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                params.push_back(parameter);
            }
            D3D12_ROOT_PARAMETER base{};
            base.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            base.Constants.ShaderRegister = 9;
            base.Constants.Num32BitValues = 1;
            base.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params.push_back(base);
        } else {
            D3D12_ROOT_PARAMETER parameter{};
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable.NumDescriptorRanges = 3u;
            parameter.DescriptorTable.pDescriptorRanges = geometryRanges;
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params.push_back(parameter);
        }
    }

    const UINT segmentParam = static_cast<UINT>(cbvRegs.size()) + (srvRanges.empty() ? 0u : 1u);
    const UINT geometryParam = segmentParam + 1u;
    const UINT constantParam = static_cast<UINT>(params.size());
    const size_t rootDwords = cbvRegs.size() * 2 + (srvRanges.empty() ? 0 : 1) +
        (meshExtras ? (visibleWork ? 9 : 3) : 0) + pushConstantBytes / 4 + (captureHash ? 4 : 0) +
        (tileExtras ? 14 : 0);
    if (pushConstantBytes > 128 || pushConstantBytes % 4 != 0 || rootDwords > 64) {
        return mcdr::err("createMeshRootSignature", "root constant size or root DWORD budget exceeded");
    }
    if (pushConstantBytes != 0) {
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = {0, 1, pushConstantBytes / 4};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(parameter);
    }

    D3D12_DESCRIPTOR_RANGE captureRanges[2]{};
    const UINT captureParam = captureHash ? mcdr::graphics_capture::appendParameters(params, captureRanges) : UINT_MAX;

    UINT tileRootParam = UINT_MAX;
    UINT tileBatchParam = UINT_MAX;
    UINT tileDescriptorParam = UINT_MAX;
    if (tileExtras) {
        if (!meshExtras || !visibleWork)
            return mcdr::err("createMeshRootSignature", "Terrain tile route requires visible mesh extras");
        D3D12_ROOT_PARAMETER constants{};
        constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants.Constants = {0, 3, 4};
        constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        tileRootParam = static_cast<UINT>(params.size());
        params.push_back(constants);

        D3D12_ROOT_PARAMETER batch{};
        batch.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        batch.Descriptor = {1, 3};
        batch.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        tileBatchParam = static_cast<UINT>(params.size());
        params.push_back(batch);

        D3D12_ROOT_PARAMETER descriptors{};
        descriptors.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        descriptors.DescriptorTable = {1, &tileRanges};
        descriptors.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        tileDescriptorParam = static_cast<UINT>(params.size());
        params.push_back(descriptors);

        D3D12_ROOT_PARAMETER chunk{};
        chunk.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        chunk.Constants = {2, 3, 5};
        chunk.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(chunk);

        D3D12_ROOT_PARAMETER envelopes{};
        envelopes.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        envelopes.Descriptor = {4, 3};
        envelopes.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(envelopes);
    }
    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(params.size());
    rsDesc.pParameters = params.empty() ? nullptr : params.data();
    rsDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
    rsDesc.pStaticSamplers = staticSamplers.empty() ? nullptr : staticSamplers.data();
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (!meshExtras) {
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    versioned.Desc_1_0 = rsDesc;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&versioned, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        std::string msg = "D3D12SerializeVersionedRootSignature hr=" + hrHex(hr);
        if (rsErr && rsErr->GetBufferSize() > 0) {
            msg += " ";
            msg.append(static_cast<const char*>(rsErr->GetBufferPointer()), rsErr->GetBufferSize());
        }
        return mcdr::err("createMeshRootSignature", std::move(msg));
    }
    ComPtr<ID3D12RootSignature> rootSig;
    hr = deviceContext().device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                                     IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        return mcdr::err("createMeshRootSignature", "CreateRootSignature hr=" + hrHex(hr));
    }
    ID3D12RootSignature* raw = rootSig.Detach();
    const uint64_t handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(raw));
    recordMeshRootSig(handle, bindings, meshExtras ? segmentParam : 0, meshExtras ? geometryParam : 0,
        visibleWork, pushConstantBytes, pushConstantBytes ? constantParam : UINT_MAX, captureParam, captureHash,
        std::move(captureBudget), tileExtras, tileRootParam, tileBatchParam, tileDescriptorParam);
    return handle;
}

void releaseMeshRootSignature(uint64_t rootSignature) {
    if (rootSignature == 0) return;
    forgetMeshRootSig(rootSignature);
    auto* sig = reinterpret_cast<ID3D12RootSignature*>(static_cast<uintptr_t>(rootSignature));
    sig->Release();
}

namespace {

struct TerrainTileRoute {
    struct SourceBinding {
        uint64_t identity = 0;
        uint64_t generation = 0;
    };
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12CommandSignature> signature;
    ComPtr<ID3D12PipelineState> envelopePipeline;
    ComPtr<ID3D12DescriptorHeap> lanePlaceholderHeap;
    Dx12Object lanePlaceholder;
    uint64_t captureHash = 0;
    uint64_t captureGeneration = 0;
    UINT constantsParam = UINT_MAX;
    UINT batchParam = UINT_MAX;
    UINT descriptorParam = UINT_MAX;
    UINT segmentParam = UINT_MAX;
    UINT geometryParam = UINT_MAX;
    uint32_t commandStride = 0;
    std::shared_ptr<void> tilePipeline;
    std::optional<mcdr_terrain_tile_stage_v2> stageContract;
    std::array<uint32_t, 5> laneBindingSlots{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint64_t tileGeneration = 0;
    std::array<SourceBinding, 6> sources{};
    uint32_t sourceCount = 0;
};

struct TerrainTileRouteHandle {
    std::shared_ptr<TerrainTileRoute> route;
};

}

mcdr::Result<void*> createTerrainTileRoute(uint64_t rootSignature, uint64_t captureHash,
                                           uint64_t captureGeneration) {
    if (rootSignature == 0 || captureHash != mcdr::oit_shader::contractHash || captureGeneration == 0)
        return mcdr::err("terrain.tile.route", "Invalid tile root or capture contract");
    const auto info = lookupMeshRootSig(rootSignature);
    if (!info.tileExtras || info.captureHash != 0 || info.tileRootParam == UINT_MAX || info.tileBatchParam == UINT_MAX ||
        info.tileDescriptorParam == UINT_MAX)
        return mcdr::err("terrain.tile.route", "Root signature has no terrain tile register space");
    if (const auto ready = ensureDevice(); !ready)
        return mcdr::err("terrain.tile.route", ready.error().message);

    auto route = std::make_shared<TerrainTileRoute>();
    route->captureHash = captureHash;
    route->captureGeneration = captureGeneration;
    route->constantsParam = info.tileRootParam;
    route->batchParam = info.tileBatchParam;
    route->descriptorParam = info.tileDescriptorParam;
    route->segmentParam = info.segmentParam;
    route->geometryParam = info.geometryParam;
    route->commandStride = sizeof(uint32_t) * 7;
    auto* root = reinterpret_cast<ID3D12RootSignature*>(static_cast<uintptr_t>(rootSignature));
    root->AddRef();
    route->root.Attach(root);

    D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[0].Constant.RootParameterIndex = route->constantsParam;
    arguments[0].Constant.DestOffsetIn32BitValues = 0;
    arguments[0].Constant.Num32BitValuesToSet = 4;
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    D3D12_COMMAND_SIGNATURE_DESC description{};
    description.ByteStride = route->commandStride;
    description.NumArgumentDescs = 2;
    description.pArgumentDescs = arguments;
    const HRESULT result = deviceContext().device->CreateCommandSignature(&description, route->root.Get(),
        IID_PPV_ARGS(&route->signature));
    if (FAILED(result))
        return mcdr::err("terrain.tile.route", "DispatchMesh command signature creation failed hr=" + hrHex(result));
    return static_cast<void*>(new TerrainTileRouteHandle{std::move(route)});
}

void destroyTerrainTileRoute(void* opaqueRoute) {
    delete static_cast<TerrainTileRouteHandle*>(opaqueRoute);
}

mcdr::Result<void> bindTerrainTileRoute(void* opaquePipeline, void* opaqueRoute) {
    auto* pipeline = static_cast<Dx12Pipeline*>(opaquePipeline);
    auto* route = static_cast<TerrainTileRouteHandle*>(opaqueRoute);
    if (!pipeline || !route || !route->route || !route->route->signature || !route->route->root)
        return mcdr::err("terrain.tile.route", "Tile pipeline or route is null");
    if (!pipeline->captureHash || pipeline->captureHash != route->route->captureHash ||
        pipeline->captureGeneration != route->route->captureGeneration)
        return mcdr::err("terrain.tile.route", "Tile route capture contract does not match pipeline");
    pipeline->terrainTileRoute = route->route;
    return {};
}

mcdr::Result<void> bindTerrainTilePipeline(void* opaqueRoute, void* opaquePipeline) {
    auto* route = static_cast<TerrainTileRouteHandle*>(opaqueRoute);
    auto* pipeline = static_cast<Dx12Pipeline*>(opaquePipeline);
    if (!route || !route->route || !pipeline || !pipeline->rootSignature || !pipeline->isMesh ||
        !pipeline->meshVisibleWork)
        return mcdr::err("terrain.tile.pipeline", "Tile route or mesh pipeline is invalid");
    if (pipeline->rootSignature.Get() != route->route->root.Get())
        return mcdr::err("terrain.tile.pipeline", "Tile mesh pipeline root does not match route root");
    if (route->route->tilePipeline) {
        if (route->route->tilePipeline.get() == pipeline) return {};
        return mcdr::err("terrain.tile.pipeline", "Tile route pipeline is immutable after binding");
    }
    pipeline->lifetimeReferences.fetch_add(1, std::memory_order_relaxed);
    route->route->tilePipeline = std::shared_ptr<void>(pipeline, [](void* value) {
        destroyPipeline(static_cast<Dx12Pipeline*>(value));
    });
    return {};
}

mcdr::Result<void> bindTerrainTileEnvelopeV1(void* opaqueRoute, const void* dxil, uint32_t bytes) {
    auto* handle = static_cast<TerrainTileRouteHandle*>(opaqueRoute);
    if (!handle || !handle->route || !handle->route->root || !dxil || !bytes)
        return mcdr::err("terrain.tile.envelope", "Tile envelope pipeline input is invalid");
    auto& route = *handle->route;
    if (route.envelopePipeline || route.sourceCount != 0)
        return mcdr::err("terrain.tile.envelope", "Tile envelope registration is closed");
    auto* device = deviceContext().device.Get();
    if (!device) return mcdr::err("terrain.tile.envelope", "Tile envelope device is unavailable");
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = route.root.Get();
    description.CS = {dxil, bytes};
    const auto result = device->CreateComputePipelineState(&description, IID_PPV_ARGS(&route.envelopePipeline));
    if (FAILED(result))
        return mcdr::err("terrain.tile.envelope", "Tile envelope PSO creation failed hr=" + hrHex(result));
    return {};
}

namespace {

mcdr::Result<void> bindTerrainTileStage(void* opaqueSource, void* opaqueRoute,
    const mcdr_terrain_tile_stage_v2& contract, std::span<const uint32_t> laneRegisters) {
    auto* source = static_cast<Dx12Pipeline*>(opaqueSource);
    auto* route = static_cast<TerrainTileRouteHandle*>(opaqueRoute);
    if (!source || !route || !route->route || !source->rootSignature || !source->withDepth ||
        !source->isMesh || !source->meshVisibleWork || !source->shaderGeneration)
        return mcdr::err("terrain.tile.stage", "Tile stage source or route is invalid");
    auto& owner = *route->route;
    if (contract.schemaVersion != 2 || contract.structSize != sizeof(contract) ||
        contract.shaderAbiVersion != mcdr::oit_shader::abiVersion ||
        contract.shaderContractHash != mcdr::oit_shader::contractHash ||
        contract.shaderContractHash != owner.captureHash || !contract.programGeneration ||
        contract.programGeneration != owner.captureGeneration ||
        contract.stage > terrainOpaqueSeedStage || (contract.quadsPerMeshlet != 16 && contract.quadsPerMeshlet != 32 &&
            contract.quadsPerMeshlet != 64) || contract.addressingVersion != 1)
        return mcdr::err("terrain.tile.stage", "Tile stage contract is invalid");
    const auto* tile = static_cast<const Dx12Pipeline*>(owner.tilePipeline.get());
    if (!tile || tile == source || !tile->isMesh || !tile->meshVisibleWork || !tile->rootSignature ||
        tile->rootSignature.Get() != owner.root.Get() || !owner.signature || !tile->withDepth ||
        !tile->shaderGeneration || owner.commandStride != 28)
        return mcdr::err("terrain.tile.stage", "Tile stage pipeline or command signature is incomplete");
    if (!source->graphicsBindingLayoutKnown || !tile->graphicsBindingLayoutKnown ||
        source->graphicsBindingRegisters.size() != source->graphicsBindingTypes.size() ||
        tile->graphicsBindingRegisters.size() != tile->graphicsBindingTypes.size() ||
        source->graphicsBindingTypes != tile->graphicsBindingTypes ||
        source->graphicsBindingRegisters != tile->graphicsBindingRegisters ||
        source->pushConstantBytes != tile->pushConstantBytes || source->pushConstantBytes > 128 ||
        source->pushConstantBytes % 4 ||
        (source->pushConstantBytes && (source->pushConstantParam == UINT_MAX || tile->pushConstantParam == UINT_MAX)) ||
        source->segmentParam != owner.segmentParam || source->geometryParam != owner.geometryParam ||
        tile->segmentParam != owner.segmentParam || tile->geometryParam != owner.geometryParam ||
        owner.constantsParam == UINT_MAX || owner.batchParam == UINT_MAX || owner.descriptorParam == UINT_MAX)
        return mcdr::err("terrain.tile.stage", "Tile stage root parameters or material layout differ");
    if (source->captureHash && (source->captureHash != contract.shaderContractHash ||
        source->captureGeneration != contract.programGeneration))
        return mcdr::err("terrain.tile.stage", "Tile stage capture source generation differs");
    const bool opaqueSeed = contract.stage == terrainOpaqueSeedStage;
    const bool compatibleTargets = opaqueSeed
        ? source->depthWrite && tile->depthWrite && !source->captureHash && !tile->rasterMotion &&
            tile->colorTargetCount == 0 && owner.envelopePipeline && laneRegisters.empty()
        : contract.stage == 1
            ? tile->colorTargetCount == 1 || tile->colorTargetCount == 2 || tile->colorTargetCount == 4
            : tile->colorTargetCount == 1;
    if (tile->captureHash || tile->depthTargetFormat != DXGI_FORMAT_D32_FLOAT ||
        source->depthTargetFormat != DXGI_FORMAT_D32_FLOAT ||
        source->colorTargetCount < tile->colorTargetCount ||
        source->colorTargetFormats.size() < tile->colorTargetCount ||
        tile->colorTargetFormats.size() != tile->colorTargetCount ||
        !compatibleTargets)
        return mcdr::err("terrain.tile.stage", "Tile stage depth or target count is incompatible");
    for (uint32_t target = 0; target < tile->colorTargetCount; ++target)
        if (tile->colorTargetFormats[target] == DXGI_FORMAT_UNKNOWN ||
            source->colorTargetFormats[target] != tile->colorTargetFormats[target])
            return mcdr::err("terrain.tile.stage", "Tile stage source and replay target formats differ");
    std::array<uint32_t, 5> laneBindingSlots;
    laneBindingSlots.fill(UINT32_MAX);
    if (!laneRegisters.empty()) {
        using mcdr::terrain_tile::ReplayStage;
        if (laneRegisters.size() != laneBindingSlots.size())
            return mcdr::err("terrain.tile.stage", "Tile lane binding count is invalid");
        const bool bounds = laneRegisters.front() != UINT32_MAX;
        const auto coefficients = laneRegisters.subspan(1);
        const auto coefficientCount = std::ranges::count_if(coefficients, [](uint32_t reg) { return reg != UINT32_MAX; });
        const auto stage = static_cast<ReplayStage>(contract.stage);
        if (bounds != (stage != ReplayStage::DepthBounds) ||
            (stage == ReplayStage::Accumulate
                ? coefficientCount != 1 && coefficientCount != 2 && coefficientCount != 4
                : coefficientCount != 0) ||
            std::ranges::any_of(coefficients.first(static_cast<size_t>(coefficientCount)),
                [](uint32_t reg) { return reg == UINT32_MAX; }))
            return mcdr::err("terrain.tile.stage", "Tile lane bindings differ from the stage inputs");
        for (size_t laneBinding = 0; laneBinding < laneRegisters.size(); ++laneBinding) {
            const auto reg = laneRegisters[laneBinding];
            if (reg == UINT32_MAX) continue;
            uint32_t slot = UINT32_MAX;
            for (uint32_t binding = 0; binding < source->graphicsBindingRegisters.size(); ++binding) {
                if (source->graphicsBindingTypes[binding] != 1 || source->graphicsBindingRegisters[binding] != reg)
                    continue;
                if (slot != UINT32_MAX)
                    return mcdr::err("terrain.tile.stage", "Tile lane binding is ambiguous");
                slot = binding;
            }
            if (slot == UINT32_MAX || std::ranges::contains(laneBindingSlots, slot))
                return mcdr::err("terrain.tile.stage", "Tile lane binding is missing or repeated");
            laneBindingSlots[laneBinding] = slot;
        }
    }
    if (source->terrainTileRoute && source->terrainTileRoute != route->route)
        return mcdr::err("terrain.tile.stage", "Source pipeline already belongs to another tile route");
    if (owner.stageContract && (std::memcmp(&*owner.stageContract, &contract, sizeof(contract)) != 0 ||
        owner.tileGeneration != tile->shaderGeneration || owner.laneBindingSlots != laneBindingSlots))
        return mcdr::err("terrain.tile.stage", "Tile stage contract is immutable after registration");
    const auto sourceIdentity = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(source));
    for (uint32_t index = 0; index < owner.sourceCount; ++index)
        if (owner.sources[index].identity == sourceIdentity) {
            if (owner.sources[index].generation == source->shaderGeneration) return {};
            return mcdr::err("terrain.tile.stage", "Source pipeline generation changed after registration");
        }
    if (owner.sourceCount == owner.sources.size())
        return mcdr::err("terrain.tile.stage", "Tile route source registry is full");
    if (!owner.lanePlaceholderHeap &&
        std::ranges::any_of(laneBindingSlots, [](uint32_t slot) { return slot != UINT32_MAX; })) {
        auto* device = deviceContext().device.Get();
        if (!device) return mcdr::err("terrain.tile.stage", "Tile stage device is unavailable");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 1;
        const HRESULT result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&owner.lanePlaceholderHeap));
        if (FAILED(result))
            return mcdr::err("terrain.tile.stage", "Tile stage descriptor allocation failed hr=" + hrHex(result));
        auto& placeholder = owner.lanePlaceholder;
        placeholder.kind = Dx12Object::Kind::TextureView;
        placeholder.packetContentOwner = route->route;
        placeholder.cpuHandle = owner.lanePlaceholderHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(nullptr, &view, placeholder.cpuHandle);
    }
    owner.stageContract = contract;
    owner.laneBindingSlots = laneBindingSlots;
    owner.tileGeneration = tile->shaderGeneration;
    owner.sources[owner.sourceCount++] = {sourceIdentity, source->shaderGeneration};
    source->terrainTileRoute = route->route;
    return {};
}

}

mcdr::Result<void> bindTerrainTileStageV2(void* source, void* route,
    const mcdr_terrain_tile_stage_v2& contract, std::span<const uint32_t> laneRegisters) {
    if (contract.stage >= terrainOpaqueSeedStage)
        return mcdr::err("terrain.tile.stage", "Tile stage contract is invalid");
    return bindTerrainTileStage(source, route, contract, laneRegisters);
}

mcdr::Result<void> bindTerrainOpaqueSeedV1(void* source, void* route, uint64_t programGeneration,
    uint32_t quadsPerMeshlet) {
    const mcdr_terrain_tile_stage_v2 contract{2, sizeof(contract), mcdr::oit_shader::abiVersion,
        terrainOpaqueSeedStage, quadsPerMeshlet, 1, mcdr::oit_shader::contractHash, programGeneration};
    return bindTerrainTileStage(source, route, contract, {});
}

std::span<const uint32_t> terrainTileLaneBindingSlots(const void* opaqueRoute) {
    const auto* route = static_cast<const TerrainTileRoute*>(opaqueRoute);
    return route ? std::span<const uint32_t>{route->laneBindingSlots} : std::span<const uint32_t>{};
}

mcdr::terrain_tile::StageAdapterDescription describeTerrainTileStageAdapter(
    const std::shared_ptr<const void>& routeLease, uint64_t sourceIdentity, uint64_t sourceGeneration) {
    mcdr::terrain_tile::StageAdapterDescription output;
    if (!routeLease) return output;
    const auto route = std::static_pointer_cast<const TerrainTileRoute>(routeLease);
    if (!route->stageContract) return output;
    output.status = mcdr::terrain_tile::StageAdapterStatus::Stale;
    for (uint32_t index = 0; index < route->sourceCount; ++index) {
        const auto& source = route->sources[index];
        if (source.identity != sourceIdentity || source.generation != sourceGeneration) continue;
        const auto* tile = static_cast<const Dx12Pipeline*>(route->tilePipeline.get());
        if (!tile || tile->shaderGeneration != route->tileGeneration) return output;
        output.contract = *route->stageContract;
        output.sourceIdentity = sourceIdentity;
        output.sourceGeneration = sourceGeneration;
        output.tileIdentity = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tile));
        output.tileGeneration = route->tileGeneration;
        output.routeLease = routeLease;
        output.status = mcdr::terrain_tile::StageAdapterStatus::Registered;
        return output;
    }
    return output;
}

namespace {

void createTileTextureSrv(Dx12Object* object, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = object ? object->dxgiFormat : DXGI_FORMAT_R32G32B32A32_FLOAT;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.MostDetailedMip = 0;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.FirstArraySlice = 0;
    view.Texture2DArray.ArraySize = object ? object->resource->GetDesc().DepthOrArraySize : 1u;
    view.Texture2DArray.PlaneSlice = 0;
    view.Texture2DArray.ResourceMinLODClamp = 0.0f;
    deviceContext().device->CreateShaderResourceView(object ? object->resource.Get() : nullptr, &view, destination);
}

mcdr::Result<DrawDescriptorRange> createTileReplayDescriptors(CommandContext* context,
    const mcdr::terrain_tile::BatchV2& batch, mcdr::terrain_tile::ReplayStage stage) {
    using mcdr::terrain_tile::ReplayStage;
    auto descriptors = allocateDrawDescriptors(context, 5);
    if (!descriptors) return std::unexpected<mcdr::Error>(descriptors.error());
    auto cpu = descriptors->cpu;
    createTileTextureSrv(stage == ReplayStage::DepthBounds ? nullptr :
        static_cast<Dx12Object*>(batch.culledBounds.object), cpu);
    const auto coefficientCount = stage == ReplayStage::Accumulate
        ? batch.stages[static_cast<uint32_t>(ReplayStage::Transmittance)].renderTargetCount : 0u;
    for (uint32_t index = 0; index < std::size(batch.coefficients); ++index) {
        cpu.ptr += deviceContext().srvInc;
        createTileTextureSrv(index < coefficientCount ?
            static_cast<Dx12Object*>(batch.coefficients[index].object) : nullptr, cpu);
    }
    return descriptors;
}

}

namespace {

mcdr::Result<void> validateTerrainReplay(void* opaqueRoute,
    const mcdr::terrain_tile::BatchV2& batch, const mcdr::terrain_tile::StageBindingV2& stage,
    const TerrainTileReplayInputsV2& inputs) {
    auto* route = static_cast<TerrainTileRoute*>(opaqueRoute);
    const uint32_t stageIndex = stage.stage;
    const bool opaqueSeed = stageIndex == terrainOpaqueSeedStage;
    if (!route || !route->root || !route->signature || !route->stageContract ||
        stageIndex > terrainOpaqueSeedStage || route->stageContract->stage != stageIndex ||
        route->stageContract->programGeneration != batch.programGeneration ||
        route->captureGeneration != batch.programGeneration)
        return mcdr::err("terrain.tile.replay", "Tile replay route is incomplete");
    if (stage.flags != 0 || stage.reserved != 0 ||
        stage.renderTargetCount > 4 || !stage.pipeline)
        return mcdr::err("terrain.tile.replay", "Tile replay stage is incomplete");
    const bool validTargetCount = stageIndex == static_cast<uint32_t>(mcdr::terrain_tile::ReplayStage::Transmittance)
        ? (stage.renderTargetCount == 1 || stage.renderTargetCount == 2 || stage.renderTargetCount == 4)
        : stage.renderTargetCount == (opaqueSeed ? 0u : 1u);
    if (!validTargetCount)
        return mcdr::err("terrain.tile.replay", "Tile replay target count is incompatible");
    const auto* pipeline = static_cast<const Dx12Pipeline*>(stage.pipeline);
    if (route->tilePipeline.get() != pipeline || pipeline->shaderGeneration != route->tileGeneration ||
        !pipeline->isMesh || !pipeline->meshVisibleWork || !pipeline->rootSignature ||
        pipeline->rootSignature.Get() != route->root.Get() || !pipeline->withDepth)
        return mcdr::err("terrain.tile.replay", "Tile replay PSO is incompatible with its route");
    if (route->segmentParam == UINT_MAX || route->geometryParam == UINT_MAX ||
        route->batchParam == UINT_MAX || route->descriptorParam == UINT_MAX ||
        pipeline->segmentParam != route->segmentParam || pipeline->geometryParam != route->geometryParam)
        return mcdr::err("terrain.tile.replay", "Tile replay root parameter metadata is incomplete");
    const auto& viewport = inputs.sourceViewport;
    const auto& scissor = inputs.sourceScissor;
    if (!std::isfinite(viewport.TopLeftX) || !std::isfinite(viewport.TopLeftY) ||
        !std::isfinite(viewport.Width) || !std::isfinite(viewport.Height) || viewport.Width <= 0.0f ||
        viewport.Height <= 0.0f ||
        viewport.TopLeftX != batch.sourceViewport[0] || viewport.TopLeftY != batch.sourceViewport[1] ||
        viewport.Width != batch.sourceViewport[2] || viewport.Height != batch.sourceViewport[3] ||
        !std::isfinite(viewport.MinDepth) || !std::isfinite(viewport.MaxDepth) || viewport.MinDepth < 0.0f ||
        viewport.MaxDepth > 1.0f || viewport.MinDepth > viewport.MaxDepth ||
        scissor.left > scissor.right || scissor.top > scissor.bottom)
        return mcdr::err("terrain.tile.replay", "Tile viewport or scissor differs from the captured draw");
    if (!pipeline->graphicsBindingLayoutKnown || pipeline->graphicsBindingTypes.size() != inputs.bindings.size() ||
        pipeline->pushConstantBytes != inputs.pushConstantBytes || !inputs.pushConstantsValid ||
        inputs.pushConstantBytes > inputs.pushConstants.size() || inputs.pushConstantBytes % 4 != 0 ||
        (inputs.pushConstantBytes != 0 && pipeline->pushConstantParam == UINT_MAX))
        return mcdr::err("terrain.tile.replay", "Tile replay material layout differs from captured inputs");
    for (uint32_t index = 0; index < inputs.bindings.size(); ++index) {
        const auto& binding = inputs.bindings[index];
        if (pipeline->graphicsBindingTypes[index] != binding.type ||
            binding.laneAttachment != std::ranges::contains(route->laneBindingSlots, index) ||
            (binding.laneAttachment && (binding.type != 1 || !route->lanePlaceholderHeap)))
            return mcdr::err("terrain.tile.replay", "Tile replay material binding type changed");
    }
    using mcdr::terrain_tile::ReplayStage;
    if (stageIndex == static_cast<uint32_t>(ReplayStage::Accumulate)) {
        const auto coefficientCount = std::ranges::count_if(std::span{route->laneBindingSlots}.subspan(1),
            [](uint32_t slot) { return slot != UINT32_MAX; });
        if (coefficientCount != 0 && coefficientCount != batch.stages[1].renderTargetCount)
            return mcdr::err("terrain.tile.replay", "Tile coefficient layout differs from the producer");
    }
    return {};
}

mcdr::terrain_tile::StageBindingV2 opaqueSeedStageBinding(const TerrainTileRoute* route,
    const mcdr::terrain_tile::ResourceRef& depth) {
    mcdr::terrain_tile::StageBindingV2 stage{};
    stage.stage = terrainOpaqueSeedStage;
    stage.pipeline = route ? route->tilePipeline.get() : nullptr;
    stage.depth = depth;
    return stage;
}

std::vector<DrawBinding> terrainTileMaterialBindings(const TerrainTileReplayInputsV2& inputs, Dx12Object& lanePlaceholder) {
    std::vector<DrawBinding> bindings;
    bindings.reserve(inputs.bindings.size());
    for (const auto& binding : inputs.bindings)
        bindings.push_back({binding.type, static_cast<Dx12Object*>(binding.buffer),
            binding.offset, binding.length, binding.texelFormat,
            binding.laneAttachment ? &lanePlaceholder : static_cast<Dx12Object*>(binding.view)});
    return bindings;
}
}

mcdr::Result<void> validateTerrainTileReplay(void* opaqueRoute, const mcdr::terrain_tile::BatchV2& batch,
    const TerrainTileReplayInputsV2& inputs, uint32_t stageIndex) {
    if (stageIndex >= std::size(batch.stages) || batch.stages[stageIndex].stage != stageIndex)
        return mcdr::err("terrain.tile.replay", "Tile replay stage is incomplete");
    return validateTerrainReplay(opaqueRoute, batch, batch.stages[stageIndex], inputs);
}

mcdr::Result<void> validateTerrainOpaqueSeedReplay(void* opaqueRoute, const mcdr::terrain_tile::BatchV2& batch,
    const TerrainTileReplayInputsV2& inputs, const mcdr::terrain_tile::ResourceRef& depth) {
    const auto* target = static_cast<const Dx12Object*>(depth.object);
    if (!target || target->kind != Dx12Object::Kind::Texture || !target->resource ||
        target->dxgiFormat != DXGI_FORMAT_D32_FLOAT || depth.offset || depth.bytes)
        return mcdr::err("terrain.seed.replay", "Terrain seed depth is invalid");
    const auto desc = target->resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Format != DXGI_FORMAT_R32_TYPELESS ||
        desc.Width != mcdr::terrain_tile::scratchSize || desc.Height != mcdr::terrain_tile::scratchSize ||
        desc.DepthOrArraySize != batch.residentTileCount || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        return mcdr::err("terrain.seed.replay", "Terrain seed depth layout is invalid");
    const auto stage = opaqueSeedStageBinding(static_cast<TerrainTileRoute*>(opaqueRoute), depth);
    return validateTerrainReplay(opaqueRoute, batch, stage, inputs);
}

bool hasTerrainTileEnvelopePipeline(const void* opaqueRoute) {
    const auto* route = static_cast<const TerrainTileRoute*>(opaqueRoute);
    return route && route->envelopePipeline;
}

mcdr::Result<void> recordTerrainTileEnvelopes(void* opaqueContext, const void* opaqueRoute,
    const mcdr::terrain_tile::BatchV2& batch, const TerrainTileGeometryV2& geometry,
    const TerrainTileReplayInputsV2& inputs, const TerrainTileEnvelopeChunk& chunk) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* route = const_cast<TerrainTileRoute*>(static_cast<const TerrainTileRoute*>(opaqueRoute));
    if (!context || !route || !route->envelopePipeline || !route->tilePipeline ||
        !context->listOpen || !context->commandList || context->inRenderPass ||
        !chunk.destination || chunk.destination % 4)
        return mcdr::err("terrain.tile.envelope", "Tile envelope recording is invalid");
    auto* commandList = context->commandList;
    commandList->SetComputeRootSignature(route->root.Get());
    restoreDrawHeap(context);
    if (auto result = pushDescriptors(context, terrainTileMaterialBindings(inputs, route->lanePlaceholder),
            DescriptorBindPoint::Compute);
        !result)
        return result;
    commandList->SetComputeRootConstantBufferView(route->segmentParam, geometry.segmentAddress);
    commandList->SetComputeRootShaderResourceView(route->geometryParam, geometry.vertexAddress);
    commandList->SetComputeRootShaderResourceView(route->geometryParam + 1, geometry.visibleAddress);
    commandList->SetComputeRootShaderResourceView(route->geometryParam + 2, geometry.atlasAddress);
    commandList->SetComputeRootConstantBufferView(route->batchParam, geometry.batchConstantsAddress);
    const auto* pipeline = static_cast<const Dx12Pipeline*>(route->tilePipeline.get());
    if (inputs.pushConstantBytes)
        commandList->SetComputeRoot32BitConstants(pipeline->pushConstantParam,
            inputs.pushConstantBytes / 4u, inputs.pushConstants.data(), 0);
    const uint32_t constants[] = {batch.primitiveCount, batch.refCapacity,
        batch.commandKind, batch.commandStride, chunk.firstVisible};
    commandList->SetComputeRoot32BitConstants(route->descriptorParam + 1, 5, constants, 0);
    commandList->SetComputeRootUnorderedAccessView(route->descriptorParam + 2, chunk.destination);
    commandList->SetPipelineState(route->envelopePipeline.Get());
    commandList->Dispatch((batch.primitiveCount + 63u) / 64u, 1, 1);
    context->cachedPso = nullptr;
    return {};
}

namespace {

mcdr::Result<void> recordTerrainReplay(void* opaqueContext, void* opaqueRoute,
    const mcdr::terrain_tile::BatchV2& batch, const mcdr::terrain_tile::StageBindingV2& stage,
    const TerrainTileGeometryV2& geometry, const TerrainTileReplayInputsV2& inputs,
    TerrainTileAttachmentMode attachmentMode) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* route = static_cast<TerrainTileRoute*>(opaqueRoute);
    if (!context || !context->listOpen || !context->commandList ||
        geometry.segmentAddress == 0 || geometry.vertexAddress == 0 ||
        geometry.visibleAddress == 0 || geometry.atlasAddress == 0 || geometry.batchConstantsAddress == 0)
        return mcdr::err("terrain.tile.replay", "Tile replay context or geometry is incomplete");
    const uint32_t stageIndex = stage.stage;
    const bool opaqueSeed = stageIndex == terrainOpaqueSeedStage;
    auto* pipeline = static_cast<Dx12Pipeline*>(stage.pipeline);
    const auto materialBindings = terrainTileMaterialBindings(inputs, route->lanePlaceholder);
    std::array<Dx12Object*, 5> targets{};
    for (uint32_t index = 0; index < stage.renderTargetCount; ++index)
        targets[index] = static_cast<Dx12Object*>(stage.renderTargets[index].object);
    auto* depth = static_cast<Dx12Object*>(stage.depth.object);
    const bool clearAttachments = attachmentMode == TerrainTileAttachmentMode::Clear;
    int clearFlags[5]{};
    if (clearAttachments) std::fill_n(clearFlags, stage.renderTargetCount, 1);
    const float clearColors[20]{};
    const auto renderWidth = static_cast<int>(mcdr::terrain_tile::scratchSize);
    const auto renderHeight = static_cast<int>(mcdr::terrain_tile::scratchSize);
    std::vector<vanilla::PassResourceUse> declarations;
    declarations.reserve(stage.renderTargetCount + inputs.reads.size() + 8);
    for (const auto& read : inputs.reads)
        declarations.push_back({nullptr, batch.resourceVersion, read.firstSubresource, read.subresourceCount,
            vanilla::ResourceAccess::Read, read.state, read.state, vanilla::ExternalResourceState::Tracked,
            read.resource, read.initialState});
    const auto declareRead = [&](void* object, uint32_t state) {
        declarations.push_back({object, batch.resourceVersion, UINT32_MAX, 0,
            vanilla::ResourceAccess::Read, state, state});
    };
    const auto declareWrite = [&](void* object, uint32_t state) {
        declarations.push_back({object, batch.resourceVersion, UINT32_MAX, 0,
            vanilla::ResourceAccess::ReadWrite, state, D3D12_RESOURCE_STATE_COMMON});
    };
    if (!opaqueSeed && stageIndex != 0)
        declareRead(batch.culledBounds.object, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (stageIndex == 2)
        for (uint32_t index = 0; index < batch.stages[1].renderTargetCount; ++index)
            declareRead(batch.coefficients[index].object, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    declareRead(batch.commands.object, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    declareRead(batch.control.object, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    for (uint32_t index = 0; index < stage.renderTargetCount; ++index)
        declareWrite(stage.renderTargets[index].object, D3D12_RESOURCE_STATE_RENDER_TARGET);
    declareWrite(stage.depth.object, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    vanilla::DeclaredPassToken* declaration = nullptr;
    if (vanilla::beginDeclaredPass(context, 0x210u + stageIndex, declarations.data(),
            static_cast<uint32_t>(declarations.size()), 3, &declaration) != 1)
        return mcdr::err("terrain.tile.replay", vanilla::lastFailureText());
    if (auto pass = beginRenderPass(context, targets.data(), static_cast<int>(stage.renderTargetCount), nullptr,
            clearFlags, clearColors, depth, 0, clearAttachments ? 1 : 0, 0.0, 0, 0, renderWidth, renderHeight); !pass)
    {
        vanilla::failRasterInputPass(context);
        return pass;
    }
    std::array<D3D12_VIEWPORT, mcdr::terrain_tile::maximumResidentTiles> viewports{};
    std::array<D3D12_RECT, mcdr::terrain_tile::maximumResidentTiles> scissors{};
    const auto& lane = mcdr::terrain_tile::laneOffsets[batch.laneIndex];
    const auto& sourceViewport = inputs.sourceViewport;
    const auto& sourceScissor = inputs.sourceScissor;
    constexpr auto guard = static_cast<int64_t>(mcdr::terrain_tile::guardPixels);
    for (uint32_t index = 0; index < batch.residentTileCount; ++index) {
        const auto& tile = batch.tiles[index];
        const int64_t shiftX = guard - tile.originX;
        const int64_t shiftY = guard - tile.originY;
        viewports[index] = {sourceViewport.TopLeftX + static_cast<float>(shiftX) - lane[0],
            sourceViewport.TopLeftY + static_cast<float>(shiftY) - lane[1],
            sourceViewport.Width, sourceViewport.Height, sourceViewport.MinDepth, sourceViewport.MaxDepth};
        const int64_t guardedWidth = tile.effectiveWidth + guard * 2;
        const int64_t guardedHeight = tile.effectiveHeight + guard * 2;
        scissors[index] = {
            static_cast<LONG>(std::clamp(sourceScissor.left + shiftX, int64_t{0}, guardedWidth)),
            static_cast<LONG>(std::clamp(sourceScissor.top + shiftY, int64_t{0}, guardedHeight)),
            static_cast<LONG>(std::clamp(sourceScissor.right + shiftX, int64_t{0}, guardedWidth)),
            static_cast<LONG>(std::clamp(sourceScissor.bottom + shiftY, int64_t{0}, guardedHeight)),
        };
    }
    context->commandList->RSSetViewports(batch.residentTileCount, viewports.data());
    context->commandList->RSSetScissorRects(batch.residentTileCount, scissors.data());
    restoreDrawHeap(context);
    context->commandList->SetPipelineState(pipeline->withDepth.Get());
    context->commandList->SetGraphicsRootSignature(pipeline->rootSignature.Get());
    context->graphicsBindingRoot = pipeline->rootSignature.Get();
    context->currentPipeline = pipeline;
    context->commandList->SetGraphicsRootConstantBufferView(route->segmentParam, geometry.segmentAddress);
    context->commandList->SetGraphicsRootShaderResourceView(route->geometryParam, geometry.vertexAddress);
    context->commandList->SetGraphicsRootShaderResourceView(route->geometryParam + 1, geometry.visibleAddress);
    context->commandList->SetGraphicsRootShaderResourceView(route->geometryParam + 2, geometry.atlasAddress);
    context->commandList->SetGraphicsRootConstantBufferView(route->batchParam,
        geometry.batchConstantsAddress);
    if (inputs.pushConstantBytes != 0)
        context->commandList->SetGraphicsRoot32BitConstants(pipeline->pushConstantParam,
            inputs.pushConstantBytes / 4u, inputs.pushConstants.data(), 0);
    if (const auto bindings = pushDescriptors(context, materialBindings, DescriptorBindPoint::GraphicsDeclared); !bindings) {
        static_cast<void>(endRenderPass(context));
        vanilla::failRasterInputPass(context);
        return bindings;
    }
    if (!opaqueSeed) {
        const auto* materialHeap = context->resourceDrawHeap.Get();
        auto descriptorsResult = createTileReplayDescriptors(context, batch,
            static_cast<mcdr::terrain_tile::ReplayStage>(stageIndex));
        if (!descriptorsResult) {
            static_cast<void>(endRenderPass(context));
            vanilla::failRasterInputPass(context);
            return std::unexpected<mcdr::Error>(descriptorsResult.error());
        }
        if (materialHeap != context->resourceDrawHeap.Get()) {
            if (const auto bindings = pushDescriptors(context, materialBindings,
                    DescriptorBindPoint::GraphicsDeclared); !bindings) {
                static_cast<void>(endRenderPass(context));
                vanilla::failRasterInputPass(context);
                return bindings;
            }
        }
        context->commandList->SetGraphicsRootDescriptorTable(route->descriptorParam, descriptorsResult->gpu);
    }
    auto* commands = static_cast<Dx12Object*>(batch.commands.object);
    auto* control = static_cast<Dx12Object*>(batch.control.object);
    if (prepareSampledRenderPassWrites(context) != SampledTextureContentStatus::Ready) {
        vanilla::failRasterInputPass(context);
        return mcdr::err("terrain.tile.replay", "Sampled texture contents could not be preserved before writing");
    }
    context->commandList->ExecuteIndirect(route->signature.Get(), batch.refCapacity,
        commands->resource.Get(), batch.commands.offset, control->resource.Get(), batch.control.offset);
    context->cachedPso = nullptr;
    for (size_t index = 0; index < context->activeColorTargetsTouched.size(); ++index)
        context->activeColorTargetsTouched[index] = true;
    context->colorTargetsWritten = stage.renderTargetCount != 0;
    if (auto ended = endRenderPass(context); !ended) {
        vanilla::failRasterInputPass(context);
        return ended;
    }
    if (vanilla::finishDeclaredPass(context, declaration) != 1)
        return mcdr::err("terrain.tile.replay", vanilla::lastFailureText());
    return {};
}

}

mcdr::Result<void> initializeTerrainTileLane(void* opaqueContext, const mcdr::terrain_tile::BatchV2& batch) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->listOpen || !context->commandList || context->inRenderPass)
        return mcdr::err("terrain.tile.initialize", "Tile lane clear requires an open command list outside a pass");
    constexpr auto transmittanceStage = static_cast<uint32_t>(mcdr::terrain_tile::ReplayStage::Transmittance);
    const auto coefficientCount = batch.stages[transmittanceStage].renderTargetCount;
    if (coefficientCount != 1 && coefficientCount != 2 && coefficientCount != 4)
        return mcdr::err("terrain.tile.initialize", "Tile lane coefficient count is invalid");

    const mcdr::terrain_tile::ResourceRef targets[] = {
        batch.bounds, batch.q, batch.coefficients[0], batch.coefficients[1],
        batch.coefficients[2], batch.coefficients[3],
    };
    vanilla::PassResourceUse declarations[std::size(targets)]{};
    const uint32_t targetCount = coefficientCount + 2;
    for (uint32_t index = 0; index < targetCount; ++index) {
        const auto* texture = static_cast<const Dx12Object*>(targets[index].object);
        const auto format = index == 0 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!texture || texture->kind != Dx12Object::Kind::Texture || !texture->resource ||
            texture->dxgiFormat != format)
            return mcdr::err("terrain.tile.initialize", "Tile lane clear target format is invalid");
        declarations[index] = {targets[index].object, batch.resourceVersion, UINT32_MAX, 0,
            vanilla::ResourceAccess::Write, D3D12_RESOURCE_STATE_RENDER_TARGET};
    }
    vanilla::DeclaredPassToken* declaration = nullptr;
    if (vanilla::beginDeclaredPass(context, 0x214u, declarations, targetCount, 0, &declaration) != 1)
        return mcdr::err("terrain.tile.initialize", vanilla::lastFailureText());
    for (uint32_t index = 0; index < targetCount; ++index) {
        const float red = index == 0 ? std::numeric_limits<float>::lowest() : 0.0f;
        if (auto cleared = clearColorTexture(context, static_cast<Dx12Object*>(targets[index].object),
                red, 0.0f, 0.0f, 0.0f); !cleared) {
            vanilla::failRasterInputPass(context);
            return cleared;
        }
    }
    if (vanilla::finishDeclaredPass(context, declaration) != 1)
        return mcdr::err("terrain.tile.initialize", vanilla::lastFailureText());
    return {};
}

mcdr::Result<void> recordTerrainTileReplay(void* context, void* route,
    const mcdr::terrain_tile::BatchV2& batch, const TerrainTileGeometryV2& geometry,
    const TerrainTileReplayInputsV2& inputs, uint32_t stageIndex, TerrainTileAttachmentMode attachmentMode) {
    if (auto valid = validateTerrainTileReplay(route, batch, inputs, stageIndex); !valid) return valid;
    return recordTerrainReplay(context, route, batch, batch.stages[stageIndex], geometry, inputs, attachmentMode);
}

mcdr::Result<void> recordTerrainOpaqueSeedReplay(void* context, void* route,
    const mcdr::terrain_tile::BatchV2& batch, const TerrainTileGeometryV2& geometry,
    const TerrainTileReplayInputsV2& inputs, const mcdr::terrain_tile::ResourceRef& depth) {
    if (auto valid = validateTerrainOpaqueSeedReplay(route, batch, inputs, depth); !valid) return valid;
    const auto stage = opaqueSeedStageBinding(static_cast<TerrainTileRoute*>(route), depth);
    return recordTerrainReplay(context, route, batch, stage, geometry, inputs, TerrainTileAttachmentMode::Load);
}

mcdr::Result<void*> createMeshPipeline(const MeshPipelineDesc& desc) {
    return createMeshPipelineImpl(desc, 0);
}

mcdr::Result<void*> createMeshPipeline(const MeshPipelineDesc& desc, uint64_t externalRootSignature) {
    return createMeshPipelineImpl(desc, externalRootSignature);
}

}
