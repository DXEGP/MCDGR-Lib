module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include "mcdr_terrain_tile.h"

export module mcdr.render.oit;

import mcdr.d3d12.device;

export namespace d3d12_mc::oit {
enum class TerrainDrawContent { OriginalDrawOnly, UnsupportedContent };

struct TerrainDrawSectionIdentity {
    uint64_t surfaceId = 0;
    uint64_t geometryGeneration = 0;
    uint64_t materialId = 0;
    uint64_t materialGeneration = 0;
    uint64_t textureGeneration = 0;
    uint32_t vertexBase = 0;
    uint32_t vertexCount = 0;
    uint32_t primitiveBase = 0;
    uint32_t primitiveCount = 0;
};

struct TerrainDrawIdentity {
    uint32_t schemaVersion = 1;
    mcdr_frame_identity frame{};
    uint64_t frameSerial = 0;
    uint64_t producerScopeSerial = 0;
    uint64_t programGeneration = 0;
    uintptr_t metadata = 0;
    uint64_t vertexBytes = 0;
    uint64_t producerId = 0;
    uint64_t producerGeneration = 0;
    uint64_t shaderGeneration = 0;
    uint32_t sectionBase = 0;
    uint32_t sectionCount = 0;
    uint32_t primitiveBase = 0;
    uint32_t primitiveCount = 0;
    std::vector<TerrainDrawSectionIdentity> sections;
};

struct TerrainReplayDraw {
    mcdr_frame_identity frame{};
    uint64_t frameSerial = 0;
    uint64_t programGeneration = 0;
    uint64_t vertexBytes = 0;
    uint32_t sectionCount = 0;
    uint32_t primitiveCount = 0;
    uint32_t stage = UINT32_MAX;
    uint64_t totalBudgetBytes = 0;
};
int preparePrograms(void* context, uint64_t generation, void** prepared);
int validatePreparedPrograms(void* context, void* prepared);
void exchangePreparedPrograms(void* context, void* prepared) noexcept;
void destroyPreparedPrograms(void* prepared) noexcept;
int shaderPrelude(char* destination, uint32_t capacity, uint32_t* required, uint32_t* abi, uint64_t* hash);
int beginFrame(void* context, const mcdr_oit_begin_v1& input, void** frame);
int beginTerrainFrame(void* context, const mcdr_oit_begin_v2& input, void** frame);
int publishTerrainMetadata(void* context, void* frame, const mcdr_oit_terrain_metadata_v2& input, void** metadata);
int bindTerrainDraw(void* context, void* frame, void* metadata, const mcdr_oit_terrain_draw_v2& input);
int validateTerrainDraw(void* context, void* vertex, uint64_t offset);
int publishMetadata(void* context, void* frame, const mcdr_oit_metadata_v1& input, void** metadata);
int bindDraw(void* context, void* frame, void* metadata, const mcdr_oit_draw_v1& input);
int unbindDraw(void* context, void* frame);
int sealFrame(void* context, void* frame);
int abortFrame(void* context, void* frame);
int resolveRecords(void* context, const mcdr_oit_resolve_v3& input);
int queuePresented(void* context, uint64_t realFrameId, uint64_t finalFenceValue);
int commitPresented(void* context, uint64_t realFrameId, uint64_t fenceValue);
void discardResolved(void* context, uint64_t realFrameId, uint64_t fenceValue);
void reset(void* context);
}
