#pragma once
#include "mcdr_oit_shader.h"
#include "mcdr_abi.h"

namespace mcdr::graphics_capture {
inline bool valid(const mcdr_oit_capture_pipeline_v1* capture) {
    return capture && capture->schemaVersion == 1 && capture->structSize == sizeof(*capture) &&
        capture->shaderAbiVersion == oit_shader::abiVersion && capture->shaderContractHash == oit_shader::contractHash &&
        capture->flags == 1 && capture->programGeneration != 0;
}
inline UINT appendParameters(std::vector<D3D12_ROOT_PARAMETER>& parameters,
                             D3D12_DESCRIPTOR_RANGE (&ranges)[2]) {
    const auto base = static_cast<UINT>(parameters.size());
    D3D12_ROOT_PARAMETER constants{};
    constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    constants.Descriptor = {0, 2};
    constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters.push_back(constants);
    for (UINT table = 0; table != 2; ++table) {
        ranges[table].RangeType = table == 0 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[table].NumDescriptors = table == 0 ? 11 : 3;
        ranges[table].RegisterSpace = 2;
        ranges[table].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &ranges[table]};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters.push_back(parameter);
    }
    return base;
}
}
