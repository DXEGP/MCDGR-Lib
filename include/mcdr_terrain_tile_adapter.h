#ifndef MCDR_TERRAIN_TILE_ADAPTER_H
#define MCDR_TERRAIN_TILE_ADAPTER_H

#include <stdint.h>

typedef struct mcdr_terrain_tile_stage_v2 {
    uint32_t schemaVersion, structSize, shaderAbiVersion, stage;
    uint32_t quadsPerMeshlet, addressingVersion;
    uint64_t shaderContractHash, programGeneration;
} mcdr_terrain_tile_stage_v2;

#ifdef __cplusplus
static_assert(sizeof(mcdr_terrain_tile_stage_v2) == 40);

#endif

#endif
