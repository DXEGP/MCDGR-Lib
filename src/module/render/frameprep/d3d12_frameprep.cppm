module;

#include "_d3d12_module_prelude.h"
#include <cstdint>

export module mcdr.render.frameprep;

import mcdr.d3d12.device;

export namespace d3d12_mc::frameprep {

struct MaskRequest {
    void* context = nullptr;
    void* source0 = nullptr;
    void* source1 = nullptr;
    void* output = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t worldEpoch = 0;
    uint64_t realFrameId = 0;
    uint64_t sourceVersion = 0;
    uint64_t viewportVersion = 0;
    void* compositionOutput = nullptr;
    uint32_t outputFlags = 1;
    const void* classification = nullptr;
    uint32_t inputFlags = 0;
    void* color = nullptr;
    void* depth = nullptr;
    void* motion = nullptr;
};

struct VrsRequest {
    void* context = nullptr;
    void* depth = nullptr;
    void* motion = nullptr;
    void* previous = nullptr;
    void* output = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t tileSize = 0;
    uint32_t edgePermille = 0;
    uint32_t motionPixels = 0;
    uint32_t flags = 0;
    uint64_t worldEpoch = 0;
    uint64_t realFrameId = 0;
    uint64_t sourceVersion = 0;
    uint64_t viewportVersion = 0;
    void* candidates = nullptr;
    const float* previousToCurrent = nullptr;
    const float* currentToPrevious = nullptr;
};

struct ExposureRequest {
    void* context = nullptr;
    void* color = nullptr;
    void* output = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t colorEncoding = 0;
    float preExposure = 1.0f;
};

int produceReactiveMask(const MaskRequest& request);
int produceProviderMasks(void* context, const void* input, uint32_t bytes);
int produceExposure(const ExposureRequest& request);
int produceVrs(const VrsRequest& request);
int prewarm(void* context, uint64_t generation, uint32_t flags);
int preparePrograms(void* context, uint64_t generation, uint32_t flags, void** prepared);
int validatePreparedPrograms(void* context, void* prepared);
void exchangePreparedPrograms(void* context, void* prepared) noexcept;
void destroyPreparedPrograms(void* prepared) noexcept;
void reset(void* context);

}
