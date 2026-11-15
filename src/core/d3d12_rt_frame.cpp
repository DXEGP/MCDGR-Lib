module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include "advanced/mcdr_nrd_impl.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <compare>
#include <flat_map>
#include <flat_set>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <span>

module mcdr.d3d12.device;

import mcdr.error;
import mcdr.d3d12.mesh;

namespace d3d12_mc {

struct RtGBufferProgram {
    std::vector<uint8_t> mesh;
    std::vector<uint8_t> pixel;
    std::vector<std::pair<uint8_t, uint8_t>> bindings;
    int32_t cull = 0;
    int32_t polygon = 0;
    std::unique_ptr<Dx12Pipeline> pipeline;
};

struct RtGeometryRange {
    uint64_t identity = 0;
    uint64_t offset = 0;
    uint64_t bytes = 0;
    std::shared_ptr<CommandRecording> recording;
};

struct RtGeometryArena {
    std::shared_ptr<Dx12Object> buffer;
    std::map<uint64_t, uint64_t> free;
    std::map<uint64_t, std::weak_ptr<RtGeometryRange>> contents;
    struct Reservation {
        uint64_t offset;
        uint64_t bytes;
        std::weak_ptr<RtGeometryRange> range;
    };
    std::vector<Reservation> reservations;

    void reclaim() {
        std::erase_if(contents, [](const auto& entry) { return entry.second.expired(); });
        std::erase_if(reservations, [&](const auto& allocation) {
            if (!allocation.range.expired()) return false;
            auto offset = allocation.offset;
            auto bytes = allocation.bytes;
            auto next = free.lower_bound(offset);
            if (next != free.begin()) {
                const auto previous = std::prev(next);
                if (previous->first + previous->second == offset) {
                    offset = previous->first;
                    bytes += previous->second;
                    free.erase(previous);
                }
            }
            if (next != free.end() && offset + bytes == next->first) {
                bytes += next->second;
                free.erase(next);
            }
            free.emplace(offset, bytes);
            return true;
        });
    }

    std::shared_ptr<RtGeometryRange> allocate(uint64_t identity, uint64_t bytes, uint64_t alignment,
        const std::shared_ptr<CommandRecording>& recording) {
        for (auto entry = free.begin(); entry != free.end(); ++entry) {
            const auto start = entry->first;
            const auto end = start + entry->second;
            const auto offset = (start + alignment - 1) / alignment * alignment;
            const auto padded = (bytes + 3) & ~uint64_t{3};
            if (offset > end || padded > end - offset) continue;
            auto range = std::make_shared<RtGeometryRange>(identity, offset, bytes, recording);
            reservations.push_back({offset, padded, range});
            contents[identity] = range;
            free.erase(entry);
            if (offset != start) free.emplace(start, offset - start);
            if (offset + padded != end) free.emplace(offset + padded, end - offset - padded);
            return range;
        }
        return {};
    }
};

struct RtGeometryStorage {
    std::array<RtGeometryArena, 3> arenas;
};

struct RtGeometrySnapshot {
    uint64_t worldEpoch = 0;
    uint64_t geometryRevision = 0;
    uint64_t materialRevision = 0;
    std::array<std::shared_ptr<Dx12Object>, 5> buffers;
    std::shared_ptr<std::vector<uint8_t>> records;
    std::array<std::shared_ptr<CommandRecording>, 5> recordings;
    std::shared_ptr<RtGeometryStorage> storage;
    std::vector<std::shared_ptr<RtGeometryRange>> ranges;
    std::vector<std::shared_ptr<CommandRecording>> dependencies;

    bool available(CommandContext* encoder, bool geometry = true, bool materials = true) const {
        for (size_t index = 0; index < recordings.size(); ++index) {
            if (!(index == 3 ? materials : geometry)) continue;
            if (!recordings[index] || !recordings[index]->available(encoder)) return false;
        }
        if (geometry && !std::ranges::all_of(dependencies, [&](const auto& recording) {
            return recording && recording->available(encoder);
        })) return false;
        return true;
    }

    bool cancelled(bool geometry = true, bool materials = true) const {
        for (size_t index = 0; index < recordings.size(); ++index) {
            if ((index == 3 ? materials : geometry) && recordings[index] && recordings[index]->cancelled) return true;
        }
        if (geometry && std::ranges::any_of(dependencies, [](const auto& recording) {
            return recording && recording->cancelled;
        })) return true;
        return false;
    }
};

namespace {

constexpr uint64_t kFrameBytes = 416;
constexpr uint64_t kConstantBytes = 512;
constexpr int64_t kRecordBytes = 64;
constexpr int64_t kTableBytes = 320;
constexpr int32_t kTraceSrvs = 7;
constexpr int32_t kTraceUavs = 5;
constexpr int32_t kGuideSrvs = 2;
constexpr int32_t kGuideUavs = 5;
constexpr int32_t kExposureSrvs = 8;
constexpr int32_t kExposureUavs = 1;
constexpr int32_t kMotionSrvs = 1;
constexpr int32_t kMotionUavs = 1;
constexpr int32_t kPackSrvs = 4;
constexpr int32_t kPackUavs = 4;
constexpr int32_t kSignalSrvs = 12;
constexpr int32_t kSignalUavs = 4;
constexpr int32_t kRemodulateSrvs = 13;
constexpr int32_t kRemodulateUavs = 2;
constexpr int32_t kCompositeSrvs = 14;
constexpr int32_t kCompositeUavs = 1;
constexpr uint32_t kPendingFeedbackBytes = 8192;
constexpr uint64_t kPendingDemandFrames = 8;
constexpr uint32_t kSourceDemandInstanceBit = 0x400000u;
constexpr uint32_t kPathTileSize = 64;
constexpr uint32_t kPathHeaderBytes = 64;
constexpr uint32_t kPathTileBytes = 128;

struct RtObjectRelease {
    void operator()(Dx12Object* object) const { destroyObject(object); }
};

struct RtAccelerationRelease {
    void operator()(Dx12Object* object) const { destroyCompletedRtAccelerationObject(object); }
};

using RtObject = std::unique_ptr<Dx12Object, RtObjectRelease>;

using RtSharedObject = std::shared_ptr<Dx12Object>;

struct RtBlasEntry {
    RtSharedObject result;
    uint32_t version = 0;
    uint32_t vertices = 0;
    uint32_t indices = 0;
    uint32_t flags = 0;
    std::shared_ptr<CommandRecording> recording;

    bool available(CommandContext* encoder) const {
        return recording && recording->available(encoder);
    }
};

struct RtBlasKey {
    uint64_t node = 0;
    uint32_t layer = 0;
    uint32_t version = 0;
    uint32_t vertices = 0;
    uint32_t indices = 0;
    uint32_t flags = 0;

    auto operator<=>(const RtBlasKey&) const = default;
};

struct RtProxyCoverage {
    std::flat_map<uint64_t, std::array<float, 6>> ranges;
    bool unbounded = false;

    bool operator==(const RtProxyCoverage&) const = default;
};

struct RtOutputBounds {
    std::array<float, 6> values{0.0f, 0.0f, 0.0f, 16.0f, 16.0f, 16.0f};
    bool finite = true;
    std::optional<std::array<float, 6>> proxyRange;
    uint32_t captureVersion = 0;
};

struct RtAcceleration {
    uint64_t owner = 0;
    uint64_t snapshotId = 0;
    std::shared_ptr<RtGeometrySnapshot> snapshot;
    std::vector<std::shared_ptr<RtBlasEntry>> entries;
    uint64_t entryRevision = 0;
    uint32_t remainingBuilds = 0;
    RtSharedObject scratch;
    RtSharedObject tlas;
    RtSharedObject tlasScratch;
    RtSharedObject view;
    RtSharedObject instances;
    std::shared_ptr<RtBlasEntry> proxy;
    std::flat_set<uint64_t> masked;
    std::map<uint64_t, uint32_t> pendingQueries;
    std::flat_map<uint64_t, uint32_t> sourceQueries;
    std::set<uint32_t> shadowProofs;
    RtProxyCoverage coverage;
    std::shared_ptr<CommandRecording> recording;
    bool complete = false;
    uint32_t instanceCount = 0;
    std::shared_ptr<CommandRecording> budgetRecording;
    uint32_t budgetUsed = 0;

    bool available(CommandContext* encoder) const {
        return !recording || recording->available(encoder);
    }
};

struct RtResidentBlas {
    std::shared_ptr<RtBlasEntry> entry;
    std::list<RtBlasKey>::iterator position;
};

struct RtWorkCharge {
    uint64_t triangles = 0;
    uint64_t predictedNanos = 0;
    uint64_t publications = 0;
};

struct RtWorkTiming {
    std::unique_ptr<QueryPool> pool;
    std::shared_ptr<CommandRecording> recording;
    uint64_t frame = 0;
    uint64_t world = 0;
    uint64_t predictedNanos = 0;
    uint64_t gpuLimitNanos = 0;
    uint64_t factor = 1024;
};

struct RtSceneWork {
    uint64_t cpuLimitNanos = 0;
    uint64_t gpuLimitNanos = 0;
    uint64_t triangleLimit = 0;
    uint64_t cpuNanos = 0;
    uint64_t predictedNanos = 0;
    uint64_t triangles = 0;
    uint64_t publications = 0;
    uint64_t deferred = 0;
    uint64_t oversized = 0;
    uint64_t samples = 0;
    uint64_t lastGpuNanos = 0;
    uint64_t totalGpuNanos = 0;
    uint64_t cpuOverruns = 0;
    uint64_t gpuOverruns = 0;
    uint64_t cancelledSamples = 0;
    uint64_t failures = 0;
    uint64_t peakGpuNanos = 0;
    uint64_t lastGpuFrame = 0;
    uint64_t lastGpuWorld = 0;
    uint64_t lastCpuNanos = 0;
    uint64_t factor = 1024;
    bool cpuExceeded = false;
    std::chrono::steady_clock::time_point started;
    std::map<std::shared_ptr<CommandRecording>, RtWorkCharge> recordings;
    std::list<RtWorkTiming> pending;
    std::vector<std::unique_ptr<QueryPool>> free;
    RtWorkTiming* timing = nullptr;
};

struct RtSceneRequest {
    uint32_t version = 0;
    uint64_t firstFrame = 0;
    uint64_t identity = 0;
    uint64_t demandFrame = 0;
};

struct RtPendingDemand {
    uint64_t node;
    uint64_t request;
    uint32_t layer;
    uint32_t version;
    uint32_t firstRecord;
};

struct RtSourceRequest {
    std::vector<std::pair<uint32_t, uint32_t>> versions;
    uint64_t identity = 0;
    uint64_t demandFrame = 0;
    uint64_t deliveredFrame = 0;
    bool queued = false;
};

struct RtSourceDemand {
    uint64_t node;
    uint64_t request;
    uint32_t bit;
};

struct RtPendingFeedback {
    RtSharedObject readback;
    std::shared_ptr<CommandRecording> recording;
    std::vector<RtPendingDemand> requests;
    std::vector<RtSourceDemand> sources;
    uint64_t scene = 0;
    uint64_t world = 0;
    uint64_t frame = 0;
    uint64_t sceneFrame = 0;
    uint64_t context = 0;
    bool accepted = false;
};

struct RtScene {
    uint64_t worldEpoch = 0;
    uint64_t liveRevision = 0;
    uint64_t published = 0;
    uint64_t pending = 0;
    uint64_t validatedPending = 0;
    uint64_t validatedPublished = 0;
    uint64_t validatedEntryRevision = 0;
    uint64_t partialPending = 0;
    uint64_t partialEntryRevision = 0;
    std::chrono::steady_clock::time_point lastPublication;
    uint64_t revision = 0;
    bool initialized = false;
    std::vector<mcdr_rt_section_version> versions;
    std::flat_map<std::pair<uint64_t, uint32_t>, uint32_t> live;
    uint32_t outputMode = 0;
    std::vector<mcdr_rt_output_support> outputs;
    std::flat_map<uint64_t, RtOutputBounds> outputBounds;
    bool explicitTargets = false;
    std::vector<uint64_t> targetNodes;
    std::flat_set<uint64_t> targets;
    std::map<RtBlasKey, RtResidentBlas> resident;
    std::list<RtBlasKey> residentLru;
    uint64_t residentBytes = 0;
    std::map<Dx12Object*, std::weak_ptr<Dx12Object>> resources;
    uint64_t frameId = 0;
    uint32_t builds = 0;
    std::map<std::shared_ptr<CommandRecording>, uint32_t> buildRecordings;
    std::flat_map<std::pair<uint64_t, uint32_t>, RtSceneRequest> firstRequestedFrames;
    std::flat_map<uint64_t, RtSourceRequest> sourceRequests;
    std::list<std::pair<uint64_t, uint64_t>> captureDemands;
    bool demandNext = false;
    RtSceneWork work;
};

struct RtDenoiseHistory {
    uint64_t generation = 0;
    uint64_t frame = 0;
    bool valid = false;
    std::shared_ptr<CommandRecording> recording;
    uint64_t pendingGeneration = 0;
    uint64_t pendingFrame = 0;
    std::shared_ptr<CommandRecording> pendingRecording;

    bool requiresReset(const mcdr_rt_denoise_desc& request) const {
        return !valid || (recording && recording->cancelled) ||
            generation != request.historyGeneration || frame + 1 != request.historyFrame;
    }

    void stage(const mcdr_rt_denoise_desc& request, const std::shared_ptr<CommandRecording>& producer) {
        pendingGeneration = request.historyGeneration;
        pendingFrame = request.historyFrame;
        pendingRecording = producer;
    }

    bool hasPending(const mcdr_rt_denoise_desc& request,
                    const std::shared_ptr<CommandRecording>& producer) const {
        return pendingRecording == producer && pendingGeneration == request.historyGeneration &&
            pendingFrame == request.historyFrame;
    }

    void commit(CommandRecording* producer) {
        if (!pendingRecording || pendingRecording.get() != producer) return;
        generation = pendingGeneration;
        frame = pendingFrame;
        valid = true;
        recording = pendingRecording;
        pendingRecording.reset();
    }

    void cancel(CommandRecording* producer) {
        if (!pendingRecording || pendingRecording.get() != producer) return;
        pendingRecording.reset();
        valid = false;
        recording.reset();
    }

    void reset() {
        valid = false;
        recording.reset();
        pendingRecording.reset();
    }
};

struct RtSpatialSupport {
    std::vector<uint8_t> bytes;
    uint64_t frameId = 0;
    uint64_t worldEpoch = 0;
    uint64_t liveRevision = 0;
    uint64_t gbufferRevision = 0;
    std::shared_ptr<CommandRecording> recording;
};

struct RtPathSnapshot {
    RtSharedObject buffer;
    std::shared_ptr<CommandRecording> recording;
    uint64_t worldEpoch = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct RtFrameContext {
    std::array<RtObject, 6> gbuffer;
    uint64_t gbufferRevision = 0;
    uint64_t gbufferResourceRevision = 0;
    int64_t gbufferFrameNanos = 0;
    int32_t gbufferWidth = 0;
    int32_t gbufferHeight = 0;
    std::shared_ptr<CommandRecording> gbufferRecording;
    bool gbufferOpen = false;
    bool gbufferReady = false;
    bool gbufferRecorded = false;
    bool gbufferFailed = false;
    RtObject pipeline;
    RtObject workGraphBacking;
    std::shared_ptr<CommandRecording> workGraphInitialized;
    RtObject guide;
    RtObject exposurePipeline;
    RtObject environmentPipeline;
    RtObject environment;
    RtObject exposure;
    RtObject motion;
    RtObject pack;
    RtObject signalPipeline;
    RtObject remodulatePipeline;
    RtObject compositePipeline;
    RtObject compositeTarget;
    mcdr_rt_denoise_output reconstructed{};
    std::array<float, 2> contentJitter{};
    RtObject table;
    RtObject constants;
    RtObject traceRecords;
    RtSpatialSupport spatial;
    std::array<RtObject, kTraceUavs> targets;
    std::array<RtObject, 5> denoiseGuides;
    std::array<RtObject, kSignalUavs> signals;
    std::array<RtObject, 2> nrdOutputs;
    RtObject rrOutput;
    RtDenoiseHistory nrdHistory;
    RtDenoiseHistory rrHistory;
    std::chrono::steady_clock::time_point nrdLastDispatch{};
    uint64_t nrdLongGaps = 0;
    std::array<RtObject, 3> srTargets;
    RtDenoiseHistory srHistory;
    mcdr_rt_denoise_desc srRequest{};
    std::array<float, 104> srConstants{};
    int32_t srBackend = MCDR_SR_BACKEND_NONE;
    int64_t srFrameNanos = 0;
    bool srPending = false;
    bool srAvailable = true;
    uint64_t denoisePlan = 0;
    uint32_t denoiseEncoding = 0;
    int32_t denoiseWidth = 0;
    int32_t denoiseHeight = 0;
    int32_t displayWidth = 0;
    int32_t displayHeight = 0;
    uint32_t reportedBackend = 0;
    bool denoisePending = false;
    bool rrAvailable = true;
    bool nrdInitFailed = false;
    std::array<Dx12Object*, 4> signalGuides{};
    std::shared_ptr<CommandRecording> recording;
    bool compositeRecorded = false;
    uint64_t lightingRevision = 0;
    uint64_t lightingEpoch = 0;
    uint64_t pendingLightingRevision = 0;
    uint64_t pendingLightingEpoch = 0;
    bool lightingActive = false;
    bool lightingPending = false;
    uint32_t lightingOffset = 0;
    uint32_t lightingCount = 0;
    uint32_t lightingFullDomain = 0;
    RtPathSnapshot path;
    RtPathSnapshot pendingPath;
    uint32_t signalEncoding = 0;
    bool rasterFallbackReady = false;
    std::array<Dx12Object*, 5> geometry{};
    uint64_t frameId = 0;
    uint64_t worldEpoch = 0;
    uint64_t sceneRevision = 0;
    uint64_t materialRevision = 0;
    uint64_t snapshotId = 0;
    int32_t width = 0;
    int32_t height = 0;
    int64_t frameNanos = 0;
    int32_t exposureMode = 0;
    float exposureEv = 0;
    bool exposureDirty = true;
    bool exposurePending = false;
    std::shared_ptr<CommandRecording> exposureRecording;
    int64_t committedFrameNanos = 0;
    int64_t pendingSrFrameNanos = 0;
    uint64_t committedWorldEpoch = 0;
    int32_t committedExposureMode = 0;
    float committedExposureEv = 0;
};

std::mutex frameMutex;
std::unordered_map<uint64_t, std::unique_ptr<RtFrameContext>> frameContexts;
std::unordered_map<uint64_t, std::shared_ptr<RtGBufferProgram>> gbufferPrograms;
std::vector<std::weak_ptr<RtGBufferProgram>> gbufferProgramCache;
std::unordered_map<uint64_t, std::shared_ptr<RtGeometrySnapshot>> geometrySnapshots;
constexpr uint64_t kGeometryMemoryLimit = 512ULL * 1024 * 1024;
constexpr uint64_t kAccelerationMemoryLimit = 512ULL * 1024 * 1024;
struct RtMemoryAccount {
    uint64_t bytes = 0;
    uint64_t peak = 0;
    uint64_t rejections = 0;
};

enum class RtMemoryDomain : size_t { GeometryGpu, GeometryUpload, Acceleration, FrameGpu, FrameHost };

struct RtMemoryLedger {
    std::mutex mutex;
    std::array<RtMemoryAccount, 5> domains;
    uint64_t bytes = 0;
    uint64_t limit = 2048ULL * 1024 * 1024;
    uint64_t peak = 0;
    uint64_t rejections = 0;
};

struct RtMemoryCharge {
    std::shared_ptr<RtMemoryLedger> memory;
    RtMemoryDomain domain = RtMemoryDomain::GeometryGpu;
    uint64_t bytes = 0;

    ~RtMemoryCharge() {
        if (bytes == 0) return;
        const std::lock_guard lock(memory->mutex);
        memory->bytes -= bytes;
        memory->domains[static_cast<size_t>(domain)].bytes -= bytes;
    }
};

const auto rtMemory = std::make_shared<RtMemoryLedger>();
std::unordered_map<uint64_t, std::unique_ptr<RtAcceleration>> accelerations;
uint64_t nextAcceleration = 1;
std::unordered_map<uint64_t, RtScene> scenes;
uint64_t nextScene = 1;
uint64_t nextSceneRequest = 1;
std::array<RtPendingFeedback, 3> pendingFeedback;
RtSharedObject pendingFeedbackZero;
uint64_t nextSceneRevision = 1;
uint64_t nextGeometrySnapshot = 1;
uint64_t nextGBufferProgram = 1;
uint64_t nextFrameContext = 1;
uint64_t activeNrdContext = 0;
uint64_t activeRrContext = 0;
uint32_t rrFrameIndex = 0;
uint64_t activeSrContext = 0;
uint32_t srFrameIndex = 0;

bool currentRtRecording(const std::shared_ptr<CommandRecording>& recording, CommandContext* encoder) {
    return recording && encoder && !recording->submitted &&
        recording == encoder->commandRecordings[encoder->currentListSlot] && recording->available(encoder);
}

void discardCancelledGBuffer(RtFrameContext& context) {
    if (!context.gbufferRecording || !context.gbufferRecording->cancelled) return;
    if (context.gbufferRecording->submitted) {
        if (!context.gbufferRecording->fence) return;
        const auto completed = context.gbufferRecording->fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < context.gbufferRecording->fenceValue) return;
    }
    context.gbufferRecording.reset();
    context.gbufferOpen = false;
    context.gbufferReady = false;
    context.gbufferRecorded = false;
    context.gbufferFailed = false;
    context.exposureDirty = true;
    context.exposurePending = false;
    context.exposureRecording.reset();
    context.recording.reset();
    context.compositeRecorded = false;
    context.pendingPath = {};
    context.denoisePending = false;
    context.srPending = false;
    context.reconstructed = {};
}

bool currentRtReplay(const RtFrameContext& context, CommandContext* encoder, uint64_t revision) {
    if (!encoder || !encoder->listOpen || !encoder->commandList || !encoder->inRenderPass ||
        !context.gbufferOpen || revision != context.gbufferRevision ||
        !currentRtRecording(context.gbufferRecording, encoder) || encoder->activeColorTargets.size() != 5 ||
        encoder->activeDepthTarget != context.gbuffer[5].get()) return false;
    for (size_t index = 0; index < 5; ++index) {
        if (encoder->activeColorTargets[index] != context.gbuffer[index].get()) return false;
    }
    return true;
}

bool replayBufferRange(Dx12Object* buffer, int64_t offset, int64_t length, int64_t alignment) {
    return buffer && buffer->kind == Dx12Object::Kind::Buffer && buffer->resource &&
        buffer->heapType != D3D12_HEAP_TYPE_READBACK && offset >= 0 && offset < buffer->size &&
        offset % alignment == 0 && length > 0 && length <= buffer->size - offset;
}

mcdr::Result<void> validateRtBatch(const mcdr_rt_gbuffer_batch& source, const Dx12Pipeline* pipeline) {
    const std::array formats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
    if (!pipeline || !pipeline->isMesh || pipeline->hasAmplification || !pipeline->reverseDepthWrite || !pipeline->withDepth ||
        !pipeline->rootSignature || pipeline->geometryParam == 0 ||
        !std::ranges::equal(pipeline->colorFormats, formats) || source.bindingCount <= 0 ||
        source.bindingCount > 64 || !source.bindings || source._pad0 != 0 ||
        pipeline->bindingTypes.size() != static_cast<size_t>(source.bindingCount) ||
        source.projectionBinding < 0 || source.projectionBinding >= source.bindingCount ||
        source.meshletCount <= 0 || source.meshletCount > 65535) {
        return mcdr::err("rt frame", "rt gbuffer: invalid batch layout");
    }
    if (!replayBufferRange(source.vertices, source.vertexOffset, 4, 4) ||
        !replayBufferRange(source.sections, source.sectionsOffset, source.sectionsLength, 4) ||
        source.sectionsLength % 32 != 0 ||
        !replayBufferRange(source.meshlets, source.meshletsOffset, source.meshletsLength, 4) ||
        source.meshletsLength % 32 != 0 || int64_t{source.meshletCount} * 32 > source.meshletsLength) {
        return mcdr::err("rt frame", "rt gbuffer: invalid geometry range");
    }
    for (int32_t index = 0; index < source.bindingCount; ++index) {
        const auto binding = source.bindings[index];
        if (binding.type < 0 || binding.type > 3 || binding.type != pipeline->bindingTypes[static_cast<size_t>(index)]) {
            return mcdr::err("rt frame", "rt gbuffer: binding type mismatch");
        }
        if (index == source.projectionBinding) {
            if (binding.type != 0) return mcdr::err("rt frame", "rt gbuffer: projection must be a constant");
        } else if (binding.type == 1) {
            const auto* view = binding.view;
            if (!view || view->kind != Dx12Object::Kind::TextureView || !view->cpuHandle.ptr ||
                !view->sourceTexture || !view->sourceTexture->resource) {
                return mcdr::err("rt frame", "rt gbuffer: invalid texture binding");
            }
        } else if (!replayBufferRange(binding.buffer, binding.offset, binding.length, binding.type == 0 ? 256 : 4) ||
            (binding.type == 0 && binding.length > 65536) || (binding.type == 3 && binding.length % 4 != 0) ||
            (binding.type == 2 && toDxgiFormat(binding.texelFormat) == DXGI_FORMAT_UNKNOWN)) {
            return mcdr::err("rt frame", "rt gbuffer: invalid binding range");
        }
    }
    return {};
}

uint64_t upscaleBytes(const RtFrameContext& context) {
    uint64_t bytes = 0;
    for (const auto& texture : context.srTargets) {
        if (texture) {
            const auto description = texture->resource->GetDesc();
            bytes += description.Width * description.Height *
                (description.Format == DXGI_FORMAT_R32_FLOAT ? 4 : 8);
        }
    }
    return bytes;
}

uint64_t denoiseBytes(const RtFrameContext& context) {
    uint64_t bytes = 0;
    const std::array textures{context.nrdOutputs[0].get(), context.nrdOutputs[1].get(), context.rrOutput.get()};
    for (const auto* texture : textures) {
        if (texture) {
            const auto description = texture->resource->GetDesc();
            bytes += description.Width * description.Height * 8;
        }
    }
    return bytes;
}

mcdr::Result<RtObject> denoiseTarget(int32_t width, int32_t height) {
    auto texture = createTexture(36, 42, width, height, 1, 1, true);
    if (!texture) return std::unexpected(texture.error());
    return RtObject(static_cast<Dx12Object*>(*texture));
}

mcdr::Result<RtObject> frameBuffer(int usage, int64_t bytes) {
    auto result = createBuffer(usage, bytes, true, false, true);
    if (!result) return std::unexpected(result.error());
    return RtObject(static_cast<Dx12Object*>(*result));
}

mcdr::Result<RtObject> frameUpload(const void* data, int64_t bytes, int64_t capacity) {
    auto buffer = frameBuffer(2, capacity);
    if (!buffer) return std::unexpected(buffer.error());
    auto mapped = mapBuffer(buffer->get(), 0, capacity, false, true);
    if (!mapped) return std::unexpected(mapped.error());
    std::memcpy(*mapped, data, static_cast<size_t>(bytes));
    if (capacity > bytes) {
        std::memset(static_cast<uint8_t*>(*mapped) + bytes, 0, static_cast<size_t>(capacity - bytes));
    }
    unmapBuffer(buffer->get());
    return std::move(*buffer);
}

uint64_t geometryAllocationBytes(uint64_t capacity, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = (capacity + 255) & ~uint64_t{255};
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    return deviceContext().device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
}

uint64_t rtMemoryDomainLimit(RtMemoryDomain domain) {
    switch (domain) {
    case RtMemoryDomain::GeometryGpu:
    case RtMemoryDomain::GeometryUpload: return kGeometryMemoryLimit;
    case RtMemoryDomain::Acceleration: return kAccelerationMemoryLimit;
    default: return 0;
    }
}

mcdr::Result<std::shared_ptr<RtMemoryCharge>> reserveRtMemory(RtMemoryDomain domain, uint64_t bytes,
        const char* owner, const char* message) {
    auto charge = std::make_shared<RtMemoryCharge>();
    charge->memory = rtMemory;
    charge->domain = domain;
    const std::lock_guard lock(rtMemory->mutex);
    auto& account = rtMemory->domains[static_cast<size_t>(domain)];
    const auto limit = rtMemoryDomainLimit(domain);
    if (bytes == 0 || bytes > rtMemory->limit - std::min(rtMemory->limit, rtMemory->bytes) ||
        (limit != 0 && bytes > limit - std::min(limit, account.bytes))) {
        ++rtMemory->rejections;
        ++account.rejections;
        return mcdr::err(owner, message);
    }
    rtMemory->bytes += bytes;
    rtMemory->peak = std::max(rtMemory->peak, rtMemory->bytes);
    account.bytes += bytes;
    account.peak = std::max(account.peak, account.bytes);
    charge->bytes = bytes;
    return charge;
}

mcdr::Result<std::shared_ptr<RtMemoryCharge>> reserveAccelerationMemory(uint64_t capacity, bool uav) {
    const auto bytes = geometryAllocationBytes(capacity,
        uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    return reserveRtMemory(RtMemoryDomain::Acceleration, bytes,
        "rt accel", "rt accel: memory budget exhausted");
}

mcdr::Result<RtObject> geometryBuffer(int usage, int64_t bytes) {
    const bool upload = (usage & 2) != 0;
    auto charge = reserveRtMemory(upload ? RtMemoryDomain::GeometryUpload : RtMemoryDomain::GeometryGpu,
        geometryAllocationBytes(static_cast<uint64_t>(bytes)), "rt geometry", upload ?
            "rt geometry: upload budget exhausted" : "rt geometry: memory budget exhausted");
    if (!charge) return std::unexpected(charge.error());
    auto created = createBuffer(usage, bytes, true, false);
    if (!created) return std::unexpected(created.error());
    RtObject resource(static_cast<Dx12Object*>(*created));
    resource->allocationBytes = (*charge)->bytes;
    resource->allocationCharge = std::move(*charge);
    return resource;
}

mcdr::Result<RtObject> geometryUpload(const void* data, int64_t bytes, int64_t capacity) {
    auto buffer = geometryBuffer(2, capacity);
    if (!buffer) return std::unexpected(buffer.error());
    auto mapped = mapBuffer(buffer->get(), 0, capacity, false, true);
    if (!mapped) return std::unexpected(mapped.error());
    std::memcpy(*mapped, data, static_cast<size_t>(bytes));
    if (capacity > bytes) {
        std::memset(static_cast<uint8_t*>(*mapped) + bytes, 0, static_cast<size_t>(capacity - bytes));
    }
    unmapBuffer(buffer->get());
    return std::move(*buffer);
}

uint64_t resourceAllocationBytes(const RtSharedObject& object) {
    if (object->allocationBytes == 0) {
        const auto description = object->resource->GetDesc();
        object->allocationBytes = deviceContext().device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    }
    return object->allocationBytes;
}

uint64_t geometryMemory(RtMemoryDomain domain) {
    const std::lock_guard lock(rtMemory->mutex);
    return rtMemory->domains[static_cast<size_t>(domain)].bytes;
}

std::array<uint32_t, 16> geometryRecord(const RtGeometrySnapshot& snapshot, size_t index) {
    std::array<uint32_t, 16> record;
    std::memcpy(record.data(), snapshot.records->data() + index * 64, 64);
    return record;
}

std::pair<uint64_t, uint32_t> geometryKey(const std::array<uint32_t, 16>& record) {
    return {uint64_t{record[14]} | uint64_t{record[15]} << 32, record[3]};
}

RtBlasKey geometryBlasKey(const std::array<uint32_t, 16>& record) {
    const auto [node, layer] = geometryKey(record);
    return {node, layer, record[11], record[8], record[9], record[10]};
}

void collectPendingFeedback() {
    for (auto& feedback : pendingFeedback) {
        if (!feedback.recording) continue;
        const auto& recording = *feedback.recording;
        if (recording.submitted) {
            const auto completed = recording.fence ? recording.fence->GetCompletedValue() : 0;
            if (completed == UINT64_MAX || completed < recording.fenceValue) continue;
        } else if (!recording.cancelled) {
            continue;
        }
        const auto found = scenes.find(feedback.scene);
        if (feedback.accepted && recording.committed && !recording.cancelled && found != scenes.end() &&
            found->second.worldEpoch == feedback.world && found->second.frameId >= feedback.sceneFrame &&
            found->second.frameId - feedback.sceneFrame <= kPendingDemandFrames) {
            auto& scene = found->second;
            void* mapped = nullptr;
            const D3D12_RANGE range{0, kPendingFeedbackBytes};
            if (SUCCEEDED(feedback.readback->resource->Map(0, &range, &mapped))) {
                std::array<uint32_t, kPendingFeedbackBytes / sizeof(uint32_t)> bits;
                std::memcpy(bits.data(), mapped, kPendingFeedbackBytes);
                const D3D12_RANGE written{0, 0};
                feedback.readback->resource->Unmap(0, &written);
                for (const auto& demand : feedback.requests) {
                    if ((bits[demand.firstRecord / 32] & (uint32_t{1} << (demand.firstRecord % 32))) == 0) continue;
                    const auto request = scene.firstRequestedFrames.find({demand.node, demand.layer});
                    if (request == scene.firstRequestedFrames.end() || request->second.identity != demand.request ||
                        request->second.version != demand.version) continue;
                    request->second.demandFrame = std::max(request->second.demandFrame, feedback.sceneFrame);
                }
                for (const auto& demand : feedback.sources) {
                    if ((bits[demand.bit / 32] & (uint32_t{1} << (demand.bit % 32))) == 0) continue;
                    const auto request = scene.sourceRequests.find(demand.node);
                    if (request == scene.sourceRequests.end() || request->second.identity != demand.request) continue;
                    request->second.demandFrame = std::max(request->second.demandFrame, feedback.sceneFrame);
                    if (!request->second.queued && request->second.demandFrame > request->second.deliveredFrame) {
                        scene.captureDemands.emplace_back(demand.node, demand.request);
                        request->second.queued = true;
                    }
                }
            }
        }
        feedback.recording.reset();
        feedback.requests.clear();
        feedback.sources.clear();
        feedback.accepted = false;
    }
}

RtPendingFeedback* preparePendingFeedback(CommandContext* encoder, const RtAcceleration& acceleration,
    uint64_t context, uint64_t frame) {
    collectPendingFeedback();
    if (acceleration.owner == 0 || (acceleration.pendingQueries.empty() && acceleration.sourceQueries.empty())) return nullptr;
    const auto idle = std::ranges::find_if(pendingFeedback, [](const auto& feedback) { return !feedback.recording; });
    if (idle == pendingFeedback.end()) return nullptr;
    auto& feedback = *idle;
    if (!feedback.readback) {
        auto buffer = frameBuffer(1, kPendingFeedbackBytes);
        if (!buffer) return nullptr;
        feedback.readback = RtSharedObject(buffer->release(), RtObjectRelease{});
    }
    if (!pendingFeedbackZero) {
        const std::array<uint8_t, kPendingFeedbackBytes> zeros{};
        auto upload = frameUpload(zeros.data(), zeros.size(), zeros.size());
        if (!upload) return nullptr;
        pendingFeedbackZero = RtSharedObject(upload->release(), RtObjectRelease{});
    }
    const auto& scene = scenes.at(acceleration.owner);
    feedback.requests.clear();
    feedback.sources.clear();
    for (const auto& [node, identity] : acceleration.pendingQueries) {
        const auto first = identity & 0xFFFFu;
        const auto count = (identity >> 16u) & 0x7Fu;
        if (count == 0 || first >= acceleration.entries.size() || count > acceleration.entries.size() - first) continue;
        for (uint32_t index = first; index < first + count; ++index) {
            if (acceleration.entries[index]) continue;
            const auto record = geometryRecord(*acceleration.snapshot, index);
            const auto key = geometryKey(record);
            const auto request = scene.firstRequestedFrames.find(key);
            if (key.first != node || request == scene.firstRequestedFrames.end() || request->second.version != record[11]) continue;
            feedback.requests.push_back({key.first, request->second.identity, key.second, record[11], first});
        }
    }
    for (const auto& [node, bit] : acceleration.sourceQueries) {
        const auto request = scene.sourceRequests.find(node);
        if (request != scene.sourceRequests.end()) feedback.sources.push_back({node, request->second.identity, bit});
    }
    if (feedback.requests.empty() && feedback.sources.empty()) return nullptr;
    feedback.scene = acceleration.owner;
    feedback.world = scene.worldEpoch;
    feedback.frame = frame;
    feedback.sceneFrame = scene.frameId;
    feedback.context = context;
    feedback.accepted = false;
    feedback.recording = currentCommandRecording(encoder);
    auto& retained = encoder->rtGeometryUploadsRetained[encoder->currentListSlot];
    retained.push_back(feedback.readback);
    retained.push_back(pendingFeedbackZero);
    return &feedback;
}

void eraseResidentBlas(RtScene& scene, std::map<RtBlasKey, RtResidentBlas>::iterator entry) {
    scene.residentBytes -= resourceAllocationBytes(entry->second.entry->result);
    scene.residentLru.erase(entry->second.position);
    scene.resident.erase(entry);
}

void pruneResidentBlas(RtScene& scene) {
    for (auto entry = scene.resident.begin(); entry != scene.resident.end();) {
        const auto current = scene.live.find({entry->first.node, entry->first.layer});
        if (!entry->second.entry->recording || entry->second.entry->recording->cancelled ||
                (current != scene.live.end() && current->second != entry->first.version)) {
            const auto removed = entry++;
            eraseResidentBlas(scene, removed);
        } else {
            ++entry;
        }
    }
}

void rememberSceneBlas(RtScene& scene, const RtBlasKey& key, const std::shared_ptr<RtBlasEntry>& entry) {
    constexpr size_t maxEntries = 1024;
    constexpr uint64_t maxBytes = 256ULL * 1024 * 1024;
    if (!entry || !entry->recording || entry->recording->cancelled ||
            resourceAllocationBytes(entry->result) > maxBytes) return;
    const auto current = scene.live.find({key.node, key.layer});
    if (current == scene.live.end() || current->second != key.version || scene.resident.contains(key)) return;
    scene.residentLru.push_back(key);
    try {
        scene.resident.emplace(key, RtResidentBlas{entry, std::prev(scene.residentLru.end())});
    } catch (...) {
        scene.residentLru.pop_back();
        throw;
    }
    scene.residentBytes += resourceAllocationBytes(entry->result);
    while (scene.resident.size() > maxEntries || scene.residentBytes > maxBytes) {
        eraseResidentBlas(scene, scene.resident.find(scene.residentLru.front()));
    }
}

void rememberSceneBlas(RtScene& scene, const RtAcceleration& acceleration) {
    for (size_t index = 0; index < acceleration.entries.size(); ++index) {
        rememberSceneBlas(scene, geometryBlasKey(geometryRecord(*acceleration.snapshot, index)), acceleration.entries[index]);
    }
}

void reuseResidentBlas(RtScene& scene, RtAcceleration& acceleration) {
    for (size_t index = 0; index < acceleration.entries.size(); ++index) {
        const auto key = geometryBlasKey(geometryRecord(*acceleration.snapshot, index));
        const auto current = scene.live.find({key.node, key.layer});
        if (current == scene.live.end() || current->second != key.version) continue;
        const auto cached = scene.resident.find(key);
        if (cached == scene.resident.end() || !cached->second.entry->recording ||
                cached->second.entry->recording->cancelled) continue;
        if (!acceleration.entries[index]) acceleration.entries[index] = cached->second.entry;
        scene.residentLru.splice(scene.residentLru.end(), scene.residentLru, cached->second.position);
    }
}

void trackSceneResource(const RtAcceleration& acceleration, const RtSharedObject& resource) {
    if (acceleration.owner != 0) scenes.at(acceleration.owner).resources.insert_or_assign(resource.get(), resource);
}

uint64_t sceneAccelerationBytes(RtScene& scene) {
    uint64_t bytes = 0;
    for (auto entry = scene.resources.begin(); entry != scene.resources.end();) {
        if (const auto resource = entry->second.lock()) {
            bytes += resourceAllocationBytes(resource);
            ++entry;
        } else {
            entry = scene.resources.erase(entry);
        }
    }
    return bytes;
}

std::flat_set<uint64_t> sceneInvalidSections(const RtScene& scene, const RtAcceleration& acceleration) {
    std::vector<uint64_t> recordNodes;
    std::vector<std::pair<uint64_t, uint32_t>> recordKeys;
    recordNodes.reserve(acceleration.entries.size());
    recordKeys.reserve(acceleration.entries.size());
    std::vector<uint64_t> invalidRecords;
    invalidRecords.reserve(acceleration.entries.size());
    for (size_t index = 0; index < acceleration.entries.size(); ++index) {
        const auto record = geometryRecord(*acceleration.snapshot, index);
        const auto key = geometryKey(record);
        recordNodes.push_back(key.first);
        recordKeys.push_back(key);
        const auto current = scene.live.find(key);
        if (!acceleration.entries[index] || !acceleration.entries[index]->recording ||
                acceleration.entries[index]->recording->cancelled ||
                current == scene.live.end() || current->second != record[11]) {
            invalidRecords.push_back(key.first);
        }
    }
    const std::flat_set<uint64_t> nodes(std::move(recordNodes));
    const std::flat_set<std::pair<uint64_t, uint32_t>> versions(std::move(recordKeys));
    for (const auto node : nodes) {
        for (auto current = scene.live.lower_bound({node, 0});
                current != scene.live.end() && current->first.first == node; ++current) {
            if (!versions.contains(current->first)) invalidRecords.push_back(node);
        }
    }
    std::vector<uint64_t> invalidTargets;
    invalidTargets.reserve(scene.targets.size());
    auto recordNode = nodes.begin();
    auto bounds = scene.outputBounds.begin();
    for (const auto node : scene.targets) {
        while (recordNode != nodes.end() && *recordNode < node) ++recordNode;
        while (bounds != scene.outputBounds.end() && bounds->first < node) ++bounds;
        if (recordNode == nodes.end() || *recordNode != node || (scene.outputMode != 0 &&
            (bounds == scene.outputBounds.end() || bounds->first != node || !bounds->second.finite ||
                bounds->second.captureVersion != 0))) {
            invalidTargets.push_back(node);
        }
    }
    const std::flat_set<uint64_t> invalidNodes(std::move(invalidRecords));
    std::vector<uint64_t> invalid;
    invalid.reserve(invalidNodes.size() + invalidTargets.size());
    std::ranges::set_union(invalidNodes, invalidTargets, std::back_inserter(invalid));
    return std::flat_set<uint64_t>(std::sorted_unique, std::move(invalid));
}

std::map<uint64_t, uint32_t> scenePendingQueries(const RtScene& scene, const RtAcceleration& acceleration,
        const std::flat_set<uint64_t>& masked, CommandContext* encoder) {
    if (masked.empty()) return {};
    std::map<uint64_t, std::map<uint32_t, uint32_t>> versions;
    std::map<uint64_t, uint32_t> records;
    std::map<uint64_t, uint32_t> lastRecords;
    for (size_t index = 0; index < acceleration.entries.size(); ++index) {
        const auto record = geometryRecord(*acceleration.snapshot, index);
        const auto [node, layer] = geometryKey(record);
        if (!masked.contains(node)) continue;
        versions[node].emplace(layer, record[11]);
        const auto& entry = acceleration.entries[index];
        if (entry && entry->available(encoder)) continue;
        records.try_emplace(node, static_cast<uint32_t>(index));
        lastRecords[node] = static_cast<uint32_t>(index);
    }
    std::erase_if(records, [&](const auto& entry) {
        const auto bounds = scene.outputBounds.find(entry.first);
        if (bounds != scene.outputBounds.end() && bounds->second.captureVersion != 0) return true;
        const auto last = lastRecords.at(entry.first);
        if (last - entry.second >= 127) return true;
        for (size_t index = entry.second; index <= last; ++index) {
            const auto record = geometryRecord(*acceleration.snapshot, index);
            if (geometryKey(record).first != entry.first) return true;
            const auto current = scene.live.find(geometryKey(record));
            if (current == scene.live.end() || current->second != record[11]) return true;
        }
        const auto& layers = versions.at(entry.first);
        size_t count = 0;
        for (auto live = scene.live.lower_bound({entry.first, 0});
                live != scene.live.end() && live->first.first == entry.first; ++live) {
            const auto found = layers.find(live->first.second);
            if (found == layers.end() || found->second != live->second) return true;
            ++count;
        }
        return count != layers.size();
    });
    for (auto& [node, record] : records) record |= (lastRecords.at(node) - record + 1) << 16;
    return records;
}

std::set<uint32_t> sceneShadowProofs(const RtScene& scene, const RtAcceleration& acceleration,
        const std::flat_set<uint64_t>& masked, CommandContext* encoder) {
    std::set<uint32_t> proofs;
    if (masked.empty()) return proofs;
    for (size_t index = 0; index < acceleration.entries.size(); ++index) {
        const auto record = geometryRecord(*acceleration.snapshot, index);
        const auto key = geometryKey(record);
        const auto& entry = acceleration.entries[index];
        if (!masked.contains(key.first) || (record[10] & 0xFF00u) != 0x100u ||
                !entry || !entry->available(encoder)) continue;
        const auto current = scene.live.find(key);
        if (current != scene.live.end() && current->second == record[11]) {
            proofs.insert(static_cast<uint32_t>(index));
        }
    }
    return proofs;
}

mcdr::Result<RtSharedObject> accelerationBuffer(uint64_t bytes) {
    const auto capacity = std::max(bytes, uint64_t{256});
    auto charge = reserveAccelerationMemory(capacity, true);
    if (!charge) return std::unexpected(charge.error());
    auto result = createAccelStructBuffer(static_cast<int64_t>(capacity));
    if (!result) return std::unexpected(result.error());
    RtSharedObject resource(static_cast<Dx12Object*>(*result), RtAccelerationRelease{});
    resource->allocationBytes = (*charge)->bytes;
    resource->allocationCharge = std::move(*charge);
    return resource;
}

mcdr::Result<RtSharedObject> accelerationFrameBuffer(int usage, uint64_t bytes) {
    auto charge = reserveAccelerationMemory(bytes, (usage & 1024) != 0);
    if (!charge) return std::unexpected(charge.error());
    auto created = createBuffer(usage, static_cast<int64_t>(bytes), true, false);
    if (!created) return std::unexpected(created.error());
    RtSharedObject resource(static_cast<Dx12Object*>(*created), RtAccelerationRelease{});
    resource->allocationBytes = (*charge)->bytes;
    resource->allocationCharge = std::move(*charge);
    return resource;
}

mcdr::Result<RtSharedObject> accelerationUpload(const void* data, uint64_t bytes) {
    auto resource = accelerationFrameBuffer(2, bytes);
    if (!resource) return std::unexpected(resource.error());
    auto mapped = mapBuffer(resource->get(), 0, static_cast<int64_t>(bytes), false, true);
    if (!mapped) return std::unexpected(mapped.error());
    std::memcpy(*mapped, data, static_cast<size_t>(bytes));
    unmapBuffer(resource->get());
    return resource;
}

mcdr::Result<void> accelerationScratch(RtSharedObject& scratch, uint64_t bytes) {
    if (scratch && static_cast<uint64_t>(scratch->size) >= bytes) return {};
    const auto capacity = std::max({bytes, scratch ? uint64_t{2} * scratch->size : 0, uint64_t{256}});
    auto created = accelerationFrameBuffer(1024, capacity);
    if (!created) return std::unexpected(created.error());
    scratch = std::move(*created);
    return {};
}

void retainAcceleration(CommandContext* encoder, const RtAcceleration& acceleration) {
    auto& retained = encoder->rtAccelerationRetained[encoder->currentListSlot];
    retained.reserve(retained.size() + acceleration.entries.size() + 7);
    encoder->rtGeometryRetained[encoder->currentListSlot].push_back(acceleration.snapshot);
    for (const auto& resource : {acceleration.scratch, acceleration.tlas, acceleration.tlasScratch,
            acceleration.view, acceleration.instances}) {
        if (resource) retained.push_back(resource);
    }
    for (const auto& entry : acceleration.entries) if (entry) retained.push_back(entry->result);
    if (acceleration.proxy) retained.push_back(acceleration.proxy->result);
}

uint64_t accelerationBytes(const RtAcceleration& acceleration, std::set<Dx12Object*>& counted) {
    uint64_t bytes = 0;
    for (const auto& resource : {acceleration.scratch, acceleration.tlas, acceleration.tlasScratch, acceleration.instances}) {
        if (resource && counted.insert(resource.get()).second) bytes += resourceAllocationBytes(resource);
    }
    for (const auto& entry : acceleration.entries) {
        if (entry && counted.insert(entry->result.get()).second) bytes += resourceAllocationBytes(entry->result);
    }
    if (acceleration.proxy && counted.insert(acceleration.proxy->result.get()).second) {
        bytes += resourceAllocationBytes(acceleration.proxy->result);
    }
    return bytes;
}

void discardCancelledAcceleration(RtAcceleration& acceleration) {
    bool cancelled = acceleration.recording && acceleration.recording->cancelled;
    for (auto& entry : acceleration.entries) {
        if (entry && (!entry->recording || entry->recording->cancelled)) {
            entry.reset();
            ++acceleration.entryRevision;
            cancelled = true;
        }
    }
    if (acceleration.proxy && (!acceleration.proxy->recording || acceleration.proxy->recording->cancelled)) {
        acceleration.proxy.reset();
        cancelled = true;
    }
    if (!cancelled) return;
    acceleration.tlas.reset();
    acceleration.view.reset();
    acceleration.instances.reset();
    acceleration.recording.reset();
    acceleration.masked.clear();
    acceleration.pendingQueries.clear();
    acceleration.sourceQueries.clear();
    acceleration.shadowProofs.clear();
    acceleration.coverage = {};
    acceleration.instanceCount = 0;
    acceleration.complete = std::ranges::all_of(acceleration.entries, [](const auto& entry) { return bool(entry); });
}

void refreshSceneBudget(RtScene& scene, uint64_t frameId) {
    if (frameId != scene.frameId) {
        scene.builds = 0;
        scene.buildRecordings.clear();
        scene.work.cpuNanos = 0;
        scene.work.predictedNanos = 0;
        scene.work.triangles = 0;
        scene.work.publications = 0;
        scene.work.cpuExceeded = false;
        scene.work.recordings.clear();
    } else {
        for (auto entry = scene.buildRecordings.begin(); entry != scene.buildRecordings.end();) {
            if (entry->first->cancelled && !entry->first->submitted) {
                scene.builds -= entry->second;
                entry = scene.buildRecordings.erase(entry);
            } else {
                ++entry;
            }
        }
        for (auto entry = scene.work.recordings.begin(); entry != scene.work.recordings.end();) {
            if (entry->first->cancelled && !entry->first->submitted) {
                scene.work.triangles -= entry->second.triangles;
                scene.work.predictedNanos -= entry->second.predictedNanos;
                scene.work.publications -= entry->second.publications;
                entry = scene.work.recordings.erase(entry);
            } else {
                ++entry;
            }
        }
    }
    scene.frameId = frameId;
}

void collectSceneWork(RtSceneWork& work) {
    for (auto entry = work.pending.begin(); entry != work.pending.end();) {
        const auto& recording = *entry->recording;
        if (recording.submitted) {
            const auto completed = recording.fence ? recording.fence->GetCompletedValue() : 0;
            if (completed == UINT64_MAX || completed < recording.fenceValue) {
                ++entry;
                continue;
            }
        } else if (!recording.cancelled) {
            ++entry;
            continue;
        }
        if (recording.cancelled || !recording.committed) {
            ++work.cancelledSamples;
            entry->pool->pending.reset();
        } else {
            std::array<long long, 2> timestamps{};
            const auto read = tryReadQueryValues(entry->pool.get(), 0, 2, timestamps.data());
            if (!read || !*read || timestamps[1] < timestamps[0] || getTimestampFrequency() == 0) {
                ++work.failures;
                entry->pool->pending.reset();
            } else {
                const auto nanos = static_cast<uint64_t>((timestamps[1] - timestamps[0]) *
                    (1000000000.0L / getTimestampFrequency()));
                ++work.samples;
                work.lastGpuNanos = nanos;
                work.totalGpuNanos += nanos;
                work.peakGpuNanos = std::max(work.peakGpuNanos, nanos);
                work.lastGpuFrame = entry->frame;
                work.lastGpuWorld = entry->world;
                if (nanos > entry->gpuLimitNanos) ++work.gpuOverruns;
                if (entry->predictedNanos != 0) {
                    const auto observed = static_cast<uint64_t>(std::clamp(
                        static_cast<long double>(entry->factor) * nanos / entry->predictedNanos, 256.0L, 1048576.0L));
                    work.factor = observed > work.factor ? observed : (work.factor * 7 + observed) / 8;
                }
            }
        }
        work.free.push_back(std::move(entry->pool));
        entry = work.pending.erase(entry);
    }
}

mcdr::Result<void> beginSceneWork(CommandContext* encoder, RtScene& scene) {
    auto& work = scene.work;
    if (work.cpuLimitNanos == 0 || work.timing) return {};
    if (work.pending.size() >= 16) {
        ++work.deferred;
        return mcdr::err("rt scene", "rt scene: timing feedback pending");
    }
    std::unique_ptr<QueryPool> pool;
    if (!work.free.empty()) {
        pool = std::move(work.free.back());
        work.free.pop_back();
    } else {
        auto created = createQueryPool(2);
        if (!created) return std::unexpected(created.error());
        pool.reset(static_cast<QueryPool*>(*created));
    }
    auto written = writeTimestampToPool(encoder, pool.get(), 0);
    if (!written) return written;
    work.pending.push_back({std::move(pool), currentCommandRecording(encoder), scene.frameId,
        scene.worldEpoch, 0, work.gpuLimitNanos, work.factor});
    work.timing = &work.pending.back();
    return {};
}

struct RtSceneWorkAttempt {
    CommandContext* encoder;
    RtSceneWork& work;
    bool finished = false;

    mcdr::Result<void> finish() {
        if (finished || work.cpuLimitNanos == 0) return {};
        finished = true;
        mcdr::Result<void> result;
        auto* timing = std::exchange(work.timing, nullptr);
        if (timing) {
            result = writeTimestampToPool(encoder, timing->pool.get(), 1);
            if (result) result = queueQueryValues(encoder, timing->pool.get(), 0, 2);
            if (!result) ++work.failures;
        }
        work.lastCpuNanos = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - work.started).count());
        work.cpuNanos += work.lastCpuNanos;
        if (!work.cpuExceeded && work.cpuNanos > work.cpuLimitNanos) {
            work.cpuExceeded = true;
            ++work.cpuOverruns;
        }
        return result;
    }

    ~RtSceneWorkAttempt() {
        if (finished) return;
        try { (void)finish(); } catch (...) { ++work.failures; }
    }
};

uint64_t sceneBlasCost(const RtSceneWork& work, uint64_t triangles) {
    return (32000 + triangles * 8) * work.factor / 1024;
}

bool admitSceneBlas(RtScene& scene, uint64_t triangles) {
    auto& work = scene.work;
    if (work.cpuLimitNanos == 0) return true;
    const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - work.started).count());
    const auto predicted = sceneBlasCost(work, triangles);
    const auto publication = work.publications == 0 ? 200000 * work.factor / 1024 : 0;
    const bool first = scene.builds == 0 && work.triangles == 0;
    if ((work.cpuNanos + elapsed >= work.cpuLimitNanos && !first) ||
        (work.cpuNanos >= work.cpuLimitNanos) ||
        ((triangles > work.triangleLimit - std::min(work.triangleLimit, work.triangles) ||
          predicted + publication > work.gpuLimitNanos - std::min(work.gpuLimitNanos, work.predictedNanos)) && !first)) {
        ++work.deferred;
        return false;
    }
    return true;
}

void chargeSceneBlas(CommandContext* encoder, RtScene& scene, uint64_t triangles) {
    auto& work = scene.work;
    if (work.cpuLimitNanos == 0) return;
    const auto predicted = sceneBlasCost(work, triangles);
    if (triangles > work.triangleLimit || predicted > work.gpuLimitNanos) ++work.oversized;
    work.triangles += triangles;
    work.predictedNanos += predicted;
    auto& recording = work.recordings[currentCommandRecording(encoder)];
    recording.triangles += triangles;
    recording.predictedNanos += predicted;
    work.timing->predictedNanos += predicted;
}

void accelerationOutput(const RtAcceleration& acceleration, mcdr_rt_acceleration_output& output) {
    std::set<Dx12Object*> counted;
    output.snapshotId = acceleration.snapshotId;
    output.acceleration = acceleration.view.get();
    output.instances = acceleration.instances.get();
    output.byteCount = accelerationBytes(acceleration, counted);
    output.instanceCount = acceleration.instanceCount;
    output.blasCount = static_cast<uint32_t>(std::ranges::count_if(acceleration.entries, [](const auto& entry) { return bool(entry); }));
    output.ready = acceleration.view != nullptr && (acceleration.complete || acceleration.owner != 0) ? 1 : 0;
    output.scratchBytes = acceleration.scratch ? acceleration.scratch->size : 0;
}

mcdr::Result<std::shared_ptr<RtBlasEntry>> recordBlas(CommandContext* encoder, RtAcceleration& acceleration,
    const mcdr_rt_geometry& geometry, uint32_t version, uint32_t flags, bool procedural = false) {
    std::array<unsigned long long, 2> sizes{};
    auto queried = procedural ? getAabbBlasPrebuildInfo(sizes.data()) : getBlasPrebuildInfo(&geometry, 1, sizes.data());
    if (!queried) return std::unexpected(queried.error());
    auto result = accelerationBuffer(sizes[0]);
    if (!result) return std::unexpected(result.error());
    auto scratch = accelerationScratch(acceleration.scratch, sizes[1]);
    if (!scratch) return std::unexpected(scratch.error());
    auto entry = std::make_shared<RtBlasEntry>();
    entry->result = std::move(*result);
    entry->version = version;
    entry->vertices = static_cast<uint32_t>(geometry.vertexCount);
    entry->indices = static_cast<uint32_t>(geometry.indexCount);
    entry->flags = flags;
    entry->recording = currentCommandRecording(encoder);
    auto& retained = encoder->rtAccelerationRetained[encoder->currentListSlot];
    retained.reserve(retained.size() + 2);
    retained.push_back(entry->result);
    retained.push_back(acceleration.scratch);
    trackSceneResource(acceleration, entry->result);
    trackSceneResource(acceleration, acceleration.scratch);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = acceleration.scratch->resource.Get();
    encoder->commandList->ResourceBarrier(1, &barrier);
    auto built = procedural ? buildAabbBlas(encoder, entry->result.get(), acceleration.scratch.get(),
        static_cast<Dx12Object*>(geometry.vertexBuffer)) :
        buildBlas(encoder, entry->result.get(), acceleration.scratch.get(), &geometry, 1, false);
    if (!built) return std::unexpected(built.error());
    acceleration.recording = entry->recording;
    return entry;
}

mcdr::Result<void> accelerationProxy(CommandContext* encoder, RtAcceleration& acceleration) {
    if (acceleration.proxy) {
        if (!acceleration.proxy->available(encoder)) return mcdr::err("rt accel", "rt accel: proxy pending");
        return {};
    }
    constexpr std::array<float, 6> data{0.0f, 0.0f, 0.0f, 16.0f, 16.0f, 16.0f};
    auto uploaded = accelerationUpload(data.data(), sizeof(data));
    auto target = accelerationFrameBuffer(8, sizeof(data));
    if (!uploaded) return std::unexpected(uploaded.error());
    if (!target) return std::unexpected(target.error());
    auto source = std::move(*uploaded);
    auto vertices = std::move(*target);
    auto& retained = encoder->rtAccelerationRetained[encoder->currentListSlot];
    retained.reserve(retained.size() + 2);
    retained.push_back(source);
    retained.push_back(vertices);
    trackSceneResource(acceleration, source);
    trackSceneResource(acceleration, vertices);
    auto copied = copyBufferToBuffer(encoder, source.get(), 0, vertices.get(), 0, sizeof(data));
    if (!copied) return copied;
    const mcdr_rt_geometry geometry{vertices.get(), sizeof(D3D12_RAYTRACING_AABB), 1, 0, nullptr, 0, 0, 0, 0};
    auto built = recordBlas(encoder, acceleration, geometry, 0, 4, true);
    if (!built) return std::unexpected(built.error());
    acceleration.proxy = std::move(*built);
    return {};
}

int32_t sectionCoordinate(uint64_t node, unsigned shift, unsigned width) {
    const auto bits = static_cast<uint32_t>((node >> shift) & ((uint64_t{1} << width) - 1));
    return static_cast<int32_t>(bits << (32 - width)) >> (32 - width);
}

std::optional<std::array<float, 6>> sceneProxyRange(uint64_t node, const RtOutputBounds& bounds) {
    if (!bounds.finite) return std::nullopt;
    const std::array origins{sectionCoordinate(node, 42, 22) * 16,
        sectionCoordinate(node, 0, 20) * 16, sectionCoordinate(node, 20, 22) * 16};
    std::array<float, 6> range{};
    for (size_t axis = 0; axis < 3; ++axis) {
        const auto minimum = static_cast<double>(origins[axis]) + bounds.values[axis];
        const auto maximum = static_cast<double>(origins[axis]) + bounds.values[axis + 3];
        auto translation = static_cast<float>(minimum);
        if (translation > minimum) translation = std::nextafter(translation, -INFINITY);
        auto upper = static_cast<float>(maximum);
        if (upper < maximum) upper = std::nextafter(upper, INFINITY);
        const auto extent = (static_cast<double>(upper) - translation) / 16.0;
        auto scale = static_cast<float>(extent);
        if (scale < extent) scale = std::nextafter(scale, INFINITY);
        if (!std::isfinite(translation) || !std::isfinite(upper) || !std::isfinite(scale) || scale <= 0.0f ||
            !std::isfinite(1.0f / scale) || 1.0f / scale < std::numeric_limits<float>::min() ||
            double{translation} > minimum || double{translation} + 16.0 * scale < maximum) {
            return std::nullopt;
        }
        range[axis] = translation;
        range[axis + 3] = scale;
    }
    return range;
}

RtProxyCoverage sceneProxyCoverage(const RtScene* scene, const std::flat_set<uint64_t>& masked) {
    RtProxyCoverage coverage;
    std::vector<uint64_t> nodes;
    std::vector<std::array<float, 6>> ranges;
    nodes.reserve(masked.size());
    ranges.reserve(masked.size());
    if (scene && scene->outputMode != 0) {
        auto found = masked.empty() ? scene->outputBounds.end() : scene->outputBounds.lower_bound(*masked.begin());
        for (const auto node : masked) {
            while (found != scene->outputBounds.end() && found->first < node) ++found;
            if (found == scene->outputBounds.end() || found->first != node || !found->second.proxyRange) {
                coverage.unbounded = true;
                continue;
            }
            nodes.push_back(node);
            ranges.push_back(*found->second.proxyRange);
        }
    } else {
        for (const auto node : masked) {
            nodes.push_back(node);
            ranges.push_back({
                static_cast<float>(sectionCoordinate(node, 42, 22) * 16),
                static_cast<float>(sectionCoordinate(node, 0, 20) * 16),
                static_cast<float>(sectionCoordinate(node, 20, 22) * 16), 1.0f, 1.0f, 1.0f});
        }
    }
    coverage.ranges = decltype(coverage.ranges)(std::sorted_unique, std::move(nodes), std::move(ranges));
    return coverage;
}

bool containsProxyCoverage(const RtProxyCoverage& published, const RtProxyCoverage& required) {
    if (published.unbounded) return true;
    if (required.unbounded) return false;
    auto previous = published.ranges.begin();
    for (const auto& [node, bounds] : required.ranges) {
        while (previous != published.ranges.end() && previous->first < node) ++previous;
        if (previous == published.ranges.end() || previous->first != node) return false;
        for (size_t axis = 0; axis < 3; ++axis) {
            const auto& retained = previous->second;
            if (retained[axis] > bounds[axis] ||
                static_cast<double>(retained[axis]) + static_cast<double>(retained[axis + 3]) * 16.0 <
                    static_cast<double>(bounds[axis]) + static_cast<double>(bounds[axis + 3]) * 16.0) return false;
        }
    }
    return true;
}

mcdr::Result<void> recordTlas(CommandContext* encoder, RtAcceleration& acceleration, const std::flat_set<uint64_t>& masked,
        RtScene* scene = nullptr, std::optional<RtProxyCoverage> preparedCoverage = std::nullopt) {
    if (scene && scene->work.cpuLimitNanos != 0) {
        auto& work = scene->work;
        if (work.publications != 0) {
            ++work.deferred;
            return mcdr::err("rt scene", "rt scene: publication budget exhausted");
        }
        auto timing = beginSceneWork(encoder, *scene);
        if (!timing) return timing;
        const auto predicted = (200000 + acceleration.entries.size() * 1000) * work.factor / 1024;
        ++work.publications;
        work.predictedNanos += predicted;
        auto& recording = work.recordings[currentCommandRecording(encoder)];
        ++recording.publications;
        recording.predictedNanos += predicted;
        work.timing->predictedNanos += predicted;
    }
    if (!masked.empty()) {
        auto proxy = accelerationProxy(encoder, acceleration);
        if (!proxy) return proxy;
    }
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    instances.reserve(acceleration.entries.size() + masked.size());
    const auto pending = scene ? scenePendingQueries(*scene, acceleration, masked, encoder) : std::map<uint64_t, uint32_t>{};
    const auto proofs = scene ? sceneShadowProofs(*scene, acceleration, masked, encoder) : std::set<uint32_t>{};
    auto coverage = preparedCoverage ? std::move(*preparedCoverage) : sceneProxyCoverage(scene, masked);
    std::set<uint32_t> feedbackBits;
    for (const auto& [node, identity] : pending) feedbackBits.insert(identity & 0xFFFFu);
    std::vector<uint64_t> sourceQueryNodes;
    std::vector<uint32_t> sourceQueryBits;
    sourceQueryNodes.reserve(masked.size());
    sourceQueryBits.reserve(masked.size());
    uint32_t sourceBit = 0;
    const auto append = [&](uint32_t index, const std::shared_ptr<RtBlasEntry>& entry,
            int32_t originX, int32_t originY, int32_t originZ, uint32_t hitGroup, uint8_t mask = 1,
            const std::array<float, 6>* range = nullptr) {
        D3D12_RAYTRACING_INSTANCE_DESC instance{};
        instance.Transform[0][0] = range ? (*range)[3] : 1.0f;
        instance.Transform[1][1] = range ? (*range)[4] : 1.0f;
        instance.Transform[2][2] = range ? (*range)[5] : 1.0f;
        instance.Transform[0][3] = static_cast<float>(originX);
        instance.Transform[1][3] = static_cast<float>(originY);
        instance.Transform[2][3] = static_cast<float>(originZ);
        if (range) {
            for (size_t axis = 0; axis < 3; ++axis) instance.Transform[axis][3] = (*range)[axis];
        }
        instance.InstanceID = index;
        instance.InstanceMask = mask;
        instance.InstanceContributionToHitGroupIndex = hitGroup;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instance.AccelerationStructure = entry->result->resource->GetGPUVirtualAddress();
        instances.push_back(instance);
    };
    for (size_t index = 0; index < acceleration.entries.size(); ++index) {
        const auto record = geometryRecord(*acceleration.snapshot, index);
        const auto key = geometryKey(record);
        const auto& entry = acceleration.entries[index];
        uint8_t mask = 1;
        if (masked.contains(key.first)) {
            const auto group = pending.find(key.first);
            const auto first = group != pending.end() ? group->second & 0xFFFFu : 0;
            const auto count = group != pending.end() ? group->second >> 16 : 0;
            if (group == pending.end() || (index >= first && index - first < count)) {
                if (!proofs.contains(static_cast<uint32_t>(index))) continue;
                mask = 0x80;
            }
        }
        if (!entry || !entry->available(encoder)) return mcdr::err("rt accel", "rt accel: BLAS unavailable");
        append(static_cast<uint32_t>(index), entry, std::bit_cast<int32_t>(record[0]),
            std::bit_cast<int32_t>(record[1]), std::bit_cast<int32_t>(record[2]),
            (record[10] & 0x600) != 0 ? 1 : 0, mask);
    }
    uint32_t proxyIndex = static_cast<uint32_t>(acceleration.entries.size());
    auto range = coverage.ranges.begin();
    auto sourceRequest = scene ? scene->sourceRequests.begin() : decltype(scene->sourceRequests.begin()){};
    for (const auto node : masked) {
        auto identity = proxyIndex++;
        while (range != coverage.ranges.end() && range->first < node) ++range;
        if (range == coverage.ranges.end() || range->first != node) continue;
        if (const auto found = pending.find(node); found != pending.end()) {
            identity = 0x800000u | found->second;
        } else if (scene) {
            while (sourceRequest != scene->sourceRequests.end() && sourceRequest->first < node) ++sourceRequest;
            if (sourceRequest != scene->sourceRequests.end() && sourceRequest->first == node) {
                while (sourceBit < 65535 && feedbackBits.contains(sourceBit)) ++sourceBit;
                if (sourceBit < 65535) {
                    sourceQueryNodes.push_back(node);
                    sourceQueryBits.push_back(sourceBit);
                    identity = kSourceDemandInstanceBit | sourceBit++;
                }
            }
        }
        append(identity, acceleration.proxy, 0, 0, 0, 2, 1, &range->second);
    }
    if (instances.empty() && coverage.unbounded) {
        append(proxyIndex, acceleration.proxy, 0, 0, 0, 2, 0);
    }
    if (instances.empty()) return mcdr::err("rt accel", "rt accel: empty scene");
    std::array<unsigned long long, 2> sizes{};
    auto queried = getTlasPrebuildInfo(static_cast<int>(instances.size()), sizes.data());
    if (!queried) return queried;
    auto result = accelerationBuffer(sizes[0]);
    if (!result) return std::unexpected(result.error());
    auto scratch = accelerationScratch(acceleration.tlasScratch, sizes[1]);
    if (!scratch) return scratch;
    const auto bytes = static_cast<int64_t>(instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    auto uploaded = accelerationUpload(instances.data(), static_cast<uint64_t>(bytes));
    if (!uploaded) return std::unexpected(uploaded.error());
    auto table = std::move(*uploaded);
    auto viewResult = createAccelStructView(result->get());
    if (!viewResult) return std::unexpected(viewResult.error());
    RtSharedObject view(static_cast<Dx12Object*>(*viewResult), RtAccelerationRelease{});
    retainAcceleration(encoder, acceleration);
    auto& retained = encoder->rtAccelerationRetained[encoder->currentListSlot];
    retained.reserve(retained.size() + 3);
    retained.push_back(*result);
    retained.push_back(view);
    retained.push_back(table);
    trackSceneResource(acceleration, *result);
    trackSceneResource(acceleration, acceleration.tlasScratch);
    trackSceneResource(acceleration, table);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = acceleration.tlasScratch->resource.Get();
    encoder->commandList->ResourceBarrier(1, &barrier);
    auto built = buildTlas(encoder, result->get(), acceleration.tlasScratch.get(), table.get(),
        static_cast<int>(instances.size()), true);
    if (!built) return built;
    acceleration.tlas = std::move(*result);
    acceleration.view = std::move(view);
    acceleration.instances = std::move(table);
    acceleration.instanceCount = static_cast<uint32_t>(instances.size());
    acceleration.recording = currentCommandRecording(encoder);
    acceleration.masked = masked;
    acceleration.pendingQueries = pending;
    acceleration.sourceQueries = decltype(acceleration.sourceQueries)(std::sorted_unique,
        std::move(sourceQueryNodes), std::move(sourceQueryBits));
    acceleration.shadowProofs = proofs;
    acceleration.coverage = std::move(coverage);
    if (!pending.empty()) {
        dbgLogDebug("rt scene: %zu query sections, %zu unknown sections", pending.size(), masked.size() - pending.size());
    }
    return {};
}

bool frameGuide(Dx12Object* texture, int32_t width, int32_t height, DXGI_FORMAT format) {
    if (!texture || texture->kind != Dx12Object::Kind::Texture || !texture->resource ||
        texture->dxgiFormat != format) return false;
    const auto description = texture->resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.Width == static_cast<uint64_t>(width) &&
        description.Height == static_cast<uint32_t>(height) && description.DepthOrArraySize == 1 &&
        description.SampleDesc.Count == 1 &&
        (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
}

mcdr::Result<void> encodeFrameSignals(CommandContext* encoder, Dx12Object* pipeline,
    const std::array<RtObject, kTraceUavs>& targets, const std::array<RtObject, kSignalUavs>& signals,
    const std::array<Dx12Object*, 4>& guides, Dx12Object* viewZ, Dx12Object* constants,
    int32_t width, int32_t height, bool encodeReblur, Dx12Object* source = nullptr,
    Dx12Object* exposure = nullptr, std::array<float, 2> contentJitter = {}) {
    const std::array signalSrvs{targets[0].get(), targets[1].get(), targets[2].get(), viewZ,
        guides[0], guides[1], targets[3].get(), targets[4].get(), guides[2], guides[3],
        source ? source : guides[0], exposure ? exposure : viewZ};
    const std::array signalUavs{signals[0].get(), signals[1].get(), signals[2].get(), signals[3].get()};
    const std::array encoding{encodeReblur ? 1 : 0, source ? 1 : 0,
        std::bit_cast<int32_t>(contentJitter[0]), std::bit_cast<int32_t>(contentJitter[1])};
    if (dispatchComputeEx(encoder, pipeline, signalSrvs.data(), kSignalSrvs,
            signalUavs.data(), kSignalUavs, encoding.data(), static_cast<int32_t>(encoding.size()), constants, 0,
            (width + 7) / 8, (height + 7) / 8, 1) != 1) {
        return mcdr::err("rt frame", "rt frame: signal encoding failed");
    }
    return {};
}

mcdr::Result<void> recordRtGBuffer(CommandContext* encoder, uint64_t handle, const void* replayData,
    std::span<const std::shared_ptr<RtGBufferProgram>> capturedPrograms = {});

}

mcdr::Result<void> createRtGeometrySnapshot(CommandContext* encoder, const void* descriptionData, void* outputData) {
    auto* output = static_cast<mcdr_rt_geometry_output*>(outputData);
    if (output) *output = {};
    const auto* description = static_cast<const mcdr_rt_geometry_desc*>(descriptionData);
    if (!description || !output || !encoder || !encoder->listOpen || !encoder->commandList ||
        !encoder->fence || encoder->inRenderPass) {
        return mcdr::err("rt geometry", "rt geometry: open main list required");
    }
    const auto request = *description;
    const std::array data{request.records, request.vertices, request.indices, request.materials, request.quads};
    const std::array lengths{request.recordBytes, request.vertexBytes, request.indexBytes,
        request.materialBytes, request.quadBytes};
    constexpr int64_t maxPayload = 512LL * 1024 * 1024;
    for (size_t index = 0; index < data.size(); ++index) {
        const bool reused = (index == 3 ? request.reuseMaterials : request.reuseGeometry) != 0;
        if (reused ? data[index] != nullptr || lengths[index] != 0 :
            data[index] == nullptr || lengths[index] <= 0 || lengths[index] > maxPayload) {
            return mcdr::err("rt geometry", "rt geometry: invalid payload");
        }
    }
    if (request.reuseGeometry == 0 && (request.recordBytes % 64 != 0 || request.recordBytes > 64LL * 65535 ||
            request.vertexBytes % 28 != 0 || request.indexBytes % 2 != 0 || request.quadBytes % 2 != 0)) {
        return mcdr::err("rt geometry", "rt geometry: invalid geometry layout");
    }
    if (request.reuseMaterials == 0 && (request.materialBytes < 40 || request.materialBytes % 4 != 0)) {
        return mcdr::err("rt geometry", "rt geometry: invalid material layout");
    }
    std::lock_guard lock(frameMutex);
    auto snapshot = std::make_shared<RtGeometrySnapshot>();
    snapshot->worldEpoch = request.worldEpoch;
    snapshot->geometryRevision = request.geometryRevision;
    snapshot->materialRevision = request.materialRevision;
    for (const auto token : {request.reuseGeometry, request.reuseMaterials}) {
        if (token == 0) continue;
        const auto found = geometrySnapshots.find(token);
        if (found != geometrySnapshots.end() &&
                found->second->cancelled(token == request.reuseGeometry, token == request.reuseMaterials)) {
            return mcdr::err("rt geometry", "rt geometry: reuse recording cancelled");
        }
        if (found == geometrySnapshots.end() || found->second->worldEpoch != request.worldEpoch ||
            !found->second->available(encoder, token == request.reuseGeometry, token == request.reuseMaterials)) {
            return mcdr::err("rt geometry", "rt geometry: reuse snapshot unavailable");
        }
        const auto& previous = *found->second;
        if ((token == request.reuseGeometry && previous.geometryRevision != request.geometryRevision) ||
            (token == request.reuseMaterials && previous.materialRevision != request.materialRevision)) {
            return mcdr::err("rt geometry", "rt geometry: reuse version mismatch");
        }
        if (token == request.reuseGeometry) {
            snapshot->records = previous.records;
            snapshot->storage = previous.storage;
            snapshot->ranges = previous.ranges;
            snapshot->dependencies = previous.dependencies;
            for (const size_t index : {0, 1, 2, 4}) {
                snapshot->buffers[index] = previous.buffers[index];
                snapshot->recordings[index] = previous.recordings[index];
            }
        }
        if (token == request.reuseMaterials) {
            snapshot->buffers[3] = previous.buffers[3];
            snapshot->recordings[3] = previous.recordings[3];
        }
    }
    if (!snapshot->records) {
        snapshot->records = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(request.recordBytes));
        std::memcpy(snapshot->records->data(), request.records, static_cast<size_t>(request.recordBytes));
        for (size_t offset = 0; offset < snapshot->records->size(); offset += 64) {
            std::array<uint32_t, 16> record;
            std::memcpy(record.data(), snapshot->records->data() + offset, 64);
            const uint64_t vertexOffset = uint64_t{record[4]} | uint64_t{record[5]} << 32;
            const uint64_t indexOffset = uint64_t{record[6]} | uint64_t{record[7]} << 32;
            const uint32_t stride = record[10] & 255;
            if (record[8] == 0 || record[8] % 4 != 0 || record[9] == 0 || record[9] % 3 != 0 ||
                (stride != 2 && stride != 4) || vertexOffset % 28 != 0 || indexOffset % stride != 0 ||
                vertexOffset > static_cast<uint64_t>(request.vertexBytes) ||
                uint64_t{record[8]} * 28 > static_cast<uint64_t>(request.vertexBytes) - vertexOffset ||
                indexOffset > static_cast<uint64_t>(request.indexBytes) ||
                uint64_t{record[9]} * stride > static_cast<uint64_t>(request.indexBytes) - indexOffset ||
                record[13] != record[8] / 4 ||
                (uint64_t{record[12]} + record[13]) * 2 > static_cast<uint64_t>(request.quadBytes)) {
                return mcdr::err("rt geometry", "rt geometry: record range invalid");
            }
        }
    }
    uint64_t requiredMemory = 0;
    for (size_t index = 0; index < data.size(); ++index) {
        if (snapshot->buffers[index]) continue;
        requiredMemory += geometryAllocationBytes(static_cast<uint64_t>(std::max(int64_t{16},
            (lengths[index] + 3) & ~int64_t{3})));
    }
    if (requiredMemory > kGeometryMemoryLimit - std::min(kGeometryMemoryLimit, geometryMemory(RtMemoryDomain::GeometryGpu))) {
        return mcdr::err("rt geometry", "rt geometry: memory budget exhausted");
    }
    if (requiredMemory > kGeometryMemoryLimit - std::min(kGeometryMemoryLimit, geometryMemory(RtMemoryDomain::GeometryUpload))) {
        return mcdr::err("rt geometry", "rt geometry: upload budget exhausted");
    }
    std::array<RtObject, 5> uploads;
    for (size_t index = 0; index < data.size(); ++index) {
        if (snapshot->buffers[index]) continue;
        const auto capacity = std::max(int64_t{16}, (lengths[index] + 3) & ~int64_t{3});
        auto target = geometryBuffer(8, capacity);
        if (!target) return std::unexpected(target.error());
        auto upload = geometryUpload(index == 0 ? snapshot->records->data() : data[index], lengths[index], capacity);
        if (!upload) return std::unexpected(upload.error());
        snapshot->buffers[index] = std::shared_ptr<Dx12Object>(target->release(), RtObjectRelease{});
        snapshot->recordings[index] = currentCommandRecording(encoder);
        uploads[index] = std::move(*upload);
    }
    auto& retained = encoder->rtGeometryRetained[encoder->currentListSlot];
    auto& retainedUploads = encoder->rtGeometryUploadsRetained[encoder->currentListSlot];
    retained.reserve(retained.size() + 1);
    retainedUploads.reserve(retainedUploads.size() + uploads.size());
    std::array<std::shared_ptr<Dx12Object>, 5> staging;
    for (size_t index = 0; index < uploads.size(); ++index) {
        if (uploads[index]) {
            staging[index] = std::shared_ptr<Dx12Object>(uploads[index].release(), RtObjectRelease{});
        }
    }
    const auto token = nextGeometrySnapshot++;
    geometrySnapshots.emplace(token, snapshot);
    retained.push_back(snapshot);
    for (const auto& buffer : staging) if (buffer) retainedUploads.push_back(buffer);
    for (size_t index = 0; index < staging.size(); ++index) {
        if (!staging[index]) continue;
        auto copied = copyBufferToBuffer(encoder, staging[index].get(), 0, snapshot->buffers[index].get(), 0,
            snapshot->buffers[index]->size);
        if (!copied) {
            geometrySnapshots.erase(token);
            return copied;
        }
    }
    uint64_t bytes = 0;
    for (const auto& buffer : snapshot->buffers) bytes += buffer->size;
    *output = {token, snapshot->buffers[0].get(), snapshot->buffers[1].get(), snapshot->buffers[2].get(),
        snapshot->buffers[3].get(), snapshot->buffers[4].get(), bytes};
    return {};
}

mcdr::Result<void> releaseRtGeometrySnapshot(uint64_t snapshot) {
    std::lock_guard lock(frameMutex);
    if (snapshot == 0 || geometrySnapshots.erase(snapshot) == 0) {
        return mcdr::err("rt geometry", "rt geometry: unknown snapshot");
    }
    return {};
}

mcdr::Result<void> getRtGeometryMemory(void* outputData) {
    auto* output = static_cast<mcdr_rt_geometry_memory*>(outputData);
    if (!output) return mcdr::err("rt geometry", "rt geometry: memory output required");
    const std::lock_guard lock(rtMemory->mutex);
    *output = {rtMemory->domains[0].bytes, rtMemory->domains[1].bytes};
    return {};
}

mcdr::Result<void> createRtGeometryDelta(CommandContext* encoder, const void* descriptionData, void* outputData) {
    auto* output = static_cast<mcdr_rt_geometry_delta_output*>(outputData);
    if (output) *output = {};
    const auto* description = static_cast<const mcdr_rt_geometry_delta_desc*>(descriptionData);
    if (!description || !output || !encoder || !encoder->listOpen || !encoder->commandList ||
        !encoder->fence || encoder->inRenderPass) {
        return mcdr::err("rt geometry", "rt geometry: open main list required");
    }
    const auto request = *description;
    constexpr uint64_t maxMemory = kGeometryMemoryLimit;
    std::array capacities{request.vertexCapacity, request.indexCapacity, request.quadCapacity};
    const bool empty = request.contentCount == 0;
    if ((empty ? request.records != nullptr || request.contents != nullptr : !request.records || !request.contents) ||
        request.contentCount > 65535 ||
        request.recordBytes != int64_t{request.contentCount} * 64 || request.reserved != 0 ||
        std::ranges::any_of(capacities, [](uint64_t value) { return value < 16 || value > maxMemory || value % 4 != 0; }) ||
        capacities[0] + capacities[1] + capacities[2] > maxMemory ||
        (request.reuseMaterials != 0 ? request.materials != nullptr || request.materialBytes != 0 :
            !request.materials || request.materialBytes < 40 || request.materialBytes > int64_t{maxMemory} ||
            request.materialBytes % 4 != 0)) {
        return mcdr::err("rt geometry", "rt geometry: invalid delta layout");
    }
    std::lock_guard lock(frameMutex);
    auto snapshot = std::make_shared<RtGeometrySnapshot>();
    snapshot->worldEpoch = request.worldEpoch;
    snapshot->geometryRevision = request.geometryRevision;
    snapshot->materialRevision = request.materialRevision;
    for (const auto token : {request.baseSnapshot, request.reuseMaterials}) {
        if (token == 0) continue;
        const auto found = geometrySnapshots.find(token);
        const bool geometry = token == request.baseSnapshot;
        const bool material = token == request.reuseMaterials;
        if (found != geometrySnapshots.end() && found->second->cancelled(geometry, material)) {
            return mcdr::err("rt geometry", "rt geometry: reuse recording cancelled");
        }
        if (found == geometrySnapshots.end() || found->second->worldEpoch != request.worldEpoch ||
            !found->second->available(encoder, geometry, material)) {
            return mcdr::err("rt geometry", "rt geometry: reuse snapshot unavailable");
        }
        if (geometry) {
            snapshot->storage = found->second->storage;
            if (!snapshot->storage) return mcdr::err("rt geometry", "rt geometry: incremental base required");
            if (!empty && found->second->records->empty()) {
                for (size_t index = 0; index < capacities.size(); ++index) {
                    if (static_cast<uint64_t>(snapshot->storage->arenas[index].buffer->size) < capacities[index]) {
                        snapshot->storage.reset();
                        break;
                    }
                }
            }
        }
        if (material) {
            if (found->second->materialRevision != request.materialRevision) {
                return mcdr::err("rt geometry", "rt geometry: reuse version mismatch");
            }
            snapshot->buffers[3] = found->second->buffers[3];
            snapshot->recordings[3] = found->second->recordings[3];
        }
    }
    if (empty && !snapshot->storage) capacities.fill(16);
    uint64_t requiredGpu = geometryAllocationBytes(static_cast<uint64_t>(std::max(int64_t{16}, request.recordBytes)));
    if (!snapshot->buffers[3]) requiredGpu += geometryAllocationBytes(static_cast<uint64_t>(request.materialBytes));
    if (!snapshot->storage) {
        for (const auto capacity : capacities) requiredGpu += geometryAllocationBytes(capacity);
    }
    if (requiredGpu > maxMemory - std::min(maxMemory, geometryMemory(RtMemoryDomain::GeometryGpu))) {
        return mcdr::err("rt geometry", "rt geometry: memory budget exhausted");
    }
    if (!snapshot->storage) {
        snapshot->storage = std::make_shared<RtGeometryStorage>();
        for (size_t index = 0; index < capacities.size(); ++index) {
            auto target = geometryBuffer(8, static_cast<int64_t>(capacities[index]));
            if (!target) return std::unexpected(target.error());
            auto& arena = snapshot->storage->arenas[index];
            arena.buffer = RtSharedObject(target->release(), RtObjectRelease{});
            arena.free.emplace(0, capacities[index]);
        }
    }
    for (auto& arena : snapshot->storage->arenas) arena.reclaim();
    const auto recording = currentCommandRecording(encoder);
    snapshot->records = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(request.recordBytes));
    if (request.recordBytes != 0) {
        std::memcpy(snapshot->records->data(), request.records, static_cast<size_t>(request.recordBytes));
    }
    struct Copy {
        RtSharedObject target;
        const void* source;
        uint64_t offset;
        uint64_t sourceOffset;
        uint64_t bytes;
    };
    std::vector<Copy> copies;
    uint64_t uploadedBytes = 0;
    uint64_t uploadCapacity = 0;
    const auto stage = [&](const RtSharedObject& target, uint64_t offset, const void* data, uint64_t bytes) -> mcdr::Result<void> {
        copies.push_back({target, data, offset, uploadCapacity, bytes});
        uploadedBytes += bytes;
        uploadCapacity += (bytes + 3) & ~uint64_t{3};
        return {};
    };
    for (size_t index = 0; index < request.contentCount; ++index) {
        auto record = geometryRecord(*snapshot, index);
        const auto& content = request.contents[index];
        const std::array identities{content.vertexIdentity, content.indexIdentity, content.quadIdentity};
        const std::array lengths{content.vertexBytes, content.indexBytes, content.quadBytes};
        const std::array data{content.vertices, content.indices, content.quads};
        const uint32_t stride = record[10] & 255;
        if (record[8] == 0 || record[8] % 4 != 0 || record[9] == 0 || record[9] % 3 != 0 ||
            (stride != 2 && stride != 4) || record[13] != record[8] / 4 ||
            lengths[0] != int64_t{record[8]} * 28 || lengths[1] != int64_t{record[9]} * stride ||
            lengths[2] != int64_t{record[13]} * 2 || std::ranges::any_of(identities, [](auto identity) { return identity == 0; })) {
            return mcdr::err("rt geometry", "rt geometry: content range invalid");
        }
        for (size_t kind = 0; kind < 3; ++kind) {
            auto& arena = snapshot->storage->arenas[kind];
            const auto found = arena.contents.find(identities[kind]);
            auto range = found == arena.contents.end() ? nullptr : found->second.lock();
            if (range) {
                if (range->bytes != static_cast<uint64_t>(lengths[kind]) || !range->recording) {
                    return mcdr::err("rt geometry", "rt geometry: content reuse unavailable");
                }
                if (!range->recording->available(encoder)) {
                    if (!range->recording->cancelled || !data[kind]) {
                        return mcdr::err("rt geometry", "rt geometry: content reuse unavailable");
                    }
                    range.reset();
                }
            }
            if (!range) {
                if (!data[kind]) return mcdr::err("rt geometry", "rt geometry: content payload required");
                range = arena.allocate(identities[kind], static_cast<uint64_t>(lengths[kind]), kind == 0 ? 28 : 4, recording);
                if (!range) return mcdr::err("rt geometry", "rt geometry: arena capacity exhausted");
                auto staged = stage(arena.buffer, range->offset, data[kind], range->bytes);
                if (!staged) return staged;
            }
            snapshot->ranges.push_back(range);
            if (std::ranges::find(snapshot->dependencies, range->recording) == snapshot->dependencies.end()) {
                snapshot->dependencies.push_back(range->recording);
            }
            if (kind == 0) {
                record[4] = static_cast<uint32_t>(range->offset);
                record[5] = 0;
            } else if (kind == 1) {
                record[6] = static_cast<uint32_t>(range->offset);
                record[7] = 0;
            } else {
                record[12] = static_cast<uint32_t>(range->offset / 2);
            }
        }
        std::memcpy(snapshot->records->data() + index * 64, record.data(), 64);
    }
    for (const auto [kind, slot] : {std::pair{0, 1}, {1, 2}, {2, 4}}) {
        snapshot->buffers[slot] = snapshot->storage->arenas[kind].buffer;
        snapshot->recordings[slot] = recording;
    }
    for (const auto slot : {0, 3}) {
        if (snapshot->buffers[slot]) continue;
        const auto bytes = slot == 0 ? request.recordBytes : request.materialBytes;
        auto target = geometryBuffer(8, std::max(int64_t{16}, (bytes + 3) & ~int64_t{3}));
        if (!target) return std::unexpected(target.error());
        snapshot->buffers[slot] = RtSharedObject(target->release(), RtObjectRelease{});
        snapshot->recordings[slot] = recording;
        if (bytes != 0) {
            auto staged = stage(snapshot->buffers[slot], 0, slot == 0 ? snapshot->records->data() : request.materials,
                static_cast<uint64_t>(bytes));
            if (!staged) return staged;
        }
    }
    RtSharedObject staging;
    if (uploadCapacity != 0) {
        uploadCapacity = std::max(uint64_t{16}, uploadCapacity);
        if (geometryAllocationBytes(uploadCapacity) >
            maxMemory - std::min(maxMemory, geometryMemory(RtMemoryDomain::GeometryUpload))) {
            return mcdr::err("rt geometry", "rt geometry: upload budget exhausted");
        }
        auto upload = geometryBuffer(2, static_cast<int64_t>(uploadCapacity));
        if (!upload) return std::unexpected(upload.error());
        staging = RtSharedObject(upload->release(), RtObjectRelease{});
        auto mapped = mapBuffer(staging.get(), 0, static_cast<int64_t>(uploadCapacity), false, true);
        if (!mapped) return std::unexpected(mapped.error());
        std::memset(*mapped, 0, static_cast<size_t>(uploadCapacity));
        for (const auto& copy : copies) {
            std::memcpy(static_cast<uint8_t*>(*mapped) + copy.sourceOffset, copy.source, static_cast<size_t>(copy.bytes));
        }
        unmapBuffer(staging.get());
    }
    auto& retained = encoder->rtGeometryRetained[encoder->currentListSlot];
    auto& uploads = encoder->rtGeometryUploadsRetained[encoder->currentListSlot];
    retained.reserve(retained.size() + 1);
    uploads.reserve(uploads.size() + 1);
    const auto token = nextGeometrySnapshot++;
    geometrySnapshots.emplace(token, snapshot);
    retained.push_back(snapshot);
    if (staging) uploads.push_back(staging);
    for (const auto& copy : copies) {
        auto copied = copyBufferToBuffer(encoder, staging.get(), static_cast<int64_t>(copy.sourceOffset), copy.target.get(),
            static_cast<int64_t>(copy.offset), static_cast<int64_t>((copy.bytes + 3) & ~uint64_t{3}));
        if (!copied) {
            geometrySnapshots.erase(token);
            return copied;
        }
    }
    uint64_t bytes = 0;
    for (const auto& buffer : snapshot->buffers) bytes += buffer->size;
    *output = {token, snapshot->buffers[0].get(), snapshot->buffers[1].get(), snapshot->buffers[2].get(),
        snapshot->buffers[3].get(), snapshot->buffers[4].get(), bytes, uploadedBytes, uploadCapacity};
    return {};
}

mcdr::Result<uint64_t> createAcceleration(uint64_t snapshot, uint64_t reuse, uint64_t owner,
    uint64_t secondaryReuse = 0) {
    const auto found = geometrySnapshots.find(snapshot);
    if (found == geometrySnapshots.end()) return mcdr::err("rt accel", "rt accel: unknown geometry");
    if (reuse != 0 && secondaryReuse == 0) {
        const auto source = accelerations.find(reuse);
        if (source == accelerations.end() || source->second->snapshot->worldEpoch != found->second->worldEpoch) {
            return mcdr::err("rt accel", "rt accel: reuse unavailable");
        }
        if (source->second->snapshotId == snapshot) {
            auto acceleration = std::make_unique<RtAcceleration>();
            acceleration->owner = owner;
            acceleration->snapshotId = snapshot;
            acceleration->snapshot = found->second;
            acceleration->entries = source->second->entries;
            for (auto& entry : acceleration->entries) {
                if (entry && (!entry->recording || entry->recording->cancelled)) entry.reset();
            }
            const auto token = nextAcceleration++;
            accelerations.emplace(token, std::move(acceleration));
            return token;
        }
    }
    std::map<RtBlasKey, std::shared_ptr<RtBlasEntry>> reusable;
    for (const auto token : {reuse, secondaryReuse}) {
        if (token == 0) continue;
        const auto source = accelerations.find(token);
        if (source == accelerations.end() || source->second->snapshot->worldEpoch != found->second->worldEpoch) {
            return mcdr::err("rt accel", "rt accel: reuse unavailable");
        }
        const auto& previous = *source->second;
        for (size_t index = 0; index < previous.entries.size(); ++index) {
            if (previous.entries[index] && previous.entries[index]->recording &&
                    !previous.entries[index]->recording->cancelled) {
                reusable.emplace(geometryBlasKey(geometryRecord(*previous.snapshot, index)), previous.entries[index]);
            }
        }
    }
    auto acceleration = std::make_unique<RtAcceleration>();
    acceleration->owner = owner;
    acceleration->snapshotId = snapshot;
    acceleration->snapshot = found->second;
    acceleration->entries.resize(found->second->records->size() / 64);
    std::set<std::pair<uint64_t, uint32_t>> keys;
    for (size_t index = 0; index < acceleration->entries.size(); ++index) {
        const auto record = geometryRecord(*acceleration->snapshot, index);
        const auto key = geometryKey(record);
        if (!keys.insert(key).second) return mcdr::err("rt accel", "rt accel: duplicate geometry key");
        const auto candidate = reusable.find(geometryBlasKey(record));
        if (candidate == reusable.end()) continue;
        const auto& entry = candidate->second;
        if (entry->version == record[11] && entry->vertices == record[8] && entry->indices == record[9] && entry->flags == record[10]) {
            acceleration->entries[index] = entry;
        }
    }
    const auto token = nextAcceleration++;
    accelerations.emplace(token, std::move(acceleration));
    return token;
}

mcdr::Result<uint64_t> createRtAcceleration(uint64_t snapshot, uint64_t reuse) {
    std::lock_guard lock(frameMutex);
    return createAcceleration(snapshot, reuse, 0);
}

mcdr::Result<void> getRtAccelerationBudget(uint64_t* values, int32_t valueCount) {
    if (!values || valueCount != 4) return mcdr::err("rt accel", "rt accel: invalid budget buffer");
    const std::lock_guard lock(rtMemory->mutex);
    const auto& acceleration = rtMemory->domains[static_cast<size_t>(RtMemoryDomain::Acceleration)];
    values[0] = acceleration.bytes;
    values[1] = kAccelerationMemoryLimit;
    values[2] = acceleration.rejections;
    values[3] = acceleration.peak;
    return {};
}

mcdr::Result<std::shared_ptr<void>> reserveRtFrameMemory(const D3D12_RESOURCE_DESC& description, bool host) {
    const auto bytes = deviceContext().device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    auto charge = reserveRtMemory(host ? RtMemoryDomain::FrameHost : RtMemoryDomain::FrameGpu,
        bytes, "rt frame", "rt frame: managed memory budget exhausted");
    if (!charge) return std::unexpected(charge.error());
    return std::shared_ptr<void>(std::move(*charge));
}

mcdr::Result<void> setRtManagedMemoryBudget(uint64_t bytes) {
    if (bytes == 0 || bytes > 16384ULL * 1024 * 1024) {
        return mcdr::err("rt memory", "rt memory: invalid managed limit");
    }
    const std::lock_guard lock(rtMemory->mutex);
    rtMemory->limit = bytes;
    return {};
}

std::shared_ptr<void> reserveRtNrdMemory(const void* description, bool host) {
    auto charge = reserveRtFrameMemory(*static_cast<const D3D12_RESOURCE_DESC*>(description), host);
    return charge ? std::move(*charge) : nullptr;
}

mcdr::Result<void> getRtManagedMemoryBudget(uint64_t* values, int32_t valueCount) {
    if (!values || valueCount != 24) return mcdr::err("rt memory", "rt memory: invalid budget buffer");
    const std::lock_guard lock(rtMemory->mutex);
    values[0] = rtMemory->bytes;
    values[1] = rtMemory->limit;
    values[2] = rtMemory->rejections;
    values[3] = rtMemory->peak;
    for (size_t index = 0; index < rtMemory->domains.size(); ++index) {
        const auto& domain = rtMemory->domains[index];
        values[4 + index * 4] = domain.bytes;
        values[5 + index * 4] = rtMemoryDomainLimit(static_cast<RtMemoryDomain>(index));
        values[6 + index * 4] = domain.rejections;
        values[7 + index * 4] = domain.peak;
    }
    return {};
}

mcdr::Result<void> releaseRtAcceleration(uint64_t acceleration) {
    std::lock_guard lock(frameMutex);
    const auto found = accelerations.find(acceleration);
    if (found == accelerations.end() || found->second->owner != 0) {
        return mcdr::err("rt accel", "rt accel: unknown context");
    }
    accelerations.erase(found);
    return {};
}

mcdr::Result<void> prepareAcceleration(CommandContext* encoder, RtAcceleration& acceleration,
    mcdr_rt_acceleration_output* output, RtScene* scene = nullptr) {
    discardCancelledAcceleration(acceleration);
    if (acceleration.snapshot->cancelled()) {
        return mcdr::err("rt accel", "rt accel: geometry recording cancelled");
    }
    if (!acceleration.snapshot->available(encoder) || !acceleration.available(encoder) ||
        !std::ranges::all_of(acceleration.entries, [&](const auto& entry) { return !entry || entry->available(encoder); })) {
        return mcdr::err("rt accel", "rt accel: producer pending");
    }
    retainAcceleration(encoder, acceleration);
    struct PendingBuild {
        size_t index;
        uint64_t firstFrame;
        bool demanded;
    };
    std::vector<PendingBuild> pending;
    if (scene && !acceleration.complete && scene->builds < 16) {
        pending.reserve(acceleration.entries.size());
        for (size_t index = 0; index < acceleration.entries.size(); ++index) {
            const auto key = geometryBlasKey(geometryRecord(*acceleration.snapshot, index));
            const auto current = scene->live.find({key.node, key.layer});
            if (current == scene->live.end() || current->second != key.version) continue;
            const auto request = scene->firstRequestedFrames.find({key.node, key.layer});
            const auto firstFrame = request != scene->firstRequestedFrames.end() && request->second.version == key.version
                ? request->second.firstFrame : scene->frameId;
            const auto demandFrame = request == scene->firstRequestedFrames.end() ? 0 : request->second.demandFrame;
            const bool demanded = demandFrame != 0 && demandFrame <= scene->frameId &&
                scene->frameId - demandFrame <= kPendingDemandFrames;
            if (!acceleration.entries[index]) pending.push_back({index, firstFrame, demanded});
        }
        std::ranges::stable_sort(pending, {}, &PendingBuild::firstFrame);
    }
    if (acceleration.complete && acceleration.view) {
        accelerationOutput(acceleration, *output);
        return {};
    }
    if (!scene && acceleration.budgetRecording != currentCommandRecording(encoder)) {
        acceleration.budgetRecording = currentCommandRecording(encoder);
        acceleration.budgetUsed = 0;
    }
    auto& budgetUsed = scene ? scene->builds : acceleration.budgetUsed;
    const auto started = std::chrono::steady_clock::now();
    uint32_t builds = 0;
    uint64_t peak = 0;
    const auto pendingCount = scene ? pending.size() : acceleration.entries.size();
    for (size_t position = 0; position < pendingCount && budgetUsed < 16; ++position) {
        if (scene && scene->demandNext) {
            const auto first = pending.begin() + position;
            const auto demanded = std::find_if(first, pending.end(), [](const auto& build) { return build.demanded; });
            if (demanded != pending.end()) std::rotate(first, demanded, std::next(demanded));
        }
        const auto index = scene ? pending[position].index : position;
        if (acceleration.entries[index]) continue;
        const auto record = geometryRecord(*acceleration.snapshot, index);
        if (scene && !admitSceneBlas(*scene, record[9] / 3)) break;
        if (scene) {
            auto timing = beginSceneWork(encoder, *scene);
            if (!timing) return timing;
        }
        const mcdr_rt_geometry geometry{acceleration.snapshot->buffers[1].get(), 28, record[8],
            static_cast<int64_t>(uint64_t{record[4]} | uint64_t{record[5]} << 32),
            acceleration.snapshot->buffers[2].get(), record[9],
            static_cast<int64_t>(uint64_t{record[6]} | uint64_t{record[7]} << 32),
            (record[10] & 0x600) != 0 ? 1 : 0, static_cast<int32_t>(record[10] & 255)};
        const auto before = std::chrono::steady_clock::now();
        auto entry = recordBlas(encoder, acceleration, geometry, record[11], record[10]);
        if (!entry) return std::unexpected(entry.error());
        peak = std::max(peak, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - before).count()));
        acceleration.entries[index] = std::move(*entry);
        ++acceleration.entryRevision;
        if (scene) {
            chargeSceneBlas(encoder, *scene, record[9] / 3);
            rememberSceneBlas(*scene, geometryBlasKey(record), acceleration.entries[index]);
            scene->demandNext = !scene->demandNext;
        }
        ++builds;
        ++budgetUsed;
    }
    const auto blasFinished = std::chrono::steady_clock::now();
    if (scene) acceleration.remainingBuilds = static_cast<uint32_t>(pending.size() - builds);
    acceleration.complete = std::ranges::all_of(acceleration.entries, [](const auto& entry) { return bool(entry); });
    if (acceleration.complete) {
        auto built = recordTlas(encoder, acceleration,
            scene ? sceneInvalidSections(*scene, acceleration) : std::flat_set<uint64_t>{}, scene);
        if (!built) return built;
    }
    accelerationOutput(acceleration, *output);
    output->buildCount = builds;
    output->blasMicros = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(blasFinished - started).count());
    output->peakBuildMicros = peak;
    output->tlasMicros = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - blasFinished).count());
    return {};
}

mcdr::Result<void> prepareRtAcceleration(CommandContext* encoder, uint64_t handle, void* outputData) {
    auto* output = static_cast<mcdr_rt_acceleration_output*>(outputData);
    if (output) *output = {};
    if (!output || !encoder || !encoder->listOpen || !encoder->commandList || !encoder->fence || encoder->inRenderPass) {
        return mcdr::err("rt accel", "rt accel: open main list required");
    }
    std::lock_guard lock(frameMutex);
    const auto found = accelerations.find(handle);
    if (found == accelerations.end() || found->second->owner != 0) return mcdr::err("rt accel", "rt accel: unknown context");
    return prepareAcceleration(encoder, *found->second, output);
}

mcdr::Result<void> filterRtAcceleration(CommandContext* encoder, uint64_t handle,
    const uint64_t* invalidSections, uint32_t count, void* outputData) {
    auto* output = static_cast<mcdr_rt_acceleration_output*>(outputData);
    if (output) *output = {};
    if (!output || !encoder || !encoder->listOpen || !encoder->commandList || !encoder->fence || encoder->inRenderPass ||
        count > 65535 || (count != 0 && !invalidSections)) {
        return mcdr::err("rt accel", "rt accel: invalid filter input");
    }
    std::lock_guard lock(frameMutex);
    const auto found = accelerations.find(handle);
    if (found == accelerations.end() || found->second->owner != 0) {
        return mcdr::err("rt accel", "rt accel: scene unavailable");
    }
    auto& acceleration = *found->second;
    discardCancelledAcceleration(acceleration);
    if (!acceleration.complete || !acceleration.available(encoder) || !acceleration.snapshot->available(encoder)) {
        return mcdr::err("rt accel", "rt accel: scene unavailable");
    }
    std::flat_set<uint64_t> masked;
    for (uint32_t index = 0; index < count; ++index) masked.insert(invalidSections[index]);
    if (masked.size() != count) return mcdr::err("rt accel", "rt accel: duplicate invalid section");
    const auto started = std::chrono::steady_clock::now();
    if (masked != acceleration.masked || !acceleration.view) {
        auto built = recordTlas(encoder, acceleration, masked);
        if (!built) return built;
    } else {
        retainAcceleration(encoder, acceleration);
    }
    accelerationOutput(acceleration, *output);
    output->tlasMicros = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count());
    return {};
}

mcdr::Result<uint64_t> createRtScene() {
    std::lock_guard lock(frameMutex);
    const auto token = nextScene++;
    scenes.emplace(token, RtScene{});
    return token;
}

void clearScene(RtScene& scene) {
    accelerations.erase(scene.pending);
    accelerations.erase(scene.published);
    scene.pending = 0;
    scene.published = 0;
    scene.validatedPending = 0;
    scene.validatedPublished = 0;
    scene.validatedEntryRevision = 0;
    scene.partialPending = 0;
    scene.partialEntryRevision = 0;
    scene.lastPublication = {};
    scene.resident.clear();
    scene.residentLru.clear();
    scene.residentBytes = 0;
    scene.firstRequestedFrames.clear();
    scene.sourceRequests.clear();
    scene.captureDemands.clear();
    scene.demandNext = false;
}

void refreshSceneRequests(RtScene& scene) {
    std::vector<std::pair<uint64_t, uint32_t>> requestedKeys;
    std::vector<RtSceneRequest> requestedValues;
    requestedKeys.reserve(scene.live.size());
    requestedValues.reserve(scene.live.size());
    auto previousRequest = scene.firstRequestedFrames.begin();
    for (const auto& [key, version] : scene.live) {
        if (scene.explicitTargets && !scene.targets.contains(key.first)) continue;
        while (previousRequest != scene.firstRequestedFrames.end() && previousRequest->first < key) ++previousRequest;
        requestedKeys.push_back(key);
        requestedValues.push_back(previousRequest != scene.firstRequestedFrames.end() && previousRequest->first == key &&
            previousRequest->second.version == version ? previousRequest->second :
            RtSceneRequest{version, scene.frameId, nextSceneRequest++});
    }
    scene.firstRequestedFrames = decltype(scene.firstRequestedFrames)(std::sorted_unique,
        std::move(requestedKeys), std::move(requestedValues));
    if (!scene.explicitTargets) {
        scene.sourceRequests.clear();
        scene.captureDemands.clear();
        return;
    }
    std::vector<uint64_t> requestNodes;
    std::vector<RtSourceRequest> requests;
    requestNodes.reserve(scene.targets.size());
    requests.reserve(scene.targets.size());
    auto previousSource = scene.sourceRequests.begin();
    auto currentLayer = scene.live.begin();
    auto currentBounds = scene.outputBounds.begin();
    for (const auto node : scene.targets) {
        while (previousSource != scene.sourceRequests.end() && previousSource->first < node) ++previousSource;
        while (currentLayer != scene.live.end() && currentLayer->first.first < node) ++currentLayer;
        const auto firstLayer = currentLayer;
        while (currentLayer != scene.live.end() && currentLayer->first.first == node) ++currentLayer;
        while (currentBounds != scene.outputBounds.end() && currentBounds->first < node) ++currentBounds;
        const auto captureVersion = currentBounds != scene.outputBounds.end() && currentBounds->first == node ?
            currentBounds->second.captureVersion : 0;
        const auto layerCount = static_cast<size_t>(currentLayer - firstLayer);
        const bool same = previousSource != scene.sourceRequests.end() && previousSource->first == node &&
            previousSource->second.versions.size() == layerCount + (captureVersion != 0) &&
            std::equal(firstLayer, currentLayer, previousSource->second.versions.begin(), [](const auto& live, const auto& old) {
                return live.first.second == old.first && live.second == old.second;
            }) && (captureVersion == 0 || previousSource->second.versions.back() == std::pair{UINT32_MAX, captureVersion});
        requestNodes.push_back(node);
        if (same) {
            requests.push_back(std::move(previousSource->second));
        } else {
            std::vector<std::pair<uint32_t, uint32_t>> versions;
            versions.reserve(layerCount + (captureVersion != 0));
            for (auto layer = firstLayer; layer != currentLayer; ++layer) versions.emplace_back(layer->first.second, layer->second);
            if (captureVersion != 0) versions.emplace_back(UINT32_MAX, captureVersion);
            requests.push_back(RtSourceRequest{std::move(versions), nextSceneRequest++});
        }
    }
    scene.sourceRequests = decltype(scene.sourceRequests)(std::sorted_unique, std::move(requestNodes), std::move(requests));
    scene.captureDemands.remove_if([&](const auto& demand) {
        const auto current = scene.sourceRequests.find(demand.first);
        return current == scene.sourceRequests.end() || current->second.identity != demand.second;
    });
}

mcdr::Result<void> releaseRtScene(uint64_t handle) {
    std::lock_guard lock(frameMutex);
    const auto found = scenes.find(handle);
    if (found == scenes.end()) return mcdr::err("rt scene", "rt scene: unknown context");
    clearScene(found->second);
    scenes.erase(found);
    return {};
}

mcdr::Result<void> prepareScene(CommandContext* encoder, uint64_t handle, const void* descriptionData,
        const uint64_t* targetData, uint32_t targetCount, bool explicitTargets, void* outputData) {
    const auto attemptStarted = std::chrono::steady_clock::now();
    auto* output = static_cast<mcdr_rt_scene_output*>(outputData);
    if (output) *output = {};
    const auto* description = static_cast<const mcdr_rt_scene_desc*>(descriptionData);
    if (!description || !output || !encoder || !encoder->listOpen || !encoder->commandList ||
        !encoder->fence || encoder->inRenderPass) {
        return mcdr::err("rt scene", "rt scene: open main list required");
    }
    const auto request = *description;
    if (request.frameId == 0 || request.reserved != 0 || request.sectionCount > 1048576 ||
        (request.sectionCount != 0 && !request.sections)) {
        return mcdr::err("rt scene", "rt scene: invalid versions");
    }
    if (targetCount > 65535 || (targetCount != 0 && !targetData)) {
        return mcdr::err("rt scene", "rt scene: invalid targets");
    }
    if (request.outputMode > 1 || request.outputCount > 1048576 ||
        (request.outputCount != 0 && !request.outputs) ||
        (request.outputMode == 0 && (request.outputCount != 0 || request.outputs)) ||
        (request.outputMode != 0 && !explicitTargets)) {
        return mcdr::err("rt scene", "rt scene: invalid output support");
    }
    std::lock_guard lock(frameMutex);
    const auto found = scenes.find(handle);
    if (found == scenes.end()) return mcdr::err("rt scene", "rt scene: unknown context");
    auto& scene = found->second;
    if (scene.explicitTargets && !explicitTargets) {
        return mcdr::err("rt scene", "rt scene: targets required");
    }
    if (scene.initialized && scene.worldEpoch == request.worldEpoch &&
        scene.outputMode != 0 && request.outputMode == 0) {
        return mcdr::err("rt scene", "rt scene: output support required");
    }
    const bool targetsChanged = scene.explicitTargets != explicitTargets || scene.targetNodes.size() != targetCount ||
        (targetCount != 0 && std::memcmp(scene.targetNodes.data(), targetData, targetCount * sizeof(uint64_t)) != 0);
    std::vector<uint64_t> targetNodes;
    std::flat_set<uint64_t> targets;
    if (targetsChanged) {
        targetNodes.resize(targetCount);
        if (targetCount != 0) std::memcpy(targetNodes.data(), targetData, targetCount * sizeof(uint64_t));
        targets = decltype(targets)(targetNodes.begin(), targetNodes.end());
        if (targets.size() != targetNodes.size()) return mcdr::err("rt scene", "rt scene: duplicate target");
    }
    const auto& selectedTargets = targetsChanged ? targets : scene.targets;
    if (scene.initialized && scene.worldEpoch == request.worldEpoch && request.frameId < scene.frameId) {
        return mcdr::err("rt scene", "rt scene: stale frame");
    }
    const RtGeometrySnapshot* requested = nullptr;
    if (request.snapshotId != 0) {
        const auto geometry = geometrySnapshots.find(request.snapshotId);
        if (geometry != geometrySnapshots.end() && geometry->second->cancelled()) {
            return mcdr::err("rt scene", "rt scene: geometry recording cancelled");
        }
        if (geometry == geometrySnapshots.end() || geometry->second->worldEpoch != request.worldEpoch ||
            !geometry->second->available(encoder)) return mcdr::err("rt scene", "rt scene: geometry unavailable");
        requested = geometry->second.get();
    }
    if (requested && requested->records->empty() && selectedTargets.empty()) {
        return mcdr::err("rt scene", "rt scene: empty target scope");
    }
    if (scene.initialized && scene.worldEpoch == request.worldEpoch && request.liveRevision < scene.liveRevision) {
        return mcdr::err("rt scene", "rt scene: stale versions");
    }
    const bool revisionChanged = !scene.initialized || scene.worldEpoch != request.worldEpoch ||
        scene.liveRevision != request.liveRevision;
    const bool sectionPayloadChanged = scene.versions.size() != request.sectionCount || (request.sectionCount != 0 &&
        std::memcmp(scene.versions.data(), request.sections, scene.versions.size() * sizeof(mcdr_rt_section_version)) != 0);
    const bool outputPayloadChanged = scene.outputMode != request.outputMode || scene.outputs.size() != request.outputCount ||
        (request.outputCount != 0 && std::memcmp(scene.outputs.data(), request.outputs,
            scene.outputs.size() * sizeof(mcdr_rt_output_support)) != 0);
    const bool versionsChanged = !scene.initialized || scene.worldEpoch != request.worldEpoch ||
        sectionPayloadChanged || outputPayloadChanged;
    std::vector<mcdr_rt_section_version> versions;
    std::flat_map<std::pair<uint64_t, uint32_t>, uint32_t> live;
    std::vector<mcdr_rt_output_support> outputs;
    std::flat_map<uint64_t, RtOutputBounds> outputBounds;
    if (!revisionChanged) {
        if (sectionPayloadChanged) {
            return mcdr::err("rt scene", "rt scene: version payload mismatch");
        }
        if (outputPayloadChanged) {
            return mcdr::err("rt scene", "rt scene: output payload mismatch");
        }
    }
    if (versionsChanged) {
        versions.resize(request.sectionCount);
        if (!versions.empty()) std::memcpy(versions.data(), request.sections, versions.size() * sizeof(mcdr_rt_section_version));
        std::vector<std::pair<std::pair<uint64_t, uint32_t>, uint32_t>> versionEntries;
        versionEntries.reserve(versions.size());
        for (const auto section : versions) versionEntries.emplace_back(std::pair{section.node, section.layer}, section.version);
        live = decltype(live)(versionEntries.begin(), versionEntries.end());
        if (live.size() != versions.size()) return mcdr::err("rt scene", "rt scene: duplicate section key");
        outputs.resize(request.outputCount);
        if (!outputs.empty()) std::memcpy(outputs.data(), request.outputs, outputs.size() * sizeof(mcdr_rt_output_support));
        std::vector<const mcdr_rt_output_support*> orderedOutputs;
        orderedOutputs.reserve(outputs.size());
        for (const auto& support : outputs) {
            orderedOutputs.push_back(&support);
        }
        std::ranges::sort(orderedOutputs, {}, [](const auto* support) { return std::pair{support->node, support->layer}; });
        std::vector<uint64_t> boundNodes;
        std::vector<RtOutputBounds> boundValues;
        boundNodes.reserve(outputs.size());
        boundValues.reserve(outputs.size());
        std::optional<std::pair<uint64_t, uint32_t>> previousKey;
        auto current = live.begin();
        size_t layerCount = 0;
        for (const auto* incoming : orderedOutputs) {
            const auto& support = *incoming;
            const auto key = std::pair{support.node, support.layer};
            if (previousKey == key) return mcdr::err("rt scene", "rt scene: invalid output key");
            previousKey = key;
            if (support.reserved != 0 || support.flags > 1 || support.version == 0) {
                return mcdr::err("rt scene", "rt scene: invalid output key");
            }
            if (support.layer != UINT32_MAX) {
                while (current != live.end() && current->first < key) ++current;
                if (current == live.end() || current->first != key || current->second != support.version) {
                    return mcdr::err("rt scene", "rt scene: output version mismatch");
                }
                ++layerCount;
            }
            const std::array values{support.minimumX, support.minimumY, support.minimumZ,
                support.maximumX, support.maximumY, support.maximumZ};
            if (boundNodes.empty() || boundNodes.back() != support.node) {
                boundNodes.push_back(support.node);
                boundValues.emplace_back();
            }
            auto& bounds = boundValues.back();
            if (support.layer == UINT32_MAX) bounds.captureVersion = support.version;
            bounds.finite = bounds.finite && support.flags != 0;
            for (size_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(values[axis]) || !std::isfinite(values[axis + 3]) ||
                    (support.flags == 0 && (values[axis] != 0.0f || values[axis + 3] != 0.0f)) ||
                    (support.flags != 0 && (values[axis] > 0.0f || values[axis + 3] < 16.0f))) {
                    return mcdr::err("rt scene", "rt scene: invalid output bounds");
                }
                bounds.values[axis] = std::min(bounds.values[axis], values[axis]);
                bounds.values[axis + 3] = std::max(bounds.values[axis + 3], values[axis + 3]);
            }
        }
        if (request.outputMode != 0 && layerCount != live.size()) {
            return mcdr::err("rt scene", "rt scene: incomplete output support");
        }
        outputBounds = decltype(outputBounds)(std::sorted_unique, std::move(boundNodes), std::move(boundValues));
        for (auto&& [node, bounds] : outputBounds) bounds.proxyRange = sceneProxyRange(node, bounds);
    }
    const auto& selectedBounds = versionsChanged ? outputBounds : scene.outputBounds;
    if (request.outputMode != 0 && !std::ranges::includes(selectedBounds.keys(), selectedTargets)) {
        return mcdr::err("rt scene", "rt scene: target support missing");
    }
    if (versionsChanged) {
        scene.validatedPending = 0;
        scene.versions = std::move(versions);
        scene.live = std::move(live);
        scene.outputs = std::move(outputs);
        scene.outputBounds = std::move(outputBounds);
        scene.outputMode = request.outputMode;
    }
    if (!scene.initialized || scene.worldEpoch != request.worldEpoch) {
        clearScene(scene);
        scene.worldEpoch = request.worldEpoch;
        scene.initialized = true;
        scene.revision = nextSceneRevision++;
        scene.frameId = 0;
        scene.builds = 0;
        scene.buildRecordings.clear();
    }
    pruneResidentBlas(scene);
    for (const auto token : {scene.published, scene.pending}) {
        if (token != 0) discardCancelledAcceleration(*accelerations.at(token));
    }
    refreshSceneBudget(scene, request.frameId);
    if (scene.work.cpuLimitNanos != 0) collectSceneWork(scene.work);
    scene.work.started = attemptStarted;
    RtSceneWorkAttempt workAttempt{encoder, scene.work};
    scene.liveRevision = request.liveRevision;
    scene.explicitTargets = explicitTargets;
    if (targetsChanged) {
        scene.validatedPending = 0;
        scene.targetNodes = std::move(targetNodes);
        scene.targets = std::move(targets);
    }
    if (versionsChanged || targetsChanged || scene.firstRequestedFrames.empty()) refreshSceneRequests(scene);
    collectPendingFeedback();
    mcdr_rt_scene_output result{};
    if (!requested) {
        if (scene.published != 0 || scene.pending != 0) scene.revision = nextSceneRevision++;
        clearScene(scene);
        scene.frameId = request.frameId;
        result.sceneRevision = scene.revision;
        result.byteCount = sceneAccelerationBytes(scene);
        auto finished = workAttempt.finish();
        if (!finished) return finished;
        *output = result;
        return {};
    }
    const auto snapshotOf = [&](uint64_t token) {
        return token == 0 ? uint64_t{0} : accelerations.at(token)->snapshotId;
    };
    if (snapshotOf(scene.published) == request.snapshotId && accelerations.at(scene.published)->complete &&
            accelerations.at(scene.published)->view) {
        accelerations.erase(scene.pending);
        scene.pending = 0;
    } else if (snapshotOf(scene.pending) != request.snapshotId) {
        auto candidate = createAcceleration(request.snapshotId, scene.published, handle, scene.pending);
        if (!candidate) return std::unexpected(candidate.error());
        auto& acceleration = *accelerations.at(*candidate);
        for (const auto previous : {scene.pending, scene.published}) {
            if (previous == 0) continue;
            const auto& source = *accelerations.at(previous);
            if (!source.available(encoder)) continue;
            if (!acceleration.scratch) acceleration.scratch = source.scratch;
            if (!acceleration.tlasScratch) acceleration.tlasScratch = source.tlasScratch;
        }
        reuseResidentBlas(scene, acceleration);
        rememberSceneBlas(scene, acceleration);
        accelerations.erase(scene.pending);
        scene.pending = *candidate;
    }
    if (scene.pending != 0) {
        auto& candidate = *accelerations.at(scene.pending);
        auto& recordingBuilds = scene.buildRecordings[currentCommandRecording(encoder)];
        const auto previousBuilds = scene.builds;
        auto prepared = prepareAcceleration(encoder, candidate, &result.candidate, &scene);
        recordingBuilds += scene.builds - previousBuilds;
        if (!prepared) return prepared;
    }
    const bool publishComplete = scene.pending != 0 && result.candidate.ready != 0;
    bool publishPartial = false;
    bool selectedValidated = false;
    bool deferredPublication = false;
    uint64_t partialTlasMicros = 0;
    auto selected = publishComplete ? scene.pending : scene.published;
    if (scene.pending != 0 && !publishComplete) {
        const auto& pending = *accelerations.at(scene.pending);
        const bool validated = scene.validatedPending == scene.pending &&
            scene.validatedPublished == scene.published && scene.published != 0 &&
            scene.validatedEntryRevision == pending.entryRevision && accelerations.at(scene.published)->view &&
            accelerations.at(scene.published)->available(encoder);
        const auto publishedEntryRevision = scene.partialPending == scene.pending ? scene.partialEntryRevision : 0;
        const bool mayDefer = !validated && scene.published != 0 && scene.explicitTargets && scene.outputMode != 0 &&
            pending.remainingBuilds != 0 && result.candidate.buildCount != 0 &&
            pending.entryRevision - publishedEntryRevision < 16 &&
            std::chrono::steady_clock::now() - scene.lastPublication < std::chrono::milliseconds(100);
        if (mayDefer) {
            const auto& published = *accelerations.at(scene.published);
            if (published.view && published.available(encoder)) {
                const auto invalid = sceneInvalidSections(scene, published);
                deferredPublication = std::ranges::includes(published.masked, invalid) &&
                    published.pendingQueries == scenePendingQueries(scene, published, published.masked, encoder) &&
                    published.shadowProofs == sceneShadowProofs(scene, published, published.masked, encoder) &&
                    containsProxyCoverage(published.coverage, sceneProxyCoverage(&scene, invalid));
                if (deferredPublication) ++scene.work.deferred;
            }
        }
        if (!validated && !deferredPublication) {
            const auto invalid = sceneInvalidSections(scene, pending);
            auto coverage = sceneProxyCoverage(&scene, invalid);
            if (snapshotOf(scene.published) != request.snapshotId ||
                accelerations.at(scene.published)->masked != invalid ||
                accelerations.at(scene.published)->pendingQueries != scenePendingQueries(scene, pending, invalid, encoder) ||
                accelerations.at(scene.published)->shadowProofs != sceneShadowProofs(scene, pending, invalid, encoder) ||
                accelerations.at(scene.published)->coverage != coverage ||
                !accelerations.at(scene.published)->view) {
                auto active = createAcceleration(request.snapshotId, scene.pending, handle);
                if (!active) return std::unexpected(active.error());
                auto& acceleration = *accelerations.at(*active);
                if (scene.published != 0) {
                    const auto& previous = *accelerations.at(scene.published);
                    if (previous.proxy && previous.proxy->available(encoder)) acceleration.proxy = previous.proxy;
                    if (previous.available(encoder)) acceleration.tlasScratch = previous.tlasScratch;
                }
                const auto started = std::chrono::steady_clock::now();
                auto filtered = recordTlas(encoder, acceleration, invalid, &scene, std::move(coverage));
                if (!filtered) {
                    accelerations.erase(*active);
                    return filtered;
                }
                partialTlasMicros = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count());
                selected = *active;
                publishPartial = true;
            }
        }
        selectedValidated = true;
    }
    if (selected != 0) {
        auto& acceleration = *accelerations.at(selected);
        if (!acceleration.view || !acceleration.available(encoder)) {
            return mcdr::err("rt scene", "rt scene: producer pending");
        }
        uint64_t filteringMicros = 0;
        bool filteringChanged = false;
        if (selectedValidated) {
            retainAcceleration(encoder, acceleration);
        } else {
            const auto invalid = sceneInvalidSections(scene, acceleration);
            const bool maskChanged = acceleration.masked != invalid ||
                acceleration.pendingQueries != scenePendingQueries(scene, acceleration, invalid, encoder) ||
                acceleration.shadowProofs != sceneShadowProofs(scene, acceleration, invalid, encoder);
            auto coverage = sceneProxyCoverage(&scene, invalid);
            const auto filteringStarted = std::chrono::steady_clock::now();
            filteringChanged = maskChanged || acceleration.coverage != coverage;
            if (filteringChanged) {
                auto filtered = recordTlas(encoder, acceleration, invalid, &scene, std::move(coverage));
                if (!filtered) return filtered;
            } else {
                retainAcceleration(encoder, acceleration);
            }
            filteringMicros = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - filteringStarted).count());
        }
        accelerationOutput(acceleration, result.active);
        result.active.tlasMicros = filteringMicros + partialTlasMicros;
        if (publishComplete || publishPartial) {
            scene.partialPending = scene.pending;
            scene.partialEntryRevision = accelerations.at(scene.pending)->entryRevision;
            result.active.buildCount = result.candidate.buildCount;
            result.active.blasMicros = result.candidate.blasMicros;
            result.active.peakBuildMicros = result.candidate.peakBuildMicros;
            result.active.tlasMicros += result.candidate.tlasMicros;
            accelerations.erase(scene.published);
            scene.published = selected;
            if (publishComplete) scene.pending = 0;
        }
        if (publishComplete || publishPartial || filteringChanged) {
            scene.revision = nextSceneRevision++;
            scene.lastPublication = std::chrono::steady_clock::now();
        }
        result.coverageFlags = acceleration.coverage.unbounded ? 1 : 0;
        result.snapshotId = acceleration.snapshotId;
        result.accelerationId = selected;
        result.geometryRevision = acceleration.snapshot->geometryRevision;
        result.materialRevision = acceleration.snapshot->materialRevision;
    }
    result.pendingSnapshotId = snapshotOf(scene.pending);
    result.pendingAccelerationId = scene.pending;
    result.sceneRevision = scene.revision;
    result.byteCount = sceneAccelerationBytes(scene);
    auto finished = workAttempt.finish();
    if (!finished) return finished;
    if (selectedValidated && !deferredPublication) {
        scene.validatedPending = scene.pending;
        scene.validatedPublished = scene.published;
        scene.validatedEntryRevision = accelerations.at(scene.pending)->entryRevision;
    }
    *output = result;
    return {};
}

mcdr::Result<void> setRtSceneWorkBudget(uint64_t handle, uint64_t cpuMicros, uint64_t gpuMicros, uint64_t triangles) {
    if (cpuMicros < 100 || cpuMicros > 1000000 || gpuMicros < 100 || gpuMicros > 1000000 ||
        triangles == 0 || triangles > 1000000000) {
        return mcdr::err("rt scene", "rt scene: invalid work budget");
    }
    const std::lock_guard lock(frameMutex);
    const auto found = scenes.find(handle);
    if (found == scenes.end()) return mcdr::err("rt scene", "rt scene: unknown context");
    auto& work = found->second.work;
    work.cpuLimitNanos = cpuMicros * 1000;
    work.gpuLimitNanos = gpuMicros * 1000;
    work.triangleLimit = triangles;
    return {};
}

mcdr::Result<void> getRtSceneWorkBudget(uint64_t handle, uint64_t* values, int32_t count) {
    if (!values || count != 24) return mcdr::err("rt scene", "rt scene: invalid work buffer");
    const std::lock_guard lock(frameMutex);
    const auto found = scenes.find(handle);
    if (found == scenes.end()) return mcdr::err("rt scene", "rt scene: unknown context");
    const auto& scene = found->second;
    const auto& work = scene.work;
    const std::array output{scene.worldEpoch, scene.frameId, work.cpuLimitNanos, work.gpuLimitNanos, work.triangleLimit,
        work.cpuNanos, work.predictedNanos, work.triangles, work.publications, work.deferred, work.oversized,
        work.samples, work.lastGpuNanos, work.totalGpuNanos, work.cpuOverruns, work.gpuOverruns,
        static_cast<uint64_t>(work.pending.size()), work.cancelledSamples, work.failures, work.peakGpuNanos,
        work.lastGpuFrame, work.lastGpuWorld, work.lastCpuNanos, work.factor};
    std::ranges::copy(output, values);
    return {};
}

mcdr::Result<void> getRtSceneCaptureDemands(uint64_t handle, uint64_t* values, int32_t count) {
    if (!values || count < 6 || count > 132 || (count - 4) % 2 != 0) {
        return mcdr::err("rt scene", "rt scene: invalid demand buffer");
    }
    const std::lock_guard lock(frameMutex);
    const auto found = scenes.find(handle);
    if (found == scenes.end()) return mcdr::err("rt scene", "rt scene: unknown context");
    collectPendingFeedback();
    auto& scene = found->second;
    std::fill_n(values, count, uint64_t{0});
    values[0] = scene.worldEpoch;
    values[1] = scene.liveRevision;
    values[2] = scene.frameId;
    const auto capacity = static_cast<size_t>((count - 4) / 2);
    for (size_t attempt = 0; attempt < capacity && !scene.captureDemands.empty(); ++attempt) {
        const auto [node, identity] = scene.captureDemands.front();
        scene.captureDemands.pop_front();
        const auto current = scene.sourceRequests.find(node);
        if (current == scene.sourceRequests.end() || current->second.identity != identity) continue;
        auto& request = current->second;
        request.queued = false;
        if (request.demandFrame <= request.deliveredFrame || request.demandFrame > scene.frameId ||
            scene.frameId - request.demandFrame > kPendingDemandFrames) continue;
        const auto index = values[3];
        values[4 + index * 2] = node;
        values[5 + index * 2] = request.demandFrame;
        request.deliveredFrame = request.demandFrame;
        ++values[3];
    }
    return {};
}

mcdr::Result<void> prepareRtScene(CommandContext* encoder, uint64_t handle, const void* descriptionData, void* outputData) {
    return prepareScene(encoder, handle, descriptionData, nullptr, 0, false, outputData);
}

mcdr::Result<void> prepareRtSceneTargets(CommandContext* encoder, uint64_t handle, const void* descriptionData,
        const uint64_t* targets, uint32_t targetCount, void* outputData) {
    return prepareScene(encoder, handle, descriptionData, targets, targetCount, true, outputData);
}

mcdr::Result<void> getRtSceneStats(uint64_t handle, uint64_t* values, int32_t valueCount) {
    if (!values || valueCount != 14) return mcdr::err("rt scene", "rt scene: invalid statistics buffer");
    std::ranges::fill(std::span(values, 14), uint64_t{0});
    std::lock_guard lock(frameMutex);
    const auto found = scenes.find(handle);
    if (found == scenes.end()) return mcdr::err("rt scene", "rt scene: unknown context");
    const auto& scene = found->second;
    values[0] = scene.worldEpoch;
    values[1] = scene.frameId;
    values[9] = scene.builds;
    values[11] = scene.revision;
    values[13] = scene.liveRevision;
    const auto token = scene.pending != 0 ? scene.pending : scene.published;
    if (token != 0) {
        const auto& target = *accelerations.at(token);
        values[2] = target.snapshotId;
        values[3] = target.entries.size();
        for (size_t index = 0; index < target.entries.size(); ++index) {
            const auto key = geometryBlasKey(geometryRecord(*target.snapshot, index));
            const auto current = scene.live.find({key.node, key.layer});
            if (current == scene.live.end()) {
                ++values[7];
            } else if (current->second != key.version) {
                ++values[6];
            } else if (const auto& entry = target.entries[index];
                    entry && entry->recording && !entry->recording->cancelled) {
                ++values[4];
            } else {
                ++values[5];
                const auto request = scene.firstRequestedFrames.find({key.node, key.layer});
                const auto frame = request != scene.firstRequestedFrames.end() && request->second.version == key.version
                    ? request->second.firstFrame : scene.frameId;
                if (values[10] == 0 || frame < values[10]) values[10] = frame;
            }
        }
    }
    if (scene.published != 0) values[8] = accelerations.at(scene.published)->masked.size();
    if (scene.pending != 0) values[12] = accelerations.at(scene.pending)->snapshotId;
    return {};
}

mcdr::Result<uint64_t> rtAccelerationMemory(uint64_t first, uint64_t second) {
    std::lock_guard lock(frameMutex);
    std::set<Dx12Object*> counted;
    uint64_t bytes = 0;
    for (const auto token : {first, second}) {
        if (token == 0) continue;
        const auto found = accelerations.find(token);
        if (found == accelerations.end()) return mcdr::err("rt accel", "rt accel: unknown context");
        bytes += accelerationBytes(*found->second, counted);
    }
    return bytes;
}

mcdr::Result<uint64_t> createRtGBufferProgram(const void* programData) {
    const auto* input = static_cast<const mcdr_rt_gbuffer_program*>(programData);
    constexpr uint64_t maxShaderBytes = 16 * 1024 * 1024;
    if (!input || !input->msData || input->msLen == 0 || input->msLen > maxShaderBytes ||
        !input->psData || input->psLen == 0 || input->psLen > maxShaderBytes ||
        !input->bindings || input->bindingCount <= 0 || input->bindingCount > 64 ||
        input->cullEnabled < 0 || input->cullEnabled > 1 ||
        input->polygonMode < 0 || input->polygonMode > 1 || input->_pad0 != 0) {
        return mcdr::err("rt gbuffer", "rt gbuffer: invalid program recipe");
    }
    auto program = std::make_shared<RtGBufferProgram>();
    std::array<bool, 256> constants{};
    std::array<bool, 256> resources{};
    uint32_t constantCount = 0;
    bool hasResources = false;
    for (int32_t index = 0; index < input->bindingCount; ++index) {
        const auto binding = input->bindings[index];
        auto& registers = binding.type == 0 ? constants : resources;
        if (binding.type > 3 || registers[binding.reg] ||
            (binding.type == 0 ? binding.reg == 8 : binding.reg >= 14 && binding.reg <= 16)) {
            return mcdr::err("rt gbuffer", "rt gbuffer: invalid program binding");
        }
        registers[binding.reg] = true;
        if (binding.type == 0) ++constantCount;
        else hasResources = true;
        program->bindings.emplace_back(binding.type, binding.reg);
    }
    if (constantCount == 0 || constantCount * 2 + (hasResources ? 1 : 0) + 3 > 64) {
        return mcdr::err("rt gbuffer", "rt gbuffer: invalid root signature size");
    }
    const auto* mesh = static_cast<const uint8_t*>(input->msData);
    const auto* pixel = static_cast<const uint8_t*>(input->psData);
    program->mesh.assign(mesh, mesh + input->msLen);
    program->pixel.assign(pixel, pixel + input->psLen);
    program->cull = input->cullEnabled;
    program->polygon = input->polygonMode;
    std::lock_guard lock(frameMutex);
    std::erase_if(gbufferProgramCache, [](const auto& cached) { return cached.expired(); });
    std::shared_ptr<RtGBufferProgram> matching;
    for (const auto& cached : gbufferProgramCache) {
        auto existing = cached.lock();
        if (existing && existing->mesh == program->mesh && existing->pixel == program->pixel &&
            existing->bindings == program->bindings && existing->cull == program->cull && existing->polygon == program->polygon) {
            matching = std::move(existing);
            break;
        }
    }
    if (matching) {
        program = std::move(matching);
    } else {
        MeshPipelineDesc description;
        description.msDxil = program->mesh;
        description.psDxil = program->pixel;
        for (const auto& [type, reg] : program->bindings) description.bindings.push_back({type, reg});
        for (const int format : {4, 4, 42, 42, 42}) {
            PipelineDesc::ColorTarget target;
            target.format = format;
            target.writeMask = 15;
            description.colorTargets.push_back(target);
        }
        description.colorCount = 5;
        description.hasDepth = true;
        description.depthFormat = 51;
        description.depthWrite = true;
        description.depthCompareOp = 5;
        description.cullEnabled = program->cull != 0;
        description.polygonMode = program->polygon;
        const auto signature = createMeshRootSignature(description.bindings, true);
        if (!signature) return std::unexpected(signature.error());
        struct RootSignatureRelease {
            uint64_t handle;
            ~RootSignatureRelease() { releaseMeshRootSignature(handle); }
        } owner{*signature};
        const auto pipeline = createMeshPipeline(description, owner.handle);
        if (!pipeline) return std::unexpected(pipeline.error());
        program->pipeline.reset(static_cast<Dx12Pipeline*>(*pipeline));
        gbufferProgramCache.push_back(program);
    }
    const uint64_t token = nextGBufferProgram++;
    gbufferPrograms.emplace(token, std::move(program));
    return token;
}

mcdr::Result<void> releaseRtGBufferProgram(uint64_t program) {
    std::lock_guard lock(frameMutex);
    if (program != 0 && gbufferPrograms.erase(program) == 0) {
        return mcdr::err("rt gbuffer", "rt gbuffer: unknown program");
    }
    return {};
}

struct RtCapturedTexture {
    Dx12Object texture;
    RtObject view;

    explicit RtCapturedTexture(const Dx12Object& source) {
        texture.kind = source.kind;
        texture.resource = source.resource;
        texture.heapType = source.heapType;
        texture.dxgiFormat = source.dxgiFormat;
        texture.usage = source.usage;
        texture.size = source.size;
    }
};

struct RtCapturedBatch {
    mcdr_rt_gbuffer_batch source{};
    std::shared_ptr<RtGBufferProgram> program;
    std::vector<mcdr_binding_slot> bindings;
    std::vector<std::unique_ptr<Dx12Object>> retained;
    std::vector<RtObject> snapshots;

    Dx12Object* retain(Dx12Object* object) {
        auto copy = std::make_unique<Dx12Object>();
        copy->kind = object->kind;
        copy->resource = object->resource;
        copy->heapType = object->heapType;
        copy->dxgiFormat = object->dxgiFormat;
        copy->usage = object->usage;
        copy->size = object->size;
        auto* result = copy.get();
        retained.push_back(std::move(copy));
        return result;
    }

    mcdr::Result<void> captureBuffer(CommandContext* encoder, Dx12Object*& buffer, int64_t& offset,
        int64_t bytes, bool copyDefault = true) {
        if (buffer->heapType != D3D12_HEAP_TYPE_UPLOAD) {
            if (!copyDefault) {
                buffer = retain(buffer);
                return {};
            }
            auto snapshot = frameBuffer(8, (bytes + 255) & ~int64_t{255});
            if (!snapshot) return std::unexpected(snapshot.error());
            const auto state = encoder->resourceState.find(buffer->resource.Get());
            const auto previous = state == encoder->resourceState.end() ? D3D12_RESOURCE_STATE_COMMON : state->second;
            if (auto copied = copyBufferToBuffer(encoder, buffer, offset, snapshot->get(), 0, bytes); !copied) return copied;
            transitionBufferTo(encoder, buffer, previous);
            buffer = snapshot->get();
            offset = 0;
            snapshots.push_back(std::move(*snapshot));
            return {};
        }
        const auto snapshot = snapshotUploadBufferSlice(encoder, buffer,
            static_cast<uint64_t>(offset), static_cast<uint64_t>(bytes));
        if (!snapshot) return std::unexpected(snapshot.error());
        buffer = snapshot->buffer;
        offset = static_cast<int64_t>(snapshot->offset);
        return {};
    }
};

struct RtGBufferCapture {
    uint64_t token = 0;
    std::shared_ptr<CommandRecording> recording;
    uint64_t pass = 0;
    int64_t frameNanos = 0;
    bool consumed = false;
    std::map<SIZE_T, std::unique_ptr<RtCapturedTexture>> textures;
    std::vector<std::unique_ptr<RtCapturedBatch>> batches;
};

mcdr::Result<uint64_t> captureRtGBuffer(CommandContext* encoder, uint64_t capture,
    int64_t frameNanos, const void* batchData) {
    if (!encoder || !encoder->listOpen || !encoder->commandList || !encoder->inRenderPass || !batchData) {
        return mcdr::err("rt capture", "rt capture: source pass required");
    }
    const auto source = *static_cast<const mcdr_rt_gbuffer_batch*>(batchData);
    std::shared_ptr<RtGBufferProgram> program;
    {
        std::lock_guard lock(frameMutex);
        if (std::ranges::any_of(frameContexts, [encoder](const auto& entry) {
                return entry.second->gbufferOpen && currentRtRecording(entry.second->gbufferRecording, encoder);
            })) return mcdr::err("rt capture", "rt capture: source pass required");
        const auto found = gbufferPrograms.find(source.program);
        if (found == gbufferPrograms.end()) return mcdr::err("rt capture", "rt capture: unknown program");
        program = found->second;
    }
    auto current = encoder->rtGBufferCapture;
    const bool samePass = current && currentRtRecording(current->recording, encoder) &&
        current->pass == encoder->renderPassSerial;
    if ((capture != 0 && (!samePass || current->token != capture || current->consumed ||
            current->frameNanos != frameNanos || current->batches.size() >= 65536)) ||
        (capture == 0 && samePass)) {
        return mcdr::err("rt capture", "rt capture: token does not match");
    }
    if (auto valid = validateRtBatch(source, program->pipeline.get()); !valid) return std::unexpected(valid.error());
    if (capture == 0) current = std::make_shared<RtGBufferCapture>();
    auto batch = std::make_unique<RtCapturedBatch>();
    std::map<SIZE_T, std::unique_ptr<RtCapturedTexture>> textures;
    batch->source = source;
    batch->program = std::move(program);
    batch->bindings.assign(source.bindings, source.bindings + source.bindingCount);
    batch->source.bindings = batch->bindings.data();
    if (auto copied = batch->captureBuffer(encoder, batch->source.vertices, batch->source.vertexOffset,
            source.vertices->size - source.vertexOffset, false); !copied) return std::unexpected(copied.error());
    if (auto copied = batch->captureBuffer(encoder, batch->source.sections, batch->source.sectionsOffset,
            source.sectionsLength); !copied) return std::unexpected(copied.error());
    if (auto copied = batch->captureBuffer(encoder, batch->source.meshlets, batch->source.meshletsOffset,
            source.meshletsLength); !copied) return std::unexpected(copied.error());
    for (size_t index = 0; index < batch->bindings.size(); ++index) {
        auto& binding = batch->bindings[index];
        if (index == static_cast<size_t>(source.projectionBinding)) {
            binding.buffer = nullptr;
        } else if (binding.type == 1) {
            const auto key = binding.view->cpuHandle.ptr;
            if (const auto found = current->textures.find(key); found != current->textures.end()) {
                binding.view = found->second->view.get();
            } else if (const auto pending = textures.find(key); pending != textures.end()) {
                binding.view = pending->second->view.get();
            } else {
                auto texture = std::make_unique<RtCapturedTexture>(*binding.view->sourceTexture);
                auto view = copyRtTextureView(binding.view, &texture->texture);
                if (!view) return std::unexpected(view.error());
                texture->view.reset(static_cast<Dx12Object*>(*view));
                binding.view = texture->view.get();
                textures.emplace(key, std::move(texture));
            }
        } else if (auto copied = batch->captureBuffer(encoder, binding.buffer, binding.offset, binding.length); !copied) {
            return std::unexpected(copied.error());
        }
    }
    current->batches.push_back(std::move(batch));
    current->textures.merge(textures);
    if (capture == 0) {
        static std::atomic<uint64_t> nextCapture{1};
        current->token = nextCapture.fetch_add(1);
        current->recording = currentCommandRecording(encoder);
        current->pass = encoder->renderPassSerial;
        current->frameNanos = frameNanos;
        encoder->rtGBufferRetained[encoder->currentListSlot].push_back(current);
        encoder->rtGBufferCapture = current;
    }
    return current->token;
}

mcdr::Result<void> releaseRtGBufferCapture(CommandContext* encoder, uint64_t capture) {
    if (capture == 0) return {};
    if (!encoder || !encoder->rtGBufferCapture || encoder->rtGBufferCapture->token != capture) {
        return mcdr::err("rt capture", "rt capture: unknown token");
    }
    encoder->rtGBufferCapture.reset();
    return {};
}

mcdr::Result<void> replayCapturedRtGBuffer(CommandContext* encoder, uint64_t handle,
    uint64_t capture, const void* replayData) {
    const auto* input = static_cast<const mcdr_rt_gbuffer_replay*>(replayData);
    if (!encoder || !input || input->batches || input->batchCount != 0 || !encoder->rtGBufferCapture) {
        return mcdr::err("rt capture", "rt capture: invalid replay");
    }
    const auto current = encoder->rtGBufferCapture;
    {
        std::lock_guard lock(frameMutex);
        const auto found = frameContexts.find(handle);
        if (found == frameContexts.end() || !currentRtReplay(*found->second, encoder, input->revision) ||
            current->token != capture || !currentRtRecording(current->recording, encoder) || current->consumed ||
            current->pass + 1 != encoder->renderPassSerial || current->frameNanos != found->second->gbufferFrameNanos) {
            return mcdr::err("rt capture", "rt capture: stale or foreign replay");
        }
    }
    std::vector<mcdr_rt_gbuffer_batch> batches;
    std::vector<std::shared_ptr<RtGBufferProgram>> programs;
    batches.reserve(current->batches.size());
    programs.reserve(current->batches.size());
    for (const auto& batch : current->batches) {
        batches.push_back(batch->source);
        programs.push_back(batch->program);
    }
    auto request = *input;
    request.batches = batches.data();
    request.batchCount = static_cast<int32_t>(batches.size());
    auto recorded = recordRtGBuffer(encoder, handle, &request, programs);
    if (recorded) current->consumed = true;
    return recorded;
}

mcdr::Result<uint64_t> createRtFrame(const void* programData, bool workGraph) {
    const auto* program = static_cast<const mcdr_rt_frame_program*>(programData);
    if (!program || !program->rayDxil || !program->guideDxil || !program->exposureDxil ||
        program->rayBytes <= 0 || program->guideBytes <= 0 || program->guideBytes > INT32_MAX ||
        program->exposureBytes <= 0 || program->exposureBytes > INT32_MAX ||
        !program->motionDxil || program->motionBytes <= 0 || program->motionBytes > INT32_MAX ||
        !program->packDxil || program->packBytes <= 0 || program->packBytes > INT32_MAX ||
        !program->signalDxil || program->signalBytes <= 0 || program->signalBytes > INT32_MAX ||
        !program->remodulateDxil || program->remodulateBytes <= 0 || program->remodulateBytes > INT32_MAX ||
        !program->compositeDxil || program->compositeBytes <= 0 || program->compositeBytes > INT32_MAX ||
        !program->environmentDxil || program->environmentBytes <= 0 || program->environmentBytes > INT32_MAX) {
        return mcdr::err("rt frame", "rt frame: invalid program");
    }
    std::lock_guard lock(frameMutex);
    mcdr_nrd_context_collect();
    auto context = std::make_unique<RtFrameContext>();
    const std::array<mcdr_rt_hit_group, 3> groups{{
        {"HITOPAQUE", "RtClosestHit", nullptr, nullptr},
        {"HITCUTOUT", "RtClosestHit", nullptr, nullptr},
        {"HITINVALID", "RtInvalidHit", nullptr, "RtInvalidIntersection"},
    }};
    auto pipeline = workGraph ? createWorkGraphPipeline(program->rayDxil, program->rayBytes, kTraceSrvs, kTraceUavs) :
        createRaytracingPipeline(program->rayDxil, program->rayBytes, "RtRayGen", "RtMiss",
            groups.data(), static_cast<int32_t>(groups.size()), kTraceSrvs, kTraceUavs, 1);
    if (!pipeline) return std::unexpected(pipeline.error());
    context->pipeline.reset(static_cast<Dx12Object*>(*pipeline));
    if (workGraph && context->pipeline->workGraphMemoryBytes != 0) {
        auto backing = frameBuffer(1024, static_cast<int64_t>(context->pipeline->workGraphMemoryBytes));
        if (!backing) return std::unexpected(backing.error());
        context->workGraphBacking = std::move(*backing);
    }
    auto guide = createComputePipelineEx(program->guideDxil, static_cast<int32_t>(program->guideBytes),
        kGuideSrvs, kGuideUavs);
    if (!guide) return std::unexpected(guide.error());
    context->guide.reset(static_cast<Dx12Object*>(*guide));
    auto exposurePipeline = createComputePipelineEx(program->exposureDxil,
        static_cast<int32_t>(program->exposureBytes), kExposureSrvs, kExposureUavs);
    if (!exposurePipeline) return std::unexpected(exposurePipeline.error());
    context->exposurePipeline.reset(static_cast<Dx12Object*>(*exposurePipeline));
    auto environmentPipeline = createComputePipelineEx(program->environmentDxil,
        static_cast<int32_t>(program->environmentBytes), 0, 1);
    if (!environmentPipeline) return std::unexpected(environmentPipeline.error());
    context->environmentPipeline.reset(static_cast<Dx12Object*>(*environmentPipeline));
    auto environment = createTexture(36, 42, 6, 1, 1, 1, true);
    if (!environment) return std::unexpected(environment.error());
    context->environment.reset(static_cast<Dx12Object*>(*environment));
    auto exposure = createTexture(36, 44, 1, 1, 1, 1, true);
    if (!exposure) return std::unexpected(exposure.error());
    context->exposure.reset(static_cast<Dx12Object*>(*exposure));
    auto motion = createComputePipelineEx(program->motionDxil,
        static_cast<int32_t>(program->motionBytes), kMotionSrvs, kMotionUavs);
    if (!motion) return std::unexpected(motion.error());
    context->motion.reset(static_cast<Dx12Object*>(*motion));
    auto pack = createComputePipelineEx(program->packDxil,
        static_cast<int32_t>(program->packBytes), kPackSrvs, kPackUavs);
    if (!pack) return std::unexpected(pack.error());
    context->pack.reset(static_cast<Dx12Object*>(*pack));
    auto signalPipeline = createComputePipelineEx(program->signalDxil,
        static_cast<int32_t>(program->signalBytes), kSignalSrvs, kSignalUavs);
    if (!signalPipeline) return std::unexpected(signalPipeline.error());
    context->signalPipeline.reset(static_cast<Dx12Object*>(*signalPipeline));
    auto remodulatePipeline = createComputePipelineEx(program->remodulateDxil,
        static_cast<int32_t>(program->remodulateBytes), kRemodulateSrvs, kRemodulateUavs);
    if (!remodulatePipeline) return std::unexpected(remodulatePipeline.error());
    context->remodulatePipeline.reset(static_cast<Dx12Object*>(*remodulatePipeline));
    auto compositePipeline = createComputePipelineEx(program->compositeDxil,
        static_cast<int32_t>(program->compositeBytes), kCompositeSrvs, kCompositeUavs);
    if (!compositePipeline) return std::unexpected(compositePipeline.error());
    context->compositePipeline.reset(static_cast<Dx12Object*>(*compositePipeline));
    if (!workGraph) {
        const std::array names{"RtRayGen", "RtMiss", "HITOPAQUE", "HITCUTOUT", "HITINVALID"};
        std::array<std::byte, kTableBytes> tableData{};
        for (size_t index = 0; index < names.size(); ++index) {
            if (getShaderIdentifier(context->pipeline.get(), names[index], tableData.data() + index * kRecordBytes) != 1) {
                return mcdr::err("rt frame", "rt frame: shader identifier unavailable");
            }
        }
        auto table = frameUpload(tableData.data(), kTableBytes, kTableBytes);
        if (!table) return std::unexpected(table.error());
        context->table = std::move(*table);
    }
    const uint64_t handle = nextFrameContext++;
    frameContexts.emplace(handle, std::move(context));
    return handle;
}

mcdr::Result<void> releaseRtFrame(uint64_t handle) {
    std::lock_guard lock(frameMutex);
    const auto found = frameContexts.find(handle);
    if (found != frameContexts.end()) discardCancelledGBuffer(*found->second);
    if (found != frameContexts.end() && found->second->gbufferOpen) {
        return mcdr::err("rt frame", "rt gbuffer: replay still open");
    }
    if (handle != 0 && frameContexts.erase(handle) == 0) {
        return mcdr::err("rt frame", "rt frame: unknown context");
    }
    if (handle != 0 && activeNrdContext == handle) {
        mcdr_nrd_context_shutdown();
        activeNrdContext = 0;
    }
    if (activeRrContext == handle) activeRrContext = 0;
    if (activeSrContext == handle) activeSrContext = 0;
    return {};
}

void releaseRtFrames() {
    std::lock_guard lock(frameMutex);
    frameContexts.clear();
    gbufferPrograms.clear();
    gbufferProgramCache.clear();
    geometrySnapshots.clear();
    accelerations.clear();
    scenes.clear();
    pendingFeedback = {};
    pendingFeedbackZero.reset();
    if (activeNrdContext != 0) mcdr_nrd_context_shutdown();
    mcdr_nrd_context_collect();
    activeNrdContext = 0;
    activeRrContext = 0;
    activeSrContext = 0;
}

mcdr::Result<void> beginRtGBuffer(CommandContext* encoder, uint64_t handle,
    int64_t frameNanos, int32_t width, int32_t height, void* outputData) {
    auto* output = static_cast<mcdr_rt_gbuffer_output*>(outputData);
    if (output) *output = {};
    if (!output || !encoder || !encoder->listOpen || !encoder->commandList || encoder->inRenderPass) {
        return mcdr::err("rt frame", "rt gbuffer: open main list required");
    }
    if (width <= 0 || height <= 0 || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        return mcdr::err("rt frame", "rt gbuffer: invalid dimensions");
    }
    std::lock_guard lock(frameMutex);
    const auto found = frameContexts.find(handle);
    if (found == frameContexts.end()) return mcdr::err("rt frame", "rt frame: unknown context");
    auto& context = *found->second;
    discardCancelledGBuffer(context);
    mcdr_nrd_context_collect();
    if (context.gbufferOpen) return mcdr::err("rt frame", "rt gbuffer: replay still open");
    if (context.gbufferRecording && !context.gbufferRecording->available(encoder)) {
        return mcdr::err("rt frame", "rt gbuffer: producer unavailable");
    }
    const bool resize = width != context.gbufferWidth || height != context.gbufferHeight;
    std::array<RtObject, 6> replacements;
    auto& attachments = resize ? replacements : context.gbuffer;
    if (resize) {
        const std::array formats{4, 4, 42, 42, 42, 51};
        for (size_t index = 0; index < attachments.size(); ++index) {
            auto texture = createTexture(index == 5 ? 8 : 44, formats[index], width, height, 1, 1, true);
            if (!texture) return std::unexpected(texture.error());
            attachments[index].reset(static_cast<Dx12Object*>(*texture));
        }
    }
    const std::array colors{attachments[0].get(), attachments[1].get(), attachments[2].get(),
        attachments[3].get(), attachments[4].get()};
    const std::array clearFlags{1, 1, 1, 1, 1};
    const std::array<float, 20> clearColors{0, 0, 0, 0, 0, 0, 0, 1};
    context.gbufferReady = false;
    context.recording.reset();
    context.denoisePending = false;
    context.srPending = false;
    context.reconstructed = {};
    auto begun = beginRenderPass(encoder, colors.data(), 5, nullptr, clearFlags.data(), clearColors.data(),
        attachments[5].get(), 0, 1, 0, 0, 0, width, height);
    if (!begun) {
        if (!encoder->activeColorTargets.empty() || encoder->activeDepthTarget) {
            encoder->inRenderPass = 1;
            static_cast<void>(endRenderPass(encoder));
        }
        return begun;
    }
    if (resize) {
        context.gbuffer = std::move(replacements);
        ++context.gbufferResourceRevision;
    }
    ++context.gbufferRevision;
    context.gbufferWidth = width;
    context.gbufferHeight = height;
    context.gbufferFrameNanos = frameNanos;
    context.gbufferRecording = currentCommandRecording(encoder);
    context.gbufferOpen = true;
    context.gbufferRecorded = false;
    context.gbufferFailed = false;
    *output = {context.gbufferRevision, context.gbufferResourceRevision, frameNanos, width, height,
        context.gbuffer[0].get(), context.gbuffer[1].get(), context.gbuffer[2].get(),
        context.gbuffer[3].get(), context.gbuffer[4].get(), context.gbuffer[5].get(),
        static_cast<uint64_t>(width) * height * 36};
    return {};
}

mcdr::Result<void> endRtGBuffer(CommandContext* encoder, uint64_t handle, uint64_t revision, bool success) {
    if (!encoder || !encoder->listOpen || !encoder->commandList || !encoder->inRenderPass) {
        return mcdr::err("rt frame", "rt gbuffer: open replay required");
    }
    std::lock_guard lock(frameMutex);
    const auto found = frameContexts.find(handle);
    if (found == frameContexts.end()) return mcdr::err("rt frame", "rt frame: unknown context");
    auto& context = *found->second;
    if (!currentRtReplay(context, encoder, revision)) {
        return mcdr::err("rt frame", "rt gbuffer: replay does not match");
    }
    if (success && context.gbufferFailed) return mcdr::err("rt frame", "rt gbuffer: cancel failed replay");
    auto ended = endRenderPass(encoder);
    context.gbufferOpen = false;
    context.gbufferReady = ended.has_value() && success;
    if (!context.gbufferReady) {
        context.exposureDirty = true;
        context.exposurePending = false;
        context.exposureRecording.reset();
    }
    return ended;
}

mcdr::Result<void> replayRtGBuffer(CommandContext* encoder, uint64_t handle, const void* replayData) {
    return recordRtGBuffer(encoder, handle, replayData);
}

namespace {

mcdr::Result<void> recordRtGBuffer(CommandContext* encoder, uint64_t handle, const void* replayData,
    std::span<const std::shared_ptr<RtGBufferProgram>> capturedPrograms) {
    const auto* input = static_cast<const mcdr_rt_gbuffer_replay*>(replayData);
    if (!input || !input->batches || input->batchCount <= 0 || input->batchCount > 65536 ||
        !input->projection || input->projectionFloatCount != 16 || input->_pad0 != 0) {
        return mcdr::err("rt frame", "rt gbuffer: invalid replay data");
    }
    const auto request = *input;
    if (!capturedPrograms.empty() && capturedPrograms.size() != static_cast<size_t>(request.batchCount)) {
        return mcdr::err("rt frame", "rt gbuffer: incomplete program capture");
    }
    std::array<float, 16> projection;
    std::copy_n(request.projection, projection.size(), projection.begin());
    const std::array camera{request.cameraOffsetX, request.cameraOffsetY, request.cameraOffsetZ};
    if (!std::ranges::all_of(projection, [](float value) { return std::isfinite(value); }) ||
        !std::ranges::all_of(camera, [](float value) { return std::isfinite(value); })) {
        return mcdr::err("rt frame", "rt gbuffer: nonfinite camera");
    }
    std::lock_guard lock(frameMutex);
    const auto found = frameContexts.find(handle);
    if (found == frameContexts.end()) return mcdr::err("rt frame", "rt frame: unknown context");
    auto& context = *found->second;
    if (!currentRtReplay(context, encoder, request.revision) || context.gbufferRecorded || context.gbufferFailed) {
        return mcdr::err("rt frame", "rt gbuffer: current unused replay required");
    }
    struct PreparedBatch {
        mcdr_rt_gbuffer_batch source;
        std::shared_ptr<RtGBufferProgram> program;
        std::vector<DrawBinding> bindings;
        FrameUploadSlice globals;
    };
    std::vector<PreparedBatch> batches;
    batches.reserve(static_cast<size_t>(request.batchCount));
    uint64_t descriptorCount = 0;
    for (int32_t batchIndex = 0; batchIndex < request.batchCount; ++batchIndex) {
        const auto source = request.batches[batchIndex];
        std::shared_ptr<RtGBufferProgram> program;
        if (capturedPrograms.empty()) {
            const auto registered = gbufferPrograms.find(source.program);
            if (registered == gbufferPrograms.end()) return mcdr::err("rt frame", "rt gbuffer: unknown program");
            program = registered->second;
        } else {
            program = capturedPrograms[static_cast<size_t>(batchIndex)];
        }
        if (auto valid = validateRtBatch(source, program->pipeline.get()); !valid) return valid;
        PreparedBatch batch{source, std::move(program), {}, {}};
        batch.bindings.reserve(static_cast<size_t>(source.bindingCount));
        for (int32_t bindingIndex = 0; bindingIndex < source.bindingCount; ++bindingIndex) {
            const auto binding = source.bindings[bindingIndex];
            if (binding.type == 1) {
                const auto* view = binding.view;
                if (std::ranges::any_of(context.gbuffer, [view](const auto& target) {
                        return target->resource == view->sourceTexture->resource;
                    })) return mcdr::err("rt frame", "rt gbuffer: invalid texture binding");
            }
            batch.bindings.push_back({static_cast<uint8_t>(binding.type), binding.buffer,
                binding.offset, binding.length, binding.texelFormat, binding.view});
            if (binding.type != 0) ++descriptorCount;
        }
        descriptorCount += 3;
        batches.push_back(std::move(batch));
    }
    if (!hasDrawDescriptorCapacity(encoder, descriptorCount)) {
        return mcdr::err("rt frame", "rt gbuffer: descriptor capacity exhausted");
    }
    auto projectionBuffer = uploadFrameData(encoder, projection.data(), sizeof(projection));
    if (!projectionBuffer) return std::unexpected(projectionBuffer.error());
    for (auto& batch : batches) {
        std::array<uint32_t, 64> globals{};
        globals[0] = static_cast<uint32_t>(batch.source.meshletCount);
        for (size_t axis = 0; axis < camera.size(); ++axis) globals[8 + axis] = std::bit_cast<uint32_t>(camera[axis]);
        auto buffer = uploadFrameData(encoder, globals.data(), sizeof(globals));
        if (!buffer) return std::unexpected(buffer.error());
        batch.globals = *buffer;
        auto& projectionBinding = batch.bindings[static_cast<size_t>(batch.source.projectionBinding)];
        projectionBinding = {0, projectionBuffer->buffer, static_cast<int64_t>(projectionBuffer->offset),
            sizeof(projection), 0, nullptr};
    }
    auto& retained = encoder->rtGBufferProgramsRetained[encoder->currentListSlot];
    retained.reserve(retained.size() + batches.size());
    for (const auto& batch : batches) retained.push_back(batch.program);
    context.gbufferFailed = true;
    for (const auto& batch : batches) {
        const auto& source = batch.source;
        if (auto bound = setPipeline(encoder, batch.program->pipeline.get(), true); !bound) return bound;
        if (auto pushed = pushDescriptors(encoder, batch.bindings); !pushed) return pushed;
        if (auto drawn = drawMeshlets(encoder, source.vertices, source.vertexOffset,
                source.sections, source.sectionsOffset, source.sectionsLength,
                source.meshlets, source.meshletsOffset, source.meshletsLength,
                batch.globals.buffer, static_cast<int64_t>(batch.globals.offset), source.meshletCount); !drawn) return drawn;
    }
    context.gbufferFailed = false;
    context.gbufferRecorded = true;
    return {};
}

}

static mcdr::Result<void> setRtFrameSpatialLocked(CommandContext* encoder, uint64_t handle, uint64_t frameId,
    uint64_t worldEpoch, uint64_t liveRevision, const void* data, int64_t bytes) {
    constexpr uint32_t pageSide = 8;
    constexpr uint32_t pageBytes = pageSide * pageSide * pageSide * 4;
    constexpr uint32_t maximumPages = 512;
    if (!encoder || !encoder->listOpen || !encoder->commandList || encoder->inRenderPass ||
        frameId == 0 || !data || bytes < 64 || bytes > 64 + maximumPages * (16 + pageBytes) || bytes % 4 != 0) {
        return mcdr::err("rt frame", "rt spatial: invalid descriptor");
    }
    std::vector<uint8_t> payload(static_cast<size_t>(bytes));
    std::memcpy(payload.data(), data, payload.size());
    std::array<uint32_t, 16> header;
    std::memcpy(header.data(), payload.data(), sizeof(header));
    const auto version = header[1];
    if (header[0] != 0x53505452u || (version != 1 && version != 2) || (header[14] & ~3u) != 0 ||
        (version == 1 && header[15] != 0) || (version == 2 && header[15] > maximumPages)) {
        return mcdr::err("rt frame", "rt spatial: invalid header");
    }
    uint64_t cells = 1;
    for (size_t axis = 0; axis < 3; ++axis) {
        const auto size = header[5 + axis];
        const auto minimum = std::bit_cast<int32_t>(header[2 + axis]);
        const int64_t limit = int64_t{1} << (axis == 1 ? 19 : 21);
        if (size == 0 || (version == 1 && size > 256) || size > limit * 2 || minimum < -limit ||
            int64_t{minimum} + size > limit) {
            return mcdr::err("rt frame", "rt spatial: invalid grid");
        }
        if (version == 1) cells *= size;
        if ((header[14] & 1u) != 0 && std::bit_cast<int32_t>(header[8 + axis]) >=
                std::bit_cast<int32_t>(header[11 + axis])) {
            return mcdr::err("rt frame", "rt spatial: invalid world bounds");
        }
    }
    if (version == 1) {
        if (cells > 65536 || static_cast<uint64_t>(bytes) != 64 + cells * 4) {
            return mcdr::err("rt frame", "rt spatial: invalid cell count");
        }
        for (size_t offset = 64; offset < payload.size(); offset += 4) {
            uint32_t state;
            std::memcpy(&state, payload.data() + offset, sizeof(state));
            if (state > 6) return mcdr::err("rt frame", "rt spatial: invalid cell state");
        }
    } else {
        const auto pageCount = header[15];
        if (static_cast<uint64_t>(bytes) != 64 + uint64_t{pageCount} * (16 + pageBytes)) {
            return mcdr::err("rt frame", "rt spatial: invalid page count");
        }
        std::array<int32_t, 3> previous{};
        for (uint32_t page = 0; page < pageCount; ++page) {
            std::array<uint32_t, 4> entry;
            std::memcpy(entry.data(), payload.data() + 64 + page * 16, sizeof(entry));
            const std::array<int32_t, 3> key{std::bit_cast<int32_t>(entry[2]),
                std::bit_cast<int32_t>(entry[1]), std::bit_cast<int32_t>(entry[0])};
            if ((page != 0 && key <= previous) || entry[3] != 64 + pageCount * 16 + page * pageBytes) {
                return mcdr::err("rt frame", "rt spatial: invalid page directory");
            }
            previous = key;
            std::array<int64_t, 3> minimum;
            for (size_t axis = 0; axis < 3; ++axis) {
                minimum[axis] = int64_t{std::bit_cast<int32_t>(entry[axis])} * pageSide;
                const int64_t limit = int64_t{1} << (axis == 1 ? 19 : 21);
                const int64_t gridMinimum = std::bit_cast<int32_t>(header[2 + axis]);
                if (minimum[axis] < -limit || minimum[axis] + pageSide > limit ||
                    minimum[axis] >= gridMinimum + header[5 + axis] || minimum[axis] + pageSide <= gridMinimum) {
                    return mcdr::err("rt frame", "rt spatial: invalid page coordinates");
                }
            }
            for (uint32_t cell = 0; cell < pageBytes / 4; ++cell) {
                uint32_t state;
                std::memcpy(&state, payload.data() + entry[3] + cell * 4, sizeof(state));
                if (state > 6) return mcdr::err("rt frame", "rt spatial: invalid cell state");
                for (size_t axis = 0; axis < 3 && state != 0; ++axis) {
                    const int64_t coordinate = minimum[axis] + ((cell >> (axis * 3)) & 7);
                    const int64_t gridMinimum = std::bit_cast<int32_t>(header[2 + axis]);
                    if (coordinate < gridMinimum || coordinate >= gridMinimum + header[5 + axis]) {
                        return mcdr::err("rt frame", "rt spatial: cell outside domain");
                    }
                }
            }
        }
    }
    const auto found = frameContexts.find(handle);
    if (found == frameContexts.end()) return mcdr::err("rt frame", "rt spatial: unknown context");
    auto& context = *found->second;
    if (frameId <= context.frameId || !context.gbufferReady ||
        !currentRtRecording(context.gbufferRecording, encoder)) {
        return mcdr::err("rt frame", "rt spatial: current replay required");
    }
    const auto& previous = context.spatial;
    if (previous.gbufferRevision == context.gbufferRevision && currentRtRecording(previous.recording, encoder)) {
        if (previous.frameId != frameId || previous.worldEpoch != worldEpoch || previous.liveRevision != liveRevision ||
            previous.bytes != payload) return mcdr::err("rt frame", "rt spatial: frozen payload mismatch");
        return {};
    }
    context.spatial = {std::move(payload), frameId, worldEpoch, liveRevision,
        context.gbufferRevision, currentCommandRecording(encoder)};
    return {};
}

mcdr::Result<void> setRtFrameSpatial(CommandContext* encoder, uint64_t handle, uint64_t frameId,
    uint64_t worldEpoch, uint64_t liveRevision, const void* data, int64_t bytes) {
    std::lock_guard lock(frameMutex);
    return setRtFrameSpatialLocked(encoder, handle, frameId, worldEpoch, liveRevision, data, bytes);
}

static void abandonRtFrameAttempt(uint64_t handle, RtFrameContext& context, CommandRecording* recording);

static mcdr::Result<void> recordRtFrameLocked(CommandContext* encoder, uint64_t handle,
    const void* descriptionData, void* outputData, Dx12Object* source = nullptr,
    std::span<const uint8_t> lighting = {}) {
    auto* output = static_cast<mcdr_rt_frame_output*>(outputData);
    if (output) *output = {};
    const auto* description = static_cast<const mcdr_rt_frame_desc*>(descriptionData);
    if (!description || !output || !encoder || !encoder->listOpen || !encoder->commandList ||
        encoder->inRenderPass) {
        return mcdr::err("rt frame", "rt frame: open main list required");
    }
    const auto request = *description;
    if (request.frameId == 0 || request.snapshotId == 0 || request.width <= 0 || request.height <= 0 ||
        request.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        request.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || !request.constants ||
        request.constantBytes != kFrameBytes || !std::isfinite(request.depthScale) || request.depthScale <= 0) {
        return mcdr::err("rt frame", "rt frame: invalid descriptor");
    }
    if (request.exposureMode < 0 || request.exposureMode > 1 || request.resetExposure > 1 ||
        !std::isfinite(request.exposureEv) || request.exposureEv < -8 || request.exposureEv > 8 ||
        !std::isfinite(request.exposureSpeed) || request.exposureSpeed < 0.1f || request.exposureSpeed > 10) {
        return mcdr::err("rt frame", "rt frame: invalid exposure parameters");
    }
    if (request.signalEncoding > 2 || !std::isfinite(request.jitterDeltaX) ||
        !std::isfinite(request.jitterDeltaY) || std::abs(request.jitterDeltaX) > 1 ||
        std::abs(request.jitterDeltaY) > 1 || !std::isfinite(request.contentJitterX) ||
        !std::isfinite(request.contentJitterY) || std::abs(request.contentJitterX) > 0.5f ||
        std::abs(request.contentJitterY) > 0.5f || !std::isfinite(request.denoisingRange) ||
        request.denoisingRange <= 0 || request.denoisingRange > std::numeric_limits<float>::max() / 2) {
        return mcdr::err("rt frame", "rt frame: invalid guide parameters");
    }
    std::array<float, kFrameBytes / sizeof(float)> constants;
    std::memcpy(constants.data(), request.constants, kFrameBytes);
    if (!std::ranges::all_of(constants, [](float value) { return std::isfinite(value); }) ||
        constants[52] != static_cast<float>(request.width) ||
        constants[53] != static_cast<float>(request.height) || constants[103] < 0 ||
        constants[103] > 16777215 || std::floor(constants[103]) != constants[103]) {
        return mcdr::err("rt frame", "rt frame: inconsistent constants");
    }
    const auto snapshotFound = geometrySnapshots.find(request.snapshotId);
    if (snapshotFound != geometrySnapshots.end() && snapshotFound->second->cancelled()) {
        return mcdr::err("rt frame", "rt frame: geometry recording cancelled");
    }
    if (snapshotFound == geometrySnapshots.end() || !snapshotFound->second->available(encoder)) {
        return mcdr::err("rt frame", "rt frame: geometry snapshot unavailable");
    }
    const auto snapshot = snapshotFound->second;
    if (snapshot->worldEpoch != request.worldEpoch || snapshot->materialRevision != request.materialRevision ||
        snapshot->records->size() / 64 != static_cast<size_t>(constants[103])) {
        return mcdr::err("rt frame", "rt frame: geometry snapshot mismatch");
    }
    const auto accelerationFound = accelerations.find(request.accelerationId);
    if (accelerationFound == accelerations.end()) return mcdr::err("rt frame", "rt frame: unknown acceleration");
    const auto& acceleration = *accelerationFound->second;
    if ((!acceleration.complete && acceleration.owner == 0) || !acceleration.view ||
        acceleration.snapshotId != request.snapshotId || !acceleration.available(encoder)) {
        return mcdr::err("rt frame", "rt frame: acceleration snapshot mismatch");
    }
    std::array<Dx12Object*, 5> geometry;
    std::ranges::transform(snapshot->buffers, geometry.begin(), [](const auto& buffer) { return buffer.get(); });
    const auto found = frameContexts.find(handle);
    if (found == frameContexts.end()) return mcdr::err("rt frame", "rt frame: unknown context");
    auto& context = *found->second;
    if (request.frameId <= context.frameId) return mcdr::err("rt frame", "rt frame: stale frame");
    if (context.frameId != 0 && request.worldEpoch == context.worldEpoch &&
        (request.sceneRevision < context.sceneRevision ||
            (request.snapshotId == context.snapshotId &&
                (request.materialRevision != context.materialRevision || geometry != context.geometry)))) {
        return mcdr::err("rt frame", "rt frame: inconsistent scene version");
    }
    if (!context.gbufferReady || request.gbufferRevision != context.gbufferRevision ||
        request.frameNanos != context.gbufferFrameNanos || request.width != context.gbufferWidth ||
        request.height != context.gbufferHeight || !currentRtRecording(context.gbufferRecording, encoder)) {
        return mcdr::err("rt frame", "rt frame: current replay required");
    }
    const auto& spatial = context.spatial;
    if (spatial.frameId != request.frameId || spatial.worldEpoch != request.worldEpoch ||
        spatial.gbufferRevision != request.gbufferRevision || !currentRtRecording(spatial.recording, encoder) ||
        (acceleration.owner != 0 && spatial.liveRevision != scenes.at(acceleration.owner).liveRevision)) {
        return mcdr::err("rt frame", "rt frame: current spatial coverage required");
    }
    const auto& gbuffer = context.gbuffer;
    const std::array guides{gbuffer[1].get(), gbuffer[2].get(), gbuffer[3].get(),
        gbuffer[0].get(), gbuffer[4].get()};
    const bool resize = request.width != context.width || request.height != context.height;
    const auto planeBytes = static_cast<int64_t>(request.width) * request.height * 16;
    const bool trackPaths = !lighting.empty();
    const auto pathColumns = (static_cast<uint32_t>(request.width) + kPathTileSize - 1) / kPathTileSize;
    const auto pathRows = (static_cast<uint32_t>(request.height) + kPathTileSize - 1) / kPathTileSize;
    const auto pathTiles = pathColumns * pathRows;
    const int64_t pathBytes = trackPaths ? kPathHeaderBytes + int64_t{pathTiles} * kPathTileBytes : 0;
    const int64_t pathTailBytes = pathBytes * 2 + (trackPaths ? int64_t{pathTiles} * 4 : 0);
    const auto auxBytes = planeBytes * 5 + pathTailBytes;
    if (auxBytes > UINT32_MAX) return mcdr::err("rt frame", "rt frame: auxiliary address range exceeded");
    const bool replaceTargets = resize || !context.targets[2] || context.targets[2]->size != auxBytes;
    const bool prepareGuides = request.signalEncoding != 0;
    const bool allocateGuides = prepareGuides && (resize || !context.denoiseGuides[0]);
    std::array<RtObject, 5> guideReplacements;
    std::array<RtObject, kSignalUavs> signalReplacements;
    auto& denoiseGuides = allocateGuides ? guideReplacements : context.denoiseGuides;
    auto& signals = allocateGuides ? signalReplacements : context.signals;
    if (allocateGuides) {
        const std::array guideFormats{42, 42, 44, 44, 44};
        for (size_t index = 0; index < denoiseGuides.size(); ++index) {
            auto texture = createTexture(36, guideFormats[index], request.width, request.height, 1, 1, true);
            if (!texture) return std::unexpected(texture.error());
            denoiseGuides[index].reset(static_cast<Dx12Object*>(*texture));
        }
        const std::array signalFormats{42, 42, 44, 42};
        for (size_t index = 0; index < signals.size(); ++index) {
            auto signal = createTexture(36, signalFormats[index], request.width, request.height, 1, 1, true);
            if (!signal) return std::unexpected(signal.error());
            signals[index].reset(static_cast<Dx12Object*>(*signal));
        }
    }
    std::array<RtObject, kTraceUavs> replacements;
    auto& targets = replaceTargets ? replacements : context.targets;
    if (replaceTargets) {
        for (size_t index = 0; index < targets.size(); ++index) {
            auto buffer = frameBuffer(1024, (index == 2 ? auxBytes : planeBytes) +
                (index == 1 ? kPendingFeedbackBytes : 0));
            if (!buffer) return std::unexpected(buffer.error());
            targets[index] = std::move(*buffer);
        }
    }
    auto frameConstants = frameUpload(constants.data(), kFrameBytes, kConstantBytes);
    if (!frameConstants) return std::unexpected(frameConstants.error());
    std::vector<uint8_t> traceRecordBytes(snapshot->records->size() + spatial.bytes.size() + lighting.size());
    if (!snapshot->records->empty()) std::memcpy(traceRecordBytes.data(), snapshot->records->data(), snapshot->records->size());
    std::memcpy(traceRecordBytes.data() + snapshot->records->size(), spatial.bytes.data(), spatial.bytes.size());
    if (!lighting.empty()) std::memcpy(traceRecordBytes.data() + snapshot->records->size() + spatial.bytes.size(),
        lighting.data(), lighting.size());
    if (acceleration.coverage.unbounded || (acceleration.owner != 0 &&
            scenes.at(acceleration.owner).explicitTargets && scenes.at(acceleration.owner).outputMode != 0)) {
        uint32_t flags = 0;
        auto* flagData = traceRecordBytes.data() + snapshot->records->size() + 56;
        std::memcpy(&flags, flagData, sizeof(flags));
        flags |= acceleration.coverage.unbounded ? 2u : 4u;
        std::memcpy(flagData, &flags, sizeof(flags));
    }
    auto traceRecords = frameUpload(traceRecordBytes.data(), static_cast<int64_t>(traceRecordBytes.size()),
        static_cast<int64_t>(traceRecordBytes.size()));
    if (!traceRecords) return std::unexpected(traceRecords.error());
    RtPathSnapshot pendingPath;
    RtSharedObject pathSeed;
    const auto& previousPath = context.path;
    const bool previousPathReady = trackPaths && previousPath.buffer && previousPath.recording &&
        previousPath.worldEpoch == request.worldEpoch && previousPath.width == request.width &&
        previousPath.height == request.height && previousPath.buffer->size == pathBytes &&
        previousPath.recording->available(encoder);
    if (trackPaths) {
        std::vector<uint32_t> seed(static_cast<size_t>(pathBytes / 4) + pathTiles);
        seed[0] = 0x50445452u;
        seed[1] = 1;
        std::memcpy(seed.data() + 2, &request.worldEpoch, sizeof(request.worldEpoch));
        std::memcpy(seed.data() + 4, &request.frameId, sizeof(request.frameId));
        std::memcpy(seed.data() + 6, lighting.data() + 32, sizeof(uint64_t));
        seed[8] = static_cast<uint32_t>(request.width);
        seed[9] = static_cast<uint32_t>(request.height);
        seed[10] = kPathTileSize;
        seed[11] = 4;
        seed[12] = pathColumns;
        seed[13] = pathRows;
        seed[14] = previousPathReady ? 1u : 0u;
        for (uint32_t tile = 0; tile < pathTiles; ++tile) {
            for (uint32_t lobe = 0; lobe < 4; ++lobe) {
                const size_t entry = kPathHeaderBytes / 4 + size_t{tile} * kPathTileBytes / 4 + lobe * 8;
                std::fill_n(seed.begin() + entry, 3, 0xff7fffffu);
                std::fill_n(seed.begin() + entry + 4, 3, 0x00800000u);
            }
        }
        auto uploaded = frameUpload(seed.data(), static_cast<int64_t>(seed.size() * sizeof(uint32_t)),
            static_cast<int64_t>(seed.size() * sizeof(uint32_t)));
        if (!uploaded) return std::unexpected(uploaded.error());
        pathSeed = RtSharedObject(uploaded->release(), RtObjectRelease{});
        auto buffer = frameBuffer(8, pathBytes);
        if (!buffer) return std::unexpected(buffer.error());
        pendingPath = {RtSharedObject(buffer->release(), RtObjectRelease{}), currentCommandRecording(encoder),
            request.worldEpoch, request.width, request.height};
    }
    const std::array traceSrvs{traceRecords->get(), geometry[1], geometry[2], geometry[3], geometry[4],
        acceleration.view.get(), context.environment.get()};
    const std::array traceUavs{targets[0].get(), targets[1].get(), targets[2].get(),
        targets[3].get(), targets[4].get()};
    const bool resetExposure = request.resetExposure != 0 || context.exposureDirty || resize ||
        request.worldEpoch != context.committedWorldEpoch ||
        request.exposureMode != context.committedExposureMode ||
        request.exposureEv != context.committedExposureEv ||
        request.frameNanos <= context.committedFrameNanos;
    const double elapsed = std::clamp((static_cast<double>(request.frameNanos) -
        static_cast<double>(context.committedFrameNanos)) * 1e-9, 0.0, 0.5);
    const float blend = static_cast<float>(-std::expm1(-request.exposureSpeed * elapsed));
    if (context.recording && !context.recording->submitted)
        abandonRtFrameAttempt(handle, context, context.recording.get());
    context.exposurePending = false;
    context.exposureRecording.reset();
    context.gbufferReady = false;
    context.recording.reset();
    context.compositeRecorded = false;
    context.pendingPath = {};
    context.signalEncoding = 0;
    context.rasterFallbackReady = false;
    context.denoisePending = false;
    context.srPending = false;
    context.reconstructed = {};
    const int32_t traceConstants = 0;
    encoder->rtGeometryRetained[encoder->currentListSlot].push_back(snapshot);
    retainAcceleration(encoder, acceleration);
    if (trackPaths) {
        auto& retained = encoder->rtGeometryUploadsRetained[encoder->currentListSlot];
        retained.push_back(pathSeed);
        retained.push_back(pendingPath.buffer);
        if (previousPathReady) retained.push_back(previousPath.buffer);
        auto seeded = copyBufferToBuffer(encoder, pathSeed.get(), 0, targets[2].get(), planeBytes * 5, pathBytes);
        if (!seeded) return seeded;
        auto cleared = copyBufferToBuffer(encoder, pathSeed.get(), pathBytes,
            targets[2].get(), planeBytes * 5 + pathBytes * 2, int64_t{pathTiles} * 4);
        if (!cleared) return cleared;
        if (previousPathReady) {
            auto copied = copyBufferToBuffer(encoder, previousPath.buffer.get(), 0,
                targets[2].get(), planeBytes * 5 + pathBytes, pathBytes);
            if (!copied) return copied;
        }
    }
    auto* feedback = preparePendingFeedback(encoder, acceleration, handle, request.frameId);
    if (feedback) {
        auto cleared = copyBufferToBuffer(encoder, pendingFeedbackZero.get(), 0,
            targets[1].get(), planeBytes, kPendingFeedbackBytes);
        if (!cleared) return cleared;
    }
    const bool workGraph = context.pipeline->kind == Dx12Object::Kind::WorkGraphPipeline;
    const std::array environmentUavs{context.environment.get()};
    if (dispatchComputeEx(encoder, context.environmentPipeline.get(), nullptr, 0,
            environmentUavs.data(), 1, nullptr, 0, frameConstants->get(), 0, 1, 1, 1) != 1) {
        return mcdr::err("rt frame", "rt frame: environment recording failed");
    }
    const bool initializeGraph = workGraph && (!context.workGraphInitialized ||
        !context.workGraphInitialized->available(encoder));
    const auto traced = workGraph ? dispatchWorkGraph(encoder, context.pipeline.get(), context.workGraphBacking.get(),
        initializeGraph, traceSrvs.data(), kTraceSrvs, traceUavs.data(), kTraceUavs,
        &traceConstants, 1, frameConstants->get(), request.width, request.height) :
        dispatchRays(encoder, context.pipeline.get(), context.table.get(), 0, kRecordBytes, kRecordBytes,
            1, kRecordBytes * 2, kRecordBytes, 3, traceSrvs.data(), kTraceSrvs, traceUavs.data(), kTraceUavs,
            &traceConstants, 1, frameConstants->get(), 0, request.width, request.height);
    if (traced != 1) {
        return mcdr::err("rt frame", "rt frame: trace recording failed");
    }
    if (initializeGraph) context.workGraphInitialized = currentCommandRecording(encoder);
    if (trackPaths) {
        auto copied = copyBufferToBuffer(encoder, targets[2].get(), planeBytes * 5,
            pendingPath.buffer.get(), 0, pathBytes);
        if (!copied) return copied;
    }
    if (feedback) {
        auto copied = copyBufferToBuffer(encoder, targets[1].get(), planeBytes,
            feedback->readback.get(), 0, kPendingFeedbackBytes);
        if (!copied) return copied;
    }
    const std::array materialSrvs{targets[2].get(), geometry[3]};
    const int32_t guideConstants = std::bit_cast<int32_t>(request.depthScale);
    if (dispatchComputeEx(encoder, context.guide.get(), materialSrvs.data(), kGuideSrvs,
            guides.data(), kGuideUavs, &guideConstants, 1, frameConstants->get(), 0,
            (request.width + 7) / 8, (request.height + 7) / 8, 1) != 1) {
        return mcdr::err("rt frame", "rt frame: guide recording failed");
    }
    const std::array exposureSrvs{targets[0].get(), targets[1].get(), targets[3].get(), targets[4].get(),
        gbuffer[0].get(), gbuffer[3].get(), gbuffer[1].get(),
        source ? source : gbuffer[0].get()};
    const std::array exposureUavs{context.exposure.get()};
    const std::array exposureConstants{request.exposureMode, std::bit_cast<int32_t>(request.exposureEv),
        std::bit_cast<int32_t>(blend), resetExposure ? 1 : 0, source ? 1 : 0};
    if (dispatchComputeEx(encoder, context.exposurePipeline.get(), exposureSrvs.data(), kExposureSrvs,
            exposureUavs.data(), kExposureUavs, exposureConstants.data(),
            static_cast<int32_t>(exposureConstants.size()), frameConstants->get(), 0, 1, 1, 1) != 1) {
        return mcdr::err("rt frame", "rt frame: exposure recording failed");
    }
    if (prepareGuides) {
        const std::array motionSrvs{gbuffer[4].get()};
        const std::array motionUavs{denoiseGuides[0].get()};
        const std::array motionConstants{std::bit_cast<int32_t>(request.depthScale),
            std::bit_cast<int32_t>(request.jitterDeltaX), std::bit_cast<int32_t>(request.jitterDeltaY)};
        if (dispatchComputeEx(encoder, context.motion.get(), motionSrvs.data(), kMotionSrvs,
                motionUavs.data(), kMotionUavs, motionConstants.data(),
                static_cast<int32_t>(motionConstants.size()), frameConstants->get(), 0,
                (request.width + 7) / 8, (request.height + 7) / 8, 1) != 1) {
            return mcdr::err("rt frame", "rt frame: motion recording failed");
        }
        const std::array packSrvs{gbuffer[2].get(), gbuffer[4].get(), targets[0].get(), targets[1].get()};
        const std::array packUavs{denoiseGuides[1].get(), denoiseGuides[2].get(),
            denoiseGuides[3].get(), denoiseGuides[4].get()};
        const std::array packConstants{std::bit_cast<int32_t>(request.depthScale),
            std::bit_cast<int32_t>(request.denoisingRange)};
        if (dispatchComputeEx(encoder, context.pack.get(), packSrvs.data(), kPackSrvs,
                packUavs.data(), kPackUavs, packConstants.data(),
                static_cast<int32_t>(packConstants.size()), frameConstants->get(), 0,
                (request.width + 7) / 8, (request.height + 7) / 8, 1) != 1) {
            return mcdr::err("rt frame", "rt frame: guide pack recording failed");
        }
        const std::array signalGuides{gbuffer[0].get(), gbuffer[3].get(), gbuffer[1].get(), gbuffer[2].get()};
        auto encoded = encodeFrameSignals(encoder, context.signalPipeline.get(), targets, signals,
            signalGuides, denoiseGuides[2].get(), frameConstants->get(), request.width, request.height,
            request.signalEncoding == 1, source, context.exposure.get(),
            {request.contentJitterX, request.contentJitterY});
        if (!encoded) return encoded;
    }
    if (replaceTargets) context.targets = std::move(replacements);
    if (allocateGuides) {
        context.denoiseGuides = std::move(guideReplacements);
        context.signals = std::move(signalReplacements);
    } else if (resize) {
        context.denoiseGuides = {};
        context.signals = {};
    }
    context.constants = std::move(*frameConstants);
    context.traceRecords = std::move(*traceRecords);
    context.width = request.width;
    context.height = request.height;
    context.frameId = request.frameId;
    context.worldEpoch = request.worldEpoch;
    context.sceneRevision = request.sceneRevision;
    context.materialRevision = request.materialRevision;
    context.snapshotId = request.snapshotId;
    context.geometry = geometry;
    context.frameNanos = request.frameNanos;
    context.exposureMode = request.exposureMode;
    context.exposureEv = request.exposureEv;
    context.exposurePending = true;
    context.signalEncoding = request.signalEncoding;
    context.rasterFallbackReady = prepareGuides && source != nullptr;
    context.denoisePending = prepareGuides;
    context.signalGuides = {gbuffer[0].get(), gbuffer[3].get(), gbuffer[1].get(), gbuffer[2].get()};
    context.contentJitter = {request.contentJitterX, request.contentJitterY};
    context.recording = currentCommandRecording(encoder);
    context.exposureRecording = context.recording;
    context.pendingPath = std::move(pendingPath);
    context.lightingOffset = trackPaths ? static_cast<uint32_t>(traceRecordBytes.size() - lighting.size()) : 0;
    context.lightingCount = 0;
    context.lightingFullDomain = 0;
    if (trackPaths) {
        std::memcpy(&context.lightingCount, lighting.data() + 44, sizeof(uint32_t));
        uint32_t reasons = 0;
        std::memcpy(&reasons, lighting.data() + 40, sizeof(uint32_t));
        context.lightingFullDomain = reasons != 0 ? 1u : 0u;
    }
    const uint64_t guideBytes = context.denoiseGuides[0] ? static_cast<uint64_t>(request.width) * request.height * 56 : 0;
    *output = {request.frameId, request.snapshotId, context.targets[0].get(), context.targets[1].get(),
        context.targets[2].get(), context.targets[3].get(), context.targets[4].get(), context.constants.get(),
        static_cast<uint64_t>(planeBytes * 9 + kConstantBytes + sizeof(float) + 6 * 8 + kPendingFeedbackBytes) +
            (context.table ? static_cast<uint64_t>(context.table->size) : 0) + context.pipeline->workGraphMemoryBytes +
            guideBytes + context.traceRecords->size + static_cast<uint64_t>(pathTailBytes + pathBytes) +
            (context.path.buffer ? static_cast<uint64_t>(context.path.buffer->size) : 0),
        context.exposure.get(), prepareGuides ? context.denoiseGuides[0].get() : nullptr,
        prepareGuides ? context.denoiseGuides[1].get() : nullptr,
        prepareGuides ? context.denoiseGuides[2].get() : nullptr,
        prepareGuides ? context.signals[0].get() : nullptr,
        prepareGuides ? context.signals[1].get() : nullptr,
        prepareGuides ? context.signals[2].get() : nullptr};
    return {};
}

mcdr::Result<void> recordRtFrame(CommandContext* encoder, uint64_t handle,
    const void* descriptionData, void* outputData) {
    std::lock_guard lock(frameMutex);
    return recordRtFrameLocked(encoder, handle, descriptionData, outputData);
}

mcdr::Result<void> reblurRtFrame(CommandContext* encoder, uint64_t handle, uint64_t frameId) {
    if (!encoder || !encoder->listOpen || !encoder->commandList || encoder->inRenderPass) {
        return mcdr::err("rt frame", "rt frame: open main list required");
    }
    std::lock_guard lock(frameMutex);
    const auto found = frameContexts.find(handle);
    if (found == frameContexts.end()) return mcdr::err("rt frame", "rt frame: unknown context");
    auto& context = *found->second;
    if (frameId == 0 || frameId != context.frameId || context.signalEncoding != 2 ||
        !context.exposurePending || !context.denoisePending || !currentRtRecording(context.recording, encoder)) {
        return mcdr::err("rt frame", "rt frame: linear signals unavailable");
    }
    context.signalEncoding = 0;
    auto encoded = encodeFrameSignals(encoder, context.signalPipeline.get(), context.targets, context.signals,
        context.signalGuides, context.denoiseGuides[2].get(), context.constants.get(),
        context.width, context.height, true);
    if (!encoded) return encoded;
    context.signalEncoding = 1;
    return {};
}

static mcdr::Result<void> denoiseRtFrameLocked(CommandContext* encoder, uint64_t handle,
    const void* descriptionData, void* outputData) {
    auto* output = static_cast<mcdr_rt_denoise_output*>(outputData);
    if (output) *output = {};
    const auto* description = static_cast<const mcdr_rt_denoise_desc*>(descriptionData);
    if (!description || !output || !encoder || !encoder->listOpen || !encoder->commandList ||
        encoder->inRenderPass) {
        return mcdr::err("rt frame", "rt denoise: open main list required");
    }
    const auto request = *description;
    if (request.frameId == 0 || request.historyFrame == 0 || request.planRevision == 0 ||
        request.outputWidth <= 0 || request.outputHeight <= 0 ||
        request.outputWidth > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        request.outputHeight > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        !request.nrdMatrices || !request.slConstants || request.nrdFloatCount != 64 || request.slFloatCount != 104) {
        return mcdr::err("rt frame", "rt denoise: invalid descriptor");
    }
    std::array<float, 64> matrices;
    std::array<float, 104> constants;
    std::memcpy(matrices.data(), request.nrdMatrices, sizeof(matrices));
    std::memcpy(constants.data(), request.slConstants, sizeof(constants));
    const auto finite = [](float value) { return std::isfinite(value); };
    if (!std::ranges::all_of(matrices, finite) || !std::ranges::all_of(constants, finite) ||
        std::any_of(constants.begin() + 68, constants.begin() + 72,
            [](float value) { return std::abs(value) > 0.5f; })) {
        return mcdr::err("rt frame", "rt denoise: invalid camera constants");
    }
    const auto found = frameContexts.find(handle);
    if (found == frameContexts.end()) return mcdr::err("rt frame", "rt frame: unknown context");
    auto& context = *found->second;
    if (request.frameId != context.frameId || !context.denoisePending || !context.exposurePending ||
        context.signalEncoding == 0 || !currentRtRecording(context.recording, encoder) ||
        request.outputWidth < context.width || request.outputHeight < context.height) {
        return mcdr::err("rt frame", "rt denoise: current signals unavailable");
    }
    context.denoisePending = false;
    context.compositeRecorded = false;
    const bool changed = context.denoisePlan != request.planRevision ||
        context.denoiseEncoding != context.signalEncoding || context.denoiseWidth != context.width ||
        context.denoiseHeight != context.height || context.displayWidth != request.outputWidth ||
        context.displayHeight != request.outputHeight;
    if (changed) {
        context.denoisePlan = request.planRevision;
        context.denoiseEncoding = context.signalEncoding;
        context.denoiseWidth = context.width;
        context.denoiseHeight = context.height;
        context.displayWidth = request.outputWidth;
        context.displayHeight = request.outputHeight;
        context.rrAvailable = true;
        context.nrdInitFailed = false;
        context.rrHistory.reset();
        context.nrdHistory.reset();
        context.srHistory.reset();
        context.srAvailable = true;
        context.srFrameNanos = 0;
    }
    const bool resetRr = activeRrContext != handle || context.rrHistory.requiresReset(request);
    const bool resetNrd = activeNrdContext != handle || context.nrdHistory.requiresReset(request);
    uint32_t extraDispatches = 0;
    std::array<char, 2048> diagnostic{};
    if (context.signalEncoding == 2) {
        if (context.rrAvailable && context.rasterFallbackReady) {
            RtObject replacement;
            if (!frameGuide(context.rrOutput.get(), request.outputWidth, request.outputHeight,
                    DXGI_FORMAT_R16G16B16A16_FLOAT)) {
                auto target = denoiseTarget(request.outputWidth, request.outputHeight);
                if (!target) return std::unexpected(target.error());
                replacement = std::move(*target);
            }
            auto* target = replacement ? replacement.get() : context.rrOutput.get();
            const std::array textures{context.signals[0].get(), target, context.denoiseGuides[2].get(),
                context.denoiseGuides[0].get(), context.signalGuides[3], context.signalGuides[0],
                context.signalGuides[2], context.signals[2].get(), context.exposure.get()};
            const std::array exposure{1.0f, 1.0f};
            const int result = dlssdEval(encoder, textures.data(), static_cast<int32_t>(textures.size()),
                context.width, context.height, request.outputWidth, request.outputHeight, ++rrFrameIndex,
                resetRr ? 1 : 0, constants.data(), diagnostic.data(), static_cast<int32_t>(diagnostic.size()),
                exposure.data());
            activeRrContext = handle;
            if (result > 0) {
                if (replacement) context.rrOutput = std::move(replacement);
                context.rrHistory.stage(request, context.recording);
                if (context.reportedBackend != 2) dbgLogInfo("rt denoise: RR");
                context.reportedBackend = 2;
                *output = {request.frameId, context.snapshotId, context.rrOutput.get(), nullptr,
                    denoiseBytes(context), 2, static_cast<uint32_t>(result)};
                context.reconstructed = *output;
                return {};
            }
            context.rrAvailable = false;
            dbgLogWarn("rt RR: unavailable, using NRD");
        }
        context.signalEncoding = 0;
        auto encoded = encodeFrameSignals(encoder, context.signalPipeline.get(), context.targets, context.signals,
            context.signalGuides, context.denoiseGuides[2].get(), context.constants.get(),
            context.width, context.height, true);
        if (!encoded) return encoded;
        context.signalEncoding = 1;
        extraDispatches = 1;
    }
    if (context.nrdInitFailed) return mcdr::err("rt frame", "rt NRD: context unavailable");
    activeNrdContext = 0;
    const auto initialized = mcdr_nrd_context_init(reinterpret_cast<void*>(getDeviceHandle()),
        context.width, context.height, reserveRtNrdMemory, diagnostic.data(), static_cast<int32_t>(diagnostic.size()));
    if (initialized <= 0) {
        context.nrdInitFailed = initialized != MCDR_NRD_MEMORY_PRESSURE;
        if (!context.nrdInitFailed) return mcdr::err("rt frame", "rt NRD: managed memory budget exhausted");
        dbgLogWarn("rt NRD: initialization failed");
        return mcdr::err("rt frame", "rt NRD: context unavailable");
    }
    activeNrdContext = handle;
    std::array<RtObject, 2> replacements;
    const bool resize = !frameGuide(context.nrdOutputs[0].get(), context.width, context.height,
        DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (resize) {
        for (auto& replacement : replacements) {
            auto target = denoiseTarget(context.width, context.height);
            if (!target) return std::unexpected(target.error());
            replacement = std::move(*target);
        }
    }
    const auto& targets = resize ? replacements : context.nrdOutputs;
    const std::array textures{context.denoiseGuides[0].get(), context.denoiseGuides[1].get(),
        context.denoiseGuides[3].get(), context.signals[0].get(), targets[0].get(),
        context.signals[1].get(), targets[1].get(), context.denoiseGuides[4].get()};
    const std::array jitter{-constants[68], -constants[69], -constants[70], -constants[71]};
    const auto denoiseAt = std::chrono::steady_clock::now();
    if (context.nrdLastDispatch.time_since_epoch().count() != 0) {
        const auto elapsed = std::chrono::duration<double, std::milli>(denoiseAt - context.nrdLastDispatch).count();
        if (elapsed > 100.0 && (++context.nrdLongGaps <= 4 || std::has_single_bit(context.nrdLongGaps))) {
            dbgLogWarn("rt NRD: dispatch gap %.1f ms, reset=%u", elapsed, resetNrd ? 1u : 0u);
        }
    }
    context.nrdLastDispatch = denoiseAt;
    if (resetNrd) dbgLogInfo("rt NRD: history reset at frame %llu", request.historyFrame);
    const bool trackDependencies = context.lightingPending && context.pendingPath.buffer && context.traceRecords;
    const McdrNrdDependencies dependencies{trackDependencies ? context.pendingPath.buffer.get() : nullptr,
        context.traceRecords.get(), context.worldEpoch, 0, context.lightingOffset,
        context.lightingCount, context.lightingFullDomain};
    const int result = nrdDenoise(encoder, textures.data(), static_cast<int32_t>(textures.size()), matrices.data(),
        static_cast<uint32_t>(request.historyFrame), resetNrd ? 1u : 0u, diagnostic.data(),
        static_cast<int32_t>(diagnostic.size()), jitter.data(), trackDependencies ? &dependencies : nullptr);
    if (result <= 0) {
        if (context.reportedBackend != 3) dbgLogWarn("rt NRD: evaluation failed");
        context.reportedBackend = 3;
        return mcdr::err("rt frame", "rt NRD: evaluation failed");
    }
    if (resize) context.nrdOutputs = std::move(replacements);
    context.nrdHistory.stage(request, context.recording);
    context.srRequest = request;
    context.srRequest.nrdMatrices = nullptr;
    context.srRequest.slConstants = nullptr;
    context.srConstants = constants;
    context.srPending = true;
    if (context.reportedBackend != 1) dbgLogInfo("rt denoise: NRD (%d passes)", result);
    context.reportedBackend = 1;
    *output = {request.frameId, context.snapshotId, context.nrdOutputs[0].get(), context.nrdOutputs[1].get(),
        denoiseBytes(context), 1, static_cast<uint32_t>(result) + extraDispatches};
    context.reconstructed = *output;
    return {};
}

mcdr::Result<void> denoiseRtFrame(CommandContext* encoder, uint64_t handle,
    const void* descriptionData, void* outputData) {
    std::lock_guard lock(frameMutex);
    return denoiseRtFrameLocked(encoder, handle, descriptionData, outputData);
}

static mcdr::Result<void> upscaleRtFrameLocked(CommandContext* encoder, uint64_t handle,
    uint64_t frameId, int32_t backend, void* outputData) {
    auto* output = static_cast<mcdr_rt_denoise_output*>(outputData);
    if (output) *output = {};
    if (!output || !encoder || !encoder->listOpen || !encoder->commandList || encoder->inRenderPass) {
        return mcdr::err("rt frame", "rt sr: open main list required");
    }
    if (frameId == 0 || backend < MCDR_SR_BACKEND_DLSS || backend > MCDR_SR_BACKEND_XESS) {
        return mcdr::err("rt frame", "rt sr: invalid request");
    }
    const auto found = frameContexts.find(handle);
    if (found == frameContexts.end()) return mcdr::err("rt frame", "rt frame: unknown context");
    auto& context = *found->second;
    if (frameId != context.frameId || !context.srPending || !context.exposurePending ||
        !context.nrdHistory.hasPending(context.srRequest, context.recording) ||
        !currentRtRecording(context.recording, encoder)) {
        return mcdr::err("rt frame", "rt sr: current NRD output unavailable");
    }
    if (!context.rasterFallbackReady) return mcdr::err("rt frame", "rt sr: current raster source required");
    const auto& constants = context.srConstants;
    if (constants[64] <= 0.0f || constants[65] <= constants[64] ||
        constants[66] <= 0.0f || constants[66] >= 3.141593f || constants[67] <= 0.0f) {
        return mcdr::err("rt frame", "rt sr: invalid camera parameters");
    }
    context.srPending = false;
    context.compositeRecorded = false;
    const bool changed = context.srBackend != backend;
    if (changed) {
        context.srBackend = backend;
        context.srAvailable = true;
        context.srHistory.reset();
        context.srFrameNanos = 0;
    }
    const bool reset = activeSrContext != handle || context.srHistory.requiresReset(context.srRequest);
    if (!context.srAvailable) return mcdr::err("rt frame", "rt sr: unavailable for current plan");
    const auto failure = [&context](const char* message) -> mcdr::Result<void> {
        context.srAvailable = false;
        dbgLogWarn("rt sr: %s", message);
        return mcdr::err("rt frame", message);
    };
    std::array<RtObject, 3> replacements;
    const bool resize = !frameGuide(context.srTargets[0].get(), context.width, context.height,
            DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !frameGuide(context.srTargets[1].get(), context.displayWidth, context.displayHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (resize) {
        const std::array formats{42, 42, 44};
        for (size_t index = 0; index < replacements.size(); ++index) {
            const bool display = index == 1;
            auto texture = createTexture(display ? 44 : 36, formats[index],
                display ? context.displayWidth : context.width,
                display ? context.displayHeight : context.height, 1, 1, true);
            if (!texture) return failure("target allocation failed");
            replacements[index].reset(static_cast<Dx12Object*>(*texture));
        }
    }
    const auto& targets = resize ? replacements : context.srTargets;
    const std::array srvs{context.nrdOutputs[0].get(), context.signalGuides[0], context.signalGuides[1],
        context.denoiseGuides[2].get(), context.nrdOutputs[1].get(), context.signalGuides[2],
        context.denoiseGuides[3].get(), context.denoiseGuides[4].get(), context.signals[3].get(),
        context.targets[1].get(), context.targets[3].get(), context.targets[4].get(), context.targets[0].get()};
    const std::array uavs{targets[0].get(), targets[2].get()};
    const bool reverse = backend != MCDR_SR_BACKEND_DLSS && constants[10] < 0.0f;
    const std::array projection{
        std::bit_cast<int32_t>(reverse ? constants[11] - constants[10] : constants[10]),
        std::bit_cast<int32_t>(reverse ? constants[15] - constants[14] : constants[14]),
        std::bit_cast<int32_t>(constants[11]), std::bit_cast<int32_t>(constants[15]),
        std::bit_cast<int32_t>(constants[68]), std::bit_cast<int32_t>(constants[69])};
    if (dispatchComputeEx(encoder, context.remodulatePipeline.get(), srvs.data(), kRemodulateSrvs,
            uavs.data(), kRemodulateUavs, projection.data(), static_cast<int32_t>(projection.size()),
            context.constants.get(), 0, (context.width + 7) / 8, (context.height + 7) / 8, 1) != 1) {
        return failure("remodulation failed");
    }
    const float frameTimeMs = context.srFrameNanos == 0 || context.frameNanos <= context.srFrameNanos ? 16.7f :
        std::clamp(static_cast<float>((static_cast<double>(context.frameNanos) -
            static_cast<double>(context.srFrameNanos)) / 1'000'000.0), 1.0f, 1000.0f);
    context.pendingSrFrameNanos = context.frameNanos;
    int32_t result = 0;
    if (backend == MCDR_SR_BACKEND_DLSS) {
        const std::array textures{targets[0].get(), targets[1].get(), targets[2].get(),
            context.denoiseGuides[0].get(), context.exposure.get()};
        const std::array exposure{1.0f, 1.0f};
        std::array<char, 256> diagnostic{};
        result = rtSrEval(encoder, textures.data(), static_cast<int32_t>(textures.size()),
            context.width, context.height, context.displayWidth, context.displayHeight, ++srFrameIndex,
            reset ? 1 : 0, constants.data(), diagnostic.data(), static_cast<int32_t>(diagnostic.size()), exposure.data());
    } else {
        mcdr_sr_dispatch description{};
        description.renderWidth = context.width;
        description.renderHeight = context.height;
        description.displayWidth = context.displayWidth;
        description.displayHeight = context.displayHeight;
        description.jitterOffsetX = constants[68];
        description.jitterOffsetY = constants[69];
        description.motionVectorScaleX = 1.0f;
        description.motionVectorScaleY = 1.0f;
        description.preExposure = 1.0f;
        description.frameTimeDeltaMs = frameTimeMs;
        description.cameraNear = constants[64];
        description.cameraFar = constants[65];
        description.cameraVerticalFov = constants[66];
        description.reset = reset ? 1 : 0;
        description.hdrInput = 1;
        description.backend = backend;
        description.flags = MCDR_SR_FLAG_UNJITTERED_MV;
        description.pColorInput = targets[0].get();
        description.pColorOutput = targets[1].get();
        description.pDepth = targets[2].get();
        description.pMotionVectors = context.denoiseGuides[0].get();
        description.depthFormatOrdinal = 44;
        description.cameraAspectRatio = constants[67];
        description.viewSpaceToMetersFactor = 1.0f;
        result = srEvaluateFrame(encoder, description);
    }
    activeSrContext = handle;
    if (result <= 0) return failure("evaluation unavailable");
    if (resize) context.srTargets = std::move(replacements);
    context.srHistory.stage(context.srRequest, context.recording);
    if (changed || reset) dbgLogDebug("rt sr: backend=%d reset=%d", backend, reset ? 1 : 0);
    *output = {frameId, context.snapshotId, context.srTargets[1].get(), nullptr,
        upscaleBytes(context), 2, static_cast<uint32_t>(result) + 1};
    context.reconstructed = *output;
    return {};
}

mcdr::Result<void> upscaleRtFrame(CommandContext* encoder, uint64_t handle,
    uint64_t frameId, int32_t backend, void* outputData) {
    std::lock_guard lock(frameMutex);
    return upscaleRtFrameLocked(encoder, handle, frameId, backend, outputData);
}

static mcdr::Result<void> compositeRtFrameLocked(CommandContext* encoder, uint64_t handle,
    uint64_t frameId, Dx12Object* source, int32_t debugMode, void* outputData) {
    auto* output = static_cast<mcdr_rt_denoise_output*>(outputData);
    if (output) *output = {};
    if (!output || !encoder || !encoder->listOpen || !encoder->commandList || encoder->inRenderPass) {
        return mcdr::err("rt frame", "rt composite: open main list required");
    }
    if (frameId == 0 || debugMode < 0 || debugMode > 10 || !source ||
        source->kind != Dx12Object::Kind::Texture || !source->resource || (source->usage & 4) == 0) {
        return mcdr::err("rt frame", "rt composite: invalid inputs");
    }
    const auto description = source->resource->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1 ||
        description.SampleDesc.Count != 1 || description.MipLevels != 1 ||
        (description.Flags & (D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0) {
        return mcdr::err("rt frame", "rt composite: incompatible source");
    }
    const auto found = frameContexts.find(handle);
    if (found == frameContexts.end()) return mcdr::err("rt frame", "rt frame: unknown context");
    auto& context = *found->second;
    if (frameId != context.frameId || !context.exposurePending || !currentRtRecording(context.recording, encoder) ||
        description.Width < static_cast<uint64_t>(context.width) ||
        description.Height < static_cast<uint32_t>(context.height) || source == context.compositeTarget.get()) {
        return mcdr::err("rt frame", "rt composite: current frame unavailable");
    }
    if (!context.rasterFallbackReady && context.signalEncoding != 0) {
        auto encoded = encodeFrameSignals(encoder, context.signalPipeline.get(), context.targets, context.signals,
            context.signalGuides, context.denoiseGuides[2].get(), context.constants.get(),
            context.width, context.height, context.signalEncoding == 1, source, context.exposure.get(),
            context.contentJitter);
        if (!encoded) return encoded;
        context.rasterFallbackReady = true;
    }
    const auto width = static_cast<int32_t>(description.Width);
    const auto height = static_cast<int32_t>(description.Height);
    RtObject replacement;
    if (!frameGuide(context.compositeTarget.get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        auto texture = createTexture(44, 42, width, height, 1, 1, true);
        if (!texture) return std::unexpected(texture.error());
        replacement.reset(static_cast<Dx12Object*>(*texture));
    }
    auto* target = replacement ? replacement.get() : context.compositeTarget.get();
    const auto& reconstructed = context.reconstructed;
    auto* denoised = reconstructed.diffuse ? reconstructed.diffuse : context.signalGuides[1];
    auto* specular = reconstructed.specular ? reconstructed.specular : denoised;
    const std::array compositeSrvs{context.signalGuides[0], context.targets[0].get(), context.signalGuides[1],
        context.targets[1].get(), source, context.targets[2].get(), denoised,
        context.targets[3].get(), context.targets[4].get(), context.signalGuides[2], specular, context.exposure.get(),
        context.denoiseGuides[3] ? context.denoiseGuides[3].get() : context.exposure.get(),
        context.denoiseGuides[4] ? context.denoiseGuides[4].get() : context.exposure.get()};
    const std::array compositeUavs{target};
    const int32_t mode = reconstructed.encoding == 1 ? 2 : reconstructed.encoding == 2 ? 1 : 0;
    const std::array compositeConstants{mode, 0, 0, debugMode,
        std::bit_cast<int32_t>(context.contentJitter[0]), std::bit_cast<int32_t>(context.contentJitter[1])};
    if (dispatchComputeEx(encoder, context.compositePipeline.get(), compositeSrvs.data(), kCompositeSrvs,
            compositeUavs.data(), kCompositeUavs, compositeConstants.data(), static_cast<int32_t>(compositeConstants.size()),
            context.constants.get(), 0, (width + 7) / 8, (height + 7) / 8, 1) != 1) {
        return mcdr::err("rt frame", "rt composite: dispatch failed");
    }
    if (replacement) context.compositeTarget = std::move(replacement);
    context.compositeRecorded = true;
    *output = {frameId, context.snapshotId, context.compositeTarget.get(), nullptr,
        static_cast<uint64_t>(width) * height * 8, reconstructed.encoding, 1};
    return {};
}

mcdr::Result<void> compositeRtFrame(CommandContext* encoder, uint64_t handle,
    uint64_t frameId, Dx12Object* source, int32_t debugMode, void* outputData) {
    std::lock_guard lock(frameMutex);
    return compositeRtFrameLocked(encoder, handle, frameId, source, debugMode, outputData);
}

static void abandonRtFrameAttempt(uint64_t handle, RtFrameContext& context, CommandRecording* recording) {
    context.lightingPending = false;
    context.pendingPath = {};
    context.nrdHistory.cancel(recording);
    context.rrHistory.cancel(recording);
    context.srHistory.cancel(recording);
    context.exposureDirty = true;
    context.exposurePending = false;
    context.exposureRecording.reset();
    context.denoisePending = false;
    context.srPending = false;
    context.reconstructed = {};
    context.compositeRecorded = false;
    if (activeNrdContext == handle) activeNrdContext = 0;
    if (activeRrContext == handle) activeRrContext = 0;
    if (activeSrContext == handle) activeSrContext = 0;
}

struct RtFrameAttemptScope {
    uint64_t handle;
    RtFrameContext& context;
    std::shared_ptr<CommandRecording> recording;
    bool complete = false;
    std::optional<RtSpatialSupport> spatialBefore;

    ~RtFrameAttemptScope() {
        if (!complete && context.gbufferReady && spatialBefore) {
            context.spatial = std::move(*spatialBefore);
        } else if (!complete && !context.gbufferReady) {
            abandonRtFrameAttempt(handle, context, recording.get());
            context.recording.reset();
        }
    }
};

mcdr::Result<void> recordRtFrameTransaction(CommandContext* encoder, uint64_t handle,
    const void* descriptionData, void* outputData, const void* lightingData, int64_t lightingBytes) {
    auto* output = static_cast<mcdr_rt_transaction_output*>(outputData);
    if (output) *output = {};
    if (!descriptionData || !output || !encoder || !encoder->listOpen || !encoder->commandList ||
        encoder->inRenderPass) return mcdr::err("rt frame", "rt transaction: open main list required");
    auto request = *static_cast<const mcdr_rt_transaction_desc*>(descriptionData);
    std::vector<uint8_t> lighting;
    uint64_t afterLightingRevision = 0;
    uint64_t lightingRevision = 0;
    if (lightingData || lightingBytes != 0) {
        if (!lightingData || lightingBytes < 64 || lightingBytes > 64 + 256 * 64 || lightingBytes % 64 != 0)
            return mcdr::err("rt frame", "rt lighting: invalid payload size");
        lighting.resize(static_cast<size_t>(lightingBytes));
        std::memcpy(lighting.data(), lightingData, lighting.size());
        std::array<uint64_t, 8> header;
        std::memcpy(header.data(), lighting.data(), sizeof(header));
        const auto reasons = static_cast<uint32_t>(header[5]);
        const auto count = static_cast<uint32_t>(header[5] >> 32);
        afterLightingRevision = header[3];
        lightingRevision = header[4];
        if (header[0] != 0x00000001434C5452ull || header[1] != request.trace.worldEpoch ||
            header[2] != request.liveRevision || afterLightingRevision > lightingRevision ||
            (reasons & ~127u) != 0 || count > 256 || lighting.size() != 64 + count * 64 ||
            header[6] != 0 || header[7] != 0 ||
            (afterLightingRevision == lightingRevision && (reasons != 0 || count != 0)))
            return mcdr::err("rt frame", "rt lighting: inconsistent header");
        std::set<uint64_t> nodes;
        for (uint32_t index = 0; index < count; ++index) {
            const auto* data = lighting.data() + 64 + index * 64;
            std::array<uint64_t, 4> identity;
            std::array<float, 6> bounds;
            uint64_t reserved;
            std::memcpy(identity.data(), data, sizeof(identity));
            std::memcpy(bounds.data(), data + 32, sizeof(bounds));
            std::memcpy(&reserved, data + 56, sizeof(reserved));
            const auto reason = static_cast<uint32_t>(identity[3]);
            const auto bounded = static_cast<uint32_t>(identity[3] >> 32);
            if (!nodes.insert(identity[0]).second || identity[1] <= afterLightingRevision ||
                identity[1] > identity[2] || identity[2] > lightingRevision || reason == 0 ||
                (reason & ~127u) != 0 || bounded > 1 || reserved != 0 ||
                !std::ranges::all_of(bounds, [](float value) { return std::isfinite(value); }) ||
                (bounded != 0 && (bounds[0] > bounds[3] || bounds[1] > bounds[4] || bounds[2] > bounds[5])))
                return mcdr::err("rt frame", "rt lighting: invalid change record");
        }
    }
    if (!request.trace.constants || request.trace.constantBytes != kFrameBytes || !request.spatial ||
        request.spatialBytes < 64 || request.spatialBytes > 64 + 512 * 2064 ||
        request.upscaler < MCDR_SR_BACKEND_NONE || request.upscaler > MCDR_SR_BACKEND_XESS ||
        request.debugMode < 0 || request.debugMode > 10 || !request.source ||
        request.source->kind != Dx12Object::Kind::Texture || !request.source->resource ||
        (request.source->usage & 4) == 0 || request.trace.signalEncoding > 2 ||
        request.reserved != 0 ||
        (request.timestamps && (request.firstTimestamp < 0 || request.firstTimestamp > request.timestamps->size - 4)) ||
        (request.trace.signalEncoding == 0 && request.upscaler != MCDR_SR_BACKEND_NONE)) {
        return mcdr::err("rt frame", "rt transaction: invalid descriptor");
    }
    const auto source = request.source->resource->GetDesc();
    const bool denoise = request.trace.signalEncoding != 0;
    if (source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || source.DepthOrArraySize != 1 ||
        source.SampleDesc.Count != 1 || source.MipLevels != 1 ||
        (source.Flags & (D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0 ||
        source.Width < static_cast<uint64_t>(request.trace.width) ||
        source.Height < static_cast<uint32_t>(request.trace.height) ||
        (denoise && (request.denoise.frameId != request.trace.frameId || request.denoise.historyFrame == 0 ||
            request.denoise.planRevision == 0 || !request.denoise.nrdMatrices || !request.denoise.slConstants ||
            request.denoise.nrdFloatCount != 64 || request.denoise.slFloatCount != 104 ||
            static_cast<uint64_t>(request.denoise.outputWidth) != source.Width ||
            static_cast<uint32_t>(request.denoise.outputHeight) != source.Height))) {
        return mcdr::err("rt frame", "rt transaction: incompatible inputs");
    }
    std::array<float, kFrameBytes / sizeof(float)> traceConstants;
    std::memcpy(traceConstants.data(), request.trace.constants, sizeof(traceConstants));
    request.trace.constants = traceConstants.data();
    std::array<float, 64> matrices{};
    std::array<float, 104> constants{};
    if (denoise) {
        std::memcpy(matrices.data(), request.denoise.nrdMatrices, sizeof(matrices));
        std::memcpy(constants.data(), request.denoise.slConstants, sizeof(constants));
        const auto finite = [](float value) { return std::isfinite(value); };
        if (!std::ranges::all_of(matrices, finite) || !std::ranges::all_of(constants, finite) ||
            std::any_of(constants.begin() + 68, constants.begin() + 72,
                [](float value) { return std::abs(value) > 0.5f; })) {
            return mcdr::err("rt frame", "rt transaction: invalid camera constants");
        }
        request.denoise.nrdMatrices = matrices.data();
        request.denoise.slConstants = constants.data();
    }
    std::vector<uint8_t> spatial(static_cast<size_t>(request.spatialBytes));
    std::memcpy(spatial.data(), request.spatial, spatial.size());
    std::lock_guard lock(frameMutex);
    const auto found = frameContexts.find(handle);
    if (found == frameContexts.end()) return mcdr::err("rt frame", "rt transaction: unknown context");
    auto& context = *found->second;
    if (!lighting.empty() && context.lightingActive && context.lightingEpoch == request.trace.worldEpoch &&
        (afterLightingRevision > context.lightingRevision || lightingRevision < context.lightingRevision))
        return mcdr::err("rt frame", "rt lighting: history cursor gap");
    if (!context.gbufferReady || request.source == context.compositeTarget.get()) {
        return mcdr::err("rt frame", "rt transaction: current replay required");
    }
    RtFrameAttemptScope recovery{handle, context, currentCommandRecording(encoder), false, std::nullopt};
    if (context.spatial.gbufferRevision != context.gbufferRevision) {
        recovery.spatialBefore = std::move(context.spatial);
        context.spatial = {};
    }
    auto frozen = setRtFrameSpatialLocked(encoder, handle, request.trace.frameId, request.trace.worldEpoch,
        request.liveRevision, spatial.data(), request.spatialBytes);
    if (!frozen) return frozen;
    mcdr_rt_transaction_output result{};
    const auto timestamp = [&](int32_t offset) {
        if (request.timestamps && writeTimestampToPool(encoder, request.timestamps, request.firstTimestamp + offset))
            result.timestampMask |= 1u << offset;
    };
    auto traced = recordRtFrameLocked(encoder, handle, &request.trace, &result.trace, request.source, lighting);
    if (!traced) return traced;
    context.lightingPending = !lighting.empty();
    context.pendingLightingEpoch = request.trace.worldEpoch;
    context.pendingLightingRevision = lightingRevision;
    timestamp(0);
    if (denoise) {
        auto denoised = denoiseRtFrameLocked(encoder, handle, &request.denoise, &result.denoise);
        if (!denoised) {
            result.fallbackFlags |= MCDR_RT_FALLBACK_RAW;
            context.reconstructed = {};
            context.nrdHistory.cancel(context.recording.get());
            context.rrHistory.cancel(context.recording.get());
            if (activeNrdContext == handle) activeNrdContext = 0;
            if (activeRrContext == handle) activeRrContext = 0;
        } else {
            if (request.trace.signalEncoding == 2 && result.denoise.encoding == 1)
                result.fallbackFlags |= MCDR_RT_FALLBACK_NRD;
        }
    }
    timestamp(1);
    if (result.denoise.encoding == 1 && request.upscaler != MCDR_SR_BACKEND_NONE) {
                auto upscaled = upscaleRtFrameLocked(encoder, handle, request.trace.frameId,
                    request.upscaler, &result.upscale);
                if (!upscaled) {
                    result.fallbackFlags |= MCDR_RT_FALLBACK_NO_SR;
                    context.srHistory.cancel(context.recording.get());
                    if (activeSrContext == handle) activeSrContext = 0;
                }
    }
    timestamp(2);
    auto composed = compositeRtFrameLocked(encoder, handle, request.trace.frameId,
        request.source, request.debugMode, &result.composite);
    if (!composed) return composed;
    timestamp(3);
    result.recorded = 1;
    *output = result;
    recovery.complete = true;
    return {};
}

void notifyRtRecordingCommitted(CommandRecording* recording) {
    if (!recording) return;
    std::lock_guard lock(frameMutex);
    for (auto& entry : frameContexts) {
        auto& context = *entry.second;
        if (context.recording.get() == recording && !context.compositeRecorded) {
            abandonRtFrameAttempt(entry.first, context, recording);
            continue;
        }
        if (context.recording.get() == recording && context.compositeRecorded) {
            context.path = std::move(context.pendingPath);
            context.pendingPath = {};
        }
        if (context.recording.get() == recording && context.compositeRecorded && context.lightingPending) {
            context.lightingEpoch = context.pendingLightingEpoch;
            context.lightingRevision = context.pendingLightingRevision;
            context.lightingActive = true;
            context.lightingPending = false;
        }
        for (auto& feedback : pendingFeedback) {
            if (feedback.recording.get() == recording && feedback.context == entry.first) {
                feedback.accepted = context.recording.get() == recording && context.compositeRecorded &&
                    feedback.frame == context.frameId;
            }
        }
        context.nrdHistory.commit(recording);
        context.rrHistory.commit(recording);
        const bool srCommitted = context.srHistory.pendingRecording.get() == recording;
        context.srHistory.commit(recording);
        if (srCommitted) context.srFrameNanos = context.pendingSrFrameNanos;
        if (context.exposureRecording && context.exposureRecording.get() == recording) {
            context.committedFrameNanos = context.frameNanos;
            context.committedWorldEpoch = context.worldEpoch;
            context.committedExposureMode = context.exposureMode;
            context.committedExposureEv = context.exposureEv;
            context.exposureDirty = false;
            context.exposurePending = false;
            context.exposureRecording.reset();
        }
    }
}

void notifyRtRecordingCancelled(CommandRecording* recording) {
    if (!recording) return;
    std::lock_guard lock(frameMutex);
    for (auto& [handle, entry] : frameContexts) {
        auto& context = *entry;
        const bool ownsRecording = (context.recording && context.recording.get() == recording) ||
            (context.gbufferRecording && context.gbufferRecording.get() == recording) ||
            (context.exposureRecording && context.exposureRecording.get() == recording) ||
            context.nrdHistory.pendingRecording.get() == recording ||
            context.rrHistory.pendingRecording.get() == recording ||
            context.srHistory.pendingRecording.get() == recording;
        context.nrdHistory.cancel(recording);
        context.rrHistory.cancel(recording);
        context.srHistory.cancel(recording);
        if (context.exposureRecording && context.exposureRecording.get() == recording) {
            context.exposureDirty = true;
            context.exposurePending = false;
            context.exposureRecording.reset();
        }
        if (context.recording && context.recording.get() == recording) {
            context.recording.reset();
            context.compositeRecorded = false;
            context.lightingPending = false;
            context.pendingPath = {};
            context.denoisePending = false;
            context.srPending = false;
            context.reconstructed = {};
        }
        if (context.gbufferRecording && context.gbufferRecording.get() == recording) {
            context.gbufferReady = false;
            context.gbufferRecorded = false;
            context.gbufferFailed = false;
        }
        if (ownsRecording) {
            context.exposureDirty = true;
            context.exposurePending = false;
            if (activeNrdContext == handle) activeNrdContext = 0;
            if (activeRrContext == handle) activeRrContext = 0;
            if (activeSrContext == handle) activeSrContext = 0;
        }
    }
}

}
