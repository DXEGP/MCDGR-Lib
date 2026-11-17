#pragma once
#include <string_view>

namespace mcdr::oit_shader {
inline constexpr std::string_view resolve = R"HLSL(
Texture2D<float4> McdrOitCurrentV1 : register(t3, space2);
Texture2D<float> McdrOitUnrecordedCoverageV2 : register(t4, space2);
RWTexture2D<uint> McdrOitStatusV1 : register(u3, space2);
RWTexture2D<float4> McdrOitCorrectedV3 : register(u4, space2);
ByteAddressBuffer McdrOitHistoryRecordsV3 : register(t5, space2);
ByteAddressBuffer McdrOitHistoryPixelsV3 : register(t6, space2);
ByteAddressBuffer McdrOitHistorySourcesV3 : register(t7, space2);
ByteAddressBuffer McdrOitHistoryPrimitivesV3 : register(t8, space2);

cbuffer McdrOitTileResolveV1 : register(b1, space2) {
    uint4 McdrOitTileInfoV1;
    uint4 McdrOitTileRegionsV1[16];
};
Texture2DArray<float4> McdrOitTileColor0V1 : register(t11, space2);
Texture2DArray<float4> McdrOitTileDepth0V1 : register(t12, space2);
Texture2DArray<float4> McdrOitTileCoeff00V1 : register(t13, space2);
Texture2DArray<float4> McdrOitTileCoeff01V1 : register(t14, space2);
Texture2DArray<float4> McdrOitTileCoeff02V1 : register(t15, space2);
Texture2DArray<float4> McdrOitTileCoeff03V1 : register(t16, space2);
Texture2DArray<float4> McdrOitTileQ0V1 : register(t17, space2);
Texture2DArray<float4> McdrOitTileColor1V1 : register(t18, space2);
Texture2DArray<float4> McdrOitTileDepth1V1 : register(t19, space2);
Texture2DArray<float4> McdrOitTileCoeff10V1 : register(t20, space2);
Texture2DArray<float4> McdrOitTileCoeff11V1 : register(t21, space2);
Texture2DArray<float4> McdrOitTileCoeff12V1 : register(t22, space2);
Texture2DArray<float4> McdrOitTileCoeff13V1 : register(t23, space2);
Texture2DArray<float4> McdrOitTileQ1V1 : register(t24, space2);
Texture2DArray<float4> McdrOitTileColor2V1 : register(t25, space2);
Texture2DArray<float4> McdrOitTileDepth2V1 : register(t26, space2);
Texture2DArray<float4> McdrOitTileCoeff20V1 : register(t27, space2);
Texture2DArray<float4> McdrOitTileCoeff21V1 : register(t28, space2);
Texture2DArray<float4> McdrOitTileCoeff22V1 : register(t29, space2);
Texture2DArray<float4> McdrOitTileCoeff23V1 : register(t30, space2);
Texture2DArray<float4> McdrOitTileQ2V1 : register(t31, space2);
Texture2DArray<float4> McdrOitTileColor3V1 : register(t32, space2);
Texture2DArray<float4> McdrOitTileDepth3V1 : register(t33, space2);
Texture2DArray<float4> McdrOitTileCoeff30V1 : register(t34, space2);
Texture2DArray<float4> McdrOitTileCoeff31V1 : register(t35, space2);
Texture2DArray<float4> McdrOitTileCoeff32V1 : register(t36, space2);
Texture2DArray<float4> McdrOitTileCoeff33V1 : register(t37, space2);
Texture2DArray<float4> McdrOitTileQ3V1 : register(t38, space2);

bool McdrOitTileEnabledV1() {
    return McdrOitTileInfoV1.x != 0u && McdrOitTileInfoV1.y > 0u &&
        McdrOitTileInfoV1.y <= 16u && McdrOitTileInfoV1.z == 15u;
}

bool McdrOitTileCoordinateV1(uint2 pixel, out uint slot, out int2 local) {
    slot = 0xffffffffu;
    local = 0;
    if (!McdrOitTileEnabledV1()) return false;
    for (uint index = 0u; index < McdrOitTileInfoV1.y; ++index) {
        uint4 region = McdrOitTileRegionsV1[index];
        if (pixel.x >= region.x && pixel.y >= region.y &&
            pixel.x < region.x + region.z && pixel.y < region.y + region.w) {
            slot = index;
            local = int2(pixel) - int2(region.xy) + 2;
            return all(local >= 0) && all(local < 68);
        }
    }
    return false;
}

struct McdrOitTileAttachmentsV1 {
    float4 color;
    float4 depth;
    float4 coeff0;
    float4 coeff1;
    float4 coeff2;
    float4 coeff3;
    float4 q;
};

bool McdrOitTileSampleV1(uint lane, int2 local, uint slot, out McdrOitTileAttachmentsV1 sample) {
    int4 coordinate = int4(local, int(slot), 0);
    if (lane == 0u) {
        sample.color = McdrOitTileColor0V1.Load(coordinate); sample.depth = McdrOitTileDepth0V1.Load(coordinate);
        sample.coeff0 = McdrOitTileCoeff00V1.Load(coordinate); sample.coeff1 = McdrOitTileCoeff01V1.Load(coordinate);
        sample.coeff2 = McdrOitTileCoeff02V1.Load(coordinate); sample.coeff3 = McdrOitTileCoeff03V1.Load(coordinate);
        sample.q = McdrOitTileQ0V1.Load(coordinate);
    } else if (lane == 1u) {
        sample.color = McdrOitTileColor1V1.Load(coordinate); sample.depth = McdrOitTileDepth1V1.Load(coordinate);
        sample.coeff0 = McdrOitTileCoeff10V1.Load(coordinate); sample.coeff1 = McdrOitTileCoeff11V1.Load(coordinate);
        sample.coeff2 = McdrOitTileCoeff12V1.Load(coordinate); sample.coeff3 = McdrOitTileCoeff13V1.Load(coordinate);
        sample.q = McdrOitTileQ1V1.Load(coordinate);
    } else if (lane == 2u) {
        sample.color = McdrOitTileColor2V1.Load(coordinate); sample.depth = McdrOitTileDepth2V1.Load(coordinate);
        sample.coeff0 = McdrOitTileCoeff20V1.Load(coordinate); sample.coeff1 = McdrOitTileCoeff21V1.Load(coordinate);
        sample.coeff2 = McdrOitTileCoeff22V1.Load(coordinate); sample.coeff3 = McdrOitTileCoeff23V1.Load(coordinate);
        sample.q = McdrOitTileQ2V1.Load(coordinate);
    } else {
        sample.color = McdrOitTileColor3V1.Load(coordinate); sample.depth = McdrOitTileDepth3V1.Load(coordinate);
        sample.coeff0 = McdrOitTileCoeff30V1.Load(coordinate); sample.coeff1 = McdrOitTileCoeff31V1.Load(coordinate);
        sample.coeff2 = McdrOitTileCoeff32V1.Load(coordinate); sample.coeff3 = McdrOitTileCoeff33V1.Load(coordinate);
        sample.q = McdrOitTileQ3V1.Load(coordinate);
    }
    return all(isfinite(sample.color)) && all(isfinite(sample.depth)) && all(isfinite(sample.coeff0)) &&
        all(isfinite(sample.coeff1)) && all(isfinite(sample.coeff2)) && all(isfinite(sample.coeff3)) &&
        all(isfinite(sample.q));
}

float McdrOitTileTotalAbsorbanceV1(McdrOitTileAttachmentsV1 sample, uint coefficientCount) {
    float coefficients[16] = {sample.coeff0.x, sample.coeff0.y, sample.coeff0.z, sample.coeff0.w,
        sample.coeff1.x, sample.coeff1.y, sample.coeff1.z, sample.coeff1.w,
        sample.coeff2.x, sample.coeff2.y, sample.coeff2.z, sample.coeff2.w,
        sample.coeff3.x, sample.coeff3.y, sample.coeff3.z, sample.coeff3.w};
    float absorbance = coefficients[0];
    if (absorbance == 0.0f) return 0.0f;
    uint treeIndex = coefficientCount * 2u - 1u;
    uint rank = coefficientCount == 4u ? 1u : coefficientCount == 8u ? 2u : 3u;
    for (uint waveletLevel = 0u; waveletLevel <= rank; ++waveletLevel) {
        uint power = rank - waveletLevel;
        float waveletHeightScale = exp2(float(power) * 0.5f);
        uint parent = treeIndex >> 1u;
        absorbance += waveletHeightScale * coefficients[parent];
        treeIndex = parent;
    }
    return absorbance;
}

bool McdrOitTileNormalizeV1(McdrOitTileAttachmentsV1 sample, out float4 normalized) {
    normalized = 0.0f.xxxx;
    uint coefficientCount = McdrOitTileInfoV1.w;
    if (coefficientCount != 4u && coefficientCount != 8u && coefficientCount != 16u) return false;
    float absorbance = McdrOitTileTotalAbsorbanceV1(sample, coefficientCount);
    if (!isfinite(absorbance)) return false;
    float transmittance = clamp(exp(-absorbance), 0.0001f, 1.0f);
    if (transmittance < 0.02f) transmittance = 0.0f;
    float coverage = 1.0f - transmittance;
    if (coverage < 1.0e-5f && dot(sample.q.rgb, 1.0f.xxx) < 1.0e-5f) return true;
    float normalization = sample.q.a > 1.0e-5f ? coverage / sample.q.a : 1.0f;
    normalized = float4(sample.q.rgb * normalization, coverage);
    return all(isfinite(normalized));
}

bool McdrOitTileCurrentV1(uint2 pixel, out float4 result) {
    result = 0.0f.xxxx;
    uint slot; int2 local;
    if (!McdrOitTileCoordinateV1(pixel, slot, local) || McdrOitTileInfoV1.w == 0u) return false;
    McdrOitTileAttachmentsV1 lane;
    float4 normalized;
    [unroll] for (uint index = 0u; index < 4u; ++index) {
        if (!McdrOitTileSampleV1(index, local, slot, lane) || !McdrOitTileNormalizeV1(lane, normalized)) return false;
        result += normalized;
    }
    result *= 0.25f;
    return all(isfinite(result));
}

bool McdrOitRecordRowsV5(uint4 identity, bool history, out uint primitiveAddress, out uint allowed) {
    primitiveAddress = 0u;
    allowed = 0u;
    if (identity.w >= (history ? McdrOitHistoryRows.x : McdrOitRowCounts.x)) return false;
    uint kind = history ? McdrOitHistorySourcesV3.Load(identity.w * 32u + 24u) :
        McdrOitSourcesV1.Load(identity.w * 32u + 24u);
    bool terrain = (identity.z & 0x80000000u) != 0u;
    if (terrain != (kind == 0u)) return false;
    if (terrain) {
        uint local = identity.z & 0x7fffffffu;
        uint historyBytes = 0u;
        if (history) McdrOitHistoryTerrainV2.GetDimensions(historyBytes);
        uint lower = 0u, upper = history ? historyBytes / 80u : McdrOitTerrainRows.x;
        while (lower < upper) {
            uint middle = lower + (upper - lower) / 2u;
            uint2 range = history ? McdrOitHistoryTerrainV2.Load2(middle * 80u + 48u) : McdrOitTerrainV2.Load2(middle * 80u + 48u);
            if (local < range.x) upper = middle;
            else if (local - range.x >= range.y) lower = middle + 1u;
            else {
                primitiveAddress = middle * 80u;
                uint2 tail = history ? McdrOitHistoryTerrainV2.Load2(primitiveAddress + 40u) : McdrOitTerrainV2.Load2(primitiveAddress + 40u);
                allowed = tail.y & 15u;
                return tail.x == identity.w;
            }
        }
        return false;
    }
    if (identity.z >= (history ? McdrOitHistoryRows.y : McdrOitRowCounts.y)) return false;
    primitiveAddress = identity.z * 56u;
    uint4 tail = history ? McdrOitHistoryPrimitivesV3.Load4(primitiveAddress + 40u) : McdrOitPrimitivesV1.Load4(primitiveAddress + 40u);
    allowed = tail.z & 15u;
    if (tail.y == 0xffffffffu) allowed &= ~6u;
    else if (!history) {
        if (tail.y >= McdrOitRowCounts.z) return false;
        allowed &= ~6u | McdrOitPreviousV1.Load(tail.y * 80u + 72u);
    }
    return tail.x == identity.w;
}

[numthreads(8,8,1)]
void InitializeRecordsV1(uint3 thread : SV_DispatchThreadID) {
    if (any(thread.xy >= McdrOitExtent.xy)) return;
    uint pixel = thread.y * McdrOitExtent.x + thread.x;
    McdrOitPixelsV1.Store2(pixel * MCDR_OIT_PIXEL_STRIDE, uint2(0xffffffffu, 0u));
    if (pixel == 0u) McdrOitAllocatorV1.Store4(0u, 0u.xxxx);
}

[numthreads(8,8,1)]
void CompactRecordsV1(uint3 thread : SV_DispatchThreadID) {
    if (any(thread.xy >= McdrOitExtent.xy)) return;
    uint pixel = thread.y * McdrOitExtent.x + thread.x;
    uint3 state = McdrOitUnpackPixelV4(McdrOitPixelsV1.Load2(pixel * MCDR_OIT_PIXEL_STRIDE));
    uint status = state.z;
    if (McdrOitDraw.w != 0u || McdrOitUnrecordedCoverageV2.Load(int3(thread.xy, 0)) > 0.0f) status |= 2u;
    if (!all(isfinite(McdrOitCurrentV1.Load(int3(thread.xy, 0))))) status |= 16u;
    if (state.y == 0u) status |= 32u;
    if (state.y > McdrOitExtent.w || state.y > McdrOitExtent.z) status |= 8u;
    uint offset = 0u;
    uint count = min(state.y, McdrOitExtent.w);
    McdrOitAllocatorV1.InterlockedAdd(4u, count, offset);
    if (offset > McdrOitExtent.z || count > McdrOitExtent.z - min(offset, McdrOitExtent.z)) {
        status |= 1u; count = 0u;
    }
    uint index = state.x;
    for (uint record = 0u; record < count; ++record) {
        if (index >= McdrOitExtent.z) { status |= 8u; break; }
        uint address = index * MCDR_OIT_RECORD_STRIDE;
        uint4 identity = McdrOitRecordsV1.Load4(address);
        uint primitiveAddress, allowed;
        if (identity.y != pixel || identity.w >= McdrOitRowCounts.x ||
            !McdrOitRecordRowsV5(identity, false, primitiveAddress, allowed)) {
            status |= 8u; break;
        }
        uint4 producer = McdrOitSourcesV1.Load4(identity.w * 32u);
        if (all(producer.xy == 0u) || all(producer.zw == 0u)) status |= 2u;
        uint validity = McdrOitRecordsV1.Load(address + 40u);
        allowed &= McdrOitSourcesV1.Load(identity.w * 32u + 28u);
        if ((validity & ~allowed) != 0u) status |= 2u;
        validity &= allowed;
        float2 geometryUv = asfloat(McdrOitRecordsV1.Load2(address + 72u));
        float geometryDepth = asfloat(McdrOitRecordsV1.Load(address + 44u));
        float2 materialUv = asfloat(McdrOitRecordsV1.Load2(address + 80u));
        float materialDepth = asfloat(McdrOitRecordsV1.Load(address + 88u));
        if (!McdrOitPreviousSampleValidV2(geometryUv, geometryDepth)) validity &= ~MCDR_OIT_GEOMETRY_PREVIOUS_VALID;
        if (!McdrOitPreviousSampleValidV2(materialUv, materialDepth)) validity &= ~MCDR_OIT_MATERIAL_PREVIOUS_VALID;
        McdrOitRecordsV1.Store(address + 40u, validity);
        if ((validity & 9u) != 9u) status |= 4u;
        if ((validity & 6u) != 6u) status |= 64u;
        McdrOitAllocatorV1.Store(16u + (offset + record) * 4u, index);
        index = identity.x;
    }
    if (index != 0xffffffffu) status |= 8u;
    McdrOitPixelsV1.Store(pixel * MCDR_OIT_PIXEL_STRIDE + 4u,
        state.y | (status << MCDR_OIT_PIXEL_STATUS_SHIFT));
    McdrOitStatusV1[thread.xy] = status;
}

bool McdrOitSameSurfaceV3(uint4 currentIdentity, uint4 candidateIdentity, bool history) {
    bool terrain = (currentIdentity.z & 0x80000000u) != 0u;
    if (terrain != ((candidateIdentity.z & 0x80000000u) != 0u)) return false;
    if (terrain) {
        uint address, candidateAddress, allowed, candidateAllowed;
        if (!McdrOitRecordRowsV5(currentIdentity, false, address, allowed) ||
            !McdrOitRecordRowsV5(candidateIdentity, history, candidateAddress, candidateAllowed)) return false;
        uint4 owner0 = McdrOitTerrainV2.Load4(address);
        uint4 owner1 = McdrOitTerrainV2.Load4(address + 16u);
        uint2 owner2 = McdrOitTerrainV2.Load2(address + 32u);
        uint4 other0 = history ? McdrOitHistoryTerrainV2.Load4(candidateAddress) : McdrOitTerrainV2.Load4(candidateAddress);
        uint4 other1 = history ? McdrOitHistoryTerrainV2.Load4(candidateAddress + 16u) : McdrOitTerrainV2.Load4(candidateAddress + 16u);
        uint2 other2 = history ? McdrOitHistoryTerrainV2.Load2(candidateAddress + 32u) : McdrOitTerrainV2.Load2(candidateAddress + 32u);
        uint candidateBase = history ? McdrOitHistoryTerrainV2.Load(candidateAddress + 48u) : McdrOitTerrainV2.Load(candidateAddress + 48u);
        uint4 source = McdrOitSourcesV1.Load4(currentIdentity.w * 32u);
        uint4 otherSource = history ? McdrOitHistorySourcesV3.Load4(candidateIdentity.w * 32u) : McdrOitSourcesV1.Load4(candidateIdentity.w * 32u);
        uint3 sourceTail = McdrOitSourcesV1.Load3(currentIdentity.w * 32u + 16u);
        uint3 otherTail = history ? McdrOitHistorySourcesV3.Load3(candidateIdentity.w * 32u + 16u) : McdrOitSourcesV1.Load3(candidateIdentity.w * 32u + 16u);
        return all(owner0 == other0) && all(owner1 == other1) && all(owner2 == other2) &&
            all(source == otherSource) && all(sourceTail == otherTail) &&
            (currentIdentity.z & 0x7fffffffu) - McdrOitTerrainV2.Load(address + 48u) ==
            (candidateIdentity.z & 0x7fffffffu) - candidateBase;
    }
    uint sourceAddress = currentIdentity.w * 32u;
    uint primitiveAddress = currentIdentity.z * 56u;
    uint candidateSourceAddress = candidateIdentity.w * 32u;
    uint candidatePrimitiveAddress = candidateIdentity.z * 56u;
    uint4 candidateSource = history ? McdrOitHistorySourcesV3.Load4(candidateSourceAddress) : McdrOitSourcesV1.Load4(candidateSourceAddress);
    uint4 candidateSourceTail = history ? McdrOitHistorySourcesV3.Load4(candidateSourceAddress + 16u) : McdrOitSourcesV1.Load4(candidateSourceAddress + 16u);
    uint4 candidatePrimitive0 = history ? McdrOitHistoryPrimitivesV3.Load4(candidatePrimitiveAddress) : McdrOitPrimitivesV1.Load4(candidatePrimitiveAddress);
    uint4 candidatePrimitive1 = history ? McdrOitHistoryPrimitivesV3.Load4(candidatePrimitiveAddress + 16u) : McdrOitPrimitivesV1.Load4(candidatePrimitiveAddress + 16u);
    uint2 candidateTexture = history ? McdrOitHistoryPrimitivesV3.Load2(candidatePrimitiveAddress + 32u) : McdrOitPrimitivesV1.Load2(candidatePrimitiveAddress + 32u);
    return all(McdrOitSourcesV1.Load4(sourceAddress) == candidateSource) &&
        all(McdrOitSourcesV1.Load4(sourceAddress + 16u).xyz == candidateSourceTail.xyz) &&
        all(McdrOitPrimitivesV1.Load4(primitiveAddress) == candidatePrimitive0) &&
        all(McdrOitPrimitivesV1.Load4(primitiveAddress + 16u) == candidatePrimitive1) &&
        all(McdrOitPrimitivesV1.Load2(primitiveAddress + 32u) == candidateTexture);
}

bool McdrOitHistoryCandidateV3(uint4 currentIdentity, uint address, int2 position, float2 projected,
    float expectedDepth, bool materialDomain, out uint matchedAddress) {
    matchedAddress = 0xffffffffu;
    if (any(position < 0) || any(position >= int2(McdrOitHistoryExtent.xy))) return false;
    uint pixel = uint(position.y) * McdrOitHistoryExtent.x + uint(position.x);
    uint3 state = McdrOitUnpackPixelV4(McdrOitHistoryPixelsV3.Load2(pixel * MCDR_OIT_PIXEL_STRIDE));
    if ((state.z & 31u) != 0u || state.y == 0u || state.y > McdrOitHistoryExtent.w) return false;
    uint index = state.x;
    uint matches = 0u;
    for (uint record = 0u; record < state.y; ++record) {
        if (index >= McdrOitHistoryExtent.z) return false;
        uint candidateAddress = index * MCDR_OIT_RECORD_STRIDE;
        uint4 candidate = McdrOitHistoryRecordsV3.Load4(candidateAddress);
        uint candidatePrimitive, candidateAllowed;
        if (candidate.y != pixel || candidate.w >= McdrOitHistoryRows.x ||
            !McdrOitRecordRowsV5(candidate, true, candidatePrimitive, candidateAllowed)) return false;
        if ((McdrOitHistoryRecordsV3.Load(candidateAddress + 40u) & 9u) == 9u &&
            McdrOitSameSurfaceV3(currentIdentity, candidate, true)) {
            float4 shape = asfloat(McdrOitHistoryRecordsV3.Load4(candidateAddress + 96u));
            float depth = asfloat(McdrOitHistoryRecordsV3.Load(candidateAddress + 32u));
            float2 displacement = projected - (float2(position) + 0.5f);
            float predictedDepth = depth + dot(shape.zw, displacement);
            bool valid = isfinite(predictedDepth) && abs(predictedDepth - expectedDepth) <= 2.0e-5f +
                0.25f * dot(abs(shape.zw), 1.0f.xx);
            float coverage = asfloat(McdrOitHistoryRecordsV3.Load(candidateAddress + 36u));
            valid = valid && abs(coverage - asfloat(McdrOitRecordsV1.Load(address + 36u))) <= 0.125f;
            if (materialDomain) {
                float4 footprint = asfloat(McdrOitHistoryRecordsV3.Load4(candidateAddress + 48u));
                float2 dy = asfloat(McdrOitHistoryRecordsV3.Load2(candidateAddress + 64u));
                float2 expectedUv = footprint.xy + footprint.zw * displacement.x + dy * displacement.y;
                float2 currentUv = asfloat(McdrOitRecordsV1.Load2(address + 48u));
                float2 tolerance = 0.5f * (abs(footprint.zw) + abs(dy)) + 1.0e-5f.xx;
                valid = valid && all(abs(currentUv - expectedUv) <= tolerance);
            }
            if (valid) { ++matches; matchedAddress = candidateAddress; }
        }
        index = candidate.x;
    }
    return index == 0xffffffffu && matches == 1u;
}

bool McdrOitHistoryRgbV3(uint4 identity, uint address, out float3 stableRgb, out float historyLength) {
    stableRgb = asfloat(McdrOitRecordsV1.Load3(address + 16u));
    historyLength = 1.0f;
    if (McdrOitHistoryRows.w == 0u || (McdrOitRecordsV1.Load(address + 40u) & 15u) != 15u) return false;
    float2 geometry = asfloat(McdrOitRecordsV1.Load2(address + 72u)) * float2(McdrOitHistoryExtent.xy);
    float geometryDepth = asfloat(McdrOitRecordsV1.Load(address + 44u));
    float2 material = asfloat(McdrOitRecordsV1.Load2(address + 80u)) * float2(McdrOitHistoryExtent.xy);
    float materialDepth = asfloat(McdrOitRecordsV1.Load(address + 88u));
    float4 projection = asfloat(McdrOitRecordsV1.Load4(address + 128u));
    float2 projectedDx = projection.xy * float2(McdrOitHistoryExtent.xy);
    float2 projectedDy = projection.zw * float2(McdrOitHistoryExtent.xy);
    float determinant = abs(projectedDx.x * projectedDy.y - projectedDx.y * projectedDy.x);
    if (!all(isfinite(projection)) || determinant < 0.25f || determinant > 4.0f ||
        max(length(projectedDx), length(projectedDy)) > 4.0f) return false;
    int2 geometryBase = int2(floor(geometry - 0.5f));
    bool geometrySupported = false;
    for (uint geometryTap = 0u; geometryTap < 4u; ++geometryTap) {
        uint matched;
        geometrySupported = McdrOitHistoryCandidateV3(identity, address,
            geometryBase + int2(geometryTap & 1u, geometryTap >> 1u), geometry, geometryDepth, false, matched) || geometrySupported;
    }
    if (!geometrySupported) return false;
    int2 materialBase = int2(floor(material - 0.5f));
    float2 fraction = frac(material - 0.5f);
    float3 historyRgb = 0.0f.xxx;
    float lengthSum = 0.0f;
    float weightSum = 0.0f;
    for (uint materialTap = 0u; materialTap < 4u; ++materialTap) {
        uint2 offset = uint2(materialTap & 1u, materialTap >> 1u);
        float2 weights = lerp(1.0f.xx - fraction, fraction, float2(offset));
        float weight = weights.x * weights.y;
        if (weight <= 1.0e-5f) continue;
        uint matched;
        if (!McdrOitHistoryCandidateV3(identity, address, materialBase + int2(offset), material, materialDepth, true, matched)) continue;
        float4 value = asfloat(McdrOitHistoryRecordsV3.Load4(matched + 112u));
        if (!all(isfinite(value)) || value.w < 1.0f || value.w > 8.0f) continue;
        historyRgb += value.xyz * weight;
        lengthSum += value.w * weight;
        weightSum += weight;
    }
    if (weightSum < 0.5f) return false;
    stableRgb = historyRgb / weightSum;
    historyLength = lengthSum / weightSum;
    return true;
}

void McdrOitCurrentRgbBoundsV3(uint4 identity, int2 position, inout float3 lower, inout float3 upper) {
    for (int offsetY = -1; offsetY <= 1; ++offsetY) {
        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
            int2 samplePosition = position + int2(offsetX, offsetY);
            if (any(samplePosition < 0) || any(samplePosition >= int2(McdrOitExtent.xy))) continue;
            uint pixel = uint(samplePosition.y) * McdrOitExtent.x + uint(samplePosition.x);
            uint3 state = McdrOitUnpackPixelV4(McdrOitPixelsV1.Load2(pixel * MCDR_OIT_PIXEL_STRIDE));
            if ((state.z & 31u) != 0u || state.y > McdrOitExtent.w) continue;
            uint index = state.x;
            for (uint record = 0u; record < state.y; ++record) {
                if (index >= McdrOitExtent.z) break;
                uint address = index * MCDR_OIT_RECORD_STRIDE;
                uint4 candidate = McdrOitRecordsV1.Load4(address);
                uint candidatePrimitive, candidateAllowed;
                if (candidate.y != pixel || candidate.w >= McdrOitRowCounts.x ||
                    !McdrOitRecordRowsV5(candidate, false, candidatePrimitive, candidateAllowed)) break;
                if ((McdrOitRecordsV1.Load(address + 40u) & 9u) == 9u && McdrOitSameSurfaceV3(identity, candidate, false)) {
                    float3 rgb = asfloat(McdrOitRecordsV1.Load3(address + 16u));
                    lower = min(lower, rgb); upper = max(upper, rgb);
                }
                index = candidate.x;
            }
        }
    }
}

[numthreads(8,8,1)]
void ResolveRgbHistoryV3(uint3 thread : SV_DispatchThreadID) {
    if (any(thread.xy >= McdrOitExtent.xy)) return;
    uint pixel = thread.y * McdrOitExtent.x + thread.x;
    uint3 state = McdrOitUnpackPixelV4(McdrOitPixelsV1.Load2(pixel * MCDR_OIT_PIXEL_STRIDE));
    uint status = state.z;
    float4 current = McdrOitCurrentV1.Load(int3(thread.xy, 0));
    float4 tileCurrent;
    if (McdrOitTileCurrentV1(thread.xy, tileCurrent)) current = tileCurrent;
    float3 correction = 0.0f.xxx;
    if ((status & 63u) == 0u) {
        uint index = state.x;
        for (uint record = 0u; record < state.y; ++record) {
            uint address = index * MCDR_OIT_RECORD_STRIDE;
            uint4 identity = McdrOitRecordsV1.Load4(address);
            float3 material = asfloat(McdrOitRecordsV1.Load3(address + 16u));
            float3 stable;
            float length;
            bool accepted = McdrOitHistoryRgbV3(identity, address, stable, length);
            if (accepted) {
                float3 lower = material, upper = material;
                McdrOitCurrentRgbBoundsV3(identity, int2(thread.xy), lower, upper);
                stable = clamp(stable, lower, upper);
                stable = lerp(material, stable, min(length / (length + 1.0f), 0.875f));
                length = min(length + 1.0f, 8.0f);
                status |= 256u;
            } else {
                stable = material; length = 1.0f; status |= 128u;
            }
            McdrOitRecordsV1.Store4(address + 112u, asuint(float4(stable, length)));
            float transmittance = asfloat(McdrOitRecordsV1.Load(address + 28u));
            float fogFactor = asfloat(McdrOitRecordsV1.Load(address + 92u));
            float4 shape = asfloat(McdrOitRecordsV1.Load4(address + 96u));
            float gain = transmittance * shape.y * (1.0f - fogFactor) * shape.x;
            correction += gain * (stable - material);
            index = identity.x;
        }
    }
    float3 corrected = current.rgb + correction;
    if (!all(isfinite(corrected))) { corrected = current.rgb; status |= 16u; }
    McdrOitCorrectedV3[thread.xy] = float4(corrected, current.a);
    McdrOitStatusV1[thread.xy] = status;
}
)HLSL";
}
