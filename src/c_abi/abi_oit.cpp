module;
#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include "mcdr_terrain_tile.h"
module mcdr.abi.common;
import mcdr.d3d12.device;
import mcdr.render.oit;
namespace {
template<class Operation> int32_t callOit(const char* name, Operation&& operation) {
    try {
        const auto result=operation();
        if (result<1) mcdr::abi::setLastErrorf(name,"OIT request failed (%d)",result);
        return result;
    } catch (const std::bad_alloc&) {
        mcdr::abi::setLastError(name,"OIT allocation failed");
        return MCDR_OIT_RESULT_BUDGET_EXCEEDED;
    } catch (const std::exception& exception) {
        mcdr::abi::setLastError(name,exception.what());
        return MCDR_OIT_RESULT_INVALID_REQUEST;
    } catch (...) {
        mcdr::abi::setLastError(name,"OIT operation failed");
        return MCDR_OIT_RESULT_INVALID_REQUEST;
    }
}
}
extern "C" {
int32_t MCDR_CALL mcdr_dx12CaptureSampledTextureContentV1(
    mcdr_command_ctx* context, mcdr_object* view, uint64_t totalBudgetBytes,
    uint64_t* ownerIdentity, uint64_t* ownerGeneration, uint64_t* contentVersion,
    uint32_t* firstMip, uint32_t* mipCount, uint32_t* firstLayer, uint32_t* layerCount) {
    return callOit("oit.sampled.capture", [&] {
        return d3d12_mc::captureSampledTextureContentV1(context, view, totalBudgetBytes,
            ownerIdentity, ownerGeneration, contentVersion, firstMip, mipCount, firstLayer, layerCount);
    });
}
int32_t MCDR_CALL mcdr_dx12OitShaderPreludeV1(char* destination, uint32_t capacity, uint32_t* required, uint32_t* abi, uint64_t* hash) {
    return callOit("oit.prelude",[&] { return d3d12_mc::oit::shaderPrelude(destination,capacity,required,abi,hash); });
}
int32_t MCDR_CALL mcdr_dx12OitBeginFrameV1(mcdr_command_ctx* context, const mcdr_oit_begin_v1* input, void** frame) {
    if (frame) *frame=nullptr;
    return callOit("oit.begin",[&] { return input ? d3d12_mc::oit::beginFrame(context,*input,frame) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitBeginFrameV2(mcdr_command_ctx* context, const mcdr_oit_begin_v2* input, void** frame) {
    if (frame) *frame=nullptr;
    return callOit("oit.begin",[&] { return input ? d3d12_mc::oit::beginTerrainFrame(context,*input,frame) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitPublishTerrainMetadataV2(mcdr_command_ctx* context, void* frame, const mcdr_oit_terrain_metadata_v2* input, void** metadata) {
    if (metadata) *metadata=nullptr;
    return callOit("oit.terrain",[&] { return input ? d3d12_mc::oit::publishTerrainMetadata(context,frame,*input,metadata) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitBindTerrainDrawV2(mcdr_command_ctx* context, void* frame, void* metadata, const mcdr_oit_terrain_draw_v2* input) {
    return callOit("oit.bind",[&] { return input ? d3d12_mc::oit::bindTerrainDraw(context,frame,metadata,*input) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitPublishMetadataV1(mcdr_command_ctx* context, void* frame, const mcdr_oit_metadata_v1* input, void** metadata) {
    if (metadata) *metadata=nullptr;
    return callOit("oit.metadata",[&] { return input ? d3d12_mc::oit::publishMetadata(context,frame,*input,metadata) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitBindDrawV1(mcdr_command_ctx* context, void* frame, void* metadata, const mcdr_oit_draw_v1* input) {
    return callOit("oit.bind",[&] { return input ? d3d12_mc::oit::bindDraw(context,frame,metadata,*input) : -1; });
}
int32_t MCDR_CALL mcdr_dx12OitUnbindDrawV1(mcdr_command_ctx* context, void* frame) {
    return callOit("oit.unbind",[&] { return d3d12_mc::oit::unbindDraw(context,frame); });
}
int32_t MCDR_CALL mcdr_dx12OitSealFrameV1(mcdr_command_ctx* context, void* frame) {
    return callOit("oit.seal",[&] { return d3d12_mc::oit::sealFrame(context,frame); });
}
int32_t MCDR_CALL mcdr_dx12OitAbortFrameV1(mcdr_command_ctx* context, void* frame) {
    return callOit("oit.abort",[&] { return d3d12_mc::oit::abortFrame(context,frame); });
}
int32_t MCDR_CALL mcdr_dx12OitResolveRecordsV3(mcdr_command_ctx* context, const mcdr_oit_resolve_v3* input) {
    return callOit("oit.resolve",[&] { return input ? d3d12_mc::oit::resolveRecords(context,*input) : -1; });
}
}
