#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include "mcdr_terrain_tile_adapter.h"

namespace mcdr::terrain_tile {
inline constexpr uint32_t schemaVersion = 1;
inline constexpr uint32_t maximumResidentTiles = 16;
inline constexpr uint32_t tileSize = 64;
inline constexpr uint32_t guardPixels = 2;
inline constexpr uint32_t scratchSize = tileSize + guardPixels * 2;
inline constexpr uint32_t laneCount = 4;
inline constexpr uint32_t batchConstantAllocationBytes = 512;
inline constexpr float laneOffsets[laneCount][2] = {
    {-0.25f, -0.25f}, {0.25f, -0.25f}, {-0.25f, 0.25f}, {0.25f, 0.25f},
};

enum class Result : uint32_t {
    UnsupportedRoute,
    UnsupportedContent,
    StaleRecording,
    InvalidBatch,
    FrameNotSealed,
    Consumed,
    RecordingFailed,
};

enum class ReplayStage : uint32_t {
    DepthBounds = 0,
    Transmittance = 1,
    Accumulate = 2,
};

enum class StageAdapterStatus : uint32_t { Unavailable, Registered, Stale };

struct StageAdapterDescription {
    StageAdapterStatus status = StageAdapterStatus::Unavailable;
    mcdr_terrain_tile_stage_v2 contract{};
    uint64_t sourceIdentity = 0;
    uint64_t sourceGeneration = 0;
    uint64_t tileIdentity = 0;
    uint64_t tileGeneration = 0;
    std::shared_ptr<const void> routeLease;
};

struct DrawRoute {
    uint32_t tileSlot = 0;
    uint32_t sourcePrimitive = 0;
    uint32_t sourceInstance = 0;
    uint32_t visibleMeshlet = 0;
};

struct Tile {
    uint32_t originX = 0;
    uint32_t originY = 0;
    uint32_t effectiveWidth = 0;
    uint32_t effectiveHeight = 0;
};

struct Batch {
    uint32_t schemaVersion = mcdr::terrain_tile::schemaVersion;
    uint32_t residentTileCount = 0;
    uint32_t laneIndex = 0;
    uint32_t flags = 0;
    float sourceViewport[4]{};
    Tile tiles[maximumResidentTiles]{};
};

struct ResourceRef {
    void* object = nullptr;
    uint64_t offset = 0;
    uint64_t bytes = 0;
};

struct StageBindingV2 {
    uint32_t stage = static_cast<uint32_t>(ReplayStage::DepthBounds);
    uint32_t flags = 0;
    uint32_t renderTargetCount = 0;
    uint32_t reserved = 0;
    void* pipeline = nullptr;
    ResourceRef depth;
    ResourceRef renderTargets[5]{};
};

// V2 keeps the original Batch ABI intact and supplies the resources that are
// required after the sealed OIT frame has handed ownership to native. Every
// object is an opaque mcdr_object handle; native validates kind, range, state
// and lifetime before recording anything.
struct BatchV2 {
    uint32_t schemaVersion = 2;
    uint32_t structSize = 0;
    uint32_t residentTileCount = 0;
    uint32_t laneIndex = 0;
    uint32_t flags = 0;
    uint32_t reserved0 = 0;
    uint64_t frameSerial = 0;
    uint64_t worldEpoch = 0;
    uint64_t programGeneration = 0;
    uint64_t resourceVersion = 0;
    float sourceViewport[4]{};
    Tile tiles[maximumResidentTiles]{};
    uint32_t primitiveCount = 0;
    uint32_t refCapacity = 0;
    uint32_t commandKind = 1;
    uint32_t commandStride = 28;
    ResourceRef binningConstants;
    ResourceRef envelopes;
    ResourceRef masks;
    ResourceRef prefixes;
    ResourceRef commands;
    ResourceRef control;
    ResourceRef bounds;
    ResourceRef culledBounds;
    ResourceRef depthTerrain;
    ResourceRef depthCloud;
    ResourceRef coefficients[4]{};
    ResourceRef q;
    ResourceRef scratchColor;
    ResourceRef scratchDepth;
    void* binningWorkGraph = nullptr;
    void* binningInitializePipeline = nullptr;
    void* binningPrefixPipeline = nullptr;
    void* binningScatterPipeline = nullptr;
    StageBindingV2 stages[3]{};
};

struct BatchV3 {
    uint32_t schemaVersion = 3;
    uint32_t structSize = 0;
    uint32_t packetOrdinal = 0;
    uint32_t packetCount = 0;
    BatchV2 payload{};
};


static_assert(offsetof(BatchV2, sourceViewport) % alignof(float) == 0);
static_assert(offsetof(BatchV2, tiles) > offsetof(BatchV2, sourceViewport));
static_assert(sizeof(StageBindingV2) > sizeof(ResourceRef));
static_assert(offsetof(BatchV3, payload) == 16);
static_assert(sizeof(BatchV3) == 1304);

using PacketPreparation = Result (*)(void* context, const std::shared_ptr<const void>& packet, const Batch& batch);
using PacketPreparationV2 = Result (*)(void* context, const std::shared_ptr<const void>& packet, const BatchV2& batch);


static_assert(sizeof(DrawRoute) == 16);
static_assert(sizeof(Tile) == 16);
static_assert(offsetof(Batch, sourceViewport) == 16);
static_assert(offsetof(Batch, tiles) == 32);
static_assert(sizeof(Batch) == 288);
}
