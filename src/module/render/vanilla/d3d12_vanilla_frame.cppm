module;

#include "_d3d12_module_prelude.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include "mcdr_graphics_recording.h"

export module mcdr.render.vanilla;

import mcdr.d3d12.device;

export namespace d3d12_mc::vanilla {

inline constexpr uint32_t kFrameSchemaVersion = 2;

enum class Phase : uint32_t {
    Idle = 0,
    Recording = 1,
    WorldReady = 2,
    WorldPost = 3,
    GuiReady = 4,
    Failed = 5,
    OpaqueReady = 6,
    OpaquePost = 7,
};

enum class Pass : uint32_t {
    Velocity = 1u << 0,
    Smaa = 1u << 1,
    Taa = 1u << 2,
    Sr = 1u << 3,
    Dlss = 1u << 4,
    Exposure = 1u << 5,
    ProviderMasks = 1u << 6,
    WorldOutput = 1u << 7,
};

struct FrameInput {
    uint32_t schemaVersion = kFrameSchemaVersion;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t worldEpoch = 0;
    uint64_t realFrameId = 0;
    uint64_t configVersion = 0;
    uint64_t resourceVersion = 0;
};

enum class WorldDepthSeedStatus : uint32_t {
    Ready, NotArmed, InvalidRequest, StaleRecording, LateArm, Busy, Cancelled,
    Incomplete, BudgetExceeded, RecordingFailed
};

struct WorldDepthSeedCaptureV1 {
    FrameInput frame;
    void* worldDepth = nullptr;
    uint64_t programGeneration = 0;
    std::shared_ptr<const void> recordingIdentity;
    void* fence = nullptr;
    uint64_t submission = 0;
    uint64_t totalBudgetBytes = 0;
    uint32_t maxEvents = 0;
    mcdr::graphics_recording::Observer observer;
};

enum class WorldDepthSeedEventKind : uint8_t { Clear, Draw };

struct WorldDepthSeedEventV1 {
    WorldDepthSeedEventKind kind = WorldDepthSeedEventKind::Clear;
    uint64_t sequence = 0;
    void* depthTarget = nullptr;
    uintptr_t pipelineIdentity = 0;
    ComPtr<ID3D12PipelineState> pso;
    uint64_t shaderGeneration = 0;
    uint64_t programGeneration = 0;
    uint32_t subresource = 0;
    uint32_t subresourceCount = 1;
    uint32_t commandFamily = 0;
    uint64_t commandOrdinal = 0;
    double clearDepth = 0.0;
    D3D12_RECT clearRect{};
    bool hasClearRect = false;
    D3D12_CPU_DESCRIPTOR_HANDLE sourceDsv{};
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    std::shared_ptr<const void> packet;
};

struct WorldDepthSeedJournalV1 {
    uint32_t schemaVersion = 1;
    void* context = nullptr;
    FrameInput frame;
    std::shared_ptr<const void> recordingIdentity;
    ComPtr<ID3D12Fence> fence;
    uint64_t submission = 0;
    void* worldDepth = nullptr;
    std::shared_ptr<void> depthOwner;
    uint64_t programGeneration = 0;
    ComPtr<ID3D12DescriptorHeap> depthDescriptors;
    D3D12_CPU_DESCRIPTOR_HANDLE retainedDsv{};
    std::vector<WorldDepthSeedEventV1> events;
};

WorldDepthSeedStatus armWorldDepthSeedCaptureV1(void* context, const WorldDepthSeedCaptureV1& input);
WorldDepthSeedStatus sealWorldDepthSeedCaptureV1(void* context, std::shared_ptr<const WorldDepthSeedJournalV1>* journal);
WorldDepthSeedStatus readWorldDepthSeedCaptureV1(void* context, std::shared_ptr<const WorldDepthSeedJournalV1>* journal);
WorldDepthSeedStatus cancelWorldDepthSeedCaptureV1(void* context);

struct PassInput {
    uint32_t schemaVersion = kFrameSchemaVersion;
    uint32_t pass = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t worldEpoch = 0;
    uint64_t realFrameId = 0;
    uint64_t configVersion = 0;
    uint64_t resourceVersion = 0;
};

using PostAdapter = int (*)(void* context, void* payload);

std::string lastFailureText();
uint64_t frozenResourceVersion(void* context);
bool matchesFrozenFrame(void* context, const void* identity, uint64_t historyGeneration);
int prepareFramePrograms(void* context, uint64_t generation, uint32_t flags, void** candidate);
int commitFramePrograms(void* context, void* candidate);
void abortFramePrograms(void* candidate) noexcept;
std::string frameProgramsFailureText();

enum class ResourceAccess : uint32_t { Read = 1, Write = 2, ReadWrite = 3, Uav = 4 };
// Tracked validates backend state; Adopt publishes a known SDK exit state without
// emitting a transition. Unknown resource effects must be rejected before recording.
enum class ExternalResourceState : uint32_t { Tracked, Adopt, Unknown };

struct PassResourceUse {
    void* object = nullptr;
    uint64_t version = 0;
    uint32_t firstSubresource = UINT32_MAX;
    uint32_t subresourceCount = 0;
    ResourceAccess access = ResourceAccess::Read;
    uint32_t requiredState = 0;
    uint32_t finalState = UINT32_MAX;
    ExternalResourceState externalState = ExternalResourceState::Tracked;
    void* resource = nullptr;
    uint32_t initialState = UINT32_MAX;
};

struct DeclaredPassToken;


// Version is the frozen frame resource manifest. UINT32_MAX/zero range means all;
// otherwise range indexes include mip/layer/plane. graphicsEffects bit0 invalidates
// graphics bindings, bit1 invalidates descriptor heap/tables. Provider internals
// and history remain adapter-owned, outside these outer resource declarations.
int beginFrame(void* context, const void* input, uint32_t byteCount);
int markWorldReady(void* context, const void* identity, uint32_t byteCount);
int markGuiReady(void* context, const void* identity, uint32_t byteCount);
int publishPreparedFrame(void* context, const void* input, uint32_t byteCount);
using PreparedAdapter = int (*)(void* context, const void* frame, const void* prepared, const void* request);
int executePreparedReconstruction(void* context, const void* input, uint32_t byteCount, PreparedAdapter adapter);
int prepareWorldOutput(void* context, const void* input, uint32_t byteCount);
int executeWorldPost(void* context, const PassInput& input, PostAdapter adapter, void* payload);
int executeDeclaredWorldPost(void* context, const PassInput& input,
                             const PassResourceUse* uses, uint32_t useCount,
                             PostAdapter adapter, void* payload, uint32_t graphicsEffects = 3);
int beginDeclaredPass(void* context, uint32_t pass, const PassResourceUse* uses,
                      uint32_t useCount, uint32_t graphicsEffects, DeclaredPassToken** token);
int finishDeclaredPass(void* context, DeclaredPassToken* token);
int beginRasterAttachmentPass(void* context, void** token);
int declareAdditionalRasterAttachment(void* context, void* target);
int beginRasterInputPass(void* context, uint32_t indexed, void* indirectCommands,
                         const void* extraBindings, uint32_t extraBindingCount, void** token);
int finishRasterInputPass(void* context, void* token);
void failRasterInputPass(void* context);
int executeSmaaWorldPost(void* context, PostAdapter adapter, void* scene);
void reset(void* context);

}
