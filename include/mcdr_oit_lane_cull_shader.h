#pragma once

#include <string_view>

namespace mcdr::oit_shader {

inline constexpr std::string_view laneCull = R"(
Texture2DArray<float4> originalBounds : register(t0);

cbuffer Projection : register(b0) {
    float4 projectionColumns[4];
};

struct CullBoundsVaryings {
    float4 position : SV_Position;
    nointerpolation uint slice : SV_RenderTargetArrayIndex;
};

struct CullBoundsOutput {
    float4 bounds : SV_Target0;
    float depth : SV_Depth;
};

CullBoundsVaryings CullBoundsVertex(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    const float2 uv = float2((vertexId << 1) & 2u, vertexId & 2u);
    CullBoundsVaryings output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.slice = instanceId;
    return output;
}

CullBoundsOutput CullBoundsPixel(CullBoundsVaryings input) {
    const float4 bounds = originalBounds.Load(int4(int2(input.position.xy), int(input.slice), 0));
    const float opaqueDeviceDepth = bounds.a;
    const float opaqueLinearDepth = projectionColumns[3].z / (opaqueDeviceDepth + projectionColumns[2].z);

    CullBoundsOutput output;
    output.bounds = float4(bounds.r, min(bounds.g, opaqueLinearDepth), bounds.b, opaqueDeviceDepth);
    output.depth = opaqueDeviceDepth;
    return output;
}
)";

}
