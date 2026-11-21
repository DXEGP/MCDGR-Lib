module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;

using namespace d3d12_mc;

extern "C" {

int32_t MCDR_CALL mcdr_dx12CreateDevice(char* buf, int32_t cap) {
    using mcdr::abi::writeString;

    const auto r = d3d12_mc::ensureDevice();
    if (!r) {
        return writeString("ERROR: " + r.error().message, buf, cap);
    }

    d3d12_mc::DeviceContext& ctx = d3d12_mc::deviceContext();
    std::string result = ctx.adapterName.empty() ? "<unknown adapter>" : ctx.adapterName;
    result += " (";
    result += d3d12_mc::featureLevelName(ctx.featureLevel);
    result += "; ";
    result += d3d12_mc::capabilitiesText(ctx.caps);
    result += ")";
    return writeString(result, buf, cap);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateTexture(int32_t usage, int32_t format, int32_t width, int32_t height,
                                              int32_t depthOrLayers, int32_t mipLevels) {
    const auto r = d3d12_mc::createTexture(usage, format, width, height, depthOrLayers, mipLevels);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateTexture", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateBuffer(int32_t usage, int64_t size) {
    const auto r = d3d12_mc::createBuffer(usage, size);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateBuffer", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateSampler(int32_t addressU, int32_t addressV, int32_t minFilter,
                                              int32_t magFilter, int32_t maxAnisotropy, float maxLod) {
    const auto r = d3d12_mc::createSampler(addressU, addressV, minFilter, magFilter, maxAnisotropy, maxLod);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateSampler", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateSamplerWithMipBias(int32_t addressU, int32_t addressV, int32_t minFilter,
                                                         int32_t magFilter, int32_t maxAnisotropy,
                                                         float maxLod, float mipLodBias) {
    const auto r = d3d12_mc::createSamplerWithMipBias(addressU, addressV, minFilter, magFilter, maxAnisotropy,
                                                      maxLod, mipLodBias);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateSamplerWithMipBias",
                                                            r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateTextureView(mcdr_object* texture, int32_t baseMipLevel,
                                                  int32_t mipLevels) {
    const auto r = d3d12_mc::createTextureView(texture, baseMipLevel, mipLevels);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateTextureView", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

void MCDR_CALL mcdr_dx12DestroyResource(mcdr_object* object) { d3d12_mc::destroyObject(object); }

mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipeline(const void* dxil, int32_t dxilSize) {
    const auto r = d3d12_mc::createComputePipeline(dxil, dxilSize);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateComputePipeline",
                                                            r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateTerrainTileBinningPipeline(const void* dxil, int32_t dxilSize) {
    const auto r = d3d12_mc::createTerrainTileBinningPipeline(dxil, dxilSize);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateTerrainTileBinningPipeline",
                                                            r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineTexture3(const void* dxil, int32_t dxilSize) {
    return mcdr_dx12CreateComputePipelineShadingRate(dxil, dxilSize);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineShadingRate(const void* dxil, int32_t dxilSize) {
    const auto r = d3d12_mc::createComputePipelineShadingRate(dxil, dxilSize);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateComputePipelineShadingRate",
                                                             r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateShadingRateImage(int32_t width, int32_t height) {
    const auto r = d3d12_mc::createShadingRateImage(width, height);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateShadingRateImage",
                                                             r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}
void* MCDR_CALL mcdr_dx12MapBuffer(mcdr_object* buffer, int64_t offset, int64_t length, int32_t read,
                                   int32_t write, int64_t* outLength) {
    if (outLength) {
        *outLength = 0;
    }
    const auto r = d3d12_mc::mapBuffer(buffer, offset, length, read != 0, write != 0);
    if (!r) {
        mcdr::abi::failVoid("mcdr_dx12MapBuffer", r.error().message);
        return nullptr;
    }
    if (outLength) {
        *outLength = length;
    }
    return *r;
}

void MCDR_CALL mcdr_dx12UnmapBuffer(mcdr_object* buffer) { d3d12_mc::unmapBuffer(buffer); }

int32_t MCDR_CALL mcdr_dx12EnumerateAdapters(char* buf, int32_t cap) {
    return mcdr::abi::writeString(d3d12_mc::enumerateAdaptersJson(), buf, cap);
}
int32_t MCDR_CALL mcdr_dx12GetAdapterInfo(char* buf, int32_t cap) {
    const std::string json = d3d12_mc::adapterInfoJson();
    if (json.empty()) {
        return 0;
    }
    if (buf == nullptr || cap <= 0) {
        return static_cast<int32_t>(json.size()) + 1;
    }
    mcdr::abi::writeString(json, buf, cap);
    return static_cast<int32_t>(json.size());
}

uint64_t MCDR_CALL mcdr_dx12GetQueueHandle(void) { return static_cast<uint64_t>(d3d12_mc::getQueueHandle()); }

uint64_t MCDR_CALL mcdr_dx12GetDeviceHandle(void) {
    return static_cast<uint64_t>(d3d12_mc::getDeviceHandle());
}

void MCDR_CALL mcdr_dx12DestroyDevice(void) { d3d12_mc::destroyDevice(); }
int32_t MCDR_CALL mcdr_dx12DestroyDeviceChecked(void) {
    auto result = d3d12_mc::destroyDeviceChecked();
    if (!result) return mcdr::abi::failWith("mcdr_dx12DestroyDeviceChecked", result.error().message);
    return 1;
}

uint64_t MCDR_CALL mcdr_dx12CreateHiddenWindow(int32_t width, int32_t height) {
    return static_cast<uint64_t>(d3d12_mc::createHiddenWindow(width, height));
}

void MCDR_CALL mcdr_dx12DestroyHiddenWindow(uint64_t hwnd) {
    d3d12_mc::destroyHiddenWindow(static_cast<uintptr_t>(hwnd));
}

}
