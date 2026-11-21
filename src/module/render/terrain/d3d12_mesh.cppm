module;

#include "_d3d12_module_prelude.h"
#include "mcdr_terrain_tile.h"
#include "mcdr_terrain_tile_adapter.h"
#include <span>

export module mcdr.d3d12.mesh;

import mcdr.d3d12.device;
import mcdr.error;

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;
struct MeshPipelineDesc {
    uint32_t pushConstantBytes = 0;
    uint64_t captureHash = 0;
    uint64_t captureGeneration = 0;
    std::vector<uint8_t> asDxil;
    std::vector<uint8_t> msDxil;
    std::vector<uint8_t> psDxil;

    int colorCount = 0;
    std::vector<PipelineDesc::ColorTarget> colorTargets;
    bool hasDepth = false;
    bool rasterMotion = false;
    int depthFormat = 0;
    bool depthWrite = false;
    int depthCompareOp = 0;
    bool cullEnabled = false;
    int polygonMode = 0;
    std::vector<PipelineDesc::Binding> bindings;
};

struct TerrainTileGeometryV2 {
    uint64_t segmentAddress = 0;
    uint64_t vertexAddress = 0;
    uint64_t visibleAddress = 0;
    uint64_t atlasAddress = 0;
    uint64_t batchConstantsAddress = 0;
};

struct TerrainTileReplayBindingV2 {
    uint8_t type = 0;
    void* buffer = nullptr;
    int64_t offset = 0;
    int64_t length = 0;
    int32_t texelFormat = 0;
    void* view = nullptr;
    bool laneAttachment = false;
};

struct TerrainTileReplayReadV2 {
    void* resource = nullptr;
    uint32_t firstSubresource = 0;
    uint32_t subresourceCount = 0;
    uint32_t state = 0;
    uint32_t initialState = 0;
};

struct TerrainTileReplayInputsV2 {
    std::vector<TerrainTileReplayBindingV2> bindings;
    std::vector<TerrainTileReplayReadV2> reads;
    D3D12_VIEWPORT sourceViewport{};
    D3D12_RECT sourceScissor{};
    std::array<uint8_t, 128> pushConstants{};
    uint32_t pushConstantBytes = 0;
    bool pushConstantsValid = false;
};

enum class TerrainTileAttachmentMode : uint32_t {
    Load = 0,
    Clear = 1,
};

struct TerrainTileEnvelopeChunk {
    uint32_t firstVisible = 0;
    D3D12_GPU_VIRTUAL_ADDRESS destination = 0;
};

mcdr::Result<void*> createMeshPipeline(const MeshPipelineDesc& desc);

mcdr::Result<void*> createMeshPipeline(const MeshPipelineDesc& desc, uint64_t externalRootSignature);

mcdr::Result<uint64_t> createMeshRootSignature(const std::vector<PipelineDesc::Binding>& bindings,
                                               bool meshExtras, bool visibleWork = false, uint32_t pushConstantBytes = 0,
                                               uint64_t captureHash = 0, bool tileExtras = false);
void releaseMeshRootSignature(uint64_t rootSignature);

mcdr::Result<void*> createTerrainTileRoute(uint64_t rootSignature,
                                           uint64_t captureHash,
                                           uint64_t captureGeneration);
void destroyTerrainTileRoute(void* route);
mcdr::Result<void> bindTerrainTileRoute(void* capturePipeline, void* route);
mcdr::Result<void> bindTerrainTilePipeline(void* route, void* tilePipeline);
mcdr::Result<void> bindTerrainTileEnvelopeV1(void* route, const void* dxil, uint32_t bytes);
mcdr::Result<void> bindTerrainTileStageV2(void* source, void* route,
    const mcdr_terrain_tile_stage_v2& contract, std::span<const uint32_t> laneRegisters = {});
inline constexpr uint32_t terrainOpaqueSeedStage = 3;
mcdr::Result<void> bindTerrainOpaqueSeedV1(void* source, void* route, uint64_t programGeneration,
    uint32_t quadsPerMeshlet);
mcdr::terrain_tile::StageAdapterDescription describeTerrainTileStageAdapter(
    const std::shared_ptr<const void>& routeLease, uint64_t sourceIdentity, uint64_t sourceGeneration);
std::span<const uint32_t> terrainTileLaneBindingSlots(const void* route);

bool meshPipelineSupported(std::string* why);

mcdr::Result<void> dispatchMesh(void* ctx, int x, int y, int z);

struct TerrainProgramDescription {
    const void* nodes;
    size_t nodesBytes;
    const void* initialize;
    size_t initializeBytes;
    const void* finalize;
    size_t finalizeBytes;
    const uint32_t* uniformRegisters;
    uint32_t uniformCount;
    uint64_t budget;
};

mcdr::Result<void*> createTerrainWorkGraph(const TerrainProgramDescription& description);
void destroyTerrainWorkGraph(void* program);
mcdr::Result<void*> prepareTerrainWorkGraph(void* context, void* program,
    void* const* buffers, const uint64_t* offsets, const uint64_t* lengths,
    uint32_t bindingCount, uint32_t sectionCount, uint32_t meshletCapacity);
mcdr::Result<void> drawTerrainWorkGraph(void* context, void* batch,
    void* vertexBuffer, uint64_t vertexOffset, uint64_t vertexBytes = 0);
mcdr::terrain_tile::Result prepareTerrainTilePacket(void* context,
    const std::shared_ptr<const void>& packet, const mcdr::terrain_tile::Batch& batch);
mcdr::terrain_tile::Result prepareTerrainTilePacketV2(void* context,
    const std::shared_ptr<const void>& packet, const mcdr::terrain_tile::BatchV2& batch,
    const mcdr::terrain_tile::ResourceRef& opaqueSeedDepth, mcdr::terrain_tile::ConstantAddresses constants,
    std::shared_ptr<const void>* replayPlan);
mcdr::Result<void> recordTerrainTileReplayPlan(void* context,
    const std::shared_ptr<const void>& replayPlan, uint32_t stageIndex,
    TerrainTileAttachmentMode attachmentMode);
mcdr::Result<void> initializeTerrainTileLane(void* context, const mcdr::terrain_tile::BatchV2& batch);
mcdr::Result<void> recordTerrainTileReplay(void* context, void* route,
    const mcdr::terrain_tile::BatchV2& batch, const TerrainTileGeometryV2& geometry,
    const TerrainTileReplayInputsV2& inputs, uint32_t stageIndex, TerrainTileAttachmentMode attachmentMode);
mcdr::Result<void> validateTerrainTileReplay(void* route,
    const mcdr::terrain_tile::BatchV2& batch, const TerrainTileReplayInputsV2& inputs,
    uint32_t stageIndex);
mcdr::Result<void> validateTerrainOpaqueSeedReplay(void* route,
    const mcdr::terrain_tile::BatchV2& batch, const TerrainTileReplayInputsV2& inputs,
    const mcdr::terrain_tile::ResourceRef& depth);
mcdr::Result<void> recordTerrainOpaqueSeedReplay(void* context, void* route,
    const mcdr::terrain_tile::BatchV2& batch, const TerrainTileGeometryV2& geometry,
    const TerrainTileReplayInputsV2& inputs, const mcdr::terrain_tile::ResourceRef& depth);
bool hasTerrainTileEnvelopePipeline(const void* route);
mcdr::Result<void> recordTerrainTileEnvelopes(void* context, const void* route,
    const mcdr::terrain_tile::BatchV2& batch, const TerrainTileGeometryV2& geometry,
    const TerrainTileReplayInputsV2& inputs, const TerrainTileEnvelopeChunk& chunk);
void destroyTerrainBatch(void* batch);

}

