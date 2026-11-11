module;

#include "_d3d12_module_prelude.h"
#include "mcdr_smaa_luts.h"
#include <cwchar>

module mcdr.d3d12.device;

import mcdr.error;

namespace d3d12_mc {
namespace {

// VS: 把 2D 位置直接作为 SV_Position 输出；texcoord 透传给 PS。
static const char kBlitVS[] = "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
                              "VS_OUT main(float2 pos : POSITION, float2 tc : TEXCOORD0) {\n"
                              "    VS_OUT o;\n"
                              "    o.pos = float4(pos, 0.0f, 1.0f);\n"
                              "    o.tc = tc;\n"
                              "    return o;\n"
                              "}\n";

// PS: 对源纹理线性采样，输出到 backbuffer；GPU 光栅器自动处理格式转换。
// 注意：'sampler' 是 HLSL 保留字，不能作变量名；PS 单独编译，须自带 VS_OUT 定义。
static const char kBlitPS[] = "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
                              "Texture2D<float4> srcTex : register(t0, space0);\n"
                              "SamplerState blitSampler : register(s0, space0);\n"
                              "float4 main(VS_OUT IN) : SV_Target {\n"
                              "    return srcTex.Sample(blitSampler, IN.tc);\n"
                              "}\n";

// P197（HDR）：同一份 blit 几何，像素着色器把 SDR 主渲染目标（RGBA8，内容按 sRGB 编码）
// 解码成线性、乘上「SDR 白 = 多少 scRGB 单位」，写进 R16G16B16A16_FLOAT 的交换链。
// scRGB 的 1.0 定义为 80 nits，所以 sdrWhiteScale = SDRWhiteLevel(nits) / 80。
// hdrEnabled 走 root constants：SDR 呈现时同一个 PSO 传 0 也能用（只是多一次分支）。
static const char kBlitPSHdr[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float4> srcTex : register(t0, space0);\n"
    "SamplerState blitSampler : register(s0, space0);\n"
    "cbuffer BlitHdrParams : register(b0, space0) {\n"
    "    float sdrWhiteScale;  // SDR white (nits) / 80\n"
    "    float hdrEnabled;     // 0 = pass-through, 1 = sRGB->linear\n"
    "    float2 hdrPadding;\n"
    "};\n"
    "float3 srgbToLinear(float3 c) {\n"
    "    return (c <= 0.04045f) ? (c / 12.92f) : pow((c + 0.055f) / 1.055f, 2.4f);\n"
    "}\n"
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    float4 c = srcTex.Sample(blitSampler, IN.tc);\n"
    "    // hdrEnabled=0：float 链但没开 HDR（或色彩空间被拒）→ 原样直通，与 SDR 等价。\n"
    "    if (hdrEnabled < 0.5f) return c;\n"
    "    return float4(srgbToLinear(c.rgb) * sdrWhiteScale, c.a);\n"
    "}\n";

}  // namespace

mcdr::Result<void> initBlitPipeline() {
    if (auto d = ensureDevice(); !d) return mcdr::err("initBlitPipeline", errorMessage(d.error()));
    if (deviceContext().blitPipeline) return {};  // 已初始化

    auto bp = std::make_unique<BlitPipeline>();

    ComPtr<ID3D12RootSignature> rootSig;
    {
        // 单个 descriptor table：SRV(t0) + static sampler(s0)
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE ranges[] = {srvRange};

        D3D12_ROOT_DESCRIPTOR_TABLE table{};
        table.NumDescriptorRanges = 1;
        table.pDescriptorRanges = ranges;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable = table;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // P197：blit 的 HDR 参数（sdrWhiteScale / hdrEnabled）。SDR PSO 不声明这个 cbuffer，
        // 但根签名里的参数必须一致，所以两个 PSO 共用同一份根签名，SDR 路径设成 (1,0) 即可。
        D3D12_ROOT_PARAMETER hdrParam{};
        hdrParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        hdrParam.Constants.ShaderRegister = 0;
        hdrParam.Constants.RegisterSpace = 0;
        hdrParam.Constants.Num32BitValues = 4;
        hdrParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_PARAMETER params[2] = {param, hdrParam};

        D3D12_STATIC_SAMPLER_DESC staticSam{};
        staticSam.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSam.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.MipLODBias = 0.0f;
        staticSam.MaxAnisotropy = 1;
        staticSam.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        staticSam.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        staticSam.MinLOD = 0.0f;
        staticSam.MaxLOD = D3D12_FLOAT32_MAX;
        staticSam.ShaderRegister = 0;
        staticSam.RegisterSpace = 0;
        staticSam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &staticSam;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
        if (FAILED(hr)) {
            return mcdr::err("initBlitPipeline", "initBlitPipeline: SerializeRootSignature hr=0x" + formatBackendResult(hr));
        }
        hr = deviceContext().device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                              IID_PPV_ARGS(&rootSig));
        if (FAILED(hr)) {
            return mcdr::err("initBlitPipeline", "initBlitPipeline: CreateRootSignature hr=0x" + formatBackendResult(hr));
        }
    }

    // 编译 VS / PS。与主图形管线同一策略：硬要求 SM 6.9 + DXC，**没有 5.1(DXBC) 兜底**
    // （2026-10-25）—— 不满足就直接报错，blit 不通就等于 Present 不通，必须当场可见。
    const std::string smProfile = graphicsShaderModelProfile();
    if (smProfile.empty()) {
        return mcdr::err("initBlitPipeline", "initBlitPipeline: " + sm69RequirementText());
    }
    std::vector<uint8_t> vsDxil, psDxil;
    const std::string vsSrc(kBlitVS, sizeof(kBlitVS) - 1);
    const std::string psSrc(kBlitPS, sizeof(kBlitPS) - 1);
    auto vsDxc = compileHlslToDxil(vsSrc, "main", "vs_" + smProfile, vsDxil);
    if (!vsDxc) {
        return mcdr::err("initBlitPipeline", "blit_vs (DXC): " + firstLine(vsDxc.error().message));
    }
    auto psDxc = compileHlslToDxil(psSrc, "main", "ps_" + smProfile, psDxil);
    if (!psDxc) {
        return mcdr::err("initBlitPipeline", "blit_ps (DXC): " + firstLine(psDxc.error().message));
    }
    const void* vsCode = vsDxil.data();
    SIZE_T vsCodeSize = vsDxil.size();
    const void* psCode = psDxil.data();
    SIZE_T psCodeSize = psDxil.size();
    dbgLogInfo("initBlitPipeline: blit pipeline compiled with DXC SM %s (DXIL vs=%zuB ps=%zuB)", smProfile.c_str(),
               vsDxil.size(), psDxil.size());

    // P197：HDR 变体的像素着色器（几何与 VS 不变，只有 RTV 格式和 PS 不同）。
    std::vector<uint8_t> hdrPsDxil;
    const std::string hdrSrc(kBlitPSHdr, sizeof(kBlitPSHdr) - 1);
    auto hdrDxc = compileHlslToDxil(hdrSrc, "main", "ps_" + smProfile, hdrPsDxil);
    if (!hdrDxc) {
        return mcdr::err("initBlitPipeline", "blit HDR PS (DXC): " + firstLine(hdrDxc.error().message));
    }
    const void* hdrPsCode = hdrPsDxil.data();
    SIZE_T hdrPsCodeSize = hdrPsDxil.size();

    // 创建 PSO（P197：SDR 用 RGBA8、HDR 用 RGBA16F，共用同一份根签名与输入布局）
    auto createBlitPso = [&](const void* pixCode, SIZE_T pixSize, DXGI_FORMAT rtvFormat,
                             ComPtr<ID3D12PipelineState>& outPso) -> mcdr::Result<void> {
        D3D12_INPUT_ELEMENT_DESC ie[2] = {};
        ie[0].SemanticName = "POSITION";
        ie[0].SemanticIndex = 0;
        ie[0].Format = DXGI_FORMAT_R32G32_FLOAT;
        ie[0].InputSlot = 0;
        ie[0].AlignedByteOffset = 0;
        ie[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        ie[0].InstanceDataStepRate = 0;

        ie[1].SemanticName = "TEXCOORD";
        ie[1].SemanticIndex = 0;
        ie[1].Format = DXGI_FORMAT_R32G32_FLOAT;
        ie[1].InputSlot = 0;
        ie[1].AlignedByteOffset = 8;
        ie[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        ie[1].InstanceDataStepRate = 0;

        D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
        rtBlend.BlendEnable = FALSE;
        rtBlend.LogicOpEnable = FALSE;
        rtBlend.SrcBlend = D3D12_BLEND_ONE;
        rtBlend.DestBlend = D3D12_BLEND_ZERO;
        rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_BLEND_DESC blendDesc{};
        blendDesc.AlphaToCoverageEnable = FALSE;
        blendDesc.IndependentBlendEnable = FALSE;
        blendDesc.RenderTarget[0] = rtBlend;

        D3D12_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
        rasterDesc.CullMode = D3D12_CULL_MODE_NONE;  // 全屏 quad，无背面剔除
        rasterDesc.FrontCounterClockwise = FALSE;
        rasterDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        rasterDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        rasterDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        rasterDesc.DepthClipEnable = TRUE;
        rasterDesc.MultisampleEnable = FALSE;
        rasterDesc.AntialiasedLineEnable = FALSE;

        D3D12_DEPTH_STENCIL_DESC dsDesc{};
        dsDesc.DepthEnable = FALSE;
        dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        dsDesc.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSig.Get();
        psoDesc.VS = {vsCode, vsCodeSize};
        psoDesc.PS = {pixCode, pixSize};
        psoDesc.InputLayout = {ie, 2};
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = rtvFormat;
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleMask = UINT_MAX;  // P25 fix：漏设 SampleMask → 0 → sample-mask 测试
        // 丢弃所有片元（dx12-native.log 632 行 WARNING）
        psoDesc.BlendState = blendDesc;
        psoDesc.RasterizerState = rasterDesc;
        psoDesc.DepthStencilState = dsDesc;
        psoDesc.Flags = D3D12_PIPELINE_STATE_FLAGS(0);

        recordPipelineCreation();
        const HRESULT hr = deviceContext().device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&outPso));
        if (FAILED(hr)) {
            return mcdr::err("initBlitPipeline",
                             "initBlitPipeline: CreateGraphicsPipelineState(rtv=" +
                                 std::to_string(static_cast<int>(rtvFormat)) + ") hr=0x" + formatBackendResult(hr));
        }
        return {};
    };

    ComPtr<ID3D12PipelineState> pso;
    if (auto cr = createBlitPso(psCode, psCodeSize, DXGI_FORMAT_R8G8B8A8_UNORM, pso); !cr) {
        return cr;
    }
    ComPtr<ID3D12PipelineState> psoHdr;
    if (auto cr = createBlitPso(hdrPsCode, hdrPsCodeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, psoHdr); !cr) {
        return cr;
    }

    // 顶点缓冲：4 顶点，R32G32(position) + R32G32(texcoord)，upload heap
    {
        struct Vertex {
            float x, y;
            float u, v;
        };
        Vertex verts[4] = {
            {-1.0f, -1.0f, 0.0f, 1.0f},  // bottom-left
            {1.0f, -1.0f, 1.0f, 1.0f},  // bottom-right
            {-1.0f, 1.0f, 0.0f, 0.0f},  // top-left
            {1.0f, 1.0f, 1.0f, 0.0f},  // top-right
        };
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(verts);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = deviceContext().device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&bp->vertBuf));
        if (FAILED(hr)) {
            return mcdr::err("initBlitPipeline", "initBlitPipeline: vertBuf create hr=0x" + formatBackendResult(hr));
        }
        void* p = nullptr;
        hr = bp->vertBuf->Map(0, nullptr, &p);
        if (SUCCEEDED(hr) && p) {
            std::memcpy(p, verts, sizeof(verts));
            bp->vertBuf->Unmap(0, nullptr);
        }
        bp->vbView.BufferLocation = bp->vertBuf->GetGPUVirtualAddress();
        bp->vbView.SizeInBytes = sizeof(verts);
        bp->vbView.StrideInBytes = sizeof(Vertex);
    }

    // 索引缓冲：2 个三角形（顺时针，CullMode=NONE，顺序无关）
    {
        uint16_t idxs[6] = {0, 1, 2, 1, 3, 2};
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(idxs);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = deviceContext().device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&bp->idxBuf));
        if (FAILED(hr)) {
            return mcdr::err("initBlitPipeline", "initBlitPipeline: idxBuf create hr=0x" + formatBackendResult(hr));
        }
        void* p = nullptr;
        hr = bp->idxBuf->Map(0, nullptr, &p);
        if (SUCCEEDED(hr) && p) {
            std::memcpy(p, idxs, sizeof(idxs));
            bp->idxBuf->Unmap(0, nullptr);
        }
        bp->ibView.BufferLocation = bp->idxBuf->GetGPUVirtualAddress();
        bp->ibView.SizeInBytes = sizeof(idxs);
        bp->ibView.Format = DXGI_FORMAT_R16_UINT;
    }

    bp->rootSig = rootSig;
    bp->pso = pso;
    bp->psoHdr = psoHdr;
    deviceContext().blitPipeline = std::move(bp);
    dbgLogInfo("initBlitPipeline: done rootSig=%p pso=%p psoHdr=%p",
               reinterpret_cast<void*>(rootSig.Get()), reinterpret_cast<void*>(pso.Get()),
               reinterpret_cast<void*>(psoHdr.Get()));
    return {};
}

const BlitPipeline* getBlitPipeline() { return deviceContext().blitPipeline.get(); }

// P121：锐化 PS —— 5-tap unsharp（中心 + 上下左右），参数走 32-bit 根常量 b1：
// {rcpSize.x, rcpSize.y, amount, pad}。alpha 恒 1（目标是 RGBA8 世界层/呈现目标）。
static const char kSharpenPS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float4> srcTex : register(t0, space0);\n"
    "SamplerState sharpenSampler : register(s0, space0);\n"
    "cbuffer SharpenParams : register(b1, space0) {\n"
    "    float2 RcpSize;\n"
    "    float Amount;\n"
    "    float Pad0;\n"
    "};\n"
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    float3 c = srcTex.Sample(sharpenSampler, IN.tc).rgb;\n"
    "    float3 blur = (srcTex.Sample(sharpenSampler, IN.tc + float2( RcpSize.x, 0.0)).rgb\n"
    "                 + srcTex.Sample(sharpenSampler, IN.tc + float2(-RcpSize.x, 0.0)).rgb\n"
    "                 + srcTex.Sample(sharpenSampler, IN.tc + float2(0.0,  RcpSize.y)).rgb\n"
    "                 + srcTex.Sample(sharpenSampler, IN.tc + float2(0.0, -RcpSize.y)).rgb) * 0.25;\n"
    "    return float4(max(c + (c - blur) * Amount, 0.0), 1.0);\n"
    "}\n";

mcdr::Result<void> initSharpenPipeline() {
    if (auto d = ensureDevice(); !d) return mcdr::err("initSharpenPipeline", errorMessage(d.error()));
    if (deviceContext().sharpenPipeline) return {};

    // 顶点/索引缓冲与 blit 共用：先确保 blit 管线建好，再把 view 抄过来。
    if (auto b = initBlitPipeline(); !b) {
        return mcdr::err("initSharpenPipeline", errorMessage(b.error()));
    }
    const BlitPipeline* bp = getBlitPipeline();
    if (bp == nullptr) {
        return mcdr::err("initSharpenPipeline", "initSharpenPipeline: blit pipeline unavailable");
    }

    auto sp = std::make_unique<SharpenPipeline>();
    ComPtr<ID3D12RootSignature> rootSig;
    {
        // 参数 0：SRV 描述符表（t0）；参数 1：4 个 32-bit 根常量（b1）。
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_DESCRIPTOR_TABLE table{};
        table.NumDescriptorRanges = 1;
        table.pDescriptorRanges = &srvRange;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = table;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 1;  // b1
        params[1].Constants.RegisterSpace = 0;
        params[1].Constants.Num32BitValues = 4;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC staticSam{};
        staticSam.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSam.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.MipLODBias = 0.0f;
        staticSam.MaxAnisotropy = 1;
        staticSam.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        staticSam.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        staticSam.MinLOD = 0.0f;
        staticSam.MaxLOD = D3D12_FLOAT32_MAX;
        staticSam.ShaderRegister = 0;
        staticSam.RegisterSpace = 0;
        staticSam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &staticSam;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
        if (FAILED(hr)) {
            return mcdr::err("initSharpenPipeline",
                             "initSharpenPipeline: SerializeRootSignature hr=0x" + formatBackendResult(hr));
        }
        hr = deviceContext().device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                              IID_PPV_ARGS(&rootSig));
        if (FAILED(hr)) {
            return mcdr::err("initSharpenPipeline",
                             "initSharpenPipeline: CreateRootSignature hr=0x" + formatBackendResult(hr));
        }
    }

    // 编译 VS（复用 blit 的）/ PS：与主图形管线同一策略 —— 硬要求 SM 6.9 + DXC，无 5.1 兜底。
    const std::string smProfile = graphicsShaderModelProfile();
    if (smProfile.empty()) {
        return mcdr::err("initSharpenPipeline", "initSharpenPipeline: " + sm69RequirementText());
    }
    std::vector<uint8_t> vsDxil, psDxil;
    const std::string vsSrc(kBlitVS, sizeof(kBlitVS) - 1);
    const std::string psSrc(kSharpenPS, sizeof(kSharpenPS) - 1);
    auto vsDxc = compileHlslToDxil(vsSrc, "main", "vs_" + smProfile, vsDxil);
    if (!vsDxc) {
        return mcdr::err("initSharpenPipeline", "sharpen_vs (DXC): " + firstLine(vsDxc.error().message));
    }
    auto psDxc = compileHlslToDxil(psSrc, "main", "ps_" + smProfile, psDxil);
    if (!psDxc) {
        return mcdr::err("initSharpenPipeline", "sharpen_ps (DXC): " + firstLine(psDxc.error().message));
    }
    const void* vsCode = vsDxil.data();
    SIZE_T vsCodeSize = vsDxil.size();
    const void* psCode = psDxil.data();
    SIZE_T psCodeSize = psDxil.size();
    dbgLogInfo("initSharpenPipeline: compiled with DXC SM %s", smProfile.c_str());

    // PSO：与 blit 完全同状态（noDepth / noBlend / noCull），只是 PS 不同。
    ComPtr<ID3D12PipelineState> pso;
    {
        D3D12_INPUT_ELEMENT_DESC ie[2] = {};
        ie[0].SemanticName = "POSITION";
        ie[0].Format = DXGI_FORMAT_R32G32_FLOAT;
        ie[0].InputSlot = 0;
        ie[0].AlignedByteOffset = 0;
        ie[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        ie[1].SemanticName = "TEXCOORD";
        ie[1].Format = DXGI_FORMAT_R32G32_FLOAT;
        ie[1].InputSlot = 0;
        ie[1].AlignedByteOffset = 8;
        ie[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

        D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
        rtBlend.BlendEnable = FALSE;
        rtBlend.SrcBlend = D3D12_BLEND_ONE;
        rtBlend.DestBlend = D3D12_BLEND_ZERO;
        rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0] = rtBlend;

        D3D12_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
        rasterDesc.CullMode = D3D12_CULL_MODE_NONE;
        rasterDesc.DepthClipEnable = TRUE;

        D3D12_DEPTH_STENCIL_DESC dsDesc{};
        dsDesc.DepthEnable = FALSE;
        dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        dsDesc.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSig.Get();
        psoDesc.VS = {vsCode, vsCodeSize};
        psoDesc.PS = {psCode, psCodeSize};
        psoDesc.InputLayout = {ie, 2};
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.BlendState = blendDesc;
        psoDesc.RasterizerState = rasterDesc;
        psoDesc.DepthStencilState = dsDesc;

        recordPipelineCreation();
        HRESULT hr = deviceContext().device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            return mcdr::err("initSharpenPipeline",
                             "initSharpenPipeline: CreateGraphicsPipelineState hr=0x" + formatBackendResult(hr));
        }
    }

    sp->rootSig = rootSig;
    sp->pso = pso;
    sp->vertBuf = bp->vertBuf;
    sp->idxBuf = bp->idxBuf;
    sp->vbView = bp->vbView;
    sp->ibView = bp->ibView;
    deviceContext().sharpenPipeline = std::move(sp);
    dbgLogInfo("initSharpenPipeline: done rootSig=%p pso=%p", reinterpret_cast<void*>(rootSig.Get()),
               reinterpret_cast<void*>(pso.Get()));
    return {};
}

const SharpenPipeline* getSharpenPipeline() { return deviceContext().sharpenPipeline.get(); }

mcdr::Result<void> blitBindSourceTexture(CommandContext* ctx, Dx12Object* srcTex,
                                         ID3D12GraphicsCommandList* cmd) {
    if (!srcTex || !srcTex->resource) {
        return mcdr::err("blitBindSourceTexture", "blitBindSourceTexture: null or invalid srcTex");
    }
    // 源纹理过渡到 PIXEL_SHADER_RESOURCE（采样必需）。
    transitionTextureTo(ctx, srcTex,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                            | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // P24 fix（黑屏根因之一）：SRV 必须写入命令列表当前设置的堆。
    // 此前在 srvHeap 分配 SRV 并直接 SetGraphicsRootDescriptorTable，但
    // beginCommandList 只 SetDescriptorHeaps({drawHeap}) → 每帧验证错误
    // "descriptor heap containing handle is different from currently set
    // descriptor heap"（dx12-native.log 658 行），根表绑定无效。
    // 改为复用 pushDescriptors 的机制：把 SRV 写到本帧 drawHeap 瞬态槽位，
    // root table 绑定该槽位的 GPU 句柄（与命令列表当前堆一致）。
    if (ctx->nextDrawSlot + 1 > syncDrawDescriptorCapacity()) {
        return mcdr::err("blitBindSourceTexture",
                         "blitBindSourceTexture: draw descriptor heap exhausted for this frame");
    }
    SIZE_T base = static_cast<SIZE_T>(ctx->drawHeapSlotBase + ctx->nextDrawSlot) * deviceContext().drawInc;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = deviceContext().drawHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += base;
    ctx->nextDrawSlot += 1;

    DXGI_FORMAT viewFmt = srcTex->resource->GetDesc().Format;
    switch (viewFmt) {
        case DXGI_FORMAT_D32_FLOAT: viewFmt = DXGI_FORMAT_R32_FLOAT; break;
        case DXGI_FORMAT_D16_UNORM: viewFmt = DXGI_FORMAT_R16_UNORM; break;
        default: break;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = viewFmt;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    deviceContext().device->CreateShaderResourceView(srcTex->resource.Get(), &srvDesc, srvCpu);

    // 绑定根描述符表（表 0 = blit 的 SRV；drawHeap 已是命令列表当前设置的堆）。
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = deviceContext().drawHeap->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += base;
    cmd->SetGraphicsRootDescriptorTable(0, srvGpu);
    // 逐帧诊断（每帧 1 次 stderr+文件 flush）→ DEBUG，需 DX12_LOG_VERBOSE=1 才输出。
    dbgLogDebug("blitBindSourceTexture: drawHeapSlotBase=%u nextDrawSlot=%u srvGpu=%llx",
                ctx->drawHeapSlotBase, ctx->nextDrawSlot, static_cast<unsigned long long>(srvGpu.ptr));
    return {};
}

// ---------------------------------------------------------------------------
// P104: SMAA（Subpixel Morphological Antialiasing）
// ---------------------------------------------------------------------------
// 插入点：blitSurface 把主颜色目标（源纹理）拷到 back buffer **之前**（见 d3d12_surface.cpp）。
// blit 本身就是"采样全屏四边形"（P22），所以这里天然能插后处理；SMAA 只看最终 LDR 颜色
// （亮度边检测），不需要深度/法线/运动矢量。
// 三个阶段，全部在**源分辨率**上跑，第三步顺带线性放大到窗口尺寸（⇒ 与分辨率解耦兼容）：
//   ① 边检测   source            → edges   (R8G8_UNORM，.r=与左邻有边 .g=与上邻有边)
//   ② 混合权重 edges + LUTs      → weights (R8G8_UNORM，.r=沿 x 混 .g=沿 y 混)
//   ③ 邻域混合 source + weights  → back buffer（写进 back buffer ⇒ 调用方跳过纯 blit）
// ② 用的是**官方算法**：把上游 SMAA.hlsl 的 SMAABlendingWeightCalculationPS（含斜向搜索、
// 拐角修正）整段搬进来，权重由 AreaTex(160x560 RG8) / SearchTex(64x16 R8) 两张 LUT 查表得出
// （数据见 include\mcdr_smaa_luts.h，字节级等于上游 Textures\AreaTexDX10.dds / SearchTex.dds）。
// LUT 初始化失败时②退回 kSmaaWeightPS 的解析式近似（WARN 一行，功能不降级为崩溃/黑屏）。
// TAA（模式 2）的实现见本文件末尾的 P152 分节（applyTaaToTexture，历史乒乓由 native 持有）；
// DLAA（3）仍由 DLSS 特性实现，setAntiAliasing 收到 3 时只记一条 WARN。
namespace {

// ① 边检测：亮度差超阈值即在 .r（与左邻）/ .g（与上邻）打标记。
const char kSmaaEdgePS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float4> srcTex : register(t0, space0);\n"
    "SamplerState smaaSampler : register(s0, space0);\n"
    "static const float kThreshold = 0.10f;\n"
    "static const float3 kLumaW = float3(0.2126f, 0.7152f, 0.0722f);\n"
    "float SmaaLuma(float3 c) { return dot(c, kLumaW); }\n"
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    uint w, h; srcTex.GetDimensions(w, h);\n"
    "    float2 texel = float2(1.0f / (float)w, 1.0f / (float)h);\n"
    "    float lc = SmaaLuma(srcTex.SampleLevel(smaaSampler, IN.tc, 0).rgb);\n"
    "    float ll = SmaaLuma(srcTex.SampleLevel(smaaSampler, IN.tc - float2(texel.x, 0.0f), 0).rgb);\n"
    "    float lt = SmaaLuma(srcTex.SampleLevel(smaaSampler, IN.tc - float2(0.0f, texel.y), 0).rgb);\n"
    "    float2 e = step(kThreshold, float2(abs(lc - ll), abs(lc - lt)));\n"
    "    return float4(e, 0.0f, 0.0f);\n"
    "}\n";

// ② 混合权重：沿边往上下（垂直边）或左右（水平边）找最近的"台阶转折"，越近权重越大。
// 找不到转折（直边）⇒ 权重 0，直边不会被糊。
const char kSmaaWeightPS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float2> edgesTex : register(t0, space0);\n"
    "SamplerState smaaSampler : register(s0, space0);\n"
    "static const int kMaxSearch = 8;\n"
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    uint w, h; edgesTex.GetDimensions(w, h);\n"
    "    float2 texel = float2(1.0f / (float)w, 1.0f / (float)h);\n"
    "    float2 e = edgesTex.SampleLevel(smaaSampler, IN.tc, 0).rg;\n"
    "    float wx = 0.0f, wy = 0.0f;\n"
    "    if (e.r > 0.5f) {\n"          // 本像素左侧有垂直边 → 沿 y 找台阶
    "        int up = 0, down = 0;\n"
    "        for (int k = 1; k <= kMaxSearch; ++k) {\n"
    "            float2 n = edgesTex.SampleLevel(smaaSampler, IN.tc - float2(0.0f, texel.y * k), 0).rg;\n"
    "            if (n.r < 0.5f) { up = (n.g > 0.5f) ? k : 0; break; }\n"
    "        }\n"
    "        for (int k = 1; k <= kMaxSearch; ++k) {\n"
    "            float2 n = edgesTex.SampleLevel(smaaSampler, IN.tc + float2(0.0f, texel.y * k), 0).rg;\n"
    "            if (n.r < 0.5f) { down = (n.g > 0.5f) ? k : 0; break; }\n"
    "        }\n"
    "        int near = (up == 0) ? down : ((down == 0) ? up : min(up, down));\n"
    "        if (near > 0) wx = saturate(0.5f / (float)near);\n"
    "    }\n"
    "    if (e.g > 0.5f) {\n"          // 本像素上方有水平边 → 沿 x 找台阶
    "        int left = 0, right = 0;\n"
    "        for (int k = 1; k <= kMaxSearch; ++k) {\n"
    "            float2 n = edgesTex.SampleLevel(smaaSampler, IN.tc - float2(texel.x * k, 0.0f), 0).rg;\n"
    "            if (n.g < 0.5f) { left = (n.r > 0.5f) ? k : 0; break; }\n"
    "        }\n"
    "        for (int k = 1; k <= kMaxSearch; ++k) {\n"
    "            float2 n = edgesTex.SampleLevel(smaaSampler, IN.tc + float2(texel.x * k, 0.0f), 0).rg;\n"
    "            if (n.g < 0.5f) { right = (n.r > 0.5f) ? k : 0; break; }\n"
    "        }\n"
    "        int near = (left == 0) ? right : ((right == 0) ? left : min(left, right));\n"
    "        if (near > 0) wy = saturate(0.5f / (float)near);\n"
    "    }\n"
    "    return float4(wx, wy, 0.0f, 1.0f);\n"
    "}\n";

// ②′ 混合权重（**官方 LUT 算法**）：与上游 SMAA.hlsl 的 SMAABlendingWeightCalculationPS 同一
// 算法与同一套常量（中等预设），只做了三处移植改动（都不改数学）：
//   - 上游 tex2D/tex2Dlod 宏 → 本工程写法：edges 用传进来的 smaaSampler，两张 LUT 用
//     smaaLutSampler（静态采样器 s1）；三张纹理都只写 SampleLevel(..., 0)。
//     上游的常量缓冲 smaa_rt_metrics / subsampleIndices / smaa_area_search_texel_size 换成
//     两个局部量 + 逐层传参：rt = float4(texelSize.xy, 1/texelSize)（对应上游 SMAA_RT_METRICS，
//     用 edges 的 texel size 在 main 里算出）、areaTexel/searchTexel = LUT 的 1/尺寸
//     （对应上游 SMAA_AREATEX_PIXEL_SIZE = 1/float2(160,560)、1/SMAA_SEARCHTEX_PACKED_SIZE
//     = 1/float2(64,16)）。之所以不照抄全局量：HLSL 的全局变量是隐式 const，不能像上游那样
//     在 ps 里赋值。SMAA 1x 的 subsampleIndices 恒 0。
//   - 上游的提前 return（SMAASearchXLeft 等函数体内的 while 循环）改成单出口：
//     循环体内 `if (!cond) break;` 等价于原 `while(cond)` 的退出条件，返回表达式逐字不变。
//     这样 SM 5.1 与 SM 6.x（DXC）都能编过，不依赖 fxc 的循环推断。
//   - 上游 r/g 与 b/a 的通道含义保持不变（r=沿 x 混、g=沿 y 混；b/a 是斜向权重，③ 本来就不用），
//     所以第 ③ 趟的读取约定一个字都不用改。
const char kSmaaWeightLutPS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float2> edgesTex : register(t0, space0);\n"
    "Texture2D<float2> areaTex  : register(t1, space0);\n"
    "Texture2D<float>  searchTex: register(t2, space0);\n"
    "SamplerState smaaSampler    : register(s0, space0);\n"  // MIN_MAG_MIP_LINEAR + CLAMP（原样保留）
    "SamplerState smaaLutSampler : register(s1, space0);\n"  // MIN_MAG_LINEAR_MIP_POINT + CLAMP
    // ---- 上游 SMAA.hlsl 的非可配置常量 ----
    "#define SMAA_THRESHOLD 0.10\n"                      // = 本工程既有 kThreshold（中等预设）
    "#define SMAA_MAX_SEARCH_STEPS 16\n"                // 中等预设
    "#define SMAA_MAX_SEARCH_STEPS_DIAG 8\n"            // 中等预设
    "#define SMAA_CORNER_ROUNDING 25\n"                 // 中等预设
    "#define SMAA_CORNER_ROUNDING_NORM 0.25\n"          // 25/100，常量折叠后避免隐式转换告警
    "#define SMAA_AREATEX_MAX_DISTANCE 16\n"
    "#define SMAA_AREATEX_MAX_DISTANCE_DIAG 20\n"
    "#define SMAA_AREATEX_SUBTEX_SIZE (1.0 / 7.0)\n"
    "#define SMAA_SEARCHTEX_SIZE float2(66.0, 33.0)\n"
    "#define SMAA_SEARCHTEX_PACKED_SIZE float2(64.0, 16.0)\n"
    // 两张 LUT 的 1/尺寸：上游写成 float2(1.0/160.0, 1.0/560.0) 与 float2(1.0/64.0, 1.0/16.0)。
    // 它们在 main 里由 areaTex / searchTex 的 GetDimensions 算出后逐层传参，数值完全一致
    // （HLSL 的全局变量是隐式 const，不能像上游那样在 ps 里赋值给全局，所以改成参数）。
    // 上游的两个宏在 HLSL4/5 下就是 smaaLutSampler + SampleLevel(..., 0)。
    "#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(smaaLutSampler, coord, 0)\n"
    "#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(smaaLutSampler, coord, 0, offset)\n"
    // ---- 上游 Diagonal Search Functions（SMAA.hlsl :831-985）----
    "float2 SMAADecodeDiagBilinearAccess(float2 e) {\n"
    "    e.r = e.r * abs(5.0 * e.r - 5.0 * 0.75);\n"
    "    return round(e);\n"
    "}\n"
    "float4 SMAADecodeDiagBilinearAccess(float4 e) {\n"
    "    e.rb = e.rb * abs(5.0 * e.rb - 5.0 * 0.75);\n"
    "    return round(e);\n"
    "}\n"
    "void SMAAMovc(bool2 cond, inout float2 variable, float2 value) {\n"
    "    if (cond.x) variable.x = value.x;\n"
    "    if (cond.y) variable.y = value.y;\n"
    "}\n"
    "float2 SMAASearchDiag1(float2 rt, float2 texcoord, float2 dir, out float2 e) {\n"
    "    float4 coord = float4(texcoord, -1.0, 1.0);\n"
    "    float3 t = float3(rt.xy, 1.0);\n"
    "    for (;;) {\n"
    "        if (!(coord.z < (float)(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)) break;\n"
    "        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);\n"
    "        e = SMAASampleLevelZero(edgesTex, coord.xy).rg;\n"
    "        coord.w = dot(e, float2(0.5, 0.5));\n"
    "    }\n"
    "    return coord.zw;\n"
    "}\n"
    "float2 SMAASearchDiag2(float2 rt, float2 texcoord, float2 dir, out float2 e) {\n"
    "    float4 coord = float4(texcoord, -1.0, 1.0);\n"
    "    coord.x += 0.25 * rt.x;\n"  // @SearchDiag2Optimization
    "    float3 t = float3(rt.xy, 1.0);\n"
    "    for (;;) {\n"
    "        if (!(coord.z < (float)(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)) break;\n"
    "        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);\n"
    "        e = SMAASampleLevelZero(edgesTex, coord.xy).rg;\n"
    "        e = SMAADecodeDiagBilinearAccess(e);\n"
    "        coord.w = dot(e, float2(0.5, 0.5));\n"
    "    }\n"
    "    return coord.zw;\n"
    "}\n"
    "float2 SMAAAreaDiag(float2 areaTexel, float2 dist, float2 e, float offset) {\n"
    "    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE_DIAG, SMAA_AREATEX_MAX_DISTANCE_DIAG), e, dist);\n"
    "    texcoord = mad(areaTexel, texcoord, 0.5 * areaTexel);\n"
    "    texcoord.x += 0.5;\n"  // 斜向区域在 AreaTex 的右半边
    "    texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;\n"
    "    return SMAASampleLevelZero(areaTex, texcoord).rg;\n"
    "}\n"
    "float2 SMAACalculateDiagWeights(float2 rt, float2 areaTexel, float2 texcoord, float2 e, float4 subsampleIndices) {\n"
    "    float2 weights = float2(0.0, 0.0);\n"
    "    float4 d;\n"
    "    float2 end;\n"
    "    if (e.r > 0.0) {\n"
    "        d.xz = SMAASearchDiag1(rt, texcoord, float2(-1.0, 1.0), end);\n"
    "        d.x += (float)(end.y > 0.9);\n"
    "    } else {\n"
    "        d.xz = float2(0.0, 0.0);\n"
    "    }\n"
    "    d.yw = SMAASearchDiag1(rt, texcoord, float2(1.0, -1.0), end);\n"
    "    [branch]\n"
    "    if (d.x + d.y > 2.0) {\n"
    "        float4 coords = mad(float4(-d.x + 0.25, d.x, d.y, -d.y - 0.25), rt.xyxy, texcoord.xyxy);\n"
    "        float4 c;\n"
    "        c.xy = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(-1, 0)).rg;\n"
    "        c.zw = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2(1, 0)).rg;\n"
    "        c.yxwz = SMAADecodeDiagBilinearAccess(c.xyzw);\n"
    "        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);\n"
    "        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));\n"
    "        weights += SMAAAreaDiag(areaTexel, d.xy, cc, subsampleIndices.z);\n"
    "    }\n"
    "    d.xz = SMAASearchDiag2(rt, texcoord, float2(-1.0, -1.0), end);\n"
    "    if (SMAASampleLevelZeroOffset(edgesTex, texcoord, int2(1, 0)).r > 0.0) {\n"
    "        d.yw = SMAASearchDiag2(rt, texcoord, float2(1.0, 1.0), end);\n"
    "        d.y += (float)(end.y > 0.9);\n"
    "    } else {\n"
    "        d.yw = float2(0.0, 0.0);\n"
    "    }\n"
    "    [branch]\n"
    "    if (d.x + d.y > 2.0) {\n"
    "        float4 coords = mad(float4(-d.x, -d.x, d.y, d.y), rt.xyxy, texcoord.xyxy);\n"
    "        float4 c;\n"
    "        c.x = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(-1, 0)).g;\n"
    "        c.y = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(0, -1)).r;\n"
    "        c.zw = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2(1, 0)).gr;\n"
    "        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);\n"
    "        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));\n"
    "        weights += SMAAAreaDiag(areaTexel, d.xy, cc, subsampleIndices.w).gr;\n"
    "    }\n"
    "    return weights;\n"
    "}\n"
    // ---- 上游 Horizontal/Vertical Search Functions（SMAA.hlsl :997-1103）----
    "float SMAASearchLength(float2 searchTexel, float2 e, float offset) {\n"
    "    float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5, -1.0);\n"
    "    float2 bias = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0);\n"
    "    scale += float2(-1.0, 1.0);\n"
    "    bias += float2(0.5, -0.5);\n"
    "    scale *= searchTexel;\n"  // 上游 *= 1.0/SMAA_SEARCHTEX_PACKED_SIZE
    "    bias *= searchTexel;\n"
    "    return SMAASampleLevelZero(searchTex, mad(scale, e, bias)).r;\n"
    "}\n"
    "float SMAASearchXLeft(float2 rt, float2 searchTexel, float2 texcoord, float end) {\n"
    "    float2 e = float2(0.0, 1.0);\n"
    "    for (;;) {\n"
    "        if (!(texcoord.x > end && e.g > 0.8281 && e.r == 0.0)) break;\n"
    "        e = SMAASampleLevelZero(edgesTex, texcoord).rg;\n"
    "        texcoord = mad(-float2(2.0, 0.0), rt.xy, texcoord);\n"
    "    }\n"
    "    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTexel, e, 0.0), 3.25);\n"
    "    return mad(rt.x, offset, texcoord.x);\n"
    "}\n"
    "float SMAASearchXRight(float2 rt, float2 searchTexel, float2 texcoord, float end) {\n"
    "    float2 e = float2(0.0, 1.0);\n"
    "    for (;;) {\n"
    "        if (!(texcoord.x < end && e.g > 0.8281 && e.r == 0.0)) break;\n"
    "        e = SMAASampleLevelZero(edgesTex, texcoord).rg;\n"
    "        texcoord = mad(float2(2.0, 0.0), rt.xy, texcoord);\n"
    "    }\n"
    "    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTexel, e, 0.5), 3.25);\n"
    "    return mad(-rt.x, offset, texcoord.x);\n"
    "}\n"
    "float SMAASearchYUp(float2 rt, float2 searchTexel, float2 texcoord, float end) {\n"
    "    float2 e = float2(1.0, 0.0);\n"
    "    for (;;) {\n"
    "        if (!(texcoord.y > end && e.r > 0.8281 && e.g == 0.0)) break;\n"
    "        e = SMAASampleLevelZero(edgesTex, texcoord).rg;\n"
    "        texcoord = mad(-float2(0.0, 2.0), rt.xy, texcoord);\n"
    "    }\n"
    "    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTexel, e.gr, 0.0), 3.25);\n"
    "    return mad(rt.y, offset, texcoord.y);\n"
    "}\n"
    "float SMAASearchYDown(float2 rt, float2 searchTexel, float2 texcoord, float end) {\n"
    "    float2 e = float2(1.0, 0.0);\n"
    "    for (;;) {\n"
    "        if (!(texcoord.y < end && e.r > 0.8281 && e.g == 0.0)) break;\n"
    "        e = SMAASampleLevelZero(edgesTex, texcoord).rg;\n"
    "        texcoord = mad(float2(0.0, 2.0), rt.xy, texcoord);\n"
    "    }\n"
    "    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTexel, e.gr, 0.5), 3.25);\n"
    "    return mad(-rt.y, offset, texcoord.y);\n"
    "}\n"
    "float2 SMAAArea(float2 areaTexel, float2 dist, float e1, float e2, float offset) {\n"
    "    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE),\n"
    "                          round(4.0 * float2(e1, e2)), dist);\n"
    "    texcoord = mad(areaTexel, texcoord, 0.5 * areaTexel);\n"
    "    texcoord.y = mad(SMAA_AREATEX_SUBTEX_SIZE, offset, texcoord.y);\n"
    "    return SMAASampleLevelZero(areaTex, texcoord).rg;\n"
    "}\n"
    // ---- 上游 Corner Detection（SMAA.hlsl :1108-1140）----
    "void SMAADetectHorizontalCornerPattern(inout float2 weights, float4 texcoord, float2 d) {\n"
    "    float2 leftRight = step(d.xy, d.yx);\n"
    "    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;\n"
    "    rounding /= leftRight.x + leftRight.y;\n"
    "    float2 factor = float2(1.0, 1.0);\n"
    "    factor.x -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(0, 1)).r;\n"
    "    factor.x -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1, 1)).r;\n"
    "    factor.y -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(0, -2)).r;\n"
    "    factor.y -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1, -2)).r;\n"
    "    weights *= saturate(factor);\n"
    "}\n"
    "void SMAADetectVerticalCornerPattern(inout float2 weights, float4 texcoord, float2 d) {\n"
    "    float2 leftRight = step(d.xy, d.yx);\n"
    "    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;\n"
    "    rounding /= leftRight.x + leftRight.y;\n"
    "    float2 factor = float2(1.0, 1.0);\n"
    "    factor.x -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(1, 0)).g;\n"
    "    factor.x -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1, 1)).g;\n"
    "    factor.y -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(-2, 0)).g;\n"
    "    factor.y -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(-2, 1)).g;\n"
    "    weights *= saturate(factor);\n"
    "}\n"
    // ---- 上游 Blending Weight Calculation（SMAA.hlsl :1145-1247），子采样索引恒 0（SMAA 1x）----
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    uint w, h; edgesTex.GetDimensions(w, h);\n"
    "    float2 texelSize = float2(1.0 / (float)w, 1.0 / (float)h);\n"
    "    float4 rt = float4(texelSize.x, texelSize.y, 1.0 / texelSize.x, 1.0 / texelSize.y);\n"
    "    uint aw, ah; areaTex.GetDimensions(aw, ah);\n"
    "    float2 areaTexel = float2(1.0 / (float)aw, 1.0 / (float)ah);\n"
    "    uint sw, sh; searchTex.GetDimensions(sw, sh);\n"
    "    float2 searchTexel = float2(1.0 / (float)sw, 1.0 / (float)sh);\n"
    "    float2 pixcoord = IN.tc * float2((float)w, (float)h);\n"
    "    float4 offset[3];\n"
    "    offset[0] = mad(texelSize.xyxy, float4(-0.25, -0.125, 0.25, -0.125), IN.tc.xyxy);\n"
    "    offset[1] = mad(texelSize.xyxy, float4(-0.125, -0.25, -0.125, 0.25), IN.tc.xyxy);\n"
    "    offset[2] = mad(texelSize.xyxy, float4(-2.0, -2.0, 2.0, 2.0), IN.tc.xyxy);\n"
    "    float4 subsampleIndices = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    float4 weights = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    float2 e = edgesTex.SampleLevel(smaaSampler, IN.tc, 0).rg;\n"
    "    [branch]\n"
    "    if (e.g > 0.0) {\n"
    "        weights.rg = SMAACalculateDiagWeights(rt.xy, areaTexel, IN.tc, e, subsampleIndices);\n"
    "        [branch]\n"
    "        if (weights.r == -weights.g) {\n"
    "            float2 d;\n"
    "            float3 coords;\n"
    "            coords.x = SMAASearchXLeft(rt.xy, areaTexel, offset[0].xy, offset[2].x);\n"
    "            coords.y = offset[1].y;\n"
    "            d.x = coords.x;\n"
    "            float e1 = SMAASampleLevelZero(edgesTex, coords.xy).r;\n"
    "            coords.z = SMAASearchXRight(rt.xy, areaTexel, offset[0].zw, offset[2].y);\n"
    "            d.y = coords.z;\n"
    "            d = abs(round(mad(rt.zz, d, -pixcoord.xx)));\n"
    "            float2 sqrt_d = sqrt(d);\n"
    "            float e2 = SMAASampleLevelZeroOffset(edgesTex, coords.zy, int2(1, 0)).r;\n"
    "            weights.rg = SMAAArea(areaTexel, sqrt_d, e1, e2, subsampleIndices.y);\n"
    "            coords.y = IN.tc.y;\n"
    "            SMAADetectHorizontalCornerPattern(weights.rg, coords.xyzy, d);\n"
    "        } else {\n"
    "            e.r = 0.0;\n"  // 命中斜向 ⇒ 跳过竖直方向的搜索
    "        }\n"
    "    }\n"
    "    [branch]\n"
    "    if (e.r > 0.0) {\n"
    "        float2 d;\n"
    "        float3 coords;\n"
    "        coords.y = SMAASearchYUp(rt.xy, areaTexel, offset[1].xy, offset[2].z);\n"
    "        coords.x = offset[0].x;\n"
    "        d.x = coords.y;\n"
    "        float e1 = SMAASampleLevelZero(edgesTex, coords.xy).g;\n"
    "        coords.z = SMAASearchYDown(rt.xy, areaTexel, offset[1].zw, offset[2].w);\n"
    "        d.y = coords.z;\n"
    "        d = abs(round(mad(rt.ww, d, -pixcoord.yy)));\n"
    "        float2 sqrt_d = sqrt(d);\n"
    "        float e2 = SMAASampleLevelZeroOffset(edgesTex, coords.xz, int2(0, 1)).g;\n"
    "        weights.ba = SMAAArea(areaTexel, sqrt_d, e1, e2, subsampleIndices.x);\n"
    "        coords.x = IN.tc.x;\n"
    "        SMAADetectVerticalCornerPattern(weights.ba, coords.xyxz, d);\n"
    "    }\n"
    "    return weights;\n"
    "}\n";

// ③ 邻域混合：取四邻权重里最大的那个，朝该方向做半像素偏移采样后按权重插值。
// 源色用同一个线性采样器 ⇒ 源分辨率 < 窗口时这一步同时完成升采样。
const char kSmaaBlendPS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float4> colorTex : register(t0, space0);\n"
    "Texture2D<float2> weightsTex : register(t1, space0);\n"
    "SamplerState smaaSampler : register(s0, space0);\n"
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    uint w, h; weightsTex.GetDimensions(w, h);\n"
    "    float2 texel = float2(1.0f / (float)w, 1.0f / (float)h);\n"
    "    float wl = weightsTex.SampleLevel(smaaSampler, IN.tc - float2(texel.x, 0.0f), 0).r;\n"
    "    float wr = weightsTex.SampleLevel(smaaSampler, IN.tc + float2(texel.x, 0.0f), 0).r;\n"
    "    float wu = weightsTex.SampleLevel(smaaSampler, IN.tc - float2(0.0f, texel.y), 0).g;\n"
    "    float wd = weightsTex.SampleLevel(smaaSampler, IN.tc + float2(0.0f, texel.y), 0).g;\n"
    "    float best = 0.0f;\n"
    "    float2 dir = float2(0.0f, 0.0f);\n"
    "    if (wl > best) { best = wl; dir = float2(-1.0f, 0.0f); }\n"
    "    if (wr > best) { best = wr; dir = float2(1.0f, 0.0f); }\n"
    "    if (wu > best) { best = wu; dir = float2(0.0f, -1.0f); }\n"
    "    if (wd > best) { best = wd; dir = float2(0.0f, 1.0f); }\n"
    "    float3 center = colorTex.SampleLevel(smaaSampler, IN.tc, 0).rgb;\n"
    "    if (best <= 0.0f) return float4(center, 1.0f);\n"
    "    float3 blended = colorTex.SampleLevel(smaaSampler, IN.tc + dir * texel * 0.5f, 0).rgb;\n"
    "    return float4(lerp(center, blended, saturate(best)), 1.0f);\n"
    "}\n";

// SMAA 中间纹理当前是否停在 PIXEL_SHADER_RESOURCE（刚创建时是 COMMON）。
bool gSmaaTargetsInSrvState = false;

// 建/取中间纹理（尺寸变化时重建）。
mcdr::Result<void> smaaEnsureTargets(UINT64 w, UINT64 h) {
    if (deviceContext().smaaEdges && deviceContext().smaaWidth == w && deviceContext().smaaHeight == h) {
        return {};
    }
    if (!deviceContext().smaaRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        // P104b：edges / weights / worldResult（就地 SMAA 的第三趟输出）三张。
        hd.NumDescriptors = 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = deviceContext().device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&deviceContext().smaaRtvHeap));
        if (FAILED(hr)) {
            return mcdr::err("smaaEnsureTargets", "CreateDescriptorHeap(RTV 3) hr=0x" + formatBackendResult(hr));
        }
        deviceContext().smaaEdgesRtv = deviceContext().smaaRtvHeap->GetCPUDescriptorHandleForHeapStart();
        deviceContext().smaaWeightsRtv = deviceContext().smaaEdgesRtv;
        deviceContext().smaaWeightsRtv.ptr += deviceContext().rtvInc;
        deviceContext().smaaWorldResultRtv = deviceContext().smaaEdgesRtv;
        deviceContext().smaaWorldResultRtv.ptr += deviceContext().rtvInc * 2;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = static_cast<UINT>(h);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    deviceContext().smaaEdges.Reset();
    deviceContext().smaaWeights.Reset();
    HRESULT hr = deviceContext().device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&deviceContext().smaaEdges));
    if (FAILED(hr)) {
        return mcdr::err("smaaEnsureTargets", "edges CreateCommittedResource hr=0x" + formatBackendResult(hr));
    }
    hr = deviceContext().device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&deviceContext().smaaWeights));
    if (FAILED(hr)) {
        return mcdr::err("smaaEnsureTargets", "weights CreateCommittedResource hr=0x" + formatBackendResult(hr));
    }
    deviceContext().device->CreateRenderTargetView(deviceContext().smaaEdges.Get(), nullptr, deviceContext().smaaEdgesRtv);
    deviceContext().device->CreateRenderTargetView(deviceContext().smaaWeights.Get(), nullptr, deviceContext().smaaWeightsRtv);
    deviceContext().smaaWidth = w;
    deviceContext().smaaHeight = h;
    gSmaaTargetsInSrvState = false;
    dbgLogInfo("SMAA intermediate targets %llux%llu (R8G8_UNORM) ready", static_cast<unsigned long long>(w),
               static_cast<unsigned long long>(h));
    return {};
}

// ---------------------------------------------------------------------------
// P104：官方 LUT（AreaTex 160x560 RG8 / SearchTex 64x16 R8）
// ---------------------------------------------------------------------------
// 数据来自 include\mcdr_smaa_luts.h（定义在 src\advanced\smaa_luts.cpp，字节级等于上游
// Textures\AreaTexDX10.dds / SearchTex.dds，见该头文件里的来源与 SHA-1）。
//
// 只需 **一次** 上传，所以走 createCommandList + 队列 fence 的一次性提交（与
// dbgReadbackTexturePixels 同一手法），而不是挂在每帧的 CommandContext 上：
// 初始化发生在 applySmaaToTexture 里「世界帧尚未开始录制」的时刻（Java 侧在画世界之前
// 就调 setAntiAliasing + 绑定世界目标），且提交后立刻 deviceWaitIdle，GPU 侧资源一定
// 已经落地，后续每帧只读。
//
// 失败语义：**任何一步失败都只返回错误，绝不让 SMAA 变黑/崩溃** —— 调用方
// （smaaEnsureLuts 的调用点，见 applySmaaToTexture）会把权重趟退回既有的解析式
// kSmaaWeightPS 并 WARN 一行。
constexpr D3D12_RESOURCE_STATES kSmaaLutReadyState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// 建一张只读 LUT 纹理并把它 copy 到 DEFAULT heap。srcRowBytes = 每行真实字节数
// （2 或 1），行距按 D3D12_TEXTURE_DATA_PITCH_ALIGNMENT 向上取整。
mcdr::Result<void> smaaCreateLutTexture(const unsigned char* data, UINT width, UINT height, UINT srcRowBytes,
                                        DXGI_FORMAT format, ComPtr<ID3D12Resource>& out) {
    if (data == nullptr || width == 0 || height == 0 || srcRowBytes == 0) {
        return mcdr::err("smaaCreateLutTexture", "invalid LUT parameters");
    }
    const UINT rowPitch = (srcRowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                          & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 uploadBytes = static_cast<UINT64>(rowPitch) * height;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    out.Reset();
    HRESULT hr = deviceContext().device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&out));
    if (FAILED(hr)) {
        return mcdr::err("smaaCreateLutTexture", "CreateCommittedResource(texture " + std::to_string(width) + "x"
                                                     + std::to_string(height) + ") hr=0x" + formatBackendResult(hr));
    }

    D3D12_RESOURCE_DESC upDesc{};
    upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upDesc.Width = uploadBytes;
    upDesc.Height = 1;
    upDesc.DepthOrArraySize = 1;
    upDesc.MipLevels = 1;
    upDesc.SampleDesc.Count = 1;
    upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    upDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES upHeap{};
    upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;
    hr = deviceContext().device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(hr)) {
        return mcdr::err("smaaCreateLutTexture", "CreateCommittedResource(upload " + std::to_string(uploadBytes)
                                                     + "B) hr=0x" + formatBackendResult(hr));
    }
    void* mapped = nullptr;
    hr = upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || mapped == nullptr) {
        return mcdr::err("smaaCreateLutTexture", "upload Map hr=0x" + formatBackendResult(hr));
    }
    auto* dst = static_cast<unsigned char*>(mapped);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<SIZE_T>(y) * rowPitch,
                    data + static_cast<SIZE_T>(y) * srcRowBytes, srcRowBytes);
    }
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> alloc;
    hr = deviceContext().device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (FAILED(hr)) {
        return mcdr::err("smaaCreateLutTexture", "CreateCommandAllocator hr=0x" + formatBackendResult(hr));
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    hr = deviceContext().device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                        IID_PPV_ARGS(&list));
    if (FAILED(hr)) {
        return mcdr::err("smaaCreateLutTexture", "CreateCommandList hr=0x" + formatBackendResult(hr));
    }
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = format;
    srcLoc.PlacedFootprint.Footprint.Width = width;
    srcLoc.PlacedFootprint.Footprint.Height = height;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;
    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = out.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;
    list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = out.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = kSmaaLutReadyState;
    list->ResourceBarrier(1, &barrier);
    hr = list->Close();
    if (FAILED(hr)) {
        return mcdr::err("smaaCreateLutTexture", "Close hr=0x" + formatBackendResult(hr));
    }
    ID3D12CommandList* lists[] = {list.Get()};
    deviceContext().queue->ExecuteCommandLists(1, lists);
    // 等 GPU 真正写完再返回：upload 与 list 都是局部对象，离开作用域即释放。
    if (auto waited = deviceWaitIdle(); !waited) {
        return mcdr::err("smaaCreateLutTexture", "deviceWaitIdle: " + errorMessage(waited.error()));
    }
    return {};
}

// 懒初始化两张 LUT + 它们的持久 SRV。成功后 deviceContext().smaaLutsReady=true，
// 权力趟用官方算法的 kSmaaWeightLutPS；失败则保持 false（调用方回退解析式权重并 WARN）。
mcdr::Result<void> smaaEnsureLuts() {
    if (deviceContext().smaaLutsReady) return {};
    if (!deviceContext().device || !deviceContext().queue) {
        return mcdr::err("smaaEnsureLuts", "device not initialized");
    }

    using namespace d3d12_mc::smaa_luts;
    ComPtr<ID3D12Resource> areaTex;
    ComPtr<ID3D12Resource> searchTex;
    if (auto r = smaaCreateLutTexture(kAreaTex, kAreaTexWidth, kAreaTexHeight, kAreaTexBytesPerPixel * kAreaTexWidth,
                                      DXGI_FORMAT_R8G8_UNORM, areaTex);
        !r) {
        return mcdr::err("smaaEnsureLuts", "AreaTex: " + errorMessage(r.error()));
    }
    if (auto r = smaaCreateLutTexture(kSearchTex, kSearchTexWidth, kSearchTexHeight,
                                      kSearchTexBytesPerPixel * kSearchTexWidth, DXGI_FORMAT_R8_UNORM, searchTex);
        !r) {
        return mcdr::err("smaaEnsureLuts", "SearchTex: " + errorMessage(r.error()));
    }
    if (!deviceContext().smaaLutSrvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;  // 0 = AreaTex，1 = SearchTex
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr = deviceContext().device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&deviceContext().smaaLutSrvHeap));
        if (FAILED(hr)) {
            return mcdr::err("smaaEnsureLuts", "CreateDescriptorHeap(SRV 2) hr=0x" + formatBackendResult(hr));
        }
        deviceContext().smaaLutSrvGpu = deviceContext().smaaLutSrvHeap->GetGPUDescriptorHandleForHeapStart();
        deviceContext().smaaLutSrvGpuInc = deviceContext().device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = deviceContext().smaaLutSrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuArea = cpuBase;
    deviceContext().device->CreateShaderResourceView(areaTex.Get(), &srvDesc, cpuArea);
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSearch = cpuBase;
    cpuSearch.ptr += deviceContext().smaaLutSrvGpuInc;
    deviceContext().device->CreateShaderResourceView(searchTex.Get(), &srvDesc, cpuSearch);

    deviceContext().smaaAreaTex = areaTex;
    deviceContext().smaaSearchTex = searchTex;
    deviceContext().smaaLutsReady = true;
    return {};
}

// P104b：就地路径的第三趟输出（与**源**同尺寸同格式），尺寸或格式变化时重建。
// 每帧用完都回 COMMON，所以本资源的稳定态就是 COMMON（与 transitionTo 的初始锚点一致）。
mcdr::Result<void> smaaEnsureWorldResult(const D3D12_RESOURCE_DESC& srcDesc) {
    if (!deviceContext().smaaRtvHeap) {
        return mcdr::err("smaaEnsureWorldResult", "RTV heap not created (smaaEnsureTargets must run first)");
    }
    if (deviceContext().smaaWorldResult && deviceContext().smaaWorldResultFormat == srcDesc.Format &&
        deviceContext().smaaWidth == srcDesc.Width && deviceContext().smaaHeight == srcDesc.Height) {
        return {};
    }

    D3D12_RESOURCE_DESC desc = srcDesc;  // 宽高 / mip / array / 格式全部对齐，CopyResource 才合法
    desc.Alignment = 0;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;  // 只要 RTV；COPY_SOURCE 无需 flag
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    deviceContext().smaaWorldResult.Reset();
    const HRESULT hr = deviceContext().device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&deviceContext().smaaWorldResult));
    if (FAILED(hr)) {
        return mcdr::err("smaaEnsureWorldResult", "CreateCommittedResource hr=0x" + formatBackendResult(hr));
    }
    deviceContext().device->CreateRenderTargetView(deviceContext().smaaWorldResult.Get(), nullptr, deviceContext().smaaWorldResultRtv);
    deviceContext().smaaWorldResultFormat = srcDesc.Format;
    dbgLogInfo("SMAA in-place result target %llux%llu (fmt=%d) ready",
               static_cast<unsigned long long>(srcDesc.Width),
               static_cast<unsigned long long>(srcDesc.Height), static_cast<int>(srcDesc.Format));
    return {};
}

// 编译一个 SMAA / TAA 像素着色器：硬要求 SM 6.9 + DXC，没有 5.1(DXBC) 兜底（2026-10-25）。
mcdr::Result<void> smaaCompilePs(const char* src, const char* debugName, const std::string& smProfile,
                                 std::vector<uint8_t>& dxilOut, const void*& code, SIZE_T& size) {
    code = nullptr;
    size = 0;
    if (smProfile.empty()) {
        return mcdr::err("smaaCompilePs", std::string(debugName) + ": " + sm69RequirementText());
    }
    const std::string text(src);
    auto r = compileHlslToDxil(text, "main", "ps_" + smProfile, dxilOut);
    if (!r) {
        return mcdr::err("smaaCompilePs", std::string(debugName) + " (DXC): " + firstLine(r.error().message));
    }
    code = dxilOut.data();
    size = dxilOut.size();
    return {};
}

// 用 SMAA 的根签名 + 给定 PS 建 PSO（输入布局/混合/光栅状态与 blit 完全一致）。
mcdr::Result<ComPtr<ID3D12PipelineState>> smaaCreatePso(const ComPtr<ID3D12RootSignature>& rootSig,
                                                       const void* vsCode, SIZE_T vsCodeSize,
                                                       const void* psCode, SIZE_T psCodeSize,
                                                       DXGI_FORMAT rtFormat) {
    D3D12_INPUT_ELEMENT_DESC ie[2] = {};
    ie[0].SemanticName = "POSITION";
    ie[0].SemanticIndex = 0;
    ie[0].Format = DXGI_FORMAT_R32G32_FLOAT;
    ie[0].InputSlot = 0;
    ie[0].AlignedByteOffset = 0;
    ie[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    ie[0].InstanceDataStepRate = 0;
    ie[1].SemanticName = "TEXCOORD";
    ie[1].SemanticIndex = 0;
    ie[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    ie[1].InputSlot = 0;
    ie[1].AlignedByteOffset = 8;
    ie[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    ie[1].InstanceDataStepRate = 0;

    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.BlendEnable = FALSE;
    rtBlend.LogicOpEnable = FALSE;
    rtBlend.SrcBlend = D3D12_BLEND_ONE;
    rtBlend.DestBlend = D3D12_BLEND_ZERO;
    rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_BLEND_DESC blendDesc{};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0] = rtBlend;

    D3D12_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.MultisampleEnable = FALSE;
    rasterDesc.AntialiasedLineEnable = FALSE;

    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable = FALSE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    dsDesc.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = {vsCode, vsCodeSize};
    psoDesc.PS = {psCode, psCodeSize};
    psoDesc.InputLayout = {ie, 2};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = rtFormat;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.BlendState = blendDesc;
    psoDesc.RasterizerState = rasterDesc;
    psoDesc.DepthStencilState = dsDesc;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAGS(0);

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = deviceContext().device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        // P104 fix：E_INVALIDARG 这类失败光看 HRESULT 无法定位。调试层开着时 InfoQueue 里会有
        // 一条明确指出「着色器用到的某个寄存器没被根签名覆盖」之类的消息，dump 出来便于下一次定位。
        dumpDeviceMessages();
        return mcdr::err("smaaCreatePso", "CreateGraphicsPipelineState hr=0x" + formatBackendResult(hr));
    }
    return pso;
}

// 懒初始化 SMAA 的三条管线（rootSig 一张，PSO 三条）。
mcdr::Result<void> smaaEnsurePipelines(DXGI_FORMAT rtFormat) {
    if (deviceContext().smaaPipelinesReady && deviceContext().smaaBlendFormat == rtFormat) {
        return {};
    }

    // 三张 SRV 表：t0 / t1 / t2，各占一个根参数（0 / 1 / 2）。
    //
    // P104 fix（E_INVALIDARG）：**必须是 3 个**，这是本次运行期回归的根因。
    // 权重趟的 kSmaaWeightLutPS 声明了三张 SRV（dxc 反射：
    // `edgesTex t0 / areaTex t1 / searchTex t2`，另加 `smaaSampler s0 / smaaLutSampler s1`），
    // 而原实现只序列化了 t0/t1 两个描述符表。D3D12 要求 PSO 里着色器用到的**每一个**
    // 寄存器（t/s/b/u）都被根签名覆盖，缺 t2 ⇒ 该 PSO 的着色器/根签名组合非法 ⇒
    // CreateGraphicsPipelineState 返回 0x80070057（E_INVALIDARG）。
    // 表现就是 smaaEnsurePipelines 里**第一条** PSO（edges，用的是同一张根签名）创建失败、
    // `P104: SMAA pipelines ready …` 永远不打印、SMAA 被一次性关掉 —— 与实机日志一致。
    //
    // 槽位对应表（= 反射实测值）：
    //   ① 边检测 kSmaaEdgePS      : t0=srcTex              s0=smaaSampler
    //   ② 解析权重 kSmaaWeightPS  : t0=edgesTex            s0=smaaSampler
    //   ②′ LUT 权重 kSmaaWeightLutPS: t0=edgesTex t1=areaTex t2=searchTex  s0=smaaSampler s1=smaaLutSampler
    //   ③ 邻域混合 kSmaaBlendPS   : t0=colorTex t1=weightsTex s0=smaaSampler
    // 只有②′用到 t2；① ② ③ 不用的表保持未绑定（与本次改动前 t1 在 ① ② 未绑定的既有行为一致，
    // 未绑定的根参数在 D3D12 里合法，只是不能是「着色器用了但根签名没有」）。
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    for (UINT i = 0; i < 3; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;  // t0 = 源色 / t1 = 辅助纹理 / t2 = SearchTex
        ranges[i].RegisterSpace = 0;
        ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }
    D3D12_ROOT_PARAMETER params[3] = {};
    for (UINT i = 0; i < 3; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_STATIC_SAMPLER_DESC staticSam[2] = {};
    staticSam[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSam[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[0].MipLODBias = 0.0f;
    staticSam[0].MaxAnisotropy = 1;
    staticSam[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSam[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSam[0].MinLOD = 0.0f;
    staticSam[0].MaxLOD = D3D12_FLOAT32_MAX;
    staticSam[0].ShaderRegister = 0;
    staticSam[0].RegisterSpace = 0;
    staticSam[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // P104 LUT 采样器（s1）：上游 SMAA.hlsl 的 LinearSampler 就是
    // MIN_MAG_LINEAR_MIP_POINT + Clamp —— AreaTex 按 texel 中心采样、SearchTex 查表，
    // 只有 mip 级别的插值被关掉。改成纯 LINEAR 会破坏 SearchTex 的查表结果。
    staticSam[1].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    staticSam[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[1].MipLODBias = 0.0f;
    staticSam[1].MaxAnisotropy = 1;
    staticSam[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSam[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSam[1].MinLOD = 0.0f;
    staticSam[1].MaxLOD = D3D12_FLOAT32_MAX;
    staticSam[1].ShaderRegister = 1;
    staticSam[1].RegisterSpace = 0;
    staticSam[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = staticSam;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        return mcdr::err("smaaEnsurePipelines", "SerializeRootSignature hr=0x" + formatBackendResult(hr));
    }
    ComPtr<ID3D12RootSignature> rootSig;
    hr = deviceContext().device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        return mcdr::err("smaaEnsurePipelines", "CreateRootSignature hr=0x" + formatBackendResult(hr));
    }

    // VS 与 blit 同一份 HLSL（全屏四边形 + 透传 texcoord）。硬要求 SM 6.9 + DXC，无 5.1 兜底。
    const std::string smProfile = graphicsShaderModelProfile();
    if (smProfile.empty()) {
        return mcdr::err("smaaEnsurePipelines", "smaaEnsurePipelines: " + sm69RequirementText());
    }
    std::vector<uint8_t> vsDxil;
    const std::string vsSrc(kBlitVS, sizeof(kBlitVS) - 1);
    auto vsDxc = compileHlslToDxil(vsSrc, "main", "vs_" + smProfile, vsDxil);
    if (!vsDxc) {
        return mcdr::err("smaaEnsurePipelines", "smaa_vs (DXC): " + firstLine(vsDxc.error().message));
    }
    const void* vsCode = vsDxil.data();
    SIZE_T vsCodeSize = vsDxil.size();

    // 三个 PS：DXIL 缓冲必须活到 PSO 建完（不再有 ID3DBlob 那条路）。
    std::vector<uint8_t> edgeDxil, weightDxil, blendDxil, weightLutDxil;
    const void* edgeCode = nullptr;
    const void* weightCode = nullptr;
    const void* blendCode = nullptr;
    const void* weightLutCode = nullptr;
    SIZE_T edgeSize = 0, weightSize = 0, blendSize = 0, weightLutSize = 0;
    if (auto r = smaaCompilePs(kSmaaEdgePS, "smaa_edge_ps", smProfile, edgeDxil, edgeCode, edgeSize); !r) {
        return mcdr::err("smaaEnsurePipelines", errorMessage(r.error()));
    }
    if (auto r = smaaCompilePs(kSmaaWeightPS, "smaa_weight_ps", smProfile, weightDxil, weightCode, weightSize);
        !r) {
        return mcdr::err("smaaEnsurePipelines", errorMessage(r.error()));
    }
    // P104：官方 LUT 版权重趟。这是本条路径的主算法；下面的解析版只是 LUT 初始化失败时的回退。
    if (auto r = smaaCompilePs(kSmaaWeightLutPS, "smaa_weight_lut_ps", smProfile, weightLutDxil, weightLutCode,
                               weightLutSize);
        !r) {
        return mcdr::err("smaaEnsurePipelines", errorMessage(r.error()));
    }
    if (auto r = smaaCompilePs(kSmaaBlendPS, "smaa_blend_ps", smProfile, blendDxil, blendCode, blendSize); !r) {
        return mcdr::err("smaaEnsurePipelines", errorMessage(r.error()));
    }

    auto edgePso = smaaCreatePso(rootSig, vsCode, vsCodeSize, edgeCode, edgeSize, DXGI_FORMAT_R8G8_UNORM);
    if (!edgePso) return mcdr::err("smaaEnsurePipelines", errorMessage(edgePso.error()));
    auto weightPso = smaaCreatePso(rootSig, vsCode, vsCodeSize, weightCode, weightSize, DXGI_FORMAT_R8G8_UNORM);
    if (!weightPso) return mcdr::err("smaaEnsurePipelines", errorMessage(weightPso.error()));
    auto weightLutPso =
        smaaCreatePso(rootSig, vsCode, vsCodeSize, weightLutCode, weightLutSize, DXGI_FORMAT_R8G8_UNORM);
    if (!weightLutPso) return mcdr::err("smaaEnsurePipelines", errorMessage(weightLutPso.error()));
    auto blendPso = smaaCreatePso(rootSig, vsCode, vsCodeSize, blendCode, blendSize, rtFormat);
    if (!blendPso) return mcdr::err("smaaEnsurePipelines", errorMessage(blendPso.error()));

    deviceContext().smaaRootSig = rootSig;
    deviceContext().smaaEdgePso = edgePso.value();
    deviceContext().smaaWeightPso = weightPso.value();
    deviceContext().smaaWeightLutPso = weightLutPso.value();
    deviceContext().smaaBlendPso = blendPso.value();
    deviceContext().smaaBlendFormat = rtFormat;
    deviceContext().smaaPipelinesReady = true;
    dbgLogInfo("SMAA pipelines ready (blend RTV fmt=%d, SM=%s)", static_cast<int>(rtFormat),
               smProfile.c_str());
    // 两条权重管线只有一条会被用到，但两条都建好 ⇒ 任何时候都不需要重建、不需要重试。
    // 「官方 LUT」那行日志**不能**放在这里：smaaEnsurePipelines 先于 smaaEnsureLuts 执行，
    // 此刻 smaaLutsReady 一定是 false。它现在打在 applySmaaToTexture 选路之后。
    return {};
}

// 把一张纹理的 SRV 写进本帧 drawHeap 的瞬态槽位并绑到根参数 rootParam 上
// （与 blitBindSourceTexture 同一机制：SRV 必须落在命令列表当前设置的堆里）。
mcdr::Result<void> smaaBindSrv(CommandContext* ctx, ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                               DXGI_FORMAT viewFmt, UINT rootParam) {
    if (!res) {
        return mcdr::err("smaaBindSrv", "smaaBindSrv: null resource");
    }
    if (ctx->nextDrawSlot + 1 > syncDrawDescriptorCapacity()) {
        return mcdr::err("smaaBindSrv", "smaaBindSrv: draw descriptor heap exhausted for this frame");
    }
    SIZE_T base = static_cast<SIZE_T>(ctx->drawHeapSlotBase + ctx->nextDrawSlot) * deviceContext().drawInc;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = deviceContext().drawHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += base;
    ctx->nextDrawSlot += 1;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = viewFmt;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    deviceContext().device->CreateShaderResourceView(res, &srvDesc, srvCpu);

    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = deviceContext().drawHeap->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += base;
    cmd->SetGraphicsRootDescriptorTable(rootParam, srvGpu);
    return {};
}

// P104：把两张官方 LUT 的 **持久** SRV 绑到权重趟的根参数 1（t1 = AreaTex）与 2（t2 = SearchTex）。
// 与 smaaBindSrv 的区别：这两张 SRV 早在 smaaEnsureLuts 里就写进 deviceContext().smaaLutSrvHeap 了，
// 每帧只需要把「堆里已有的表」挂到根参数上 —— 不占每帧的 drawHeap 槽位，也不重新建视图。
mcdr::Result<void> smaaBindLuts(ID3D12GraphicsCommandList* cmd) {
    if (!deviceContext().smaaLutSrvHeap || !deviceContext().smaaLutSrvGpu.ptr || deviceContext().smaaLutSrvGpuInc == 0) {
        return mcdr::err("smaaBindLuts", "LUT SRV heap is not ready");
    }
    D3D12_GPU_DESCRIPTOR_HANDLE areaGpu = deviceContext().smaaLutSrvGpu;
    D3D12_GPU_DESCRIPTOR_HANDLE searchGpu = deviceContext().smaaLutSrvGpu;
    searchGpu.ptr += deviceContext().smaaLutSrvGpuInc;
    cmd->SetGraphicsRootDescriptorTable(1, areaGpu);
    cmd->SetGraphicsRootDescriptorTable(2, searchGpu);
    return {};
}

}  // namespace

const char* antiAliasingName(int mode) {
    switch (mode) {
        case 0: return "OFF";
        case 1: return "SMAA";
        case 2: return "TAA";
        case 3: return "DLAA";
        default: return "UNKNOWN";
    }
}

int setAntiAliasing(int mode) {
    if (mode < 0 || mode > 3) {
        dbgLogError("invalid anti-aliasing mode %d (valid: 0=OFF 1=SMAA 2=TAA 3=DLAA)", mode);
        return -1;
    }
    // P77/P152：TAA（2）不再降级，且从 P152 起它的 resolve 也在 native 侧
    // （applyTaaToTexture：历史纹理乒乓 + jitter + 管线都由本模块持有），JVM 只送参数。
    // `applySmaaToTexture` 的闸门是 `aaMode != 1`，所以 TAA 模式下 SMAA 自动让路。
    // DLAA（3）仍然由 DLSS 特性实现：JVM 侧会把它映射成 OFF 再传下来，真收到 3 说明另有
    // 调用方，同样按"无 native 后处理"处理，不该再用"未实现"把用户的选项踢回 OFF。
    const int prev = deviceContext().aaMode;
    deviceContext().aaMode = mode;
    // 换模式时把两条路径的"一次性失败"闸门都复位：用户可以靠改设置再试一次。
    if (mode != prev) {
        deviceContext().smaaDisabled = false;
        deviceContext().smaaUnavailableLogged = false;
        deviceContext().smaaAppliedFrames = 0;
        deviceContext().taaDisabled = false;
        deviceContext().taaUnavailableLogged = false;
        deviceContext().taaSharpenFallbackWarned = false;
        deviceContext().taaAppliedFrames = 0;
        // 历史内容属于上一种模式下的画面，切回来时不可信（下一帧以 reset=1 开始）。
        deviceContext().taaHistoryValid = false;
    }
    if (mode != prev) {
        dbgLogInfo("anti-aliasing = %s (%s)", antiAliasingName(mode),
                   mode == 1 ? "SMAA, three passes in place after the world and before the GUI"
                   : mode == 2 ? "TAA, resolved natively after the world layer (RGBA16F history)"
                               : "no post-processing; plain blit");
    }
    // 契约（JVM 的 `D3D12AntiAliasing.push` 用 `status == pushed` 判成功）：成功时返回**生效的
    // 模式本身**，0 是合法成功值（OFF）。旧实现恒返回 1，于是 OFF 被误报成"返回值异常"。
    return mode;
}


bool applySmaaToTexture(CommandContext* ctx, Dx12Object* tex) {
    // P104：只有 1(SMAA) 才走三趟；0/2/3 一律跳过（语义与 P104 时一致）。
    // P108（用户 m10920）：DLSS 已从抗锯齿枚举里拆出去，**不再**在这里判 DLSS —— SMAA 与
    // DLSS 现在是独立开关，两者都开时这一趟先跑（在低分辨率世界层上），再由
    // applyDlssToTexture 把它升采样到窗口尺寸。
    if (deviceContext().aaMode != 1 || deviceContext().smaaDisabled) {
        return false;
    }
    if (!ctx || !ctx->listOpen || !tex || !tex->resource) {
        return false;
    }
    ID3D12GraphicsCommandList* cmd = ctx->commandList;
    if (cmd == nullptr) {
        return false;
    }
    const D3D12_RESOURCE_DESC srcDesc = tex->resource->GetDesc();
    const UINT64 srcW64 = srcDesc.Width;
    const UINT64 srcH64 = srcDesc.Height;
    if (srcW64 == 0 || srcH64 == 0) {
        return false;
    }
    if (srcDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || srcDesc.SampleDesc.Count != 1) {
        // 非 2D / MSAA 目标既不能当 SRV 采样也不能 CopyResource：静默不适用（不是错误）。
        if (!deviceContext().smaaUnavailableLogged) {
            deviceContext().smaaUnavailableLogged = true;
            dbgLogWarn("target is not a single-sampled 2D texture; skipping the in-place SMAA path");
        }
        return false;
    }
    DXGI_FORMAT srcFormat = srcDesc.Format;
    if (srcFormat == DXGI_FORMAT_UNKNOWN) {
        srcFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    // 任何一步失败都彻底关掉 SMAA，避免每帧重试刷日志 /
    // 把中间纹理留在与预期不符的状态上继续用。
    auto fail = [](const char* where, const std::string& what) {
        deviceContext().smaaDisabled = true;
        deviceContext().aaMode = 0;
        dbgLogError("SMAA failed in %s; SMAA disabled: %s", where, firstLine(what).c_str());
        return false;
    };
    if (auto r = smaaEnsurePipelines(srcFormat); !r) {
        deviceContext().smaaUnavailableLogged = true;
        return fail("smaaEnsurePipelines", r.error().message);
    }
    // 官方 LUT：两条权重管线都已建好，这里只是初始化两张 LUT（一次，幂等）并选一条。
    // 失败**不是**致命错误：退回解析式权重趟（kSmaaWeightPS），只 WARN 一行、只报一次。
    // 注意用独立的 smaaLutFallbackWarned，别占用 smaaUnavailableLogged（那是
    // 「目标不是单采样 2D 纹理」的一次性提示，两者是不同的语义）。
    if (!deviceContext().smaaLutsReady) {
        auto lut = smaaEnsureLuts();
        if (!lut && !deviceContext().smaaLutFallbackWarned) {
            deviceContext().smaaLutFallbackWarned = true;
            dbgLogWarn("SMAA falling back to the analytic blending-weight shader; official LUTs unavailable: %s",
                       firstLine(lut.error().message).c_str());
        }
    }
    const bool useLutWeights = deviceContext().smaaLutsReady && deviceContext().smaaWeightLutPso;
    if (!useLutWeights) {
        if (!deviceContext().smaaLutFallbackWarned) {
            deviceContext().smaaLutFallbackWarned = true;
            dbgLogWarn("SMAA falling back to the analytic blending-weight shader (official LUTs unavailable)");
        }
    } else if (deviceContext().smaaAppliedFrames == 0 && !deviceContext().smaaLutReadyLogged) {
        // P104 fix：这一行原来挂在 smaaEnsurePipelines 里的 `if (deviceContext().smaaLutsReady)` 分支上，
        // 但**调用顺序是 pipelines → LUTs**，那一刻 smaaLutsReady 必然还是 false ⇒ 永远打不出来。
        // 挪到这里（LUT 刚初始化完）才反映真实状态。
        // 必须用一次性闸门：applySmaaToTexture **每帧**都会走到这里，没有闸门就会逐帧刷屏
        // （实测 18:09:53–18:10:08 刷了 15 秒）。smaaAppliedFrames==0 表示「首帧之前」。
        deviceContext().smaaLutReadyLogged = true;
        dbgLogInfo("SMAA using the official AreaTex/SearchTex LUTs (160x560 RG8, 64x16 R8)");
    }
    if (auto r = smaaEnsureTargets(srcW64, srcH64); !r) {
        return fail("smaaEnsureTargets", r.error().message);
    }
    if (auto r = smaaEnsureWorldResult(srcDesc); !r) {
        return fail("smaaEnsureWorldResult", r.error().message);
    }
    if (auto r = initBlitPipeline(); !r) {
        return fail("initBlitPipeline", r.error().message);
    }
    const BlitPipeline* bp = getBlitPipeline();
    if (bp == nullptr) {
        return fail("getBlitPipeline", "blit pipeline unavailable");
    }

    const D3D12_VIEWPORT srcVp{0.0f, 0.0f, static_cast<FLOAT>(srcW64), static_cast<FLOAT>(srcH64), 0.0f, 1.0f};
    const D3D12_RECT srcSc{0, 0, static_cast<LONG>(srcW64), static_cast<LONG>(srcH64)};

    cmd->SetGraphicsRootSignature(deviceContext().smaaRootSig.Get());
    cmd->IASetVertexBuffers(0, 1, &bp->vbView);
    cmd->IASetIndexBuffer(&bp->ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const D3D12_RESOURCE_STATES targetsPrev =
        gSmaaTargetsInSrvState ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;

    // ① 边检测：source → edges
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTo(ctx, deviceContext().smaaEdges.Get(), targetsPrev, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->OMSetRenderTargets(1, &deviceContext().smaaEdgesRtv, FALSE, nullptr);
    cmd->RSSetViewports(1, &srcVp);
    cmd->RSSetScissorRects(1, &srcSc);
    cmd->SetPipelineState(deviceContext().smaaEdgePso.Get());
    if (auto r = smaaBindSrv(ctx, cmd, tex->resource.Get(), srcFormat, 0); !r) {
        return fail("pass1.bind", r.error().message);
    }
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // ② 混合权重：edges (+ AreaTex/SearchTex) → weights
    transitionTo(ctx, deviceContext().smaaEdges.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTo(ctx, deviceContext().smaaWeights.Get(), targetsPrev, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->OMSetRenderTargets(1, &deviceContext().smaaWeightsRtv, FALSE, nullptr);
    cmd->RSSetViewports(1, &srcVp);
    cmd->RSSetScissorRects(1, &srcSc);
    cmd->SetPipelineState(useLutWeights ? deviceContext().smaaWeightLutPso.Get() : deviceContext().smaaWeightPso.Get());
    if (auto r = smaaBindSrv(ctx, cmd, deviceContext().smaaEdges.Get(), DXGI_FORMAT_R8G8_UNORM, 0); !r) {
        return fail("pass2.bind", r.error().message);
    }
    if (useLutWeights) {
        // 官方 LUT 路径额外需要 t1 = AreaTex、t2 = SearchTex（SRV 常驻，见 smaaBindLuts）。
        if (auto r = smaaBindLuts(cmd); !r) {
            return fail("pass2.bind.luts", r.error().message);
        }
    }
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // ③ 邻域混合：source + weights → scratch（**源纹理本身正在被采样，不能同时当 RTV**）
    transitionTo(ctx, deviceContext().smaaWeights.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gSmaaTargetsInSrvState = true;
    transitionTo(ctx, deviceContext().smaaWorldResult.Get(), D3D12_RESOURCE_STATE_COMMON,
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->OMSetRenderTargets(1, &deviceContext().smaaWorldResultRtv, FALSE, nullptr);
    cmd->RSSetViewports(1, &srcVp);
    cmd->RSSetScissorRects(1, &srcSc);
    cmd->SetPipelineState(deviceContext().smaaBlendPso.Get());
    if (auto r = smaaBindSrv(ctx, cmd, tex->resource.Get(), srcFormat, 0); !r) {
        return fail("pass3.bind.t0", r.error().message);
    }
    if (auto r = smaaBindSrv(ctx, cmd, deviceContext().smaaWeights.Get(), DXGI_FORMAT_R8G8_UNORM, 1); !r) {
        return fail("pass3.bind.t1", r.error().message);
    }
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // ④ 拷回并交还：scratch(COPY_SOURCE) → 源纹理(COPY_DEST) → 源纹理回 RENDER_TARGET，
    // 这样紧跟着的 GUI render pass 可以照常把主渲染目标当附件用；scratch 回 COMMON
    // （它的稳定态就是 COMMON，与 transitionTo 的初始锚点一致）。
    transitionTo(ctx, deviceContext().smaaWorldResult.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(tex->resource.Get(), deviceContext().smaaWorldResult.Get());
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transitionTo(ctx, deviceContext().smaaWorldResult.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                 D3D12_RESOURCE_STATE_COMMON);

    ++deviceContext().smaaAppliedFrames;
    if (deviceContext().smaaAppliedFrames == 1) {
        dbgLogInfo("SMAA applied on the first frame: world layer %llux%llu anti-aliased in place (fmt=%d)",
                   static_cast<unsigned long long>(srcW64), static_cast<unsigned long long>(srcH64),
                   static_cast<int>(srcFormat));
    }
    return true;
}

// ---------------------------------------------------------------------------
// P152：TAA（时域抗锯齿）—— 历史与管线都在 native 侧
// ---------------------------------------------------------------------------
// 与 SMAA 的区别不只是"又一个后处理"，而是**状态归属**：SMAA 是单帧形态学，三趟只借自身的
// 中间纹理；TAA 需要跨帧状态（历史纹理乒乓、jitter、历史代），此前全部由 JVM 的 D3D12TAA
// 持有，并且每帧要录一趟 beginRenderPass + draw + 回填 blit。这里把整条链路搬进 native：
// JVM 每帧只调一次 applyTaaToTexture，不再跨语言搬运 GPU 资源句柄。
//
// 两趟：
//   ① resolve  scene(t1) + history[read](t2) + motion(t3) → history[write]（RGBA16F）
//   ② 回填    history[write] → 主目标（锐化量 > 0 走 5-tap unsharp，否则纯采样 blit）
// 历史**不参与锐化**：把锐化结果写回历史，会随每帧的 lerp 递归叠加，几十帧后边缘出现振铃。
// 历史也不是 RGBA8：混合权重是 1/16，8 位历史会把每帧 1/16 的增量就地量化掉，静止画面会
// 出现缓慢漂移的时间性带状。
namespace {

// 历史纹理格式：16 位浮点。8 位历史会把每帧 1/16 的混合增量就地量化掉。
constexpr DXGI_FORMAT kTaaHistoryFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// 参数块：10 个有效 float + 2 个对齐 pad。走 32-bit 根常量，不需要 CBV 与上传缓冲
// （JVM 侧当年用 CBV，是因为它只能走通用的 pushDescriptors 路径）。
constexpr UINT kTaaParam32BitCount = 12;

// 上一帧结束时两张历史纹理是否都停在 PIXEL_SHADER_RESOURCE。首帧（刚创建）与
// taaEnsureHistory 重建之后都是 COMMON，所以需要这个跨帧标记 —— 与 SMAA 的
// gSmaaTargetsInSrvState 同一套理由（D3D12 不允许在同一个命令列表里显式回切 COMMON）。
bool gTaaHistoryInSrvState = false;

// resolve 的像素着色器：P152 起这是**唯一**实现（JVM 侧的 D3D12TAA 已删掉历史/管线/HLSL，
// 只负责算出每帧参数再调本函数）。
// 全部用 Load + 手算双线性 —— 静态采样器是点采样 + 环绕，点采样会丢掉亚像素精度，环绕取样
// 在 UV 出界时会从屏幕另一侧拖内容进来，这两件事都必须变成显式行为。
static const char kTaaPS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "cbuffer Params : register(b0, space0) {\n"
    "    float2 ScreenSize;\n"
    "    float2 RcpScreenSize;\n"
    "    float Blend;\n"
    "    float ResetHistory;\n"
    "    float2 Jitter;\n"
    "    float2 PrevJitter;\n"
    "    float2 ParamsPad;\n"
    "};\n"
    "Texture2D<float4> Scene : register(t1, space0);\n"
    "Texture2D<float4> HistoryBuffer : register(t2, space0);\n"
    "Texture2D<float2> Motion : register(t3, space0);\n"
    "float3 toYCoCg(float3 c) {\n"
    "    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)),\n"
    "                  dot(c, float3(-0.25, 0.5, -0.25)));\n"
    "}\n"
    "float3 fromYCoCg(float3 y) {\n"
    "    float t = y.x - y.z;\n"
    "    return float3(t + y.y, y.x + y.z, t - y.y);\n"
    "}\n"
    "float3 fetchScene(int2 coord, int2 maxCoord) {\n"
    "    return Scene.Load(int3(clamp(coord, int2(0, 0), maxCoord), 0)).rgb;\n"
    "}\n"
    "float3 fetchHistory(int2 coord, int2 maxCoord) {\n"
    "    return HistoryBuffer.Load(int3(clamp(coord, int2(0, 0), maxCoord), 0)).rgb;\n"
    "}\n"
    "float3 bilinearScene(float2 uv, int2 maxCoord) {\n"
    "    float2 texel = uv * ScreenSize - 0.5;\n"
    "    int2 baseCoord = int2(floor(texel));\n"
    "    float2 f = texel - float2(baseCoord);\n"
    "    float3 c00 = fetchScene(baseCoord, maxCoord);\n"
    "    float3 c10 = fetchScene(baseCoord + int2(1, 0), maxCoord);\n"
    "    float3 c01 = fetchScene(baseCoord + int2(0, 1), maxCoord);\n"
    "    float3 c11 = fetchScene(baseCoord + int2(1, 1), maxCoord);\n"
    "    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);\n"
    "}\n"
    "float3 bilinearHistory(float2 uv, int2 maxCoord) {\n"
    "    float2 texel = uv * ScreenSize - 0.5;\n"
    "    int2 baseCoord = int2(floor(texel));\n"
    "    float2 f = texel - float2(baseCoord);\n"
    "    float3 c00 = fetchHistory(baseCoord, maxCoord);\n"
    "    float3 c10 = fetchHistory(baseCoord + int2(1, 0), maxCoord);\n"
    "    float3 c01 = fetchHistory(baseCoord + int2(0, 1), maxCoord);\n"
    "    float3 c11 = fetchHistory(baseCoord + int2(1, 1), maxCoord);\n"
    "    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);\n"
    "}\n"
    "float4 main(VS_OUT IN) : SV_Target0 {\n"
    "    int2 size = int2(ScreenSize + 0.5);\n"
    "    int2 maxCoord = size - 1;\n"
    "    int2 coord = clamp(int2(IN.pos.xy), int2(0, 0), maxCoord);\n"
    "    // 累加网格不抖动：本帧颜色按 jitter 的反方向重采样回未抖动网格，历史只按真实运动\n"
    "    // 矢量取样。jitter 注入的是投影矩阵列 2，内容在 NDC 里往 +2j/尺寸 走；换算成像素\n"
    "    // 坐标（y 向下）就是内容整体移动 (+J.x, -J.y) ⇒ 要取回 I(c) 得采样 c - (+J.x, -J.y)。\n"
    "    float2 curUv = (float2(coord) + 0.5 - float2(Jitter.x, -Jitter.y)) * RcpScreenSize;\n"
    "    float3 currentRgb = bilinearScene(curUv, maxCoord);\n"
    "    float currentA = Scene.Load(int3(coord, 0)).a;\n"
    "    // 被拒绝的像素用像素中心的原始样本：重采样在不连续处会按亚像素比例混合前景/背景，\n"
    "    // 这个比例每帧随 Jitter 变却不随场景移动 ⇒ 边缘看起来不跟随摄像机。\n"
    "    float3 rawRgb = Scene.Load(int3(coord, 0)).rgb;\n"
    "    // 运动矢量约定 prevPos - curPos（像素单位，真实运动、不含 jitter）。\n"
    "    float2 mv = Motion.Load(int3(coord, 0)).xy;\n"
    "    float2 historyUv = (float2(coord) + 0.5 + mv) * RcpScreenSize;\n"
    "    // 3x3 邻域的 YCoCg 统计（variance clipping 的包围盒）。邻域样本必须和 currentRgb\n"
    "    // 处在同一个域（未抖动网格），否则包围盒本身逐帧随 Jitter 抖。\n"
    "    float3 sum = 0.0;\n"
    "    float3 sumSq = 0.0;\n"
    "    [unroll] for (int dy = -1; dy <= 1; ++dy) {\n"
    "        [unroll] for (int dx = -1; dx <= 1; ++dx) {\n"
    "            float3 y = toYCoCg(bilinearScene(curUv + float2(dx, dy) * RcpScreenSize, maxCoord));\n"
    "            sum += y;\n"
    "            sumSq += y * y;\n"
    "        }\n"
    "    }\n"
    "    const float samples = 9.0;\n"
    "    float3 mean = sum / samples;\n"
    "    float3 variance = max(sumSq / samples - mean * mean, 0.0);\n"
    "    float3 lo = mean - 1.5 * sqrt(variance);\n"
    "    float3 hi = mean + 1.5 * sqrt(variance);\n"
    "    float3 history = bilinearHistory(historyUv, maxCoord);\n"
    "    // variance clipping 只钳不拒，权重恒为 1 - Blend：按「钳掉多少」调制权重会让静态边缘\n"
    "    // 逐帧跳，用户看到的就是「静止也有随机抖动」。\n"
    "    float3 clamped = clamp(toYCoCg(history), lo, hi);\n"
    "    float3 resolved = lerp(fromYCoCg(clamped), currentRgb, Blend);\n"
    "    float motionLength = length(mv);\n"
    "    bool outside = (historyUv.x < 0.0) || (historyUv.y < 0.0) || (historyUv.x > 1.0) ||\n"
    "                   (historyUv.y > 1.0);\n"
    "    bool absurd = !(motionLength <= 0.25 * max(ScreenSize.x, ScreenSize.y));\n"
    "    if (ResetHistory > 0.5 || outside || absurd) {\n"
    "        resolved = rawRgb;\n"
    "    }\n"
    "    return float4(resolved, currentA);\n"
    "}\n";

// 建 TAA 的根签名 + resolve PSO（懒初始化，幂等）。
// 根参数：0 = b0 的 12 个 32-bit 常量；1..3 = t1/t2/t3 各一张 SRV 表。三张表分开建，
// 而不是塞进一张 NumDescriptors=3 的表 —— smaaBindSrv 一次只写一个槽位并把该槽位挂成表，
// 连续三张分开的表正好与它兼容（也就不需要再写一套堆槽位计算）。
mcdr::Result<void> taaEnsurePipelines() {
    if (deviceContext().taaRootSig && deviceContext().taaPso) {
        return {};
    }

    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    for (UINT i = 0; i < 3; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i + 1;  // t1 = scene / t2 = history / t3 = motion
        ranges[i].RegisterSpace = 0;
        ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = kTaaParam32BitCount;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = 0; i < 3; ++i) {
        params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        params[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 4;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 0;
    rsDesc.pStaticSamplers = nullptr;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        return mcdr::err("taaEnsurePipelines", "SerializeRootSignature hr=0x" + formatBackendResult(hr));
    }
    ComPtr<ID3D12RootSignature> rootSig;
    hr = deviceContext().device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        return mcdr::err("taaEnsurePipelines", "CreateRootSignature hr=0x" + formatBackendResult(hr));
    }

    // VS 与 blit / SMAA 同一份全屏四边形 HLSL（POSITION + TEXCOORD，配合 blit 的顶点缓冲）。
    // 硬要求 SM 6.9 + DXC，无 5.1(DXBC) 兜底。
    const std::string smProfile = graphicsShaderModelProfile();
    if (smProfile.empty()) {
        return mcdr::err("taaEnsurePipelines", "taaEnsurePipelines: " + sm69RequirementText());
    }
    std::vector<uint8_t> vsDxil;
    const std::string vsSrc(kBlitVS, sizeof(kBlitVS) - 1);
    auto vsDxc = compileHlslToDxil(vsSrc, "main", "vs_" + smProfile, vsDxil);
    if (!vsDxc) {
        return mcdr::err("taaEnsurePipelines", "taa_vs (DXC): " + firstLine(vsDxc.error().message));
    }
    const void* vsCode = vsDxil.data();
    SIZE_T vsCodeSize = vsDxil.size();

    // PS：DXIL 缓冲必须活到 PSO 建完（不再有 ID3DBlob 那条路）。
    std::vector<uint8_t> psDxil;
    const void* psCode = nullptr;
    SIZE_T psCodeSize = 0;
    if (auto r = smaaCompilePs(kTaaPS, "taa_ps", smProfile, psDxil, psCode, psCodeSize); !r) {
        return mcdr::err("taaEnsurePipelines", errorMessage(r.error()));
    }

    auto pso = smaaCreatePso(rootSig, vsCode, vsCodeSize, psCode, psCodeSize, kTaaHistoryFormat);
    if (!pso) {
        return mcdr::err("taaEnsurePipelines", errorMessage(pso.error()));
    }

    deviceContext().taaRootSig = rootSig;
    deviceContext().taaPso = pso.value();
    return {};
}

// 懒创建两张 RGBA16F 历史纹理 + 2 槽 RTV 堆；尺寸变化时整组重建。
mcdr::Result<void> taaEnsureHistory(UINT64 w, UINT64 h) {
    if (deviceContext().taaHistory[0] && deviceContext().taaHistory[1] && deviceContext().taaWidth == w && deviceContext().taaHeight == h) {
        return {};
    }
    deviceContext().taaHistory[0].Reset();
    deviceContext().taaHistory[1].Reset();
    deviceContext().taaHistoryRtv[0] = {};
    deviceContext().taaHistoryRtv[1] = {};
    deviceContext().taaRtvHeap.Reset();
    deviceContext().taaHistoryValid = false;
    gTaaHistoryInSrvState = false;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HRESULT hr = deviceContext().device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&deviceContext().taaRtvHeap));
    if (FAILED(hr)) {
        return mcdr::err("taaEnsureHistory", "CreateDescriptorHeap(RTV) hr=0x" + formatBackendResult(hr));
    }
    const UINT rtvInc = deviceContext().device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = deviceContext().taaRtvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = w;
    texDesc.Height = static_cast<UINT>(h);
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = kTaaHistoryFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    for (int i = 0; i < 2; ++i) {
        hr = deviceContext().device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&deviceContext().taaHistory[i]));
        if (FAILED(hr)) {
            return mcdr::err("taaEnsureHistory", "CreateCommittedResource(history) hr=0x" + formatBackendResult(hr));
        }
        deviceContext().taaHistoryRtv[i] = handle;
        deviceContext().device->CreateRenderTargetView(deviceContext().taaHistory[i].Get(), nullptr, handle);
        handle.ptr += rtvInc;
    }
    deviceContext().taaWidth = w;
    deviceContext().taaHeight = h;
    return {};
}

}  // namespace
int applyTaaToTexture(CommandContext* ctx, Dx12Object* scene, Dx12Object* motion, float jitterX,
                      float jitterY, float prevJitterX, float prevJitterY, float blend, int reset,
                      float sharpenAmount) {

    if (deviceContext().aaMode != 2 || deviceContext().taaDisabled) {
        return 0;
    }
    if (!ctx || !ctx->listOpen || ctx->commandList == nullptr || !scene || !scene->resource || !motion) {
        return 0;
    }

    // 运动矢量按 TextureView 传进来（D3D12Velocity 的 view）：解包到底层纹理。
    // 注意 view 对象自身的 resource 是空的（它只是底层纹理的一个视图），所以这里不能像
    // scene 那样要求 !motion->resource —— 早期版本这么写过，结果 TAA 每帧静默返回 0。
    Dx12Object* motionTex = motion;
    if (motion->kind == Dx12Object::Kind::TextureView) {
        if (motion->sourceTexture == nullptr || !motion->sourceTexture->resource) {
            return 0;
        }
        motionTex = motion->sourceTexture;
    } else if (!motion->resource) {
        return 0;
    }

    ID3D12GraphicsCommandList* cmd = ctx->commandList;
    const D3D12_RESOURCE_DESC sceneDesc = scene->resource->GetDesc();
    const D3D12_RESOURCE_DESC motionDesc = motionTex->resource->GetDesc();
    if (sceneDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sceneDesc.SampleDesc.Count != 1 ||
        motionDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || motionDesc.SampleDesc.Count != 1) {
        if (!deviceContext().taaUnavailableLogged) {
            deviceContext().taaUnavailableLogged = true;
            dbgLogWarn("TAA needs single-sampled 2D scene and motion textures; skipping TAA");
        }
        return 0;
    }
    const UINT64 w = sceneDesc.Width;
    const UINT64 h = sceneDesc.Height;
    if (w == 0 || h == 0 || motionDesc.Width != w || motionDesc.Height != h) {
        if (!deviceContext().taaUnavailableLogged) {
            deviceContext().taaUnavailableLogged = true;
            dbgLogWarn("TAA needs matching scene/motion sizes (%llux%llu vs %llux%llu); skipping TAA",
                       static_cast<unsigned long long>(w), static_cast<unsigned long long>(h),
                       static_cast<unsigned long long>(motionDesc.Width),
                       static_cast<unsigned long long>(motionDesc.Height));
        }
        return 0;
    }
    // 回填走 blit / sharpen 两条现成管线，它们的 RTVFormats[0] 都写死 R8G8B8A8_UNORM ⇒ 场景
    // 不是这个格式就没有可用的目标管线。UNKNOWN 按 SMAA 的既有约定视为 RGBA8。
    DXGI_FORMAT sceneFormat = sceneDesc.Format;
    if (sceneFormat == DXGI_FORMAT_UNKNOWN) {
        sceneFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (sceneFormat != DXGI_FORMAT_R8G8B8A8_UNORM) {
        if (!deviceContext().taaUnavailableLogged) {
            deviceContext().taaUnavailableLogged = true;
            dbgLogWarn("TAA needs an RGBA8 scene target (fmt=%d); skipping TAA",
                       static_cast<int>(sceneFormat));
        }
        return 0;
    }

    // 任何一步失败都彻底关掉 TAA：避免每帧重试刷日志，也避免把历史留在与预期不符的状态上。
    auto fail = [](const char* where, const std::string& what) {
        deviceContext().taaDisabled = true;
        dbgLogError("TAA failed in %s; TAA disabled: %s", where, firstLine(what).c_str());
        return -1;
    };
    if (auto r = taaEnsurePipelines(); !r) {
        deviceContext().taaUnavailableLogged = true;
        return fail("taaEnsurePipelines", r.error().message);
    }
    if (auto r = taaEnsureHistory(w, h); !r) {
        deviceContext().taaUnavailableLogged = true;
        return fail("taaEnsureHistory", r.error().message);
    }
    if (auto r = initBlitPipeline(); !r) {
        return fail("initBlitPipeline", r.error().message);
    }
    const BlitPipeline* bp = getBlitPipeline();
    if (bp == nullptr) {
        return fail("getBlitPipeline", "blit pipeline unavailable");
    }

    // 尺寸/格式变化后历史不可信：本帧必须以 reset 开始（JVM 侧 reset() 时同样会置 false）。
    const bool resetNow = reset != 0 || !deviceContext().taaHistoryValid;
    const int readIndex = deviceContext().taaReadIndex;
    const int writeIndex = deviceContext().taaWriteIndex;

    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<FLOAT>(w), static_cast<FLOAT>(h), 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};

    // 历史在上一帧结束时**两张都停在 PIXEL_SHADER_RESOURCE**（见本节末尾的回切），所以本帧
    // 的初始锚点是它；首帧（刚创建）是 COMMON。不用 transitionTo 的 COMMON 回落语义：
    // D3D12 禁止在同一个命令列表里把已提升的资源显式回切 COMMON。
    const D3D12_RESOURCE_STATES historyPrev =
        gTaaHistoryInSrvState ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;

    // ① resolve：scene + history[read] + motion → history[write]
    transitionTextureTo(ctx, scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTo(ctx, deviceContext().taaHistory[readIndex].Get(), historyPrev,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTextureTo(ctx, motionTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTo(ctx, deviceContext().taaHistory[writeIndex].Get(), historyPrev, D3D12_RESOURCE_STATE_RENDER_TARGET);

    cmd->SetGraphicsRootSignature(deviceContext().taaRootSig.Get());
    cmd->SetPipelineState(deviceContext().taaPso.Get());
    cmd->IASetVertexBuffers(0, 1, &bp->vbView);
    cmd->IASetIndexBuffer(&bp->ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->OMSetRenderTargets(1, &deviceContext().taaHistoryRtv[writeIndex], FALSE, nullptr);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    // b0 = {w, h, 1/w, 1/h, blend, reset, jitter, prevJitter, pad}（顺序与 kTaaPS 的 cbuffer 一致）
    const float params[kTaaParam32BitCount] = {static_cast<float>(w),
                                               static_cast<float>(h),
                                               w == 0 ? 0.0f : 1.0f / static_cast<float>(w),
                                               h == 0 ? 0.0f : 1.0f / static_cast<float>(h),
                                               blend,
                                               resetNow ? 1.0f : 0.0f,
                                               jitterX,
                                               jitterY,
                                               prevJitterX,
                                               prevJitterY,
                                               0.0f,
                                               0.0f};
    cmd->SetGraphicsRoot32BitConstants(0, kTaaParam32BitCount, params, 0);

    if (auto r = smaaBindSrv(ctx, cmd, scene->resource.Get(), sceneFormat, 1); !r) {
        return fail("pass1.bind.scene", r.error().message);
    }
    if (auto r = smaaBindSrv(ctx, cmd, deviceContext().taaHistory[readIndex].Get(), kTaaHistoryFormat, 2); !r) {
        return fail("pass1.bind.history", r.error().message);
    }
    if (auto r = smaaBindSrv(ctx, cmd, motionTex->resource.Get(), motionTex->dxgiFormat, 3); !r) {
        return fail("pass1.bind.motion", r.error().message);
    }
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // ② 回填：history[write] → 主目标。不能 CopyResource —— 历史是 16F、主目标是 8 位。
    // 栈上临时包一层 Dx12Object，好复用 blitTextToTexture / sharpenTextureToTexture
    // （它们按 ID3D12Resource* 追踪状态，所以这个包装不需要进 gCtx）。
    Dx12Object historyObj{};
    historyObj.kind = Dx12Object::Kind::Texture;
    historyObj.resource = deviceContext().taaHistory[writeIndex];
    historyObj.dxgiFormat = kTaaHistoryFormat;
    bool filled = false;
    if (sharpenAmount > 0.001f) {
        auto sr = sharpenTextureToTexture(ctx, &historyObj, scene, sharpenAmount);
        if (sr) {
            filled = true;
        } else if (!deviceContext().taaSharpenFallbackWarned) {
            deviceContext().taaSharpenFallbackWarned = true;
            dbgLogWarn("TAA sharpen pass failed; falling back to a plain blit: %s",
                       firstLine(sr.error().message).c_str());
        }
    }
    if (!filled) {
        auto br = blitTextureToTexture(ctx, &historyObj, scene);
        if (!br) {
            return fail("pass2.blit", br.error().message);
        }
    }

    // 历史回切到 PIXEL_SHADER_RESOURCE：两张都停在这里，下一帧的锚点就是常量
    // （blit/sharpen 收尾会把源放回 RENDER_TARGET，这一步必须显式做）。
    transitionTo(ctx, deviceContext().taaHistory[writeIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gTaaHistoryInSrvState = true;

    // 交换乒乓：本帧的写缓冲就是下一帧的历史。
    deviceContext().taaReadIndex = writeIndex;
    deviceContext().taaWriteIndex = readIndex;
    deviceContext().taaHistoryValid = true;
    ++deviceContext().taaAppliedFrames;
    if (deviceContext().taaAppliedFrames == 1) {
        dbgLogInfo("TAA applied on the first frame: %llux%llu, history RGBA16F double buffered, blend=%.3f",
                   static_cast<unsigned long long>(w), static_cast<unsigned long long>(h),
                   static_cast<double>(blend));
    }
    return 1;
}
// ---------------------------------------------------------------------------

}
