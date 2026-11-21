module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include <utility>

module mcdr.render.frameprep;

import mcdr.d3d12.device;
import mcdr.d3d12.workgraph;
import mcdr.error;

namespace d3d12_mc::frameprep {
namespace {

namespace workgraph = mcdr::workgraph;

constexpr uint32_t kEntryRecordSize = 112;
constexpr uint64_t kBackingInstanceBudget = 64ull * 1024ull * 1024ull;
constexpr DXGI_FORMAT kCoefficientFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kMaskFormat = DXGI_FORMAT_R8_UNORM;
constexpr DXGI_FORMAT kVrsFormat = DXGI_FORMAT_R8_UINT;
static_assert(sizeof(mcdr_provider_mask_input) == 200);
static_assert(offsetof(mcdr_provider_mask_input, currentToPrevious) == 136);

struct VrsEntryRecord {
    uint32_t grid[3];
    uint32_t width;
    uint32_t height;
    uint32_t tileSize;
    uint32_t edgePermille;
    uint32_t motionPixels;
    uint32_t flags;
    float previousToCurrent[16];
    float currentToPrevious[16];
};

static_assert(sizeof(VrsEntryRecord) == 164);

constexpr char kShader[] = R"hlsl(
Texture2D<float4> coefficient0 : register(t0);
Texture2D<float4> coefficient1 : register(t1);
Texture2D<float4> frameColor : register(t2);
Texture2D<float> frameDepth : register(t3);
Texture2D<float2> frameMotion : register(t4);
Texture2D<float4> historyColor : register(t5);
Texture2D<float> historyDepth : register(t6);
RWTexture2D<float> maskOutput : register(u0);
RWTexture2D<float> compositionOutput : register(u1);
RWTexture2D<float> responsiveOutput : register(u2);

struct FramePrepEntryRecord {
    uint3 grid : SV_DispatchGrid;
    uint width;
    uint height;
    uint tilesX;
    uint tilesY;
    uint outputFlags;
    uint inputFlags;
    float2 historyJitter;
    column_major float4x4 currentToPrevious;
    uint reserved;
};

struct TileRecord {
    uint3 grid : SV_DispatchGrid;
    uint2 origin;
    uint width;
    uint height;
    uint outputFlags;
    uint inputFlags;
    float2 historyJitter;
    column_major float4x4 currentToPrevious;
    uint reserved;
};

float sampleAbsorbance(float coefficients[8]) {
    float averageAbsorbance = coefficients[0];
    if (averageAbsorbance == 0.0) return 0.0;

    int treeIndexB = 7;
    bool shouldSampleA = treeIndexB >= 1;
    int treeIndexA = treeIndexB - 1;
    treeIndexB += 8;
    treeIndexA += 8;
    float sampleB = averageAbsorbance;
    float sampleA = averageAbsorbance;

    for (int waveletLevel = 0; waveletLevel <= 2; ++waveletLevel) {
        int power = 2 - waveletLevel;
        float waveletWidth = exp2(-(float)power);
        float waveletHeightScale = exp2((float)power * 0.5);
        int parentIndexB = treeIndexB >> 1;
        int isRightHalfB = treeIndexB & 1;
        int waveletSignB = 1 - (isRightHalfB << 1);
        float differenceCoefficientB = coefficients[parentIndexB];
        sampleB -= waveletHeightScale * differenceCoefficientB * waveletSignB;
        treeIndexB = parentIndexB;
        if (shouldSampleA) {
            int parentIndexA = treeIndexA >> 1;
            float differenceCoefficientA = parentIndexA == parentIndexB
                ? differenceCoefficientB : coefficients[parentIndexA];
            int isRightHalfA = treeIndexA & 1;
            int waveletSignA = 1 - (isRightHalfA << 1);
            sampleA -= waveletHeightScale * differenceCoefficientA * waveletSignA;
            treeIndexA = parentIndexA;
        }
    }
    return max(sampleA, 0.0);
}

float3 classification(uint2 pixel, TileRecord tile) {
    float opacity = 0.0;
    if ((tile.inputFlags & 2u) != 0u) {
        float4 firstCoefficients = coefficient0.Load(int3(pixel, 0));
        float4 secondCoefficients = coefficient1.Load(int3(pixel, 0));
        float coefficients[8] = {firstCoefficients.x, firstCoefficients.y, firstCoefficients.z, firstCoefficients.w,
            secondCoefficients.x, secondCoefficients.y, secondCoefficients.z, secondCoefficients.w};
        opacity = saturate(1.0 - clamp(exp(-sampleAbsorbance(coefficients)), 0.0001, 1.0));
    }
    if ((tile.inputFlags & 1u) == 0u) return float3(opacity, 0.0, 0.0);
    float depth = frameDepth.Load(int3(pixel, 0));
    float2 motion = frameMotion.Load(int3(pixel, 0));
    if (!isfinite(depth) || !all(isfinite(motion))) return float3(opacity, 0.0, 1.0);
    float unavailable = depth > 0.0 ? 1.0 : 0.0;
    if ((tile.inputFlags & 4u) == 0u) return float3(opacity, 0.0, unavailable);
    float2 previousPixel = float2(pixel) + motion + tile.historyJitter;
    bool inside = all(previousPixel >= 0.0) && all(previousPixel < float2(tile.width, tile.height));
    if (!inside) return float3(opacity, 0.0, unavailable);
    int2 previous = int2(floor(previousPixel + 0.5));
    previous = clamp(previous, int2(0, 0), int2(tile.width - 1, tile.height - 1));
    float previousDepth = historyDepth.Load(int3(previous, 0));
    float2 uv = (float2(pixel) + 0.5) / float2(tile.width, tile.height);
    float4 projected = mul(tile.currentToPrevious, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    if (!all(isfinite(projected)) || projected.w <= 0.0) return float3(opacity, 0.0, unavailable);
    float expectedDepth = projected.z / projected.w;
    bool compatible = isfinite(previousDepth) &&
        abs(previousDepth - expectedDepth) <= max(0.0005, abs(expectedDepth) * 0.02);
    if (!compatible) return float3(opacity, 0.0, unavailable);
    float3 currentColor = frameColor.Load(int3(pixel, 0)).rgb;
    float3 previousColor = historyColor.Load(int3(previous, 0)).rgb;
    if (!all(isfinite(currentColor)) || !all(isfinite(previousColor))) return float3(opacity, 0.0, 1.0);
    float3 residual = abs(currentColor - previousColor);
    float scale = max(0.1, max(max(currentColor.r, currentColor.g), currentColor.b));
    float colorChange = saturate((max(max(residual.r, residual.g), residual.b) / scale - 0.05) * 2.0);
    return float3(opacity, colorChange, 0.0);
}

bool occupied(TileRecord tile) {
    [loop]
    for (uint local = 0; local < 64; ++local) {
        uint2 pixel = tile.origin + uint2(local & 7u, local >> 3u);
        if (pixel.x >= tile.width || pixel.y >= tile.height) continue;
        if (any(classification(pixel, tile) > 0.0)) return true;
    }
    return false;
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(65535, 1, 1)]
[NumThreads(64, 1, 1)]
void TileClassify(DispatchNodeInputRecord<FramePrepEntryRecord> input,
    uint dispatchThread : SV_DispatchThreadID,
    [MaxRecords(64)] [NodeID("TileReconstruct")] NodeOutput<TileRecord> reconstruct,
    [MaxRecordsSharedWith(reconstruct)] [NodeID("TileEmpty")] NodeOutput<TileRecord> empty) {
    FramePrepEntryRecord entry = input.Get();
    uint tileCount = entry.tilesX * entry.tilesY;
    bool valid = dispatchThread < tileCount;
    uint2 tile = valid ? uint2(dispatchThread % entry.tilesX, dispatchThread / entry.tilesX) : 0;
    TileRecord record;
    record.grid = uint3(1u, 1u, 1u);
    record.origin = tile * 8u;
    record.width = entry.width;
    record.height = entry.height;
    record.outputFlags = entry.outputFlags;
    record.inputFlags = entry.inputFlags;
    record.historyJitter = entry.historyJitter;
    record.currentToPrevious = entry.currentToPrevious;
    record.reserved = 0;
    bool hasContent = valid && occupied(record);
    ThreadNodeOutputRecords<TileRecord> reconstructRecords =
        reconstruct.GetThreadNodeOutputRecords(hasContent ? 1 : 0);
    ThreadNodeOutputRecords<TileRecord> emptyRecords =
        empty.GetThreadNodeOutputRecords(valid && !hasContent ? 1 : 0);
    if (hasContent) reconstructRecords.Get() = record;
    if (valid && !hasContent) emptyRecords.Get() = record;
    reconstructRecords.OutputComplete();
    emptyRecords.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NumThreads(64, 1, 1)]
[NodeMaxDispatchGrid(1, 1, 1)]
void TileReconstruct(DispatchNodeInputRecord<TileRecord> input,
    uint local : SV_DispatchThreadID) {
    TileRecord tile = input.Get();
    uint2 p = tile.origin + uint2(local & 7u, local >> 3u);
    if (p.x >= tile.width || p.y >= tile.height) return;
    float3 facts = classification(p, tile);
    if ((tile.outputFlags & 1u) != 0u) maskOutput[p] = max(facts.x * 0.9, facts.y);
    if ((tile.outputFlags & 2u) != 0u) compositionOutput[p] = facts.x;
    if ((tile.outputFlags & 4u) != 0u) responsiveOutput[p] = max(max(facts.x * 0.9, facts.y), facts.z);
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NumThreads(64, 1, 1)]
[NodeMaxDispatchGrid(1, 1, 1)]
void TileEmpty(DispatchNodeInputRecord<TileRecord> input,
    uint local : SV_DispatchThreadID) {
    TileRecord tile = input.Get();
    uint2 p = tile.origin + uint2(local & 7u, local >> 3u);
    if (p.x < tile.width && p.y < tile.height) {
        if ((tile.outputFlags & 1u) != 0u) maskOutput[p] = 0.0;
        if ((tile.outputFlags & 2u) != 0u) compositionOutput[p] = 0.0;
        if ((tile.outputFlags & 4u) != 0u) responsiveOutput[p] = 0.0;
    }
}
)hlsl";

constexpr char kVrsShader[] = R"hlsl(
Texture2D<float> depthInput : register(t0);
Texture2D<float2> motionInput : register(t1);
Texture2D<uint> previousInput : register(t2);
RWTexture2D<uint> vrsOutput : register(u0);
RWTexture2D<uint> candidateOutput : register(u1);

struct VrsEntryRecord {
    uint3 grid : SV_DispatchGrid;
    uint width;
    uint height;
    uint tileSize;
    uint edgePermille;
    uint motionPixels;
    uint flags;
    float previousToCurrent[16];
    float currentToPrevious[16];
};

float4 transformClip(float coefficients[16], float4 clipPoint) {
    return float4(coefficients[0], coefficients[1], coefficients[2], coefficients[3]) * clipPoint.x +
        float4(coefficients[4], coefficients[5], coefficients[6], coefficients[7]) * clipPoint.y +
        float4(coefficients[8], coefficients[9], coefficients[10], coefficients[11]) * clipPoint.z +
        float4(coefficients[12], coefficients[13], coefficients[14], coefficients[15]) * clipPoint.w;
}

bool projectUv(float4 clip, out float2 uv, out float depth) {
    uv = 0.0;
    depth = 0.0;
    if (!all(isfinite(clip)) || clip.w <= 0.0) return false;
    float3 ndc = clip.xyz / clip.w;
    uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    depth = ndc.z;
    return all(isfinite(ndc)) && depth >= 0.0 && depth <= 1.0 &&
        all(uv >= 0.0) && all(uv <= 1.0);
}

bool depthFootprint(VrsEntryRecord entry, float2 uv, out float depth, out int2 base) {
    depth = 0.0;
    base = int2(0, 0);
    float2 pixel = uv * float2(entry.width, entry.height) - 0.5;
    if (!all(isfinite(pixel)) || any(pixel < 0.0) ||
        any(pixel >= float2(entry.width - 1u, entry.height - 1u))) return false;
    base = int2(floor(pixel));
    float4 depths = float4(depthInput.Load(int3(base, 0)),
        depthInput.Load(int3(base + int2(1, 0), 0)),
        depthInput.Load(int3(base + int2(0, 1), 0)),
        depthInput.Load(int3(base + int2(1, 1), 0)));
    if (!all(isfinite(depths)) || any(depths <= 1.0e-5) || any(depths >= 0.99999)) return false;
    float minimum = min(min(depths.x, depths.y), min(depths.z, depths.w));
    float maximum = max(max(depths.x, depths.y), max(depths.z, depths.w));
    if ((maximum - minimum) / max(minimum, 1.0e-4) > (float)entry.edgePermille * 0.001) return false;
    float2 fraction = frac(pixel);
    depth = lerp(lerp(depths.x, depths.y, fraction.x), lerp(depths.z, depths.w, fraction.x), fraction.y);
    return true;
}

bool reprojectSample(VrsEntryRecord entry, float2 targetUv, out float currentDepth, out bool priorCoarse) {
    currentDepth = 0.0;
    priorCoarse = false;
    if (any(targetUv < 0.0) || any(targetUv > 1.0)) return false;
    float2 targetNdc = float2(targetUv.x * 2.0 - 1.0, 1.0 - targetUv.y * 2.0);
    float4 initial = transformClip(entry.currentToPrevious, float4(targetNdc, 0.0, 1.0));
    if (!all(isfinite(initial)) || initial.w <= 0.0) return false;
    float2 sourceUv = float2(initial.x / initial.w * 0.5 + 0.5, 0.5 - initial.y / initial.w * 0.5);
    int2 footprint = int2(0, 0);
    bool converged = false;
    [loop]
    for (uint iteration = 0u; iteration < 4u; ++iteration) {
        float sourceDepth;
        if (!depthFootprint(entry, sourceUv, sourceDepth, footprint)) return false;
        float2 sourceNdc = float2(sourceUv.x * 2.0 - 1.0, 1.0 - sourceUv.y * 2.0);
        float2 projectedUv;
        if (!projectUv(transformClip(entry.previousToCurrent, float4(sourceNdc, sourceDepth, 1.0)),
            projectedUv, currentDepth)) return false;
        float2 residual = targetUv - projectedUv;
        if (length(residual * float2(entry.width, entry.height)) <= 0.75) {
            converged = true;
            break;
        }
        sourceUv += residual;
    }
    if (!converged) return false;
    priorCoarse = (entry.flags & 8u) != 0u;
    [unroll]
    for (uint corner = 0u; corner < 4u; ++corner) {
        int2 sourcePixel = footprint + int2(corner & 1u, corner >> 1u);
        if ((entry.flags & 4u) != 0u) {
            float2 motion = motionInput.Load(int3(sourcePixel, 0));
            if (!all(isfinite(motion)) || any(abs(motion) > (float)entry.motionPixels)) return false;
        }
        if ((entry.flags & 8u) != 0u) {
            uint2 previousTile = uint2(sourcePixel) / entry.tileSize;
            priorCoarse = priorCoarse && previousInput.Load(int3(previousTile, 0)) == 5u;
        }
    }
    return true;
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(65535, 1, 1)]
[NumThreads(64, 1, 1)]
void VrsClassify(DispatchNodeInputRecord<VrsEntryRecord> input,
    uint dispatchThread : SV_DispatchThreadID) {
    VrsEntryRecord entry = input.Get();
    uint tilesX = (entry.width + entry.tileSize - 1u) / entry.tileSize;
    uint tilesY = (entry.height + entry.tileSize - 1u) / entry.tileSize;
    uint tileCount = tilesX * tilesY;
    if (dispatchThread >= tileCount) return;
    uint2 tile = uint2(dispatchThread % tilesX, dispatchThread / tilesX);
    uint2 origin = tile * entry.tileSize;
    uint2 end = min(uint2(entry.width - 1u, entry.height - 1u),
                    origin + uint2(entry.tileSize - 1u, entry.tileSize - 1u));
    bool candidateCoarse = false;
    bool priorCoarse = (entry.flags & 8u) != 0u;
    if ((entry.flags & 1u) != 0u) {
        candidateCoarse = true;
        float minimumDepth = 1.0;
        float maximumDepth = 0.0;
        [loop]
        for (uint sampleIndex = 0u; sampleIndex < 9u; ++sampleIndex) {
            float2 fraction = float2(sampleIndex % 3u, sampleIndex / 3u) * 0.5;
            float2 targetPixel = lerp(float2(origin) - 0.5, float2(end) + 1.5, fraction);
            float currentDepth;
            bool samplePriorCoarse;
            if (!reprojectSample(entry, targetPixel / float2(entry.width, entry.height),
                currentDepth, samplePriorCoarse)) {
                candidateCoarse = false;
                break;
            }
            minimumDepth = min(minimumDepth, currentDepth);
            maximumDepth = max(maximumDepth, currentDepth);
            priorCoarse = priorCoarse && samplePriorCoarse;
        }
        candidateCoarse = candidateCoarse && (maximumDepth - minimumDepth) / max(minimumDepth, 1.0e-4) <=
            (float)entry.edgePermille * 0.001;
    }
    candidateOutput[tile] = candidateCoarse ? 5u : 0u;
    vrsOutput[tile] = candidateCoarse && ((entry.flags & 2u) == 0u || priorCoarse) ? 5u : 0u;
}
)hlsl";

constexpr char kExposureShader[] = R"hlsl(
Texture2D<float4> sourceColor : register(t0);
RWStructuredBuffer<float2> tileOutput : register(u0);
StructuredBuffer<float2> tileInput : register(t1);
RWTexture2D<float> exposureOutput : register(u1);
cbuffer ExposureConstants : register(b0) {
    uint width; uint height; uint tilesX; uint tileCount;
    uint colorEncoding; float preExposure;
};
groupshared float2 reduction[256];
float linearChannel(float value) {
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}
[numthreads(16, 16, 1)]
void MeterTiles(uint3 pixel : SV_DispatchThreadID, uint3 group : SV_GroupID,
    uint local : SV_GroupIndex) {
    float2 sampleValue = 0.0;
    if (pixel.x < width && pixel.y < height) {
        float3 color = sourceColor.Load(int3(pixel.xy, 0)).rgb;
        color = all(isfinite(color)) ? max(color, 0.0) : 0.0;
        if ((colorEncoding & 1u) != 0) color = float3(linearChannel(color.r), linearChannel(color.g), linearChannel(color.b));
        float luminance = dot(color / preExposure, float3(0.2126, 0.7152, 0.0722));
        sampleValue = float2(log2(clamp(luminance, 0.000001, 65504.0)), 1.0);
    }
    reduction[local] = sampleValue;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (local < stride) reduction[local] += reduction[local + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (local == 0) tileOutput[group.y * tilesX + group.x] = reduction[0];
}
[numthreads(256, 1, 1)]
void ReduceExposure(uint local : SV_GroupIndex) {
    if ((colorEncoding & 2u) != 0) {
        if (local == 0) exposureOutput[uint2(0, 0)] = 1.0;
        return;
    }
    float2 total = 0.0;
    for (uint index = local; index < tileCount; index += 256) total += tileInput[index];
    reduction[local] = total;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (local < stride) reduction[local] += reduction[local + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (local == 0) exposureOutput[uint2(0, 0)] = clamp(0.18 * exp2(-reduction[0].x / max(reduction[0].y, 1.0)), 0.0001, 10000.0);
}
)hlsl";

struct ExposureSlot {
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> partials;
    uint32_t tileCapacity = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};

struct ExposurePipeline {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> meter;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> reduce;
    std::vector<std::shared_ptr<ExposureSlot>> slots;
};

struct Slot {
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    uint64_t serial = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> colorHistory;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthHistory;
    mcdr_frame_identity identity{};
    uint64_t historyGeneration = 0;
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Owner {
    void* context = nullptr;
    uint64_t generation = 0;
    uint64_t recordingVersion = 0;
    uint64_t worldEpoch = 0;
    uint64_t realFrameId = 0;
    uint64_t sourceVersion = 0;
    uint64_t viewportVersion = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    void* graph = nullptr;
    void* vrsGraph = nullptr;
    std::vector<std::shared_ptr<Slot>> slots;
    std::shared_ptr<Slot> maskHistory;
    std::vector<std::shared_ptr<Slot>> vrsSlots;
    std::shared_ptr<ExposurePipeline> exposure;
    ~Owner() {
        if (graph) workgraph::destroyProgram(graph);
        if (vrsGraph) workgraph::destroyProgram(vrsGraph);
    }
};

struct PreparedPrograms {
    std::shared_ptr<void> expected;
    std::shared_ptr<void> replacement;
    uint64_t recordingVersion = 0;
};

bool transitionMaskHistory(CommandContext* context, ID3D12Resource* resource, D3D12_RESOURCE_STATES state) {
    D3D12_FEATURE_DATA_FORMAT_INFO format{resource->GetDesc().Format};
    if (FAILED(deviceContext().device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format))) ||
        format.PlaneCount == 0) return false;
    return static_cast<bool>(transitionResourceRange(context, resource, 0, format.PlaneCount,
        D3D12_RESOURCE_STATE_COMMON, state));
}

bool sameCapture(const Owner& owner, const MaskRequest& request) {
    return owner.worldEpoch == request.worldEpoch && owner.realFrameId == request.realFrameId &&
        owner.sourceVersion == request.sourceVersion && owner.viewportVersion == request.viewportVersion &&
        owner.width == request.width && owner.height == request.height;
}

std::shared_ptr<Slot> acquireSlot(std::vector<std::shared_ptr<Slot>>& slots) {
    for (const auto& slot : slots) if (slot.use_count() == 1) return slot;
    auto slot = std::make_shared<Slot>();
    slots.push_back(slot);
    return slot;
}

bool validTexture(Dx12Object* object, DXGI_FORMAT format, uint32_t width, uint32_t height,
                  bool uav) {
    if (!object || object->kind != Dx12Object::Kind::Texture || !object->resource) return false;
    const auto desc = object->resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == width &&
        desc.Height == height && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
        desc.SampleDesc.Count == 1 && object->dxgiFormat == format &&
        !(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
        (!uav || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
}

bool createProviderMaskGraph(Owner& owner) {
    std::vector<uint8_t> dxil;
    auto compiled = compileHlslToDxil(kShader, "", "lib_6_9", dxil);
    if (!compiled) {
        dbgLogError("frameprep: Work Graph shader compile failed: %s", compiled.error().message.c_str());
        return false;
    }
    const workgraph::RootBinding bindings[] = {{3, 0, 0}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0},
        {3, 4, 0}, {3, 5, 0}, {3, 6, 0}, {4, 0, 0}, {4, 1, 0}, {4, 2, 0}};
    const workgraph::ProgramDescription description{
        dxil.data(), dxil.size(), L"FramePrepGraph", L"TileClassify", 0, kEntryRecordSize,
        bindings, 10,
        kBackingInstanceBudget * std::size(static_cast<CommandContext*>(owner.context)->recordingLeases),
        kBackingInstanceBudget};
    auto graph = workgraph::createProgram(description);
    if (!graph) {
        dbgLogError("frameprep: Work Graph creation failed: %s", graph.error().message.c_str());
        return false;
    }
    owner.graph = *graph;
    return true;
}

bool createVrsGraph(Owner& owner) {
    std::vector<uint8_t> dxil;
    auto compiled = compileHlslToDxil(kVrsShader, "", "lib_6_9", dxil);
    if (!compiled) {
        dbgLogError("frameprep: VRS Work Graph shader compile failed: %s", compiled.error().message.c_str());
        return false;
    }
    const workgraph::RootBinding bindings[] = {{3, 0, 0}, {3, 1, 0}, {3, 2, 0}, {4, 0, 0}, {4, 1, 0}};
    const workgraph::ProgramDescription description{
        dxil.data(), dxil.size(), L"VrsFramePrepGraph", L"VrsClassify", 0, sizeof(VrsEntryRecord),
        bindings, 5,
        kBackingInstanceBudget * std::size(static_cast<CommandContext*>(owner.context)->recordingLeases),
        kBackingInstanceBudget};
    auto graph = workgraph::createProgram(description);
    if (!graph) {
        dbgLogError("frameprep: VRS Work Graph creation failed: %s", graph.error().message.c_str());
        return false;
    }
    owner.vrsGraph = *graph;
    return true;
}

bool ensureHeap(Slot& slot, uint32_t descriptorCount) {
    if (slot.heap) return true;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = descriptorCount;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    return SUCCEEDED(deviceContext().device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&slot.heap)));
}

DXGI_FORMAT depthViewFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool validDepthTexture(Dx12Object* object, uint32_t width, uint32_t height) {
    if (!object || object->kind != Dx12Object::Kind::Texture || !object->resource ||
        depthViewFormat(object->dxgiFormat) == DXGI_FORMAT_UNKNOWN) return false;
    const auto desc = object->resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == width &&
        desc.Height == height && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
        desc.SampleDesc.Count == 1 && !(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}

bool initializeExposure(ExposurePipeline& pipeline) {
    auto* device = deviceContext().device.Get();
    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    D3D12_ROOT_PARAMETER roots[5]{};
    for (uint32_t index = 0; index < 4; ++index) {
        ranges[index].RangeType = index % 2 == 0 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[index].NumDescriptors = 1;
        ranges[index].BaseShaderRegister = index / 2;
        roots[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[index].DescriptorTable.NumDescriptorRanges = 1;
        roots[index].DescriptorTable.pDescriptorRanges = &ranges[index];
    }
    roots[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    roots[4].Constants.Num32BitValues = 6;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 5;
    rootDesc.pParameters = roots;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostic;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostic)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&pipeline.root)))) return false;
    auto createStage = [&](const char* entry, Microsoft::WRL::ComPtr<ID3D12PipelineState>& stage) {
        std::vector<uint8_t> dxil;
        auto compiled = compileHlslToDxil(kExposureShader, entry, "cs_6_6", dxil);
        if (!compiled) {
            dbgLogError("frameprep: exposure shader compile failed: %s", compiled.error().message.c_str());
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
        description.pRootSignature = pipeline.root.Get();
        description.CS = {dxil.data(), dxil.size()};
        return SUCCEEDED(device->CreateComputePipelineState(&description, IID_PPV_ARGS(&stage)));
    };
    return createStage("MeterTiles", pipeline.meter) && createStage("ReduceExposure", pipeline.reduce);
}

bool initializeExposureSlot(ExposureSlot& slot, uint32_t tileCount) {
    auto* device = deviceContext().device.Get();
    if (!slot.heap) {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        description.NumDescriptors = 4;
        description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&slot.heap)))) return false;
    }
    if (slot.tileCapacity >= tileCount) return true;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = static_cast<uint64_t>(tileCount) * 8;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> partials;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&partials)))) return false;
    slot.partials = std::move(partials);
    slot.tileCapacity = tileCount;
    slot.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

uint32_t supportedProgramGraphs() {
    const auto& caps = deviceContext().caps;
    if (caps.workGraphsTier < D3D12_WORK_GRAPHS_TIER_1_0) return 0;
    return caps.shadingRateImageSupported() && caps.shadingRateImageTileSize > 0 ? 3u : 1u;
}

std::shared_ptr<Owner> createProgramOwner(void* context, uint64_t generation, uint32_t supportedGraphs) {
    auto staged = std::make_shared<Owner>();
    staged->context = context;
    staged->generation = generation;
    auto exposure = std::make_shared<ExposurePipeline>();
    if (!initializeExposure(*exposure)) return {};
    staged->exposure = std::move(exposure);
    if (((supportedGraphs & 1u) != 0 && !createProviderMaskGraph(*staged)) ||
        ((supportedGraphs & 2u) != 0 && !createVrsGraph(*staged))) return {};
    const auto* current = static_cast<const Owner*>(static_cast<CommandContext*>(context)->frameprepState.get());
    if (current) {
        staged->slots = current->slots;
        staged->vrsSlots = current->vrsSlots;
    }
    return staged;
}

}

int produceExposure(const ExposureRequest& request) {
    auto* context = static_cast<CommandContext*>(request.context);
    auto* color = static_cast<Dx12Object*>(request.color);
    auto* output = static_cast<Dx12Object*>(request.output);
    if (!context || !context->listOpen || !context->commandList || !color || color == output ||
        color->kind != Dx12Object::Kind::Texture || !color->resource ||
        request.width == 0 || request.height == 0 || request.colorEncoding > 1 ||
        !std::isfinite(request.preExposure) || request.preExposure <= 0.0f ||
        !std::isfinite(1.0f / request.preExposure) ||
        !validTexture(output, DXGI_FORMAT_R32_FLOAT, 1, 1, true)) return 0;
    const auto colorDesc = color->resource->GetDesc();
    const bool hardwareSrgb = color->dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        color->dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || color->dxgiFormat == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    const bool hdrInput = request.colorEncoding == 0 && (color->dxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        color->dxgiFormat == DXGI_FORMAT_R32G32B32A32_FLOAT || color->dxgiFormat == DXGI_FORMAT_R11G11B10_FLOAT);
    if (!hdrInput && request.preExposure != 1.0f) return 0;
    switch (color->dxgiFormat) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT: break;
        default: return 0;
    }
    if (colorDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || colorDesc.Width != request.width ||
        colorDesc.Height != request.height || colorDesc.DepthOrArraySize != 1 || colorDesc.SampleDesc.Count != 1 ||
        (colorDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || color->resource == output->resource) return 0;
    const uint64_t tilesX = (static_cast<uint64_t>(request.width) + 15) / 16;
    const uint64_t tilesY = (static_cast<uint64_t>(request.height) + 15) / 16;
    if (tilesX > 65535 || tilesY > 65535 || tilesX * tilesY > UINT32_MAX) return 0;
    auto* owner = static_cast<Owner*>(context->frameprepState.get());
    if (!owner || owner->generation == 0 || !owner->exposure) {
        dbgLogError("frameprep: exposure pipeline was not prewarmed");
        return -1;
    }
    if (owner->recordingVersion == UINT64_MAX) return -1;
    ++owner->recordingVersion;
    auto pipeline = owner->exposure;
    std::shared_ptr<ExposureSlot> slot;
    for (const auto& candidate : pipeline->slots) {
        if (candidate.use_count() == 1) { slot = candidate; break; }
    }
    if (!slot) {
        slot = std::make_shared<ExposureSlot>();
        pipeline->slots.push_back(slot);
    }
    const auto tileCount = static_cast<uint32_t>(tilesX * tilesY);
    if (!initializeExposureSlot(*slot, tileCount)) return -1;
    context->recordingLeases[context->currentListSlot].push_back(pipeline);
    context->recordingLeases[context->currentListSlot].push_back(slot);
    retainObjectForRecording(context, color);
    retainObjectForRecording(context, output);
    auto* device = deviceContext().device.Get();
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = slot->heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = slot->heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC colorView{};
    colorView.Format = color->dxgiFormat;
    colorView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    colorView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    colorView.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(color->resource.Get(), &colorView, cpu);
    cpu.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC partialUav{};
    partialUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    partialUav.Buffer.NumElements = tileCount;
    partialUav.Buffer.StructureByteStride = 8;
    device->CreateUnorderedAccessView(slot->partials.Get(), nullptr, &partialUav, cpu);
    cpu.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC partialSrv{};
    partialSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    partialSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    partialSrv.Buffer.NumElements = tileCount;
    partialSrv.Buffer.StructureByteStride = 8;
    device->CreateShaderResourceView(slot->partials.Get(), &partialSrv, cpu);
    cpu.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputView{};
    outputView.Format = DXGI_FORMAT_R32_FLOAT;
    outputView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(output->resource.Get(), nullptr, &outputView, cpu);
    transitionTextureTo(context, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transitionTextureTo(context, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto transitionPartials = [&](D3D12_RESOURCE_STATES state) {
        if (slot->state == state) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = slot->partials.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = slot->state;
        barrier.Transition.StateAfter = state;
        context->commandList->ResourceBarrier(1, &barrier);
        slot->state = state;
    };
    transitionPartials(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {slot->heap.Get()};
    context->commandList->SetDescriptorHeaps(1, heaps);
    context->commandList->SetComputeRootSignature(pipeline->root.Get());
    for (UINT index = 0; index < 4; ++index) {
        D3D12_GPU_DESCRIPTOR_HANDLE descriptor{gpu.ptr + static_cast<uint64_t>(index) * increment};
        context->commandList->SetComputeRootDescriptorTable(index, descriptor);
    }
    struct Constants { uint32_t width, height, tilesX, tileCount, colorEncoding; float preExposure; };
    const Constants constants{request.width, request.height, static_cast<uint32_t>(tilesX), tileCount,
        (hardwareSrgb ? 0u : request.colorEncoding) | (hdrInput ? 0u : 2u), request.preExposure};
    context->commandList->SetComputeRoot32BitConstants(4, 6, &constants, 0);
    if (hdrInput) {
        context->commandList->SetPipelineState(pipeline->meter.Get());
        context->commandList->Dispatch(static_cast<UINT>(tilesX), static_cast<UINT>(tilesY), 1);
    }
    transitionPartials(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    context->commandList->SetPipelineState(pipeline->reduce.Get());
    context->commandList->Dispatch(1, 1, 1);
    transitionPartials(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transitionTextureTo(context, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    restoreDrawHeap(context);
    return 1;
}

int prewarm(void* opaqueContext, uint64_t generation, uint32_t flags) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->commandList || context->inRenderPass || context->submissionFailed.load() ||
        generation == 0 || (flags & ~3u) != 0) return 0;
    const auto supportedGraphs = supportedProgramGraphs();
    if ((flags & ~supportedGraphs) != 0) return 0;
    const auto* current = static_cast<const Owner*>(context->frameprepState.get());
    if (current && generation < current->generation) return 0;
    if (current && generation == current->generation &&
        ((supportedGraphs & 1u) == 0 || current->graph) &&
        ((supportedGraphs & 2u) == 0 || current->vrsGraph) && current->exposure) return 1;
    auto staged = createProgramOwner(opaqueContext, generation, supportedGraphs);
    if (!staged) return -1;
    context->frameprepState = std::move(staged);
    return 1;
}

int preparePrograms(void* opaqueContext, uint64_t generation, uint32_t flags, void** prepared) {
    if (!prepared) return MCDR_FRAME_PROGRAMS_INVALID_REQUEST;
    *prepared = nullptr;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    const auto supportedGraphs = supportedProgramGraphs();
    if (generation == 0 || (flags & ~3u) != 0 || (flags & ~supportedGraphs) != 0)
        return MCDR_FRAME_PROGRAMS_INVALID_REQUEST;
    const auto* current = static_cast<const Owner*>(context->frameprepState.get());
    if (current && generation <= current->generation) return MCDR_FRAME_PROGRAMS_STALE;
    auto candidate = std::make_unique<PreparedPrograms>();
    candidate->expected = context->frameprepState;
    candidate->recordingVersion = current ? current->recordingVersion : 0;
    candidate->replacement = createProgramOwner(opaqueContext, generation, supportedGraphs);
    if (!candidate->replacement) return MCDR_FRAME_PROGRAMS_COMPILATION_FAILED;
    *prepared = candidate.release();
    return MCDR_FRAME_PROGRAMS_SUCCESS;
}

int validatePreparedPrograms(void* opaqueContext, void* opaquePrepared) {
    if (!opaquePrepared) return MCDR_FRAME_PROGRAMS_INVALID_REQUEST;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    const auto* prepared = static_cast<const PreparedPrograms*>(opaquePrepared);
    if (context->frameprepState != prepared->expected) return MCDR_FRAME_PROGRAMS_STALE;
    const auto* current = static_cast<const Owner*>(context->frameprepState.get());
    if (current && current->recordingVersion != prepared->recordingVersion)
        return MCDR_FRAME_PROGRAMS_STALE;
    return MCDR_FRAME_PROGRAMS_SUCCESS;
}

void exchangePreparedPrograms(void* opaqueContext, void* opaquePrepared) noexcept {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* prepared = static_cast<PreparedPrograms*>(opaquePrepared);
    context->frameprepState.swap(prepared->replacement);
}

void destroyPreparedPrograms(void* opaquePrepared) noexcept {
    const std::unique_ptr<PreparedPrograms> prepared(static_cast<PreparedPrograms*>(opaquePrepared));
}

int produceReactiveMask(const MaskRequest& request) {
    auto* context = static_cast<CommandContext*>(request.context);
    auto* source0 = static_cast<Dx12Object*>(request.source0);
    auto* source1 = static_cast<Dx12Object*>(request.source1);
    auto* output = (request.outputFlags & 5u) != 0 ? static_cast<Dx12Object*>(request.output) : nullptr;
    auto* compositionOutput = (request.outputFlags & 2u) != 0 ? static_cast<Dx12Object*>(request.compositionOutput) : nullptr;
    auto* responsiveOutput = (request.outputFlags & 4u) != 0 ? static_cast<Dx12Object*>(request.output) : nullptr;
    const auto* classificationInput = static_cast<const mcdr_provider_mask_input*>(request.classification);
    auto* color = static_cast<Dx12Object*>(request.color);
    auto* depth = static_cast<Dx12Object*>(request.depth);
    auto* motion = static_cast<Dx12Object*>(request.motion);
    const bool hasClassification = classificationInput != nullptr;
    const bool hasOit = !hasClassification || (request.inputFlags & 2u) != 0;
    const bool validFlags = request.outputFlags == 1 || request.outputFlags == 2 ||
        request.outputFlags == 3 || request.outputFlags == 4;
    if (!context || !context->listOpen || !context->commandList || context->inRenderPass ||
        context->submissionFailed.load() || request.width == 0 ||
        request.height == 0 || request.worldEpoch == 0 || request.realFrameId == 0 ||
        request.sourceVersion == 0 || request.viewportVersion == 0 || !validFlags ||
        (hasOit && (source0 == source1 ||
            !validTexture(source0, kCoefficientFormat, request.width, request.height, false) ||
            !validTexture(source1, kCoefficientFormat, request.width, request.height, false))) ||
        (hasClassification && (!color || !validTexture(color, color->dxgiFormat, request.width, request.height, false) ||
            !validDepthTexture(depth, request.width, request.height) ||
            !validTexture(motion, DXGI_FORMAT_R16G16_FLOAT, request.width, request.height, false))) ||
        ((request.outputFlags & 1u) != 0 && !validTexture(output, kMaskFormat, request.width, request.height, true)) ||
        ((request.outputFlags & 2u) != 0 && !validTexture(compositionOutput, kMaskFormat, request.width, request.height, true)) ||
        ((request.outputFlags & 4u) != 0 && !validTexture(responsiveOutput, kMaskFormat, request.width, request.height, true)) ||
        (output && compositionOutput && output->resource.Get() == compositionOutput->resource.Get())) {
        dbgLogError("frameprep: incomplete or incompatible reactive-mask request");
        return 0;
    }
    for (auto* input : {hasOit ? source0 : nullptr, hasOit ? source1 : nullptr,
        hasClassification ? color : nullptr, hasClassification ? depth : nullptr,
        hasClassification ? motion : nullptr}) {
        if (input && ((output && input->resource == output->resource) ||
            (compositionOutput && input->resource == compositionOutput->resource))) return 0;
    }
    auto* owner = static_cast<Owner*>(context->frameprepState.get());
    if (!owner || owner->generation == 0 || !owner->graph) {
        dbgLogError("frameprep: provider mask graph was not prewarmed");
        return -1;
    }
    if (owner->recordingVersion == UINT64_MAX) return -1;
    ++owner->recordingVersion;
    if (!sameCapture(*owner, request)) {
        owner->worldEpoch = request.worldEpoch;
        owner->realFrameId = request.realFrameId;
        owner->sourceVersion = request.sourceVersion;
        owner->viewportVersion = request.viewportVersion;
        owner->width = request.width;
        owner->height = request.height;
    }
    const uint64_t tilesX = (static_cast<uint64_t>(request.width) + 7) / 8;
    const uint64_t tilesY = (static_cast<uint64_t>(request.height) + 7) / 8;
    const uint64_t groups = (tilesX * tilesY + 63) / 64;
    if (groups == 0 || groups > 65535) return -1;
    const auto previous = owner->maskHistory;
    const bool historyCompatible = hasClassification && (request.inputFlags & 4u) != 0 && previous &&
        previous->identity.worldEpoch == classificationInput->identity.worldEpoch &&
        previous->identity.configVersion == classificationInput->identity.configVersion &&
        previous->identity.resourceVersion == classificationInput->identity.resourceVersion &&
        previous->identity.realFrameId == classificationInput->previousRealFrameId &&
        previous->historyGeneration == classificationInput->historyGeneration &&
        previous->width == request.width && previous->height == request.height &&
        previous->colorFormat == color->dxgiFormat && previous->depthFormat == depth->dxgiFormat;
    auto slot = acquireSlot(owner->slots);
    if (!ensureHeap(*slot, 10)) return -1;
    auto lease = std::make_shared<std::shared_ptr<Slot>>(slot);
    context->recordingLeases[context->currentListSlot].push_back(lease);
    if (hasOit) {
        retainObjectForRecording(context, source0);
        retainObjectForRecording(context, source1);
    }
    if (hasClassification) {
        retainObjectForRecording(context, color);
        retainObjectForRecording(context, depth);
        retainObjectForRecording(context, motion);
        if (historyCompatible) context->recordingLeases[context->currentListSlot].push_back(previous);
    }
    if (output) retainObjectForRecording(context, output);
    if (compositionOutput) retainObjectForRecording(context, compositionOutput);
    if (responsiveOutput) retainObjectForRecording(context, responsiveOutput);
    const UINT increment = deviceContext().device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = slot->heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = slot->heap->GetGPUDescriptorHandleForHeapStart();
    auto createSrv = [&](UINT index, ID3D12Resource* resource, DXGI_FORMAT format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        auto target = cpu;
        target.ptr += static_cast<SIZE_T>(index) * increment;
        deviceContext().device->CreateShaderResourceView(resource, &view, target);
    };
    createSrv(0, hasOit ? source0->resource.Get() : nullptr, kCoefficientFormat);
    createSrv(1, hasOit ? source1->resource.Get() : nullptr, kCoefficientFormat);
    createSrv(2, hasClassification ? color->resource.Get() : nullptr,
        hasClassification ? color->dxgiFormat : DXGI_FORMAT_R8G8B8A8_UNORM);
    createSrv(3, hasClassification ? depth->resource.Get() : nullptr,
        hasClassification ? depthViewFormat(depth->dxgiFormat) : DXGI_FORMAT_R32_FLOAT);
    createSrv(4, hasClassification ? motion->resource.Get() : nullptr, DXGI_FORMAT_R16G16_FLOAT);
    createSrv(5, historyCompatible ? previous->colorHistory.Get() : nullptr,
        historyCompatible ? previous->colorFormat : DXGI_FORMAT_R8G8B8A8_UNORM);
    createSrv(6, historyCompatible ? previous->depthHistory.Get() : nullptr,
        historyCompatible ? depthViewFormat(previous->depthFormat) : DXGI_FORMAT_R32_FLOAT);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = kMaskFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    auto createUav = [&](UINT index, Dx12Object* object) {
        auto target = cpu;
        target.ptr += static_cast<SIZE_T>(index) * increment;
        deviceContext().device->CreateUnorderedAccessView(object ? object->resource.Get() : nullptr, nullptr, &uav, target);
    };
    Dx12Object* fallbackOutput = output ? output : compositionOutput;
    createUav(7, (request.outputFlags & 1u) != 0 ? output : fallbackOutput);
    createUav(8, compositionOutput ? compositionOutput : fallbackOutput);
    createUav(9, responsiveOutput ? responsiveOutput : fallbackOutput);
    if (hasOit) {
        transitionTextureTo(context, source0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transitionTextureTo(context, source1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (hasClassification) {
        transitionTextureTo(context, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transitionTextureTo(context, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transitionTextureTo(context, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (historyCompatible)
            for (const auto& resource : {previous->colorHistory, previous->depthHistory})
                if (!transitionMaskHistory(context, resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) return -1;
    }
    if (output) transitionTextureTo(context, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (compositionOutput && compositionOutput != output) transitionTextureTo(context, compositionOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (responsiveOutput && responsiveOutput != output && responsiveOutput != compositionOutput) transitionTextureTo(context, responsiveOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {slot->heap.Get()};
    context->commandList->SetDescriptorHeaps(1, heaps);
    alignas(16) uint32_t record[kEntryRecordSize / sizeof(uint32_t)] = {
        static_cast<uint32_t>(groups), 1u, 1u,
        request.width, request.height, static_cast<uint32_t>(tilesX), static_cast<uint32_t>(tilesY), request.outputFlags};
    record[8] = (hasClassification ? 1u : 0u) | (hasOit ? 2u : 0u) | (historyCompatible ? 4u : 0u);
    if (hasClassification) {
        std::memcpy(record + 9, &classificationInput->historyJitterX, 2 * sizeof(float));
        std::memcpy(record + 11, classificationInput->currentToPrevious, 16 * sizeof(float));
    }
    const uint64_t roots[] = {gpu.ptr, gpu.ptr + increment, gpu.ptr + 2ull * increment,
        gpu.ptr + 3ull * increment, gpu.ptr + 4ull * increment, gpu.ptr + 5ull * increment,
        gpu.ptr + 6ull * increment, gpu.ptr + 7ull * increment, gpu.ptr + 8ull * increment,
        gpu.ptr + 9ull * increment};
    auto dispatched = workgraph::dispatch(context, owner->graph, record, 1, kEntryRecordSize,
        roots, 10);
    restoreDrawHeap(context);
    if (!dispatched) {
        dbgLogError("frameprep: tile Work Graph dispatch failed: %s", dispatched.error().message.c_str());
        return -1;
    }
    if (historyCompatible)
        for (const auto& resource : {previous->colorHistory, previous->depthHistory})
            if (!transitionMaskHistory(context, resource.Get(), D3D12_RESOURCE_STATE_COMMON)) return -1;
    if (output) transitionTextureTo(context, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (compositionOutput && compositionOutput != output) transitionTextureTo(context, compositionOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (responsiveOutput && responsiveOutput != output && responsiveOutput != compositionOutput) transitionTextureTo(context, responsiveOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    slot->serial = request.realFrameId;
    if (hasClassification) {
        const bool allocate = !slot->colorHistory || slot->width != request.width || slot->height != request.height ||
            slot->colorFormat != color->dxgiFormat || slot->depthFormat != depth->dxgiFormat;
        if (allocate) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            for (const auto& pair : {std::pair{color, std::addressof(slot->colorHistory)},
                std::pair{depth, std::addressof(slot->depthHistory)}}) {
                auto description = pair.first->resource->GetDesc();
                description.Flags = D3D12_RESOURCE_FLAG_NONE;
                pair.second->Reset();
                if (FAILED(deviceContext().device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                    &description, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(pair.second->GetAddressOf())))) return -1;
            }
        }
        for (const auto& pair : {std::pair{color, slot->colorHistory.Get()}, std::pair{depth, slot->depthHistory.Get()}}) {
            transitionTextureTo(context, pair.first, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (!transitionMaskHistory(context, pair.second, D3D12_RESOURCE_STATE_COPY_DEST)) return -1;
            context->commandList->CopyResource(pair.second, pair.first->resource.Get());
            if (!transitionMaskHistory(context, pair.second, D3D12_RESOURCE_STATE_COMMON)) return -1;
            transitionTextureTo(context, pair.first, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        slot->identity = classificationInput->identity;
        slot->historyGeneration = classificationInput->historyGeneration;
        slot->colorFormat = color->dxgiFormat;
        slot->depthFormat = depth->dxgiFormat;
        slot->width = request.width;
        slot->height = request.height;
        owner->maskHistory = slot;
    }
    return 1;
}

int produceProviderMasks(void* context, const void* opaqueInput, uint32_t bytes) {
    if (!opaqueInput || bytes != sizeof(mcdr_provider_mask_input)) return 0;
    const auto input = *static_cast<const mcdr_provider_mask_input*>(opaqueInput);
    if (input.schemaVersion != MCDR_PROVIDER_MASK_SCHEMA_VERSION || input.structSize != sizeof(input) || input.width == 0 || input.height == 0 ||
        input.identity.worldEpoch == 0 || input.identity.realFrameId == 0 || input.identity.configVersion == 0 ||
        input.identity.resourceVersion == 0 || (input.inputFlags & ~7u) != 0 ||
        (input.inputFlags & 1u) == 0 || !std::isfinite(input.historyJitterX) || !std::isfinite(input.historyJitterY) ||
        ((input.inputFlags & 4u) != 0 && (input.previousRealFrameId == 0 ||
            input.previousRealFrameId + 1 != input.identity.realFrameId))) return 0;
    for (const auto element : input.currentToPrevious) if (!std::isfinite(element)) return 0;
    return produceReactiveMask({context, input.source0, input.source1, input.output, input.width, input.height,
        input.identity.worldEpoch, input.identity.realFrameId, input.identity.realFrameId,
        input.identity.resourceVersion, input.compositionOutput, input.outputFlags, &input, input.inputFlags,
        input.color, input.depth, input.motion});
}

int produceVrs(const VrsRequest& request) {
    auto* context = static_cast<CommandContext*>(request.context);
    auto* depth = static_cast<Dx12Object*>(request.depth);
    auto* motion = static_cast<Dx12Object*>(request.motion);
    auto* previous = static_cast<Dx12Object*>(request.previous);
    auto* output = static_cast<Dx12Object*>(request.output);
    auto* candidates = static_cast<Dx12Object*>(request.candidates);
    const uint32_t knownFlags = 1u | 2u | 4u | 8u;
    const auto& caps = deviceContext().caps;
    const uint32_t tileSize = caps.shadingRateImageTileSize;
    const uint64_t tilesX64 = tileSize == 0 ? 0 : (static_cast<uint64_t>(request.width) + tileSize - 1u) / tileSize;
    const uint64_t tilesY64 = tileSize == 0 ? 0 : (static_cast<uint64_t>(request.height) + tileSize - 1u) / tileSize;
    const uint64_t groups = (tilesX64 * tilesY64 + 63u) / 64u;
    const auto tilesX = static_cast<uint32_t>(tilesX64);
    const auto tilesY = static_cast<uint32_t>(tilesY64);
    const bool compatible = (request.flags & 1u) != 0;
    const bool hasMotion = compatible && (request.flags & 4u) != 0;
    const bool hasPrevious = compatible && (request.flags & 8u) != 0;
    if (!context || !context->listOpen || !context->commandList || request.width == 0 || request.height == 0 ||
        request.worldEpoch == 0 || request.realFrameId == 0 || request.sourceVersion == 0 ||
        request.viewportVersion == 0 || request.tileSize != tileSize || tileSize == 0 ||
        (request.flags & ~knownFlags) != 0 || groups == 0 || groups > 65535 ||
        request.edgePermille > 1000 ||
        !validTexture(output, kVrsFormat, tilesX, tilesY, true) ||
        !validTexture(candidates, kVrsFormat, tilesX, tilesY, true) ||
        !caps.shadingRateImageSupported() ||
        (compatible && !validDepthTexture(depth, request.width, request.height)) ||
        (hasPrevious && !validTexture(previous, kVrsFormat, tilesX, tilesY, false)) ||
        (hasMotion && !validTexture(motion, DXGI_FORMAT_R16G16_FLOAT,
            request.width, request.height, false)) ||
        (compatible && (!request.previousToCurrent || !request.currentToPrevious))) {
        dbgLogError("frameprep: incomplete or incompatible VRS request");
        return 0;
    }
    if (output->resource.Get() == candidates->resource.Get()) return 0;
    for (auto* input : {compatible ? depth : nullptr, hasMotion ? motion : nullptr,
        hasPrevious ? previous : nullptr}) {
        if (input && (input->resource.Get() == output->resource.Get() ||
            input->resource.Get() == candidates->resource.Get())) return 0;
    }
    alignas(16) VrsEntryRecord record{{static_cast<uint32_t>(groups), 1u, 1u}, request.width, request.height,
        request.tileSize, request.edgePermille, request.motionPixels, request.flags, {}, {}};
    if (compatible) {
        for (uint32_t element = 0; element < 16; ++element) {
            if (!std::isfinite(request.previousToCurrent[element]) ||
                !std::isfinite(request.currentToPrevious[element])) return 0;
            record.previousToCurrent[element] = request.previousToCurrent[element];
            record.currentToPrevious[element] = request.currentToPrevious[element];
        }
    }
    auto* owner = static_cast<Owner*>(context->frameprepState.get());
    if (!owner || owner->generation == 0 || !owner->vrsGraph) {
        dbgLogError("frameprep: VRS graph was not prewarmed");
        return -1;
    }
    if (owner->recordingVersion == UINT64_MAX) return -1;
    ++owner->recordingVersion;
    auto slot = acquireSlot(owner->vrsSlots);
    if (!ensureHeap(*slot, 5)) return -1;
    auto lease = std::make_shared<std::shared_ptr<Slot>>(slot);
    context->recordingLeases[context->currentListSlot].push_back(lease);
    retainObjectForRecording(context, output);
    retainObjectForRecording(context, candidates);
    if (compatible) retainObjectForRecording(context, depth);
    if (hasMotion) retainObjectForRecording(context, motion);
    if (hasPrevious) retainObjectForRecording(context, previous);
    const UINT increment = deviceContext().device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu = slot->heap->GetCPUDescriptorHandleForHeapStart();
    const auto gpu = slot->heap->GetGPUDescriptorHandleForHeapStart();
    auto createSrv = [&](UINT index, Dx12Object* object, DXGI_FORMAT format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        auto target = cpu;
        target.ptr += static_cast<SIZE_T>(index) * increment;
        deviceContext().device->CreateShaderResourceView(object ? object->resource.Get() : nullptr, &view, target);
    };
    createSrv(0, compatible ? depth : nullptr, compatible ? depthViewFormat(depth->dxgiFormat) : DXGI_FORMAT_R32_FLOAT);
    createSrv(1, hasMotion ? motion : nullptr, DXGI_FORMAT_R16G16_FLOAT);
    createSrv(2, hasPrevious ? previous : nullptr, kVrsFormat);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = kVrsFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    auto uavCpu = cpu;
    uavCpu.ptr += static_cast<SIZE_T>(3) * increment;
    deviceContext().device->CreateUnorderedAccessView(output->resource.Get(), nullptr, &uav, uavCpu);
    uavCpu.ptr += increment;
    deviceContext().device->CreateUnorderedAccessView(candidates->resource.Get(), nullptr, &uav, uavCpu);
    if (compatible) transitionTextureTo(context, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (hasMotion) transitionTextureTo(context, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (hasPrevious) transitionTextureTo(context, previous, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transitionTextureTo(context, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transitionTextureTo(context, candidates, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint64_t roots[] = {gpu.ptr, gpu.ptr + increment, gpu.ptr + 2ull * increment,
        gpu.ptr + 3ull * increment, gpu.ptr + 4ull * increment};
    ID3D12DescriptorHeap* heaps[] = {slot->heap.Get()};
    context->commandList->SetDescriptorHeaps(1, heaps);
    auto dispatched = workgraph::dispatch(context, owner->vrsGraph, &record, 1, sizeof(record), roots, 5);
    restoreDrawHeap(context);
    if (!dispatched) {
        dbgLogError("frameprep: VRS Work Graph dispatch failed: %s", dispatched.error().message.c_str());
        return -1;
    }
    transitionTextureTo(context, output, D3D12_RESOURCE_STATE_COMMON);
    transitionTextureTo(context, candidates, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return 1;
}

void reset(void* context) {
    if (context) static_cast<CommandContext*>(context)->frameprepState.reset();
}

}
