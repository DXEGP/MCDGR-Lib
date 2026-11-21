module;
#include "_d3d12_module_prelude.h"

#include "xess.h"
#include "xess_d3d12.h"
#include "mcdr_abi.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

module mcdr.d3d12.device;

namespace d3d12_mc {
namespace {

constexpr const char* kXessDll = "libxess.dll";
constexpr int kMaxSide = 8192;

constexpr char kColorShader[] = R"hlsl(
Texture2D<float4> sourceColor : register(t0);
cbuffer Conversion : register(b0) { uint operation; };
struct VertexOutput { float4 position : SV_Position; };
VertexOutput ColorVertex(uint vertex : SV_VertexID) {
    VertexOutput result;
    result.position = float4(vertex == 2 ? 3.0 : -1.0, vertex == 1 ? 3.0 : -1.0, 0.0, 1.0);
    return result;
}
float decodeChannel(float value) {
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}
float encodeChannel(float value) {
    return value <= 0.0031308 ? value * 12.92 : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}
float4 ColorPixel(VertexOutput input) : SV_Target0 {
    float4 color = sourceColor.Load(int3(uint2(input.position.xy), 0));
    color = all(isfinite(color)) ? color : 0.0;
    color.rgb = max(color.rgb, 0.0);
    if (operation == 1) color.rgb = float3(decodeChannel(color.r), decodeChannel(color.g), decodeChannel(color.b));
    if (operation == 2) color.rgb = float3(encodeChannel(color.r), encodeChannel(color.g), encodeChannel(color.b));
    return color;
}
)hlsl";

bool srgbFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

bool encodedColorFormat(DXGI_FORMAT format) {
    return srgbFormat(format) || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM ||
        format == DXGI_FORMAT_R10G10B10A2_UNORM;
}

struct ColorConversionPipeline {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> decode;
    ComPtr<ID3D12PipelineState> encode;
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
};

struct ColorConversionSlot {
    Dx12Object* input = nullptr;
    Dx12Object* output = nullptr;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    uint32_t inputWidth = 0, inputHeight = 0, outputWidth = 0, outputHeight = 0;
    ~ColorConversionSlot() {
        if (input) destroyObject(input);
        if (output) destroyObject(output);
    }
};

bool createColorPipeline(ColorConversionPipeline& pipeline, DXGI_FORMAT format) {
    auto* device = deviceContext().device.Get();
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 1;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = 2;
    root.pParameters = parameters;
    ComPtr<ID3DBlob> serialized, diagnostic;
    if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostic)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&pipeline.root)))) return false;
    std::vector<uint8_t> vertex, pixel;
    if (!compileHlslToDxil(kColorShader, "ColorVertex", "vs_6_6", vertex) ||
        !compileHlslToDxil(kColorShader, "ColorPixel", "ps_6_6", pixel)) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = pipeline.root.Get();
    description.VS = {vertex.data(), vertex.size()};
    description.PS = {pixel.data(), pixel.size()};
    description.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    description.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    description.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    description.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    description.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    description.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    description.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    description.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.BackFace = description.DepthStencilState.FrontFace;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.SampleDesc.Count = 1;
    description.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline.decode)))) return false;
    description.RTVFormats[0] = format;
    if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline.encode)))) return false;
    pipeline.outputFormat = format;
    return true;
}

bool initializeColorSlot(ColorConversionSlot& slot, uint32_t width, uint32_t height, uint32_t displayWidth, uint32_t displayHeight) {
    auto input = createTexture(4 | 8 | 32, 43, static_cast<int>(width), static_cast<int>(height), 1, 1);
    if (!input) return false;
    slot.input = static_cast<Dx12Object*>(*input);
    auto output = createTexture(4 | 8 | 32, 43, static_cast<int>(displayWidth), static_cast<int>(displayHeight), 1, 1);
    if (!output) return false;
    slot.output = static_cast<Dx12Object*>(*output);
    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    description.NumDescriptors = 2;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(deviceContext().device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&slot.descriptors)))) return false;
    slot.inputWidth = width;
    slot.inputHeight = height;
    slot.outputWidth = displayWidth;
    slot.outputHeight = displayHeight;
    return true;
}

bool convertColor(CommandContext* context, ColorConversionPipeline& pipeline, ColorConversionSlot& slot,
    Dx12Object* source, Dx12Object* target, UINT index, uint32_t operation) {
    auto targetRtv = acquireTextureRtv(context, target);
    if (!targetRtv) return false;
    retainObjectForRecording(context, source);
    retainObjectForRecording(context, target);
    transitionTextureTo(context, source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTextureTo(context, target, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const UINT increment = deviceContext().device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = slot.descriptors->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * increment;
    auto gpu = slot.descriptors->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<uint64_t>(index) * increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = source->dxgiFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    deviceContext().device->CreateShaderResourceView(source->resource.Get(), &view, cpu);
    ID3D12DescriptorHeap* heaps[] = {slot.descriptors.Get()};
    context->commandList->SetDescriptorHeaps(1, heaps);
    context->commandList->SetGraphicsRootSignature(pipeline.root.Get());
    context->commandList->SetPipelineState(index == 0 ? pipeline.decode.Get() : pipeline.encode.Get());
    context->commandList->SetGraphicsRootDescriptorTable(0, gpu);
    context->commandList->SetGraphicsRoot32BitConstant(1, operation, 0);
    const auto description = target->resource->GetDesc();
    D3D12_VIEWPORT viewport{0, 0, static_cast<float>(description.Width), static_cast<float>(description.Height), 0, 1};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(description.Width), static_cast<LONG>(description.Height)};
    context->commandList->RSSetViewports(1, &viewport);
    context->commandList->RSSetScissorRects(1, &scissor);
    context->commandList->OMSetRenderTargets(1, &*targetRtv, FALSE, nullptr);
    context->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->commandList->DrawInstanced(3, 1, 0, 0);
    return true;
}

std::wstring selfDirectory() {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&selfDirectory), &self) == FALSE ||
        self == nullptr || ::GetModuleFileNameW(self, path, MAX_PATH) == 0) {
        return {};
    }
    std::wstring dir(path);
    const std::wstring::size_type slash = dir.find_last_of(L"\\/");
    dir.resize(slash == std::wstring::npos ? 0 : slash);
    return dir;
}

struct XessApi {
    HMODULE module = nullptr;
    decltype(&xessD3D12CreateContext) createContext = nullptr;
    decltype(&xessD3D12Init) init = nullptr;
    decltype(&xessD3D12Execute) execute = nullptr;
    decltype(&xessDestroyContext) destroy = nullptr;
    decltype(&xessGetVersion) getVersion = nullptr;
    decltype(&xessSetExposureMultiplier) setExposureMultiplier = nullptr;
    decltype(&xessGetOptimalInputResolution) getOptimalInputResolution = nullptr;
    decltype(&xessD3D12BuildPipelines) buildPipelines = nullptr;
    decltype(&xessIsOptimalDriver) isOptimalDriver = nullptr;
    decltype(&xessGetProperties) getProperties = nullptr;

    bool ready() const {
        return createContext != nullptr && init != nullptr && execute != nullptr &&
               destroy != nullptr && getVersion != nullptr && getOptimalInputResolution != nullptr &&
               setExposureMultiplier != nullptr;
    }
};

struct XessRuntime {
    std::mutex lock;
    bool attempted = false;
    XessApi api{};
    std::string problem;
    std::string version;
};

XessRuntime& runtime() {
    static XessRuntime instance;
    return instance;
}

bool loadLocked(XessRuntime& rt) {
    if (rt.attempted) { return rt.problem.empty(); }
    rt.attempted = true;

    std::wstring candidate = selfDirectory();
    if (!candidate.empty()) {
        candidate += L"\\";
        candidate += std::wstring(kXessDll, kXessDll + std::strlen(kXessDll));
    }
    HMODULE module = candidate.empty() ? nullptr : ::LoadLibraryW(candidate.c_str());
    if (module == nullptr) {
        module = ::LoadLibraryA(kXessDll);
    }
    if (module == nullptr) {
        rt.problem = "cannot load ";
        rt.problem += kXessDll;
        rt.problem += " (win32 error ";
        rt.problem += std::to_string(static_cast<int>(::GetLastError()));
        rt.problem += ")";
        return false;
    }

    XessApi api{};
    api.module = module;
    api.createContext = reinterpret_cast<decltype(api.createContext)>(
        ::GetProcAddress(module, "xessD3D12CreateContext"));
    api.init = reinterpret_cast<decltype(api.init)>(::GetProcAddress(module, "xessD3D12Init"));
    api.execute = reinterpret_cast<decltype(api.execute)>(
        ::GetProcAddress(module, "xessD3D12Execute"));
    api.setExposureMultiplier = reinterpret_cast<decltype(api.setExposureMultiplier)>(
        ::GetProcAddress(module, "xessSetExposureMultiplier"));
    api.destroy = reinterpret_cast<decltype(api.destroy)>(
        ::GetProcAddress(module, "xessDestroyContext"));
    api.getVersion = reinterpret_cast<decltype(api.getVersion)>(
        ::GetProcAddress(module, "xessGetVersion"));
    api.getOptimalInputResolution = reinterpret_cast<decltype(api.getOptimalInputResolution)>(
        ::GetProcAddress(module, "xessGetOptimalInputResolution"));
    api.buildPipelines = reinterpret_cast<decltype(api.buildPipelines)>(
        ::GetProcAddress(module, "xessD3D12BuildPipelines"));
    api.isOptimalDriver = reinterpret_cast<decltype(api.isOptimalDriver)>(
        ::GetProcAddress(module, "xessIsOptimalDriver"));
    api.getProperties = reinterpret_cast<decltype(api.getProperties)>(
        ::GetProcAddress(module, "xessGetProperties"));

    if (!api.ready()) {
        rt.problem = "xess entry points missing in ";
        rt.problem += kXessDll;
        return false;
    }

    xess_version_t version{};
    if (api.getVersion(&version) == XESS_RESULT_SUCCESS) {
        rt.version = std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
                     std::to_string(version.patch);
    }
    rt.api = api;
    rt.problem.clear();
    return true;
}

bool isAvailable(std::string& why) {
    XessRuntime& rt = runtime();
    std::lock_guard<std::mutex> guard(rt.lock);
    const bool ok = loadLocked(rt);
    why = rt.problem;
    return ok;
}

xess_quality_settings_t qualityForRatio(double ratio) {
    struct Entry {
        double ratio;
        xess_quality_settings_t setting;
    };
    static const Entry kTable[] = {
        {1.00, XESS_QUALITY_SETTING_AA},
        {1.30, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS},
        {1.50, XESS_QUALITY_SETTING_ULTRA_QUALITY},
        {1.70, XESS_QUALITY_SETTING_QUALITY},
        {2.00, XESS_QUALITY_SETTING_BALANCED},
        {2.30, XESS_QUALITY_SETTING_PERFORMANCE},
        {3.00, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE},
    };
    if (!(ratio > 0.0) || !std::isfinite(ratio)) { return XESS_QUALITY_SETTING_QUALITY; }
    double bestDistance = 1.0e9;
    xess_quality_settings_t best = XESS_QUALITY_SETTING_QUALITY;
    for (const Entry& entry : kTable) {
        const double distance = std::fabs(ratio - entry.ratio);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = entry.setting;
        }
    }
    return best;
}

Dx12Object* asObject(void* handle) {
    return handle == nullptr ? nullptr : static_cast<Dx12Object*>(handle);
}

void writeNote(char* dst, size_t cap, const std::string& text) {
    if (dst == nullptr || cap == 0) { return; }
    const size_t limit = cap - 1;
    const size_t length = text.size() < limit ? text.size() : limit;
    std::memcpy(dst, text.data(), length);
    dst[length] = '\0';
}

void fillCapsFields(mcdr_sr_caps& out, bool available, const std::string& note) {
    out.backend = MCDR_SR_BACKEND_XESS;
    out.available = available ? 1 : 0;
    out.coupledDenoise = 0;
    out.needsExposure = 0;
    out.takesMasks = MCDR_SR_MASK_RESPONSIVE;
    out.externalHeap = 1;
    out.jitterPhaseCount = 8;
    out.minRenderWidth = 320;
    out.minRenderHeight = 180;
    out.maxRenderWidth = 7680;
    out.maxRenderHeight = 4320;
    out.nativeAA = 1;
    out.reserved = 0;
    writeNote(out.note, sizeof(out.note), note);
}

}

class XessSrProvider final : public IUpscaleProvider {
public:
    SrBackendId backend() const override { return SrBackendId::XeSS; }

    std::string failureText() const override { return problem_; }

    void fillCaps(mcdr_sr_caps& out) const override {
        std::string why;
        const bool ok = isAvailable(why);
        if (ok) {
            std::string note = "libxess";
            {
                std::lock_guard<std::mutex> guard(runtime().lock);
                if (!runtime().version.empty()) {
                    note += " ";
                    note += runtime().version;
                }
            }
            fillCapsFields(out, true, note);
        } else {
            fillCapsFields(out, false, why);
        }
    }

    bool initialize(CommandContext* ctx, const mcdr_sr_dispatch& seed) override {
        device_ = deviceContext().device.Get();
        if (device_ == nullptr) {
            problem_ = "no d3d12 device";
            return false;
        }
        adoptSizes(seed.renderWidth, seed.renderHeight, seed.displayWidth, seed.displayHeight);
        maskExpected_ = seed.pReactiveMask != nullptr;
        exposureExpected_ = seed.pExposure != nullptr;
        hdrInput_ = seed.hdrInput != 0;
        return ensureContext(ctx);
    }

    void onResize(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                  int32_t displayHeight) override {
        adoptSizes(renderWidth, renderHeight, displayWidth, displayHeight);
    }

    bool evaluate(CommandContext* ctx, const mcdr_sr_dispatch& dispatch) override {
        if (ctx == nullptr || ctx->commandList == nullptr) {
            problem_ = "no command list";
            return false;
        }
        Dx12Object* color = asObject(dispatch.pColorInput);
        Dx12Object* depth = asObject(dispatch.pDepth);
        Dx12Object* motion = asObject(dispatch.pMotionVectors);
        Dx12Object* output = asObject(dispatch.pColorOutput);
        Dx12Object* mask = asObject(dispatch.pReactiveMask);
        Dx12Object* exposure = asObject(dispatch.pExposure);
        if (color == nullptr) { problem_ = "missing color input"; return false; }
        if (depth == nullptr) { problem_ = "missing depth"; return false; }
        if (motion == nullptr) { problem_ = "missing motion vectors"; return false; }
        if (output == nullptr) { problem_ = "missing color output"; return false; }
        if (exposure != nullptr && (!exposure->resource || exposure->dxgiFormat != DXGI_FORMAT_R32_FLOAT ||
            exposure->resource->GetDesc().Width != 1 || exposure->resource->GetDesc().Height != 1)) {
            problem_ = "incompatible exposure texture";
            return false;
        }
        if (color->resource == nullptr || depth->resource == nullptr ||
            motion->resource == nullptr || output->resource == nullptr) {
            problem_ = "resource handle without d3d12 resource";
            return false;
        }
        const bool convertLdr = encodedColorFormat(color->dxgiFormat);
        if (convertLdr && (!encodedColorFormat(output->dxgiFormat) || dispatch.hdrInput != 0 ||
            (dispatch.preExposure != 0.0f && dispatch.preExposure != 1.0f))) {
            problem_ = "encoded LDR color requires encoded output and unity preExposure";
            return false;
        }
        const bool actualHdrInput = !convertLdr && dispatch.hdrInput != 0;
        if (convertLdr) {
            const auto sourceDescription = color->resource->GetDesc();
            const auto outputDescription = output->resource->GetDesc();
            if (sourceDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                outputDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                sourceDescription.SampleDesc.Count != 1 || outputDescription.SampleDesc.Count != 1 ||
                sourceDescription.DepthOrArraySize != 1 || outputDescription.DepthOrArraySize != 1 ||
                sourceDescription.Width != static_cast<uint64_t>(dispatch.renderWidth) ||
                sourceDescription.Height != static_cast<uint32_t>(dispatch.renderHeight) ||
                outputDescription.Width != static_cast<uint64_t>(dispatch.displayWidth) ||
                outputDescription.Height != static_cast<uint32_t>(dispatch.displayHeight) ||
                (sourceDescription.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
                !(outputDescription.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
                problem_ = "incompatible XeSS encoded color resources";
                return false;
            }
        }
        if (mask != nullptr && mask->resource == nullptr) { mask = nullptr; }
        if (dispatch.renderWidth < 320 || dispatch.renderHeight < 180) {
            if (!tooSmallLogged_) {
                tooSmallLogged_ = true;
                dbgLogError("render %dx%d is below the %s minimum of 320x180; skipping "
                            "the evaluate (raise the render scale)",
                            dispatch.renderWidth, dispatch.renderHeight, "Intel XeSS");
            }
            problem_ = "render size below the 320x180 minimum";
            return false;
        }
        if (mask == nullptr && maskExpected_) {
            maskExpected_ = false;
            needsRecreate_ = true;
        } else if (mask != nullptr && !maskExpected_) {
            maskExpected_ = true;
            needsRecreate_ = true;
        }
        adoptSizes(dispatch.renderWidth, dispatch.renderHeight, dispatch.displayWidth,
                   dispatch.displayHeight);
        if (exposureExpected_ != (exposure != nullptr)) {
            exposureExpected_ = exposure != nullptr;
            needsRecreate_ = true;
        }
        if (hdrInput_ != actualHdrInput) {
            hdrInput_ = actualHdrInput;
            needsRecreate_ = true;
        }
        if (!ensureContext(ctx)) { return false; }
        const float inversePreExposure = dispatch.preExposure > 0.0f ? 1.0f / dispatch.preExposure : 1.0f;
        if (runtime().api.setExposureMultiplier(handle_, inversePreExposure) != XESS_RESULT_SUCCESS) {
            problem_ = "xessSetExposureMultiplier failed";
            return false;
        }

        std::shared_ptr<ColorConversionSlot> conversion;
        std::shared_ptr<ColorConversionPipeline> conversionPipeline;
        Dx12Object* sdkColor = color;
        Dx12Object* sdkOutput = output;
        if (convertLdr) {
            for (const auto& candidate : colorPipelines_) {
                if (candidate->outputFormat == output->dxgiFormat) { conversionPipeline = candidate; break; }
            }
            if (!conversionPipeline) {
                conversionPipeline = std::make_shared<ColorConversionPipeline>();
                if (!createColorPipeline(*conversionPipeline, output->dxgiFormat)) {
                    problem_ = "XeSS color conversion pipeline creation failed";
                    return false;
                }
                colorPipelines_.push_back(conversionPipeline);
            }
            for (const auto& candidate : colorSlots_) {
                if (candidate.use_count() == 1 && candidate->inputWidth == static_cast<uint32_t>(renderW_) &&
                    candidate->inputHeight == static_cast<uint32_t>(renderH_) &&
                    candidate->outputWidth == static_cast<uint32_t>(displayW_) && candidate->outputHeight == static_cast<uint32_t>(displayH_)) {
                    conversion = candidate;
                    break;
                }
            }
            if (!conversion) {
                colorSlots_.erase(std::remove_if(colorSlots_.begin(), colorSlots_.end(),
                    [](const auto& candidate) { return candidate.use_count() == 1; }), colorSlots_.end());
                conversion = std::make_shared<ColorConversionSlot>();
                if (!initializeColorSlot(*conversion, static_cast<uint32_t>(renderW_), static_cast<uint32_t>(renderH_),
                    static_cast<uint32_t>(displayW_), static_cast<uint32_t>(displayH_))) {
                    problem_ = "XeSS linear color resource creation failed";
                    return false;
                }
                colorSlots_.push_back(conversion);
            }
            contextPin(ctx, conversionPipeline, conversion);
            if (!convertColor(ctx, *conversionPipeline, *conversion, color, conversion->input, 0,
                srgbFormat(color->dxgiFormat) ? 0u : 1u)) {
                problem_ = "XeSS input linearization failed";
                restoreDrawHeap(ctx);
                return false;
            }
            sdkColor = conversion->input;
            sdkOutput = conversion->output;
        }

        transitionTextureTo(ctx, sdkColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (convertLdr) transitionTextureTo(ctx, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transitionTextureTo(ctx, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transitionTextureTo(ctx, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (exposure) transitionTextureTo(ctx, exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (mask != nullptr) {
            transitionTextureTo(ctx, mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        transitionTextureTo(ctx, sdkOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        xess_d3d12_execute_params_t params{};
        params.pColorTexture = sdkColor->resource.Get();
        params.pVelocityTexture = motion->resource.Get();
        params.pDepthTexture = depth->resource.Get();
        params.pExposureScaleTexture = exposure ? exposure->resource.Get() : nullptr;
        params.pResponsivePixelMaskTexture =
            mask != nullptr ? mask->resource.Get() : nullptr;
        params.pOutputTexture = sdkOutput->resource.Get();
        params.jitterOffsetX = dispatch.jitterOffsetX;
        params.jitterOffsetY = dispatch.jitterOffsetY;
        params.exposureScale = 1.0f;
        params.resetHistory = dispatch.reset != 0 ? 1u : 0u;
        params.inputWidth = static_cast<uint32_t>(renderW_);
        params.inputHeight = static_cast<uint32_t>(renderH_);
        params.pDescriptorHeap = nullptr;

        const xess_result_t rc = runtime().api.execute(handle_, ctx->commandList, &params);
        if (rc == XESS_RESULT_SUCCESS && conversion && !convertColor(ctx, *conversionPipeline, *conversion,
            sdkOutput, output, 1, srgbFormat(output->dxgiFormat) ? 0u : 2u)) {
            problem_ = "XeSS output encoding failed";
            transitionTextureTo(ctx, output, D3D12_RESOURCE_STATE_RENDER_TARGET);
            restoreDrawHeap(ctx);
            return false;
        }
        transitionTextureTo(ctx, output, D3D12_RESOURCE_STATE_RENDER_TARGET);
        restoreDrawHeap(ctx);
        if (rc != XESS_RESULT_SUCCESS) {
            problem_ = "xessD3D12Execute failed rc=";
            problem_ += std::to_string(static_cast<int>(rc));
            if (!logged_) {
                dbgLogWarn("XeSS execute failed: %s", problem_.c_str());
                logged_ = true;
            }
            return false;
        }
        problem_.clear();
        return true;
    }

    void shutdown() override {
        destroyContext();
        colorSlots_.clear();
        colorPipelines_.clear();
        problem_.clear();
        logged_ = false;
    }

private:
    void contextPin(CommandContext* context, const std::shared_ptr<ColorConversionPipeline>& pipeline,
        const std::shared_ptr<ColorConversionSlot>& slot) {
        context->recordingLeases[context->currentListSlot].push_back(pipeline);
        context->recordingLeases[context->currentListSlot].push_back(slot);
        retainObjectForRecording(context, slot->input);
        retainObjectForRecording(context, slot->output);
    }

    void adoptSizes(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                    int32_t displayHeight) {
        if (renderWidth > 0) { renderW_ = renderWidth; }
        if (renderHeight > 0) { renderH_ = renderHeight; }
        if (displayWidth > 0) { displayW_ = displayWidth; }
        if (displayHeight > 0) { displayH_ = displayHeight; }
    }

    xess_quality_settings_t pickQualityForSize(const xess_2d_t& outputResolution,
                                               xess_2d_t& optimalOut) {
        static constexpr xess_quality_settings_t kCandidates[] = {
            XESS_QUALITY_SETTING_QUALITY,
            XESS_QUALITY_SETTING_BALANCED,
            XESS_QUALITY_SETTING_ULTRA_QUALITY,
            XESS_QUALITY_SETTING_PERFORMANCE,
            XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
            XESS_QUALITY_SETTING_ULTRA_PERFORMANCE,
            XESS_QUALITY_SETTING_AA,
        };
        const double wanted = static_cast<double>(displayW_) / static_cast<double>(renderW_);
        xess_quality_settings_t best = XESS_QUALITY_SETTING_QUALITY;
        double bestScore = 1.0e30;
        for (const xess_quality_settings_t candidate : kCandidates) {
            xess_2d_t optimal{};
            xess_2d_t minSize{};
            xess_2d_t maxSize{};
            if (runtime().api.getOptimalInputResolution(handle_, &outputResolution, candidate,
                                                        &optimal, &minSize, &maxSize) !=
                XESS_RESULT_SUCCESS) {
                continue;
            }
            const double got = static_cast<double>(displayW_) /
                               static_cast<double>(optimal.x == 0 ? 1u : optimal.x);
            const double score = got > wanted ? got - wanted : wanted - got;
            if (minSize.x <= static_cast<uint32_t>(renderW_) &&
                minSize.y <= static_cast<uint32_t>(renderH_) &&
                maxSize.x >= static_cast<uint32_t>(renderW_) &&
                maxSize.y >= static_cast<uint32_t>(renderH_)) {
                inputMin_ = minSize;
                inputMax_ = maxSize;
                optimalOut = optimal;
                if (candidate != XESS_QUALITY_SETTING_QUALITY) {
                    dbgLogInfo("XeSS quality %d fits render %dx%d (range %ux%u..%ux%u)",
                               static_cast<int>(candidate), renderW_, renderH_, minSize.x,
                               minSize.y, maxSize.x, maxSize.y);
                }
                return candidate;
            }
            if (score < bestScore) {
                bestScore = score;
                best = candidate;
                inputMin_ = minSize;
                inputMax_ = maxSize;
                optimalOut = optimal;
            }
        }
        return best;
    }
    bool ensureContext(CommandContext* ctx) {
        std::string why;
        if (!isAvailable(why)) {
            if (!logged_) {
                dbgLogWarn("XeSS unavailable: %s", why.c_str());
                logged_ = true;
            }
            problem_ = why.empty() ? std::string("libxess unavailable") : why;
            return false;
        }
        if (renderW_ <= 0 || renderH_ <= 0 || displayW_ <= 0 || displayH_ <= 0) {
            problem_ = "sizes not set";
            return false;
        }
        if (renderW_ > kMaxSide || renderH_ > kMaxSide || displayW_ > kMaxSide ||
            displayH_ > kMaxSide) {
            problem_ = "size beyond 8k";
            return false;
        }
        if (handle_ != nullptr && !needsRecreate_ && displayW_ == builtDisplayW_ &&
            displayH_ == builtDisplayH_ && renderW_ >= static_cast<int32_t>(inputMin_.x) &&
            renderW_ <= static_cast<int32_t>(inputMax_.x) &&
            renderH_ >= static_cast<int32_t>(inputMin_.y) &&
            renderH_ <= static_cast<int32_t>(inputMax_.y)) {
            return true;
        }
        if (handle_ != nullptr) {
            if (ctx != nullptr && ctx->fenceValue > 0) {
                if (!waitForFenceValue(ctx, ctx->fenceValue, 2000000000ull)) {
                    problem_ = "context resize fence timed out";
                    return false;
                }
            }
            destroyContext();
        }

        const xess_result_t createRc =
            runtime().api.createContext(device_, &handle_);
        if (createRc != XESS_RESULT_SUCCESS || handle_ == nullptr) {
            handle_ = nullptr;
            problem_ = "xessD3D12CreateContext failed rc=";
            problem_ += std::to_string(static_cast<int>(createRc));
            if (!logged_) {
                dbgLogWarn("XeSS context creation failed: %s", problem_.c_str());
                logged_ = true;
            }
            return false;
        }
        if (runtime().api.isOptimalDriver != nullptr &&
            runtime().api.isOptimalDriver(handle_) == XESS_RESULT_WARNING_OLD_DRIVER) {
            dbgLogWarn("XeSS: driver is older than recommended (XESS_RESULT_WARNING_OLD_DRIVER)");
        }

        xess_2d_t outputResolution{static_cast<uint32_t>(displayW_),
                                   static_cast<uint32_t>(displayH_)};
        xess_2d_t optimal{};
        inputMin_ = xess_2d_t{};
        inputMax_ = xess_2d_t{};
        const xess_quality_settings_t quality = pickQualityForSize(outputResolution, optimal);

        uint32_t flags = XESS_INIT_FLAG_INVERTED_DEPTH;
        if (maskExpected_) { flags |= XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK; }
        if (exposureExpected_) { flags |= XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE; }
        if (!hdrInput_) { flags |= XESS_INIT_FLAG_LDR_INPUT_COLOR; }

        if (runtime().api.buildPipelines != nullptr) {
            (void)runtime().api.buildPipelines(handle_, nullptr, false, flags);
        }

        ensureTempHeaps(outputResolution);

        xess_d3d12_init_params_t initParams{};
        initParams.outputResolution = outputResolution;
        initParams.qualitySetting = quality;
        initParams.initFlags = flags;
        initParams.creationNodeMask = 0;
        initParams.visibleNodeMask = 0;
        initParams.pTempBufferHeap = tempBufferHeap_.Get();
        initParams.pTempTextureHeap = tempTextureHeap_.Get();
        initParams.bufferHeapOffset = 0;
        initParams.textureHeapOffset = 0;
        initParams.pPipelineLibrary = nullptr;

        const xess_result_t initRc = runtime().api.init(handle_, &initParams);
        if (initRc != XESS_RESULT_SUCCESS) {
            destroyContext();
            problem_ = "xessD3D12Init failed rc=";
            problem_ += std::to_string(static_cast<int>(initRc));
            if (!logged_) {
                dbgLogWarn("XeSS init failed: %s (render=%dx%d display=%dx%d quality=%d)",
                           problem_.c_str(), renderW_, renderH_, displayW_,
                           static_cast<int>(quality));
                logged_ = true;
            }
            return false;
        }

        builtDisplayW_ = displayW_;
        builtDisplayH_ = displayH_;
        needsRecreate_ = false;
        std::string version;
        {
            std::lock_guard<std::mutex> guard(runtime().lock);
            version = runtime().version;
        }
        dbgLogInfo("XeSS ready: libxess %s render %dx%d display %dx%d quality=%d flags=0x%x "
                   "inputRange %ux%u..%ux%u",
                   version.empty() ? "?" : version.c_str(), renderW_, renderH_, displayW_,
                   displayH_, static_cast<int>(quality), flags, inputMin_.x, inputMin_.y,
                   inputMax_.x, inputMax_.y);
        problem_.clear();
        return true;
    }

    void destroyContext() {
        if (handle_ != nullptr) {
            (void)runtime().api.destroy(handle_);
            handle_ = nullptr;
        }
        builtDisplayW_ = 0;
        builtDisplayH_ = 0;
        tempBufferHeap_.Reset();
        tempTextureHeap_.Reset();
    }

    void ensureTempHeaps(const xess_2d_t& outputResolution) {
        if (tempBufferHeap_ != nullptr && tempTextureHeap_ != nullptr) { return; }
        if (device_ == nullptr || runtime().api.getProperties == nullptr) { return; }
        xess_properties_t props{};
        if (runtime().api.getProperties(handle_, &outputResolution, &props) != XESS_RESULT_SUCCESS) {
            return;
        }
        const uint64_t kAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        auto alignUp = [kAlign](uint64_t v) { return (v + kAlign - 1) & ~(kAlign - 1); };
        D3D12_HEAP_DESC desc{};
        desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        desc.Properties.CreationNodeMask = 1;
        desc.Properties.VisibleNodeMask = 1;
        desc.Alignment = kAlign;
        desc.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;

        ComPtr<ID3D12Heap> bufferHeap;
        desc.SizeInBytes = alignUp(props.tempBufferHeapSize);
        HRESULT hr = desc.SizeInBytes > 0 ? device_->CreateHeap(&desc, IID_PPV_ARGS(&bufferHeap)) : E_INVALIDARG;
        if (FAILED(hr)) {
            warnHeapOnce("temp buffer", hr, props.tempBufferHeapSize);
            return;
        }

        ComPtr<ID3D12Heap> textureHeap;
        desc.SizeInBytes = alignUp(props.tempTextureHeapSize);
        hr = desc.SizeInBytes > 0 ? device_->CreateHeap(&desc, IID_PPV_ARGS(&textureHeap)) : E_INVALIDARG;
        if (FAILED(hr)) {
            warnHeapOnce("temp texture", hr, props.tempTextureHeapSize);
            return;
        }

        tempBufferHeap_ = bufferHeap;
        tempTextureHeap_ = textureHeap;
        dbgLogInfo("XeSS external heaps: buffer=%.1f MB texture=%.1f MB (descriptors=%u)",
                   static_cast<double>(props.tempBufferHeapSize) / (1024.0 * 1024.0),
                   static_cast<double>(props.tempTextureHeapSize) / (1024.0 * 1024.0),
                   props.requiredDescriptorCount);
    }

    void warnHeapOnce(const char* what, HRESULT hr, uint64_t bytes) {
        if (heapWarned_) { return; }
        heapWarned_ = true;
        dbgLogWarn("XeSS: could not create the %s heap (%llu B, hr=0x%08lx); "
                   "falling back to XeSS-allocated memory",
                   what, static_cast<unsigned long long>(bytes), static_cast<unsigned long>(hr));
    }

    ComPtr<ID3D12Heap> tempBufferHeap_;
    ComPtr<ID3D12Heap> tempTextureHeap_;
    bool heapWarned_ = false;

    ID3D12Device* device_ = nullptr;
    xess_context_handle_t handle_ = nullptr;
    xess_2d_t inputMin_{};
    xess_2d_t inputMax_{};
    int32_t builtDisplayW_ = 0;
    int32_t builtDisplayH_ = 0;
    int32_t renderW_ = 0;
    int32_t renderH_ = 0;
    int32_t displayW_ = 0;
    int32_t displayH_ = 0;
    bool maskExpected_ = false;
    bool exposureExpected_ = false;
    bool hdrInput_ = false;
    bool needsRecreate_ = false;
    std::string problem_;
    bool logged_ = false;
    /** P291i：尺寸过小只记一次日志，避免每帧刷屏。 */
    bool tooSmallLogged_ = false;
    std::vector<std::shared_ptr<ColorConversionPipeline>> colorPipelines_;
    std::vector<std::shared_ptr<ColorConversionSlot>> colorSlots_;
};

void fillXessCaps(mcdr_sr_caps& out) {
    std::string why;
    const bool ok = isAvailable(why);
    if (ok) {
        std::string note = "libxess";
        {
            std::lock_guard<std::mutex> guard(runtime().lock);
            if (!runtime().version.empty()) {
                note += " ";
                note += runtime().version;
            }
        }
        fillCapsFields(out, true, note);
    } else {
        fillCapsFields(out, false, why);
    }
}

std::unique_ptr<IUpscaleProvider> makeXessProvider() {
    std::string why;
    if (!isAvailable(why)) {
        dbgLogInfo("XeSS provider not created: %s", why.c_str());
        return nullptr;
    }
    return std::make_unique<XessSrProvider>();
}

bool xessRuntimeAvailable() {
    std::string why;
    return isAvailable(why);
}

}
