#ifndef MCDR_GRAPHICS_RECORDING_H
#define MCDR_GRAPHICS_RECORDING_H

#include <cstdint>
#include <memory>

namespace mcdr::graphics_recording {
enum class CommandFamily : uint32_t { Draw = 1, DrawIndexed = 2, DrawIndirect = 3, DrawIndexedIndirect = 4, Mesh = 5 };
enum class ContentState : uint32_t { Unfrozen = 0, Frozen = 1, Unsupported = 2, Stale = 3 };
enum class AdapterState : uint32_t { Unavailable = 0, Ready = 1, UnsupportedTopology = 2, UnsupportedBinding = 3, UnsupportedShaderAddressing = 4 };
enum class AdapterRegistration : uint32_t { Unavailable = 0, Registered = 1, Stale = 2 };
enum class Completion : uint32_t { Succeeded, Failed };
// Content capture is opt-in. Ordinary operation recording must remain a facts-only
// path until an executor has a real adapter and will consume the frozen bindings.
enum class ContentPolicy : uint32_t { FactsOnly = 0, FreezeBindings = 1 };
enum class TileDrawScope : uint32_t { DepthBounds = 0, Transmittance = 1, Accumulate = 2, Seed, None };

struct AdapterFacts {
    AdapterState state = AdapterState::Unavailable;
    AdapterRegistration registration = AdapterRegistration::Unavailable;
    uint64_t sourceIdentity = 0, sourceGeneration = 0;
    uint64_t tileIdentity = 0, tileGeneration = 0;
    uint64_t contractHash = 0, programGeneration = 0;
    uint32_t schemaVersion = 0, structSize = 0, shaderAbiVersion = 0, addressingVersion = 0;
    uint32_t stage = 0, quadsPerMeshlet = 0;
    std::shared_ptr<const void> routeLease;
    std::shared_ptr<const void> replayContent;
};

struct Command {
    CommandFamily family = CommandFamily::Draw;
    uint32_t count = 0, instanceCount = 0, firstVertexOrIndex = 0;
    int32_t baseVertex = 0;
    uint32_t firstInstance = 0;
    void* indirectBuffer = nullptr;
    uint64_t indirectOffset = 0;
    uint32_t meshGroups[3]{};
};

struct PacketFacts {
    Command command;
    uint64_t pipelineIdentity = 0, packetIdentity = 0, readSetIdentity = 0;
    ContentState content = ContentState::Unfrozen;
    AdapterState adapter = AdapterState::Unavailable;
    AdapterFacts adapterFacts;
    bool empty = false;
};

struct Observer {
    std::weak_ptr<void> owner;
    uint64_t totalBudgetBytes = 0;
    ContentPolicy contentPolicy = ContentPolicy::FactsOnly;
    const void* depthWriteTarget = nullptr;
    TileDrawScope (*tileDrawScope)(void* owner) noexcept = nullptr;
    int (*prepare)(void* owner, void* context, const PacketFacts&, std::shared_ptr<const void> packet) = nullptr;
    int (*confirm)(void* owner, void* context, const void* packet) = nullptr;
    int (*complete)(void* owner, void* context, Completion completion) = nullptr;
    void (*budgetExceeded)(void* owner, void* context) noexcept = nullptr;
};
}

#endif
