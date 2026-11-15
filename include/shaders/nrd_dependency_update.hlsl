ByteAddressBuffer inputDependencies : register(t0, space3);
ByteAddressBuffer inputPaths : register(t1, space3);
ByteAddressBuffer inputChanges : register(t2, space3);
RWByteAddressBuffer outputDependencies : register(u0, space3);
RWTexture2D<uint> rejectionMask : register(u1, space3);
cbuffer DependencyUpdate : register(b0, space3) {
    uint mode;
    uint entryCount;
    uint readOffset;
    uint writeOffset;
    uint width;
    uint height;
    uint tileSize;
    uint lobe;
    uint pathOffset;
    uint changeOffset;
    uint changeCount;
    uint fullDomain;
    uint maskOffset;
    uint fullWidth;
    uint fullHeight;
    uint unusedConstant;
};

void Empty(uint address) {
    outputDependencies.Store4(address, uint4(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u));
    outputDependencies.Store4(address + 16u, 0u);
}

void Include(uint destination, uint source) {
    uint state = inputDependencies.Load(source + 12u);
    if (state == 0u) return;
    outputDependencies.Store3(destination, min(outputDependencies.Load3(destination), inputDependencies.Load3(source)));
    outputDependencies.Store3(destination + 16u, max(outputDependencies.Load3(destination + 16u), inputDependencies.Load3(source + 16u)));
    outputDependencies.Store(destination + 12u, outputDependencies.Load(destination + 12u) | state);
}

uint Ordered(float value) {
    uint bits = asuint(value);
    return bits ^ ((bits & 0x80000000u) != 0u ? 0xFFFFFFFFu : 0x80000000u);
}

bool Changed(uint address) {
    uint state = inputDependencies.Load(address + 12u);
    if (state == 0u) return false;
    if (fullDomain != 0u || (state & 2u) != 0u) return true;
    uint3 minimum = inputDependencies.Load3(address);
    uint3 maximum = inputDependencies.Load3(address + 16u);
    for (uint eventIndex = 0u; eventIndex < changeCount; ++eventIndex) {
        uint entry = changeOffset + 64u + eventIndex * 64u;
        if (inputChanges.Load(entry + 28u) == 0u) return true;
        uint2 node = inputChanges.Load2(entry);
        int3 section = int3(asint(node.y) >> 10, asint(node.x << 12u) >> 12,
            asint(((node.x >> 20u) | (node.y << 12u)) << 10u) >> 10);
        float3 origin = float3(section) * 16.0;
        float3 localMinimum = asfloat(inputChanges.Load3(entry + 32u));
        float3 localMaximum = asfloat(inputChanges.Load3(entry + 44u));
        float3 uncertainty = 8.0 * 1.192092896e-7 *
            (abs(origin) + max(abs(localMinimum), abs(localMaximum)) + 16.0);
        float3 changedMinimum = origin + localMinimum - uncertainty;
        float3 changedMaximum = origin + localMaximum + uncertainty;
        uint3 encodedMinimum = uint3(Ordered(changedMinimum.x), Ordered(changedMinimum.y), Ordered(changedMinimum.z));
        uint3 encodedMaximum = uint3(Ordered(changedMaximum.x), Ordered(changedMaximum.y), Ordered(changedMaximum.z));
        if (all(minimum <= encodedMaximum) && all(maximum >= encodedMinimum)) return true;
    }
    return false;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint index = dispatchId.x + dispatchId.y * (65535u * 64u);
    if (index >= entryCount) return;
    uint address = writeOffset + index * 32u;
    if (mode == 0u) {
        Empty(address);
    } else if (mode == 1u) {
        Empty(address);
        for (uint category = 0u; category < 4u; ++category) {
            bool selected = category == 0u || (lobe == 0u ? category == 1u || category == 2u : category == 3u);
            if (!selected || (lobe == 2u && category != 0u)) continue;
            uint source = pathOffset + 64u + index * 128u + category * 32u;
            uint state = inputPaths.Load(source + 12u);
            if (state == 0u) continue;
            outputDependencies.Store3(address, min(outputDependencies.Load3(address), inputPaths.Load3(source)));
            outputDependencies.Store3(address + 16u, max(outputDependencies.Load3(address + 16u), inputPaths.Load3(source + 16u)));
            outputDependencies.Store(address + 12u, outputDependencies.Load(address + 12u) | state);
        }
    } else if (mode == 2u) {
        uint columns = (width + tileSize - 1u) / tileSize;
        uint2 tile = uint2(index % columns, index / columns);
        uint2 size = min(tileSize, uint2(width, height) - tile * tileSize);
        if (outputDependencies.Load(address + 28u) != size.x * size.y) Include(address, readOffset + index * 32u);
        outputDependencies.Store(address + 28u, 0u);
    } else if (mode == 3u) {
        if (Changed(readOffset + index * 32u)) outputDependencies.InterlockedOr(maskOffset + index * 4u, 1u << lobe);
    } else if (mode == 4u) {
        outputDependencies.Store(maskOffset + index * 4u, 0u);
    } else if (mode == 5u) {
        uint2 pixel = uint2(index % fullWidth, index / fullWidth);
        uint columns = (fullWidth + 63u) / 64u;
        uint tile = (pixel.y / 64u) * columns + pixel.x / 64u;
        rejectionMask[pixel] = inputDependencies.Load(maskOffset + tile * 4u);
    } else if (mode == 6u) {
        if ((inputDependencies.Load(maskOffset + index * 4u) & (1u << lobe)) != 0u) Empty(address);
    }
}
