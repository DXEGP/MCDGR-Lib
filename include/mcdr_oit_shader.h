#pragma once
#include <cstdint>
#include <string_view>

namespace mcdr::oit_shader {
inline constexpr uint32_t abiVersion = 5;
inline constexpr uint32_t recordStride = 144;
inline constexpr uint32_t pixelStride = 8;
inline constexpr uint32_t captureConstantsBytes = 208;
inline constexpr std::string_view prelude = R"HLSL(
#ifndef MCDR_OIT_CAPTURE_V1
#define MCDR_OIT_CAPTURE_V1
#define MCDR_OIT_CURRENT_MATERIAL_VALID 1u
#define MCDR_OIT_GEOMETRY_PREVIOUS_VALID 2u
#define MCDR_OIT_MATERIAL_PREVIOUS_VALID 4u
#define MCDR_OIT_COVERAGE_VALID 8u
#define MCDR_OIT_PREVIOUS_GEOMETRY 1u
#define MCDR_OIT_PREVIOUS_MATERIAL_UV 2u
#define MCDR_OIT_RECORD_STRIDE 144u
#define MCDR_OIT_PIXEL_STRIDE 8u
#define MCDR_OIT_PIXEL_COUNT_MASK 255u
#define MCDR_OIT_PIXEL_STATUS_SHIFT 8u
cbuffer McdrOitConstantsV1 : register(b0, space2) {
    uint4 McdrOitExtent;
    uint4 McdrOitRowCounts;
    uint4 McdrOitRowBases;
    uint4 McdrOitDraw;
    uint4 McdrOitInstances;
    uint4 McdrOitScope;
    uint4 McdrOitHistoryExtent;
    uint4 McdrOitHistoryRows;
    uint4 McdrOitTerrainRows;
    column_major float4x4 McdrOitPreviousViewProjection;
};
ByteAddressBuffer McdrOitSourcesV1 : register(t0, space2);
ByteAddressBuffer McdrOitPrimitivesV1 : register(t1, space2);
ByteAddressBuffer McdrOitPreviousV1 : register(t2, space2);
ByteAddressBuffer McdrOitTerrainV2 : register(t9, space2);
ByteAddressBuffer McdrOitHistoryTerrainV2 : register(t10, space2);
RWByteAddressBuffer McdrOitRecordsV1 : register(u0, space2);
RWByteAddressBuffer McdrOitPixelsV1 : register(u1, space2);
RWByteAddressBuffer McdrOitAllocatorV1 : register(u2, space2);
uint3 McdrOitUnpackPixelV4(uint2 packed) {
    return uint3(packed.x, packed.y & MCDR_OIT_PIXEL_COUNT_MASK, packed.y >> MCDR_OIT_PIXEL_STATUS_SHIFT);
}
struct McdrOitFragmentV1 {
    float4 position;
    float4 finalQ;
    float3 materialRgb;
    float materialAlpha;
    float sampleAlpha;
    float transmittance;
    float accumAlpha;
    float2 materialUv;
    float2 materialDx;
    float2 materialDy;
    float2 previousUv;
    float expectedPreviousDepth;
    float2 materialPreviousUv;
    float materialExpectedPreviousDepth;
    float response;
    float3 fogColorRgb;
    float fogFactor;
    float chunkVisibility;
    float depthDx;
    float depthDy;
    float2 materialPreviousDx;
    float2 materialPreviousDy;
    float coverage;
    uint primitiveIndex;
    uint validity;
};
uint McdrOitPrimitiveIndexV1(uint primitiveId, uint instanceId) {
    if (instanceId < McdrOitInstances.z || instanceId - McdrOitInstances.z >= McdrOitInstances.y ||
        primitiveId >= McdrOitInstances.x || McdrOitInstances.x == 0u) return 0xffffffffu;
    uint instance = instanceId - McdrOitInstances.z;
    if (instance > (0xffffffffu - primitiveId) / McdrOitInstances.x) return 0xffffffffu;
    uint index = instance * McdrOitInstances.x + primitiveId;
    return index < McdrOitDraw.z ? index : 0xffffffffu;
}
uint McdrOitTerrainPrimitiveIndexV1(uint sectionIndex, uint sectionPrimitiveId) {
    if (McdrOitTerrainRows.w != 0u) {
        if (sectionIndex >= McdrOitTerrainRows.w) return 0xffffffffu;
        uint address = (McdrOitTerrainRows.z + sectionIndex) * 80u;
        uint2 range = McdrOitTerrainV2.Load2(address + 48u);
        if (sectionPrimitiveId >= range.y || range.x > 0x7fffffffu - sectionPrimitiveId) return 0xffffffffu;
        return 0x80000000u | (range.x + sectionPrimitiveId);
    }
    if (sectionIndex >= McdrOitRowCounts.w) return 0xffffffffu;
    uint base = McdrOitPreviousV1.Load(McdrOitRowBases.w + sectionIndex * 4u);
    uint end = sectionIndex + 1u < McdrOitRowCounts.w ?
        McdrOitPreviousV1.Load(McdrOitRowBases.w + (sectionIndex + 1u) * 4u) : McdrOitRowCounts.y;
    if (base > end || end > McdrOitRowCounts.y || sectionPrimitiveId >= end - base) return 0xffffffffu;
    uint absoluteLocal = base + sectionPrimitiveId;
    if (absoluteLocal < McdrOitDraw.y || absoluteLocal - McdrOitDraw.y >= McdrOitDraw.z) return 0xffffffffu;
    return absoluteLocal - McdrOitDraw.y;
}
bool McdrOitFragmentRowsV2(uint primitiveIndex, out uint sourceAddress, out uint primitiveAddress) {
    sourceAddress = 0u;
    primitiveAddress = 0u;
    if ((primitiveIndex & 0x80000000u) != 0u) {
        uint local = primitiveIndex & 0x7fffffffu;
        if (local >= McdrOitTerrainRows.y) return false;
        uint lower = 0u, upper = McdrOitTerrainRows.x;
        while (lower < upper) {
            uint middle = lower + (upper - lower) / 2u;
            uint2 range = McdrOitTerrainV2.Load2(middle * 80u + 48u);
            if (local < range.x) upper = middle;
            else if (local - range.x >= range.y) lower = middle + 1u;
            else {
                primitiveAddress = middle * 80u;
                uint source = McdrOitTerrainV2.Load(primitiveAddress + 40u);
                if (source >= McdrOitRowCounts.x) return false;
                sourceAddress = source * 32u;
                return McdrOitSourcesV1.Load(sourceAddress + 24u) == 0u;
            }
        }
        return false;
    }
    if (primitiveIndex >= McdrOitDraw.z || McdrOitDraw.y > McdrOitRowCounts.y ||
        primitiveIndex >= McdrOitRowCounts.y - McdrOitDraw.y || McdrOitDraw.x >= McdrOitRowCounts.x)
        return false;
    uint sourceRow = McdrOitRowBases.x + McdrOitDraw.x;
    uint primitiveRow = McdrOitRowBases.y + McdrOitDraw.y + primitiveIndex;
    uint sourceBytes, primitiveBytes;
    McdrOitSourcesV1.GetDimensions(sourceBytes);
    McdrOitPrimitivesV1.GetDimensions(primitiveBytes);
    if (sourceRow < McdrOitRowBases.x || primitiveRow < McdrOitRowBases.y ||
        sourceRow >= sourceBytes / 32u || primitiveRow >= primitiveBytes / 56u) return false;
    sourceAddress = sourceRow * 32u;
    primitiveAddress = primitiveRow * 56u;
    return McdrOitPrimitivesV1.Load(primitiveAddress + 40u) == sourceRow &&
        McdrOitSourcesV1.Load(sourceAddress + 24u) != 0u;
}
uint McdrOitPreviousAddressV2(uint primitiveAddress) {
    uint previousRow = McdrOitPrimitivesV1.Load(primitiveAddress + 44u);
    if (previousRow < McdrOitRowBases.z || previousRow - McdrOitRowBases.z >= McdrOitRowCounts.z)
        return 0xffffffffu;
    uint previousBytes;
    McdrOitPreviousV1.GetDimensions(previousBytes);
    return previousRow < previousBytes / 80u ? previousRow * 80u : 0xffffffffu;
}
bool McdrOitProjectPreviousV2(float4 clip, out float2 uv, out float depth);
float4 McdrOitTerrainPreviousClipV2(uint sectionIndex, float3 position) {
    if (sectionIndex >= McdrOitTerrainRows.w) return 0.0f.xxxx;
    uint address = (McdrOitTerrainRows.z + sectionIndex) * 80u;
    if ((McdrOitTerrainV2.Load(address + 44u) & MCDR_OIT_GEOMETRY_PREVIOUS_VALID) == 0u) return 0.0f.xxxx;
    float3 origin = asfloat(McdrOitTerrainV2.Load3(address + 56u));
    return mul(McdrOitPreviousViewProjection, float4(origin + position, 1.0f));
}
void McdrOitResolveTerrainPreviousV2(inout McdrOitFragmentV1 fragment, float4 previousClip) {
    fragment.validity &= ~6u;
    uint sourceAddress, primitiveAddress;
    if (!McdrOitFragmentRowsV2(fragment.primitiveIndex, sourceAddress, primitiveAddress)) return;
    uint allowed = McdrOitSourcesV1.Load(sourceAddress + 28u) & McdrOitTerrainV2.Load(primitiveAddress + 44u);
    if ((allowed & 2u) != 0u && McdrOitProjectPreviousV2(previousClip, fragment.previousUv, fragment.expectedPreviousDepth)) {
        fragment.validity |= 2u;
        if ((allowed & 4u) != 0u) {
            fragment.materialPreviousUv = fragment.previousUv;
            fragment.materialExpectedPreviousDepth = fragment.expectedPreviousDepth;
            fragment.validity |= 4u;
        }
    }
}
bool McdrOitPreviousSampleValidV2(float2 uv, float depth) {
    return all(isfinite(uv)) && isfinite(depth) && all(uv >= 0.0f) && all(uv <= 1.0f) &&
        depth >= 0.0f && depth <= 1.0f;
}
bool McdrOitProjectPreviousV2(float4 clip, out float2 uv, out float depth) {
    uv = 0.0f.xx;
    depth = 0.0f;
    if (!all(isfinite(clip)) || clip.w <= 0.0f || clip.z < 0.0f || clip.z > clip.w) return false;
    float3 projected = clip.xyz / clip.w;
    float2 projectedUv = projected.xy * float2(0.5f, -0.5f) + 0.5f;
    if (!McdrOitPreviousSampleValidV2(projectedUv, projected.z)) return false;
    uv = projectedUv;
    depth = projected.z;
    return true;
}
void McdrOitResolvePreviousV2(inout McdrOitFragmentV1 fragment, float3 perspectiveBarycentrics, uint previousDomain) {
    uint requested = fragment.validity & 6u;
    fragment.validity &= ~6u;
    fragment.previousUv = 0.0f.xx;
    fragment.expectedPreviousDepth = 0.0f;
    fragment.materialPreviousUv = 0.0f.xx;
    fragment.materialExpectedPreviousDepth = 0.0f;
    if (requested == 0u || (previousDomain != MCDR_OIT_PREVIOUS_GEOMETRY &&
        previousDomain != MCDR_OIT_PREVIOUS_MATERIAL_UV)) return;
    uint sourceAddress, primitiveAddress;
    if (!McdrOitFragmentRowsV2(fragment.primitiveIndex, sourceAddress, primitiveAddress)) return;
    uint previousAddress = McdrOitPreviousAddressV2(primitiveAddress);
    if (previousAddress == 0xffffffffu) return;
    uint allowed = requested & McdrOitSourcesV1.Load(sourceAddress + 28u) &
        McdrOitPrimitivesV1.Load(primitiveAddress + 48u) & McdrOitPreviousV1.Load(previousAddress + 72u);
    if (allowed == 0u) return;
    float4 clip0 = asfloat(McdrOitPreviousV1.Load4(previousAddress));
    float4 clip1 = asfloat(McdrOitPreviousV1.Load4(previousAddress + 16u));
    float4 clip2 = asfloat(McdrOitPreviousV1.Load4(previousAddress + 32u));
    if (!all(isfinite(clip0)) || !all(isfinite(clip1)) || !all(isfinite(clip2))) return;
    bool geometryBarycentricsValid = all(isfinite(perspectiveBarycentrics)) &&
        all(perspectiveBarycentrics >= -1.0e-4f) && all(perspectiveBarycentrics <= 1.0001f) &&
        abs(dot(perspectiveBarycentrics, 1.0f.xxx) - 1.0f) <= 1.0e-3f;
    float2 geometryUv = 0.0f.xx;
    float geometryDepth = 0.0f;
    bool geometryValid = false;
    if (geometryBarycentricsValid) {
        float4 geometryClip = clip0 * perspectiveBarycentrics.x + clip1 * perspectiveBarycentrics.y +
            clip2 * perspectiveBarycentrics.z;
        geometryValid = McdrOitProjectPreviousV2(geometryClip, geometryUv, geometryDepth);
    }
    if (geometryValid && (allowed & MCDR_OIT_GEOMETRY_PREVIOUS_VALID) != 0u) {
        fragment.previousUv = geometryUv;
        fragment.expectedPreviousDepth = geometryDepth;
        fragment.validity |= MCDR_OIT_GEOMETRY_PREVIOUS_VALID;
    }
    if ((allowed & MCDR_OIT_MATERIAL_PREVIOUS_VALID) == 0u) return;
    if (previousDomain == MCDR_OIT_PREVIOUS_GEOMETRY) {
        if (geometryValid) {
            fragment.materialPreviousUv = geometryUv;
            fragment.materialExpectedPreviousDepth = geometryDepth;
            fragment.validity |= MCDR_OIT_MATERIAL_PREVIOUS_VALID;
        }
        return;
    }
    float2 uv0 = asfloat(McdrOitPreviousV1.Load2(previousAddress + 48u));
    float2 uv1 = asfloat(McdrOitPreviousV1.Load2(previousAddress + 56u));
    float2 uv2 = asfloat(McdrOitPreviousV1.Load2(previousAddress + 64u));
    if (!all(isfinite(uv0)) || !all(isfinite(uv1)) || !all(isfinite(uv2)) ||
        !all(isfinite(fragment.materialUv))) return;
    float2 edge0 = uv1 - uv0;
    float2 edge1 = uv2 - uv0;
    float2 delta = fragment.materialUv - uv0;
    if (!all(isfinite(edge0)) || !all(isfinite(edge1)) || !all(isfinite(delta))) return;
    float uvScale = max(max(abs(edge0.x), abs(edge0.y)), max(abs(edge1.x), abs(edge1.y)));
    if (uvScale <= 0.0f) return;
    edge0 /= uvScale;
    edge1 /= uvScale;
    delta /= uvScale;
    float uvArea = edge0.x * edge1.y - edge0.y * edge1.x;
    if (!isfinite(uvArea) || abs(uvArea) <= 1.0e-6f) return;
    float3 materialBarycentrics;
    materialBarycentrics.y = (delta.x * edge1.y - delta.y * edge1.x) / uvArea;
    materialBarycentrics.z = (edge0.x * delta.y - edge0.y * delta.x) / uvArea;
    materialBarycentrics.x = 1.0f - materialBarycentrics.y - materialBarycentrics.z;
    if (!all(isfinite(materialBarycentrics))) return;
    float4 materialClip = clip0 * materialBarycentrics.x + clip1 * materialBarycentrics.y +
        clip2 * materialBarycentrics.z;
    if (McdrOitProjectPreviousV2(materialClip, fragment.materialPreviousUv, fragment.materialExpectedPreviousDepth))
        fragment.validity |= MCDR_OIT_MATERIAL_PREVIOUS_VALID;
}
void AppendV1(McdrOitFragmentV1 fragment) {
    if (!all(isfinite(fragment.position)) || any(fragment.position.xy < 0.0f) ||
        any(fragment.position.xy >= float2(McdrOitExtent.xy))) return;
    uint2 position = uint2(fragment.position.xy);
    uint pixel = position.y * McdrOitExtent.x + position.x;
    uint pixelAddress = pixel * MCDR_OIT_PIXEL_STRIDE;
    uint ignored;
    uint sourceAddress, primitiveAddress;
    if (!McdrOitFragmentRowsV2(fragment.primitiveIndex, sourceAddress, primitiveAddress)) {
        McdrOitPixelsV1.InterlockedOr(pixelAddress + 4u, 2u << MCDR_OIT_PIXEL_STATUS_SHIFT, ignored);
        return;
    }
    uint packed = McdrOitPixelsV1.Load(pixelAddress + 4u);
    for (;;) {
        uint count = packed & MCDR_OIT_PIXEL_COUNT_MASK;
        if (count >= McdrOitExtent.w) {
            McdrOitPixelsV1.InterlockedOr(pixelAddress + 4u, 1u << MCDR_OIT_PIXEL_STATUS_SHIFT, ignored);
            return;
        }
        uint observed;
        McdrOitPixelsV1.InterlockedCompareExchange(pixelAddress + 4u, packed, packed + 1u, observed);
        if (observed == packed) break;
        packed = observed;
    }
    // One atomic per fragment: a compare-exchange retry loop on this single word serialises every
    // captured fragment in the frame through the same cache line.
    uint index = 0u;
    McdrOitAllocatorV1.InterlockedAdd(0u, 1u, index);
    if (index >= McdrOitExtent.z) {
        McdrOitPixelsV1.InterlockedOr(pixelAddress + 4u, 1u << MCDR_OIT_PIXEL_STATUS_SHIFT, ignored);
        return;
    }
    bool terrain = (fragment.primitiveIndex & 0x80000000u) != 0u;
    uint primitiveFlags = terrain ? McdrOitTerrainV2.Load(primitiveAddress + 44u) :
        McdrOitPrimitivesV1.Load(primitiveAddress + 48u);
    uint validity = fragment.validity & McdrOitSourcesV1.Load(sourceAddress + 28u) & primitiveFlags & 15u;
    if (!terrain) {
        uint previousAddress = McdrOitPreviousAddressV2(primitiveAddress);
        if (previousAddress == 0xffffffffu) validity &= ~6u;
        else validity &= (~6u | McdrOitPreviousV1.Load(previousAddress + 72u));
    }
    if (!all(isfinite(fragment.finalQ)) || !all(isfinite(fragment.materialRgb)) ||
        !all(isfinite(float4(fragment.materialAlpha, fragment.transmittance, fragment.accumAlpha, fragment.response))) ||
        !all(isfinite(float4(fragment.fogColorRgb, fragment.fogFactor))) ||
        !all(isfinite(float4(fragment.chunkVisibility, fragment.sampleAlpha, fragment.depthDx, fragment.depthDy))) ||
        !all(isfinite(float4(fragment.materialUv, fragment.materialDx))) ||
        !all(isfinite(fragment.materialDy)) || !isfinite(fragment.coverage)) validity = 0u;
    if (!McdrOitPreviousSampleValidV2(fragment.previousUv, fragment.expectedPreviousDepth))
        validity &= ~MCDR_OIT_GEOMETRY_PREVIOUS_VALID;
    if (!McdrOitPreviousSampleValidV2(fragment.materialPreviousUv, fragment.materialExpectedPreviousDepth) ||
        !all(isfinite(float4(fragment.materialPreviousDx, fragment.materialPreviousDy))))
        validity &= ~MCDR_OIT_MATERIAL_PREVIOUS_VALID;
    if ((validity & MCDR_OIT_GEOMETRY_PREVIOUS_VALID) == 0u) {
        fragment.previousUv = 0.0f.xx;
        fragment.expectedPreviousDepth = 0.0f;
    }
    if ((validity & MCDR_OIT_MATERIAL_PREVIOUS_VALID) == 0u) {
        fragment.materialPreviousUv = 0.0f.xx;
        fragment.materialExpectedPreviousDepth = 0.0f;
    }
    if ((validity & 9u) != 9u)
        McdrOitPixelsV1.InterlockedOr(pixelAddress + 4u, 4u << MCDR_OIT_PIXEL_STATUS_SHIFT, ignored);
    uint previous;
    McdrOitPixelsV1.InterlockedExchange(pixelAddress, index, previous);
    uint address = index * MCDR_OIT_RECORD_STRIDE;
    McdrOitRecordsV1.Store4(address, uint4(previous, pixel,
        terrain ? fragment.primitiveIndex : McdrOitRowBases.y + McdrOitDraw.y + fragment.primitiveIndex,
        sourceAddress / 32u));
    McdrOitRecordsV1.Store4(address + 16u, asuint(float4(fragment.materialRgb, fragment.transmittance)));
    McdrOitRecordsV1.Store4(address + 32u, uint4(asuint(fragment.position.z), asuint(fragment.coverage), validity,
        asuint(fragment.expectedPreviousDepth)));
    McdrOitRecordsV1.Store4(address + 48u, asuint(float4(fragment.materialUv, fragment.materialDx)));
    McdrOitRecordsV1.Store4(address + 64u, asuint(float4(fragment.materialDy, fragment.previousUv)));
    McdrOitRecordsV1.Store4(address + 80u, asuint(float4(fragment.materialPreviousUv,
        fragment.materialExpectedPreviousDepth, fragment.fogFactor)));
    McdrOitRecordsV1.Store4(address + 96u, asuint(float4(fragment.chunkVisibility, fragment.sampleAlpha, fragment.depthDx, fragment.depthDy)));
    McdrOitRecordsV1.Store4(address + 112u, asuint(float4(fragment.materialRgb, 1.0f)));
    McdrOitRecordsV1.Store4(address + 128u, asuint(float4(fragment.materialPreviousDx, fragment.materialPreviousDy)));
}
#endif
)HLSL";
inline constexpr uint64_t contractHash = [] {
    uint64_t hash = 14695981039346656037ull;
    for (const auto character : prelude) hash = (hash ^ static_cast<uint8_t>(character)) * 1099511628211ull;
    return hash;
}();
}
