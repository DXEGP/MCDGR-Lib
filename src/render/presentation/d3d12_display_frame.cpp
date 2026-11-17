module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

module mcdr.render.display;

import mcdr.d3d12.device;
import mcdr.d3d12.workgraph;
import mcdr.error;

namespace d3d12_mc::display {
namespace {
namespace workgraph = mcdr::workgraph;

thread_local std::string failureText;

constexpr char kDisplayShader[] = R"hlsl(
struct Layer {
    column_major float4x4 inverseRaster;
    column_major float4x4 sourceToCommon;
    uint2 extent;
    uint flags;
    uint composition;
    float2 jitter;
    float2 padding;
};
cbuffer DisplayConstants : register(b0) {
    Layer world;
    Layer viewmodel;
    column_major float4x4 canonicalProjection;
    column_major float4x4 inverseCurrentPost;
    column_major float4x4 previousPost;
    uint2 displayExtent;
    uint layerCount;
    uint dilate;
};
Texture2D<float> worldDepth : register(t0);
Texture2D<float2> worldMotion : register(t1);
Texture2D<float> viewmodelDepth : register(t2);
Texture2D<float4> viewmodelMotion : register(t3);
Texture2D<float4> worldCoverage : register(t4);
Texture2D<float4> sourceTuples : register(t5);
RWTexture2D<float4> mergedTuples : register(u0);
RWTexture2D<float> finalDepth : register(u1);
RWTexture2D<float2> finalMotion : register(u2);
RWByteAddressBuffer guideValidity : register(u3);

struct DisplayRecord {
    uint3 grid : SV_DispatchGrid;
    uint stage;
};

float2 mapUv(column_major float4x4 transform, float2 uv) {
    float4 mappedPoint = mul(transform, float4(uv, 0.0, 1.0));
    return mappedPoint.xy / mappedPoint.w;
}

float canonicalDepth(Layer layer, uint2 coordinate, float depth, out bool valid) {
    valid = isfinite(depth) && depth >= 0.0 && depth <= 1.0;
    if (!valid || depth == 0.0) return 0.0;
    float2 uv = (float2(coordinate) + 0.5) / float2(layer.extent);
    float4 eye = mul(layer.inverseRaster, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    if (!all(isfinite(eye)) || abs(eye.w) <= 1e-7) { valid = false; return 0.0; }
    eye /= eye.w;
    float4 common = mul(layer.sourceToCommon, eye);
    float4 encoded = mul(canonicalProjection, common);
    if (!all(isfinite(encoded)) || encoded.w <= 1e-7) { valid = false; return 0.0; }
    float result = encoded.z / encoded.w;
    valid = result >= 0.0 && result <= 1.0;
    return saturate(result);
}

float4 sourceTuple(uint kind, Layer layer, float2 unwarpedUv, float2 currentFinal) {
    float2 samplePosition = unwarpedUv * float2(layer.extent) + layer.jitter - 0.5;
    int2 first = int2(floor(samplePosition));
    float4 selected = 0.0;
    float bestDistance = 1e30;
    [unroll]
    for (int index = 0; index < 4; ++index) {
        int2 coordinate = clamp(first + int2(index & 1, index >> 1), int2(0, 0), int2(layer.extent) - 1);
        float depth = kind == 1u ? worldDepth.Load(int3(coordinate, 0)) : viewmodelDepth.Load(int3(coordinate, 0));
        float coverageSample = kind == 1u ? ((layer.flags & 8u) != 0u ? worldCoverage.Load(int3(coordinate, 0)).x
            : 3.0 / 255.0) : sourceTuples.Load(int3(coordinate, 0)).x;
        if (!isfinite(coverageSample)) { guideValidity.InterlockedOr(0, 2u); continue; }
        uint coverage = uint(round(saturate(coverageSample) * 255.0));
        if (coverage != 0u && coverage != 1u && coverage != 3u) {
            guideValidity.InterlockedOr(0, 2u);
            continue;
        }
        if ((coverage & 1u) == 0u) continue;
        bool depthValid;
        float encoded = canonicalDepth(layer, uint2(coordinate), depth, depthValid);
        float distance = dot(float2(coordinate) - samplePosition, float2(coordinate) - samplePosition);
        if (encoded < selected.z || (encoded == selected.z && distance >= bestDistance)) continue;
        float2 motion = kind == 1u ? worldMotion.Load(int3(coordinate, 0))
            : viewmodelMotion.Load(int3(coordinate, 0)).xy;
        bool motionValid = depthValid && (layer.flags & 3u) == 3u && (coverage & 2u) != 0u && all(isfinite(motion));
        float2 previousUv = unwarpedUv + motion / float2(layer.extent);
        float2 previousFinal = mapUv(previousPost, previousUv);
        float2 displayMotion = (previousFinal - currentFinal) * float2(displayExtent);
        motionValid = motionValid && all(isfinite(displayMotion)) && all(abs(displayMotion) <= 65504.0);
        uint provenance = kind | ((layer.flags & 4u) != 0u ? 4u : 0u) | (motionValid ? 8u : 0u);
        selected = float4(motionValid ? displayMotion : float2(0.0, 0.0), encoded, (float)provenance);
        bestDistance = distance;
    }
    return selected;
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(2048, 2048, 1)]
[NumThreads(8, 8, 1)]
void DisplayGuides(DispatchNodeInputRecord<DisplayRecord> input, uint3 threadId : SV_DispatchThreadID) {
    uint2 pixel = threadId.xy;
    if (any(pixel >= displayExtent)) return;
    if (input.Get().stage == 0u) {
        float2 finalUv = (float2(pixel) + 0.5) / float2(displayExtent);
        float2 sourceUv = mapUv(inverseCurrentPost, finalUv);
        float4 selected = sourceTuple(1u, world, sourceUv, finalUv);
        if (layerCount > 1u) {
            float4 hand = sourceTuple(2u, viewmodel, sourceUv, finalUv);
            if (hand.w > 0.0 && (viewmodel.composition == 1u || hand.z > selected.z)) selected = hand;
        }
        mergedTuples[pixel] = selected;
        return;
    }
    float4 selected = sourceTuples.Load(int3(pixel, 0));
    if (dilate != 0u) {
        [unroll]
        for (int index = 0; index < 9; ++index) {
            int2 coordinate = clamp(int2(pixel) + int2(index % 3 - 1, index / 3 - 1),
                int2(0, 0), int2(displayExtent) - 1);
            float4 candidate = sourceTuples.Load(int3(coordinate, 0));
            if (candidate.z > selected.z) selected = candidate;
        }
    }
    finalDepth[pixel] = selected.z;
    bool motionValid = (uint(selected.w) & 8u) != 0u;
    if (!motionValid) guideValidity.InterlockedOr(0, 1u);
    finalMotion[pixel] = motionValid ? selected.xy : float2(0.0, 0.0);
}
)hlsl";

struct LayerConstants {
    float inverseRaster[16];
    float sourceToCommon[16];
    uint32_t extent[2];
    uint32_t flags;
    uint32_t composition;
    float jitter[2];
    float padding[2];
};

struct DisplayConstants {
    LayerConstants layers[2];
    float canonicalProjection[16];
    float inverseCurrentPost[16];
    float previousPost[16];
    uint32_t extent[2];
    uint32_t layerCount;
    uint32_t dilate;
};

struct DisplayRecord {
    uint32_t grid[3];
    uint32_t stage;
};

static_assert(sizeof(LayerConstants) == 160);
static_assert(sizeof(DisplayConstants) == 528);
static_assert(sizeof(DisplayRecord) == 16);
static_assert(sizeof(mcdr_display_layer_input) == 584);
static_assert(sizeof(mcdr_display_post_input) == 144);
static_assert(sizeof(mcdr_display_frame_input) == 672);

struct DisplaySlot {
    uint32_t width = 0;
    uint32_t height = 0;
    Dx12Object* tuples = nullptr;
    Dx12Object* depth = nullptr;
    Dx12Object* motion = nullptr;
    ComPtr<ID3D12Resource> constants;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12Resource> validity;
    ComPtr<ID3D12Resource> validityReadback;
    ~DisplaySlot() {
        if (motion) destroyObject(motion);
        if (depth) destroyObject(depth);
        if (tuples) destroyObject(tuples);
    }
};

struct DisplayLease {
    std::shared_ptr<DisplaySlot> slot;
    std::vector<std::shared_ptr<void>> sources;
};

bool sameBinding(const mcdr_prepared_binding& left, const mcdr_prepared_binding& right) {
    return left.object == right.object && left.firstSubresource == right.firstSubresource &&
        left.subresourceCount == right.subresourceCount;
}

bool sameCamera(const mcdr_raster_camera& left, const mcdr_raster_camera& right) {
    return std::equal(std::begin(left.origin), std::end(left.origin), std::begin(right.origin)) &&
        left.jitterX == right.jitterX && left.jitterY == right.jitterY &&
        std::equal(std::begin(left.projection), std::end(left.projection), std::begin(right.projection)) &&
        std::equal(std::begin(left.rasterProjection), std::end(left.rasterProjection),
            std::begin(right.rasterProjection)) &&
        std::equal(std::begin(left.viewRotation), std::end(left.viewRotation), std::begin(right.viewRotation)) &&
        left.nearPlane == right.nearPlane && left.farPlane == right.farPlane &&
        left.verticalFovRadians == right.verticalFovRadians && left.flags == right.flags &&
        left.yaw == right.yaw && left.pitch == right.pitch;
}

struct PendingDisplayFrame {
    enum class Progress { Unavailable, Recording, Submitted };
    mcdr_display_frame_input frame{};
    std::vector<mcdr_display_layer_input> layers;
    std::vector<mcdr_display_post_input> posts;
    FgPresentTicket ticket;
    uint64_t generation = 0;
    uint64_t guideFence = 0;
    Progress progress = Progress::Unavailable;
    std::shared_ptr<DisplayLease> lease;

    bool matches(const mcdr_display_frame_input& final) const {
        if (frame.schemaVersion != final.schemaVersion || frame.structSize != final.structSize ||
            frame.identity.worldEpoch != final.identity.worldEpoch ||
            frame.identity.realFrameId != final.identity.realFrameId ||
            frame.identity.configVersion != final.identity.configVersion ||
            frame.identity.resourceVersion != final.identity.resourceVersion ||
            frame.historyGeneration != final.historyGeneration || frame.previousRealFrameId != final.previousRealFrameId ||
            frame.width != final.width || frame.height != final.height || frame.flags != (final.flags & ~2u) ||
            (final.flags & ~3u) != 0 || frame.resetReasons != final.resetReasons ||
            frame.frameDeltaMs != final.frameDeltaMs || frame.viewSpaceToMeters != final.viewSpaceToMeters ||
            frame.frameGenerationBackend != final.frameGenerationBackend ||
            frame.frameGenerationMode != final.frameGenerationMode ||
            frame.frameGenerationFrames != final.frameGenerationFrames || frame.colorEncoding != final.colorEncoding ||
            !sameBinding(frame.scene, final.scene) || !sameCamera(frame.current, final.current) ||
            !sameCamera(frame.previous, final.previous) || frame.layerCount != final.layerCount ||
            frame.layerStride != final.layerStride || !final.layers || frame.postCount != final.postCount ||
            frame.postStride != final.postStride || (final.postCount != 0 && !final.posts)) return false;
        for (uint32_t index = 0; index < frame.layerCount; ++index) {
            const auto& expected = layers[index];
            const auto& actual = final.layers[index];
            if (expected.structSize != actual.structSize || expected.kind != actual.kind || expected.flags != actual.flags ||
                expected.composition != actual.composition || expected.width != actual.width || expected.height != actual.height ||
                !sameBinding(expected.depth, actual.depth) || !sameBinding(expected.motion, actual.motion) ||
                !sameBinding(expected.coverage, actual.coverage) || !sameCamera(expected.current, actual.current) ||
                !sameCamera(expected.previous, actual.previous)) return false;
        }
        for (uint32_t index = 0; index < frame.postCount; ++index) {
            const auto& expected = posts[index];
            const auto& actual = final.posts[index];
            if (expected.structSize != actual.structSize || expected.kind != actual.kind || expected.flags != actual.flags ||
                expected.reserved != actual.reserved ||
                !std::equal(std::begin(expected.current), std::end(expected.current), std::begin(actual.current)) ||
                !std::equal(std::begin(expected.previous), std::end(expected.previous), std::begin(actual.previous))) return false;
        }
        return true;
    }
};

struct DisplayOwner {
    void* graph = nullptr;
    uint64_t generation = 0;
    uint64_t recordingVersion = 0;
    mcdr_frame_identity lastSubmission{};
    std::vector<std::shared_ptr<DisplaySlot>> slots;
    std::unique_ptr<PendingDisplayFrame> pending;
    ~DisplayOwner() {
        if (graph) workgraph::destroyProgram(graph);
    }
};

struct PreparedPrograms {
    std::shared_ptr<void> expected;
    std::shared_ptr<void> replacement;
    uint64_t recordingVersion = 0;
};

int fail(const std::string& detail) {
    failureText = detail;
    return -1;
}

std::shared_ptr<DisplayOwner> createProgramOwner(CommandContext* context, uint64_t generation) {
    auto staged = std::make_shared<DisplayOwner>();
    staged->generation = generation;
    std::vector<uint8_t> dxil;
    if (auto compiled = compileHlslToDxil(kDisplayShader, "", "lib_6_9", dxil); !compiled) {
        fail(compiled.error().message);
        return {};
    }
    const workgraph::RootBinding bindings[] = {
        {0, 0, 0}, {3, 0, 0}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {3, 5, 0},
        {4, 0, 0}, {4, 1, 0}, {4, 2, 0}, {4, 3, 0}};
    const workgraph::ProgramDescription description{dxil.data(), dxil.size(), L"DisplayFrameGuides",
        L"DisplayGuides", 0, sizeof(DisplayRecord), bindings, 11, 64ull * 1024ull * 1024ull};
    auto graph = workgraph::createProgram(description);
    if (!graph) {
        fail(graph.error().message);
        return {};
    }
    staged->graph = *graph;
    const auto* current = static_cast<const DisplayOwner*>(context->displayFrameState.get());
    if (current) {
        staged->lastSubmission = current->lastSubmission;
        staged->slots = current->slots;
    }
    return staged;
}

bool invertMatrix(const float* matrix, float* inverse) {
    double rows[4][8]{};
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column) rows[row][column] = matrix[column * 4 + row];
        rows[row][row + 4] = 1.0;
    }
    for (uint32_t column = 0; column < 4; ++column) {
        uint32_t pivot = column;
        for (uint32_t row = column + 1; row < 4; ++row)
            if (std::abs(rows[row][column]) > std::abs(rows[pivot][column])) pivot = row;
        if (!std::isfinite(rows[pivot][column]) || std::abs(rows[pivot][column]) < 1e-15) return false;
        for (uint32_t item = 0; item < 8; ++item) std::swap(rows[column][item], rows[pivot][item]);
        const double divisor = rows[column][column];
        for (auto& value : rows[column]) value /= divisor;
        for (uint32_t row = 0; row < 4; ++row) {
            if (row == column) continue;
            const double factor = rows[row][column];
            for (uint32_t item = 0; item < 8; ++item) rows[row][item] -= factor * rows[column][item];
        }
    }
    for (uint32_t row = 0; row < 4; ++row)
        for (uint32_t column = 0; column < 4; ++column)
            inverse[column * 4 + row] = static_cast<float>(rows[row][column + 4]);
    return true;
}

void multiplyMatrix(const float* left, const float* right, float* output) {
    float product[16]{};
    for (uint32_t column = 0; column < 4; ++column)
        for (uint32_t row = 0; row < 4; ++row)
            for (uint32_t inner = 0; inner < 4; ++inner)
                product[column * 4 + row] += left[inner * 4 + row] * right[column * 4 + inner];
    std::copy_n(product, 16, output);
}

void identityMatrix(float* matrix) {
    std::fill_n(matrix, 16, 0.0f);
    for (uint32_t index = 0; index < 4; ++index) matrix[index * 5] = 1.0f;
}

bool validCamera(const mcdr_raster_camera& camera) {
    for (const auto value : camera.origin) if (!std::isfinite(value)) return false;
    for (const auto value : camera.projection) if (!std::isfinite(value)) return false;
    for (const auto value : camera.rasterProjection) if (!std::isfinite(value)) return false;
    for (const auto value : camera.viewRotation) if (!std::isfinite(value)) return false;
    return (camera.flags & 1u) != 0 && std::isfinite(camera.jitterX) && std::isfinite(camera.jitterY) &&
        std::isfinite(camera.nearPlane) && std::isfinite(camera.farPlane) &&
        camera.nearPlane > 0.0f && camera.farPlane > camera.nearPlane &&
        std::isfinite(camera.verticalFovRadians) && camera.verticalFovRadians > 0.0f &&
        camera.verticalFovRadians < 3.141592654f;
}

DXGI_FORMAT depthViewFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

Dx12Object* texture(const mcdr_prepared_binding& binding, uint32_t width, uint32_t height) {
    auto* object = reinterpret_cast<Dx12Object*>(binding.object);
    if (!object || object->kind != Dx12Object::Kind::Texture || !object->resource ||
        binding.firstSubresource != 0 || binding.subresourceCount != 1) return nullptr;
    const auto description = object->resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.Width == width && description.Height == height && description.DepthOrArraySize == 1 &&
        description.MipLevels == 1 && description.SampleDesc.Count == 1 &&
        !(description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ? object : nullptr;
}

DisplayOwner& ownerFor(CommandContext* context) {
    if (!context->displayFrameState) context->displayFrameState = std::make_shared<DisplayOwner>();
    return *static_cast<DisplayOwner*>(context->displayFrameState.get());
}

mcdr::Result<void> allocateSlot(DisplaySlot& slot, uint32_t width, uint32_t height) {
    const auto allocate = [&](int format, Dx12Object*& target) -> mcdr::Result<void> {
        auto created = createTexture(39, format, static_cast<int>(width), static_cast<int>(height), 1, 1);
        if (!created) return std::unexpected<mcdr::Error>(created.error());
        target = static_cast<Dx12Object*>(*created);
        return {};
    };
    if (auto result = allocate(47, slot.tuples); !result) return result;
    if (auto result = allocate(44, slot.depth); !result) return result;
    if (auto result = allocate(41, slot.motion); !result) return result;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 20;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    auto* device = deviceContext().device.Get();
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&slot.descriptors))))
        return mcdr::err("display.allocate", "Descriptor allocation failed");
    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 768;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&slot.constants))))
        return mcdr::err("display.allocate", "Constant upload allocation failed");
    buffer.Width = sizeof(uint32_t);
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES local{};
    local.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&local, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.validity))))
        return mcdr::err("display.allocate", "Guide validity allocation failed");
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    local.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device->CreateCommittedResource(&local, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.validityReadback))))
        return mcdr::err("display.allocate", "Guide validity readback allocation failed");
    const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu = slot.descriptors->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t index = 0; index < 6; ++index) {
        auto* source = index == 5 ? slot.tuples : nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        view.Format = source ? source->dxgiFormat : (index >= 3 ? DXGI_FORMAT_R32G32B32A32_FLOAT :
            (index == 1 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32_FLOAT));
        auto target = cpu;
        target.ptr += static_cast<SIZE_T>(index + 10) * increment;
        device->CreateShaderResourceView(source ? source->resource.Get() : nullptr, &view, target);
    }
    const std::array<Dx12Object*, 3> outputs{slot.tuples, slot.depth, slot.motion};
    for (uint32_t stage = 0; stage < 2; ++stage) {
        for (uint32_t index = 0; index < outputs.size(); ++index) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            view.Format = outputs[index]->dxgiFormat;
            auto target = cpu;
            target.ptr += static_cast<SIZE_T>(stage * 10 + index + 6) * increment;
            device->CreateUnorderedAccessView(stage == 1 && index == 0 ? nullptr : outputs[index]->resource.Get(),
                nullptr, &view, target);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC validityView{};
        validityView.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        validityView.Format = DXGI_FORMAT_R32_TYPELESS;
        validityView.Buffer.NumElements = 1;
        validityView.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        auto validityHandle = cpu;
        validityHandle.ptr += static_cast<SIZE_T>(stage * 10 + 9) * increment;
        device->CreateUnorderedAccessView(slot.validity.Get(), nullptr, &validityView, validityHandle);
    }
    slot.width = width;
    slot.height = height;
    return {};
}

bool layerConstants(const mcdr_display_layer_input& layer, const mcdr_raster_camera& camera,
                    LayerConstants& constants) {
    if (!invertMatrix(layer.current.rasterProjection, constants.inverseRaster)) return false;
    float sourceInverse[16];
    if (!invertMatrix(layer.current.viewRotation, sourceInverse)) return false;
    multiplyMatrix(camera.viewRotation, sourceInverse, constants.sourceToCommon);
    const double delta[3] = {layer.current.origin[0] - camera.origin[0],
        layer.current.origin[1] - camera.origin[1], layer.current.origin[2] - camera.origin[2]};
    for (uint32_t row = 0; row < 3; ++row)
        constants.sourceToCommon[12 + row] += static_cast<float>(camera.viewRotation[row] * delta[0] +
            camera.viewRotation[4 + row] * delta[1] + camera.viewRotation[8 + row] * delta[2]);
    constants.extent[0] = layer.width;
    constants.extent[1] = layer.height;
    constants.flags = layer.flags | (layer.coverage.object ? 8u : 0u);
    constants.composition = layer.composition;
    constants.jitter[0] = static_cast<float>(layer.current.jitterX);
    constants.jitter[1] = -static_cast<float>(layer.current.jitterY);
    return true;
}
}

std::string lastFailureText() { return failureText; }

int prewarm(void* opaqueContext, uint64_t generation) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->commandList || context->inRenderPass ||
        context->submissionFailed.load() || generation == 0)
        return fail("Display prewarm requires a valid context and generation outside a render pass");
    const auto* current = static_cast<const DisplayOwner*>(context->displayFrameState.get());
    if (current && current->pending &&
        (generation != current->generation || !current->graph)) {
        if (fgDisplayFrameCurrent(current->pending->ticket))
            return fail("Display reload requires the prepared frame to finish");
        if (retire(context) != 1) return -1;
    }
    if (current && generation < current->generation)
        return fail("Display prewarm generation is stale");
    if (current && generation == current->generation && current->graph) {
        failureText.clear();
        return 1;
    }
    auto staged = createProgramOwner(context, generation);
    if (!staged) return -1;
    context->displayFrameState = std::move(staged);
    failureText.clear();
    dbgLogInfo("display guides: prewarmed (generation=%llu)", static_cast<unsigned long long>(generation));
    return 1;
}

int preparePrograms(void* opaqueContext, uint64_t generation, void** prepared) {
    if (!prepared) return MCDR_FRAME_PROGRAMS_INVALID_REQUEST;
    *prepared = nullptr;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (generation == 0) return MCDR_FRAME_PROGRAMS_INVALID_REQUEST;
    const auto* current = static_cast<const DisplayOwner*>(context->displayFrameState.get());
    if (current && current->pending) return MCDR_FRAME_PROGRAMS_BUSY;
    if (current && generation <= current->generation) return MCDR_FRAME_PROGRAMS_STALE;
    auto candidate = std::make_unique<PreparedPrograms>();
    candidate->expected = context->displayFrameState;
    candidate->recordingVersion = current ? current->recordingVersion : 0;
    candidate->replacement = createProgramOwner(context, generation);
    if (!candidate->replacement) return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
    *prepared = candidate.release();
    return MCDR_FRAME_PROGRAMS_SUCCESS;
}

int validatePreparedPrograms(void* opaqueContext, void* opaquePrepared) {
    if (!opaquePrepared) return MCDR_FRAME_PROGRAMS_INVALID_REQUEST;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    const auto* prepared = static_cast<const PreparedPrograms*>(opaquePrepared);
    if (context->displayFrameState != prepared->expected) return MCDR_FRAME_PROGRAMS_STALE;
    const auto* current = static_cast<const DisplayOwner*>(context->displayFrameState.get());
    if (current && current->pending) return MCDR_FRAME_PROGRAMS_BUSY;
    if (current && current->recordingVersion != prepared->recordingVersion)
        return MCDR_FRAME_PROGRAMS_STALE;
    return MCDR_FRAME_PROGRAMS_SUCCESS;
}

void exchangePreparedPrograms(void* opaqueContext, void* opaquePrepared) noexcept {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* prepared = static_cast<PreparedPrograms*>(opaquePrepared);
    context->displayFrameState.swap(prepared->replacement);
}

void destroyPreparedPrograms(void* opaquePrepared) noexcept {
    const std::unique_ptr<PreparedPrograms> prepared(static_cast<PreparedPrograms*>(opaquePrepared));
}

int prepare(void* opaqueContext, const void* opaqueInput, uint32_t bytes) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->listOpen || !context->commandList || context->inRenderPass ||
        context->submissionFailed.load() || !opaqueInput || bytes != sizeof(mcdr_display_frame_input))
        return fail("Display frame requires an active command recording outside a render pass");
    const auto input = *static_cast<const mcdr_display_frame_input*>(opaqueInput);
    if (input.schemaVersion != MCDR_DISPLAY_FRAME_SCHEMA_VERSION || input.structSize != sizeof(input) ||
        input.identity.worldEpoch == 0 || input.identity.realFrameId == 0 || input.identity.configVersion == 0 ||
        input.identity.resourceVersion == 0 || input.width == 0 || input.height == 0 ||
        input.width > 16384 || input.height > 16384 || input.layerCount == 0 || input.layerCount > 2 ||
        input.layerStride != sizeof(mcdr_display_layer_input) || !input.layers || input.postCount > 64 ||
        (input.postCount && (input.postStride != sizeof(mcdr_display_post_input) || !input.posts)) ||
        (input.flags & ~1u) != 0 || input.ui.object != nullptr ||
        input.ui.firstSubresource != 0 || input.ui.subresourceCount != 0 || !validCamera(input.current) ||
        ((input.flags & 1u) && (!validCamera(input.previous) || input.previousRealFrameId == 0)) ||
        input.frameGenerationBackend > 3 || input.frameGenerationMode > 2 || input.frameGenerationFrames > 5 ||
        !std::isfinite(input.frameDeltaMs) || input.frameDeltaMs < 0 ||
        !std::isfinite(input.viewSpaceToMeters) || input.viewSpaceToMeters <= 0 || input.colorEncoding != 1)
        return fail("Invalid display schema, identity, camera or source descriptors");
    auto& owner = ownerFor(context);
    if (owner.recordingVersion == UINT64_MAX) return fail("Display recording version is exhausted");
    ++owner.recordingVersion;
    if (owner.pending) {
        if (fgDisplayFrameCurrent(owner.pending->ticket))
            return fail("Display frame preparation is already pending");
        if (retire(context) != 1) return -1;
    }
    if (owner.lastSubmission.worldEpoch == input.identity.worldEpoch &&
        owner.lastSubmission.realFrameId == input.identity.realFrameId)
        return fail("Duplicate display submission for the same real frame");
    std::vector<mcdr_display_layer_input> layers(input.layers, input.layers + input.layerCount);
    std::vector<mcdr_display_post_input> posts;
    if (input.postCount) posts.assign(input.posts, input.posts + input.postCount);
    auto* scene = texture(input.scene, input.width, input.height);
    if (!scene) return fail("Display scene extent or range is invalid");
    DisplayConstants constants{};
    constants.extent[0] = input.width;
    constants.extent[1] = input.height;
    constants.layerCount = input.layerCount;
    constants.dilate = input.frameGenerationBackend == MCDR_FG_BACKEND_XESSFG ? 1u : 0u;
    std::copy_n(input.current.projection, 16, constants.canonicalProjection);
    identityMatrix(constants.inverseCurrentPost);
    identityMatrix(constants.previousPost);
    float currentPost[16];
    identityMatrix(currentPost);
    bool inputsAvailable = (input.flags & 3u) == 1u && input.frameDeltaMs > 0;
    for (const auto& post : posts) {
        if (post.structSize != sizeof(post) || post.kind > 2 || post.flags != 0 || post.reserved != 0)
            return fail("Post effect descriptor is invalid");
        for (const auto value : post.current) if (!std::isfinite(value)) return fail("Invalid current post mapping");
        for (const auto value : post.previous) if (!std::isfinite(value)) return fail("Invalid previous post mapping");
        if (post.kind == 1) {
            multiplyMatrix(post.current, currentPost, currentPost);
            multiplyMatrix(post.previous, constants.previousPost, constants.previousPost);
        }
        if (post.kind == 2) inputsAvailable = false;
    }
    if (!invertMatrix(currentPost, constants.inverseCurrentPost)) inputsAvailable = false;
    std::array<Dx12Object*, 6> sources{};
    for (uint32_t index = 0; index < input.layerCount; ++index) {
        const auto& layer = layers[index];
        if (layer.structSize != sizeof(layer) || layer.kind != index + 1 || (layer.flags & ~7u) != 0 ||
            layer.composition > 1 || layer.width == 0 || layer.height == 0 ||
            layer.width > 16384 || layer.height > 16384 ||
            ((layer.flags & 2u) != 0 && (layer.flags & 1u) == 0) ||
            !validCamera(layer.current) || ((layer.flags & 1u) && !validCamera(layer.previous)) ||
            !layerConstants(layer, input.current, constants.layers[index]))
            return fail("Display layer camera, ordering or motion declaration is invalid");
        auto* depth = texture(layer.depth, layer.width, layer.height);
        auto* motion = layer.motion.object ? texture(layer.motion, layer.width, layer.height) : nullptr;
        auto* coverage = layer.coverage.object ? texture(layer.coverage, layer.width, layer.height) : nullptr;
        if (!depth || depthViewFormat(depth->dxgiFormat) == DXGI_FORMAT_UNKNOWN ||
            ((layer.flags & 2u) != 0 && !motion) || (layer.motion.object && !motion) ||
            (motion && motion->dxgiFormat != DXGI_FORMAT_R16G16_FLOAT && motion->dxgiFormat != DXGI_FORMAT_R32G32_FLOAT &&
             motion->dxgiFormat != DXGI_FORMAT_R32G32B32A32_FLOAT && motion->dxgiFormat != DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            (layer.coverage.object && (!coverage || coverage->dxgiFormat != DXGI_FORMAT_R8_UNORM)) ||
            (index == 1 && !coverage))
            return fail("Display layer depth/motion/coverage resources are incompatible");
        if ((layer.flags & 3u) != 3u) inputsAvailable = false;
        if (index == 0) { sources[0] = depth; sources[1] = motion; sources[4] = coverage; }
        else { sources[2] = depth; sources[3] = motion; sources[5] = coverage; }
    }
    const int prepared = checkDisplayFramePreparation(context, &input, sizeof(input));
    if (prepared < 0) return fail("Display frame does not match the active GUI frame");
    auto pending = std::make_unique<PendingDisplayFrame>();
    pending->frame = input;
    pending->layers = std::move(layers);
    pending->posts = std::move(posts);
    pending->frame.layers = pending->layers.data();
    pending->frame.posts = pending->posts.empty() ? nullptr : pending->posts.data();
    pending->ticket = fgDisplayFrameTicket(context);
    pending->generation = owner.generation;
    auto lease = std::make_shared<DisplayLease>();
    for (auto* source : {scene, sources[0], sources[1], sources[2], sources[3], sources[4], sources[5]}) {
        if (!source) continue;
        auto retained = retainObjectLease(source);
        if (!retained) return fail("Display source lease could not be retained");
        lease->sources.push_back(std::move(retained));
    }
    pending->lease = lease;
    if (prepared == 0 || !inputsAvailable) {
        owner.pending = std::move(pending);
        failureText.clear();
        return 0;
    }
    if (!owner.graph || owner.generation == 0)
        return fail("Display guides must be prewarmed before submission");
    std::erase_if(owner.slots, [&](const auto& candidate) {
        return candidate.use_count() == 1 && (candidate->width != input.width || candidate->height != input.height);
    });
    std::shared_ptr<DisplaySlot> slot;
    for (const auto& candidate : owner.slots)
        if (candidate.use_count() == 1 && candidate->width == input.width && candidate->height == input.height) {
            slot = candidate;
            break;
        }
    if (!slot) {
        slot = std::make_shared<DisplaySlot>();
        if (auto allocated = allocateSlot(*slot, input.width, input.height); !allocated)
            return fail(allocated.error().message);
        owner.slots.push_back(slot);
    }
    lease->slot = slot;
    void* mapped = nullptr;
    D3D12_RANGE readRange{};
    if (FAILED(slot->constants->Map(0, &readRange, &mapped))) return fail("Display constants mapping failed");
    std::memcpy(mapped, &constants, sizeof(constants));
    const uint32_t emptyValidity = 0;
    std::memcpy(static_cast<uint8_t*>(mapped) + sizeof(constants), &emptyValidity, sizeof(emptyValidity));
    const D3D12_RANGE written{0, sizeof(constants) + sizeof(emptyValidity)};
    slot->constants->Unmap(0, &written);
    pending->progress = PendingDisplayFrame::Progress::Recording;
    owner.pending = std::move(pending);
    for (auto* source : {scene, sources[0], sources[1], sources[2], sources[3], sources[4], sources[5]})
        if (source) retainObjectForRecording(context, source);
    context->recordingLeases[context->currentListSlot].push_back(lease);
    auto* device = deviceContext().device.Get();
    const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu = slot->descriptors->GetCPUDescriptorHandleForHeapStart();
    const auto gpu = slot->descriptors->GetGPUDescriptorHandleForHeapStart();
    for (uint32_t index = 0; index < 6; ++index) {
        auto* source = sources[index];
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        view.Format = !source ? (index >= 3 ? DXGI_FORMAT_R32G32B32A32_FLOAT :
            (index == 1 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32_FLOAT)) :
            ((index == 0 || index == 2) ? depthViewFormat(source->dxgiFormat) : source->dxgiFormat);
        auto target = cpu;
        target.ptr += static_cast<SIZE_T>(index) * increment;
        device->CreateShaderResourceView(source ? source->resource.Get() : nullptr, &view, target);
    }
    for (auto* output : {slot->tuples, slot->depth, slot->motion}) {
        retainObjectForRecording(context, output);
        transitionTextureTo(context, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    D3D12_RESOURCE_BARRIER validityBarrier{};
    validityBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    validityBarrier.Transition.pResource = slot->validity.Get();
    validityBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    validityBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    validityBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    context->commandList->ResourceBarrier(1, &validityBarrier);
    context->commandList->CopyBufferRegion(slot->validity.Get(), 0, slot->constants.Get(),
        sizeof(constants), sizeof(uint32_t));
    validityBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    validityBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    context->commandList->ResourceBarrier(1, &validityBarrier);
    for (auto* source : sources) if (source)
        transitionTextureTo(context, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = {slot->descriptors.Get()};
    context->commandList->SetDescriptorHeaps(1, heaps);
    std::array<uint64_t, 11> roots{};
    roots[0] = slot->constants->GetGPUVirtualAddress();
    for (uint32_t index = 0; index < 10; ++index) roots[index + 1] = gpu.ptr + static_cast<uint64_t>(index) * increment;
    alignas(16) DisplayRecord record{{(input.width + 7) / 8, (input.height + 7) / 8, 1}, 0};
    auto merged = workgraph::dispatch(context, owner.graph, &record, 1, sizeof(record), roots.data(), 11);
    if (!merged) { restoreDrawHeap(context); return fail(merged.error().message); }
    transitionTextureTo(context, slot->tuples, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    record.stage = 1;
    for (uint32_t index = 0; index < 10; ++index)
        roots[index + 1] = gpu.ptr + static_cast<uint64_t>(index + 10) * increment;
    auto resolved = workgraph::dispatch(context, owner.graph, &record, 1, sizeof(record), roots.data(), 11);
    restoreDrawHeap(context);
    if (!resolved) return fail(resolved.error().message);
    validityBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    validityBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    context->commandList->ResourceBarrier(1, &validityBarrier);
    context->commandList->CopyBufferRegion(slot->validityReadback.Get(), 0, slot->validity.Get(), 0, sizeof(uint32_t));
    validityBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    validityBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    context->commandList->ResourceBarrier(1, &validityBarrier);
    if (submitDisplayGuidesAndResume(context, &owner.pending->guideFence) != 1)
        return fail("Display guide submission or recording resume failed");
    owner.pending->progress = PendingDisplayFrame::Progress::Submitted;
    context->recordingLeases[context->currentListSlot].push_back(lease);
    failureText.clear();
    return 1;
}

int submit(void* opaqueContext, const void* opaqueInput, uint32_t bytes) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->listOpen || !context->commandList || context->inRenderPass ||
        context->submissionFailed.load() || !opaqueInput || bytes != sizeof(mcdr_display_frame_input))
        return fail("Display finalization requires an active command recording");
    const auto input = *static_cast<const mcdr_display_frame_input*>(opaqueInput);
    auto* owner = static_cast<DisplayOwner*>(context->displayFrameState.get());
    if (!owner || !owner->pending) return fail("Display finalization has no prepared frame");
    auto& pending = *owner->pending;
    if (pending.generation != owner->generation || !pending.matches(input) ||
        pending.ticket != fgDisplayFrameTicket(context))
        return fail("Display finalization differs from the prepared frame");
    auto* scene = texture(input.scene, input.width, input.height);
    auto* ui = texture(input.ui, input.width, input.height);
    if (!scene || !ui || ui->resource == scene->resource)
        return fail("Display final scene/UI resources are incompatible");
    if (pending.progress == PendingDisplayFrame::Progress::Recording)
        return fail("Display guide recording did not complete");
    if (pending.progress == PendingDisplayFrame::Progress::Unavailable || (input.flags & 2u) != 0) {
        const int skipped = skipDisplayFrameGeneration(context, &input, sizeof(input));
        if (skipped < 0) return fail("Display frame could not invalidate unavailable inputs");
        owner->lastSubmission = input.identity;
        owner->pending.reset();
        failureText.clear();
        return 0;
    }
    if (pending.guideFence != 0) {
        const auto waited = waitForFenceValue(context, pending.guideFence, 5'000'000'000ULL);
        if (!waited) {
            context->submissionFailed = true;
            return fail("Display guide completion is unknown: " + waited.error().message);
        }
    }
    if (pending.progress != PendingDisplayFrame::Progress::Submitted || pending.guideFence == 0 ||
        !pending.lease || !pending.lease->slot)
        return fail("Display guide submission has no retained result");
    auto lease = pending.lease;
    auto slot = lease->slot;
    void* mapped = nullptr;
    const D3D12_RANGE validityRange{0, sizeof(uint32_t)};
    if (FAILED(slot->validityReadback->Map(0, &validityRange, &mapped)))
        return fail("Display guide validity mapping failed");
    uint32_t validity = 0;
    std::memcpy(&validity, mapped, sizeof(validity));
    const D3D12_RANGE noWrite{};
    slot->validityReadback->Unmap(0, &noWrite);
    if ((validity & ~3u) != 0) return fail("Display validity contains an undefined value");
    if (validity != 0) {
        const int skipped = skipDisplayFrameGeneration(context, &input, sizeof(input));
        if (skipped < 0) return fail("Display frame could not invalidate incomplete pixel motion");
        owner->lastSubmission = input.identity;
        owner->pending.reset();
        failureText.clear();
        return 0;
    }
    transitionTextureTo(context, slot->depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transitionTextureTo(context, slot->motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transitionTextureTo(context, scene, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto uiLease = retainObjectLease(ui);
    if (!uiLease) return fail("Display UI lease could not be retained");
    lease->sources.push_back(std::move(uiLease));
    retainObjectForRecording(context, ui);
    context->recordingLeases[context->currentListSlot].push_back(lease);
    transitionTextureTo(context, ui, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FgFrameInputs guides{};
    guides.cmdList = context->commandList;
    guides.pColorOutput = scene->resource.Get();
    guides.colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    guides.pDepth = slot->depth->resource.Get();
    guides.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    guides.pMotionVectors = slot->motion->resource.Get();
    guides.motionVectorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    guides.pUi = ui->resource.Get();
    guides.uiState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    guides.renderWidth = guides.displayWidth = static_cast<int32_t>(input.width);
    guides.renderHeight = guides.displayHeight = static_cast<int32_t>(input.height);
    guides.realFrameId = input.identity.realFrameId;
    guides.motionVectorsDilated = input.frameGenerationBackend == MCDR_FG_BACKEND_XESSFG;
    guides.resourceLease = std::move(lease);
    const int result = submitDisplayFrameGeneration(context, &input, sizeof(input), guides);
    if (result < 0) return fail("Selected frame-generation adapter rejected the late display frame");
    owner->lastSubmission = input.identity;
    owner->pending.reset();
    failureText.clear();
    return result;
}

int retire(void* opaqueContext) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context) return fail("Display retirement has no command context");
    auto* owner = static_cast<DisplayOwner*>(context->displayFrameState.get());
    if (!owner || !owner->pending) return 1;
    if (context->submissionFailed.load()) return fail("Display retirement retains unknown GPU work");
    const auto fenceValue = owner->pending->guideFence;
    if (fenceValue != 0) {
        const auto completed = context->fence ? context->fence->GetCompletedValue() : UINT64_MAX;
        if (completed == UINT64_MAX || completed < fenceValue)
            return fail("Display retirement is waiting for guide completion");
    }
    owner->pending.reset();
    failureText.clear();
    return 1;
}
}
