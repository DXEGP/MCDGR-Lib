ByteAddressBuffer mcdrReadDependencies : register(t0, space3);
RWByteAddressBuffer mcdrWriteDependencies : register(u0, space3);
cbuffer McdrDependencyLayout : register(b0, space3) {
    uint4 mcdrResourceLayout[32];
};

static uint3 mcdrThreadMinimum = 0xFFFFFFFFu;
static uint3 mcdrThreadMaximum = 0u;
static uint mcdrThreadState = 0u;
groupshared uint mcdrGroupMinimum[3];
groupshared uint mcdrGroupMaximum[3];
groupshared uint mcdrGroupState;
groupshared uint mcdrOutputMinimum[16];
groupshared uint mcdrOutputMaximum[16];
static bool mcdrOutputWritten[8];

void McdrInclude(uint resourceIndex, int2 position, bool clampPosition) {
    uint4 layout = mcdrResourceLayout[resourceIndex];
    if (layout.x == 0xFFFFFFFFu) return;
    if (clampPosition) position = clamp(position, 0, int2(layout.yz) - 1);
    else if (any(position < 0) || any(position >= int2(layout.yz))) return;
    uint columns = (layout.y + layout.w - 1u) / layout.w;
    uint tile = (uint(position.y) / layout.w) * columns + uint(position.x) / layout.w;
    uint address = layout.x + tile * 32u;
    uint state = mcdrReadDependencies.Load(address + 12u);
    if (state == 0u) return;
    mcdrThreadMinimum = min(mcdrThreadMinimum, mcdrReadDependencies.Load3(address));
    mcdrThreadMaximum = max(mcdrThreadMaximum, mcdrReadDependencies.Load3(address + 16u));
    mcdrThreadState |= state;
}

void McdrSample(uint resourceIndex, float2 coordinate, int2 offset) {
    float2 scaled = coordinate * float2(mcdrResourceLayout[resourceIndex].yz) - 0.5;
    int2 position = int2(floor(clamp(scaled, -65536.0, 65536.0))) + offset;
    McdrInclude(resourceIndex, position, true);
    McdrInclude(resourceIndex, position + int2(1, 0), true);
    McdrInclude(resourceIndex, position + int2(0, 1), true);
    McdrInclude(resourceIndex, position + int2(1, 1), true);
}

void McdrWrite(uint resourceIndex, uint outputIndex, int2 position) {
    uint4 layout = mcdrResourceLayout[resourceIndex];
    if (layout.x == 0xFFFFFFFFu || any(position < 0) || any(position >= int2(layout.yz))) return;
    InterlockedMin(mcdrOutputMinimum[outputIndex * 2u], uint(position.x));
    InterlockedMin(mcdrOutputMinimum[outputIndex * 2u + 1u], uint(position.y));
    InterlockedMax(mcdrOutputMaximum[outputIndex * 2u], uint(position.x));
    InterlockedMax(mcdrOutputMaximum[outputIndex * 2u + 1u], uint(position.y));
    if (!mcdrOutputWritten[outputIndex]) {
        uint columns = (layout.y + layout.w - 1u) / layout.w;
        uint tile = (uint(position.y) / layout.w) * columns + uint(position.x) / layout.w;
        mcdrWriteDependencies.InterlockedAdd(layout.x + tile * 32u + 28u, 1u);
        mcdrOutputWritten[outputIndex] = true;
    }
}

void McdrBegin(uint threadIndex) {
    if (threadIndex == 0u) {
        for (uint axis = 0u; axis < 3u; ++axis) {
            mcdrGroupMinimum[axis] = 0xFFFFFFFFu;
            mcdrGroupMaximum[axis] = 0u;
        }
        mcdrGroupState = 0u;
        for (uint coordinate = 0u; coordinate < 16u; ++coordinate) {
            mcdrOutputMinimum[coordinate] = 0xFFFFFFFFu;
            mcdrOutputMaximum[coordinate] = 0u;
        }
    }
    GroupMemoryBarrierWithGroupSync();
}

void McdrEnd(uint threadIndex, uint inputs, uint outputs) {
    if (mcdrThreadState != 0u) {
        for (uint axis = 0u; axis < 3u; ++axis) {
            InterlockedMin(mcdrGroupMinimum[axis], mcdrThreadMinimum[axis]);
            InterlockedMax(mcdrGroupMaximum[axis], mcdrThreadMaximum[axis]);
        }
        InterlockedOr(mcdrGroupState, mcdrThreadState);
    }
    GroupMemoryBarrierWithGroupSync();
    if (threadIndex != 0u || mcdrGroupState == 0u) return;
    for (uint outputIndex = 0u; outputIndex < outputs; ++outputIndex) {
        uint4 layout = mcdrResourceLayout[inputs + outputIndex];
        uint2 minimum = uint2(mcdrOutputMinimum[outputIndex * 2u], mcdrOutputMinimum[outputIndex * 2u + 1u]);
        uint2 maximum = uint2(mcdrOutputMaximum[outputIndex * 2u], mcdrOutputMaximum[outputIndex * 2u + 1u]);
        if (layout.x == 0xFFFFFFFFu || any(minimum > maximum)) continue;
        uint columns = (layout.y + layout.w - 1u) / layout.w;
        minimum /= layout.w;
        maximum /= layout.w;
        for (uint tileY = minimum.y; tileY <= maximum.y; ++tileY) {
            for (uint tileX = minimum.x; tileX <= maximum.x; ++tileX) {
                uint address = layout.x + (tileY * columns + tileX) * 32u;
                for (uint axis = 0u; axis < 3u; ++axis) {
                    mcdrWriteDependencies.InterlockedMin(address + axis * 4u, mcdrGroupMinimum[axis]);
                    mcdrWriteDependencies.InterlockedMax(address + 16u + axis * 4u, mcdrGroupMaximum[axis]);
                }
                mcdrWriteDependencies.InterlockedOr(address + 12u, mcdrGroupState);
            }
        }
    }
}
