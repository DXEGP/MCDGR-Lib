module;

#include "_d3d12_module_prelude.h"
#include "mcdr_graphics_recording.h"
#include "mcdr_oit_budget.h"
#include <span>

#include "mcdr_rtx_support.h"

#include "mcdr_abi.h"

#include "mcdr_dlss_impl.h"

export module mcdr.d3d12.device;

import mcdr.error;

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

enum class DbgLogLevel : int {
    ERR = 0,
    WARN = 1,
    INFO = 2,
    DEBUG = 3,
};

double nowMs();

int formatLogPrefix(char* out, size_t outSize, const char* level, const char* logger);

void dbgLog(const char* fmt, ...);
void dbgLogError(const char* fmt, ...);
void dbgLogWarn(const char* fmt, ...);
void dbgLogInfo(const char* fmt, ...);
void dbgLogProgress(const char* fmt, ...);
void dbgLogDebug(const char* fmt, ...);
void setLogLevel(int level);
void setShutdownTraceAlways(bool always);
void setLogFile(const char* utf8Path);

int setPixCapture(bool enabled);

int setPixDirectory(const char* utf8Path);

int startPixTimingCapture(const char* utf8Path);
int stopPixTimingCapture();

int setShadingRate(int rate);

int setShadingRateNow(CommandContext* ctx, int rate);

int setShadingRateCombiners(int first, int second);

int setAntiAliasing(int mode);
int antiAliasingMode();
std::string antiAliasingLastFailureText();
const char* antiAliasingName(int mode);

int setDlssEnabled(int enabled);
bool dlssIsEnabled();
std::string dlssLastFailureText();

int snapshotDlssDepth(CommandContext* ctx, mcdr_object* depth);

int setDlssQuality(int tier);

int setDlssRenderPreset(int preset);

int setDlssOutput(mcdr_object* tex);

#ifdef DX12_DEBUG_VERBOSE
#define DBG_LOG_DEBUG(...) ::d3d12_mc::dbgLogDebug(__VA_ARGS__)
#else
#define DBG_LOG_DEBUG(...) ((void)0)
#endif

struct BlitPipeline;
struct SharpenPipeline;

struct DeviceContext {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Device2> device2;

    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12DescriptorHeap> srvCpuHeap;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> frameRtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12DescriptorHeap> samplerHeap;
    ComPtr<ID3D12DescriptorHeap> drawHeap;

    ComPtr<ID3D12DescriptorHeap> computeUavHeap;
    UINT computeUavStride = 0;
    UINT computeUavNext = 0;

    UINT syncRingReserve = 0;

    ComPtr<ID3D12CommandSignature> cmdSigIndexed;
    ComPtr<ID3D12CommandSignature> cmdSigNonIndexed;

    UINT srvInc = 0, rtvInc = 0, dsvInc = 0, samplerInc = 0;
    UINT drawInc = 0;

    std::string adapterName;
    ComPtr<IDXGIAdapter> adapter;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;

    struct Capabilities {
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_2;
        D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;
        D3D12_MESH_SHADER_TIER meshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
        D3D12_WORK_GRAPHS_TIER workGraphsTier = D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED;
        D3D12_SAMPLER_FEEDBACK_TIER samplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
        D3D12_RAYTRACING_TIER raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
        D3D12_VARIABLE_SHADING_RATE_TIER shadingRateTier = D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
        UINT shadingRateImageTileSize = 0;
        bool additionalShadingRates = false;
        D3D12_RESOURCE_BINDING_TIER resourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
        bool enhancedBarriers = false;
        bool barycentricsSupported = false;
        bool queried = false;

        bool dxcAvailable = false;
        std::string dxcPath;
        std::string dxcProfile;
        std::string dxcNote;

        constexpr bool meshShadersSupported() const {
            return meshShaderTier >= D3D12_MESH_SHADER_TIER_1;
        }

        constexpr bool shadingRateSupported() const {
            return shadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1;
        }
        constexpr bool shadingRateImageSupported() const {
            return shadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2;
        }
    };
    Capabilities caps;

    mcdr::RtxSupport rtx;

    int requestedShadingRate = -1;
    UINT64 shadingRateAppliedLists = 0;
    bool shadingRateUnsupportedLogged = false;
    bool shadingRateList5Logged = false;
    int shadingRateCombiner0 = static_cast<int>(D3D12_SHADING_RATE_COMBINER_PASSTHROUGH);
    int shadingRateCombiner1 = static_cast<int>(D3D12_SHADING_RATE_COMBINER_PASSTHROUGH);
    UINT64 timestampFrequency = 0;
    ComPtr<ID3D12CommandQueue> queue;

    ComPtr<ID3D12Fence> queueFence;
    HANDLE queueFenceEvent = nullptr;
    UINT64 queueFenceValue = 0;

    ComPtr<ID3D12InfoQueue> infoQueue;

    std::unique_ptr<BlitPipeline> blitPipeline;
    std::unique_ptr<SharpenPipeline> sharpenPipeline;

    int aaMode = 0;
    bool dlssEnabled = false;
    int dlssQualityTier = 2;
    int dlssPreset = 0;
    mcdr_object* dlssOutputOverride = nullptr;
    std::shared_ptr<void> dlssOutputOverrideLease;
    ComPtr<ID3D12Resource> smaaEdges;
    ComPtr<ID3D12Resource> smaaWeights;
    D3D12_CPU_DESCRIPTOR_HANDLE smaaEdgesRtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE smaaWeightsRtv{};
    UINT64 smaaWidth = 0;
    UINT64 smaaHeight = 0;
    ComPtr<ID3D12Resource> smaaAreaTex;
    ComPtr<ID3D12Resource> smaaSearchTex;
    bool smaaLutsReady = false;
    bool smaaLutReadyLogged = false;
    ComPtr<ID3D12PipelineState> smaaWeightLutPso;
    bool smaaLutFallbackWarned = false;
    ComPtr<ID3D12Resource> smaaWorldResult;
    D3D12_CPU_DESCRIPTOR_HANDLE smaaWorldResultRtv{};
    DXGI_FORMAT smaaWorldResultFormat = DXGI_FORMAT_UNKNOWN;
    bool smaaPipelinesReady = false;
    bool smaaUnavailableLogged = false;
    ComPtr<ID3D12RootSignature> smaaRootSig;
    ComPtr<ID3D12PipelineState> smaaEdgePso;
    ComPtr<ID3D12PipelineState> smaaWeightPso;
    ComPtr<ID3D12PipelineState> smaaBlendPso;
    UINT64 smaaAppliedFrames = 0;
    ComPtr<ID3D12DescriptorHeap> smaaRtvHeap;
    DXGI_FORMAT smaaBlendFormat = DXGI_FORMAT_UNKNOWN;
    bool smaaDisabled = false;

    ComPtr<ID3D12Resource> taaHistory[2];
    D3D12_CPU_DESCRIPTOR_HANDLE taaHistoryRtv[2]{};
    UINT64 taaWidth = 0;
    UINT64 taaHeight = 0;
    ComPtr<ID3D12DescriptorHeap> taaRtvHeap;
    ComPtr<ID3D12RootSignature> taaRootSig;
    ComPtr<ID3D12PipelineState> taaPso;
    int taaReadIndex = 0;
    int taaWriteIndex = 1;
    bool taaHistoryValid = false;
    bool taaDisabled = false;
    bool taaUnavailableLogged = false;
    bool taaSharpenFallbackWarned = false;
    UINT64 taaAppliedFrames = 0;
};

struct TextureWriteRecording {
    std::shared_ptr<const void> identity;
    std::shared_ptr<std::atomic<bool>> discarded;
    ComPtr<ID3D12Fence> fence;
    uint64_t submission = 0;
    uint32_t firstSubresource = 0;
    uint32_t subresourceCount = 0;
};
struct TextureWriteRecordingEntry {
    std::shared_ptr<void> budget;
    TextureWriteRecording recording;
    std::unique_ptr<TextureWriteRecordingEntry> next;
};
struct Dx12Object {
    enum class Kind { Texture, Buffer, Sampler, TextureView, ComputePipeline };

    Kind kind = Kind::Buffer;
    std::atomic<uint32_t> recordingReferences{0};
    std::atomic<bool> closeRequested{false};
    std::weak_ptr<void> packetContentOwner;
    std::mutex sampledContentMutex;
    uint32_t sampledSubresourceCount = 0;
    TextureWriteRecording textureWriteRecording;
    std::unique_ptr<TextureWriteRecordingEntry> additionalTextureWriteRecordings;
    std::atomic<std::shared_ptr<void>> sampledContentOwner;
    ComPtr<ID3D12Resource> resource;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
    void* mappedPtr = nullptr;
    int usage = 0;
    long long size = 0;
    int descSlot = -1;
    Dx12Object* sourceTexture = nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    bool hasSrvDesc = false;
    uint32_t texturePlanes = 0;
    ComPtr<ID3D12RootSignature> computeRootSignature;
    ComPtr<ID3D12PipelineState> computePipelineState;
    bool terrainTileBinning = false;
};

mcdr::Result<void> ensureDevice();
void destroyDevice();
mcdr::Result<void> destroyDeviceChecked();
int32_t invalidateReconstructionInputs(void* context);
std::shared_ptr<void> retainObjectLease(void* object);
void* recordingContextForCommandList(void* commandList);
void retainObjectForRecording(void* context, void* object);

const char* featureLevelName(D3D_FEATURE_LEVEL level);
constexpr D3D_SHADER_MODEL kShaderModelCeiling = static_cast<D3D_SHADER_MODEL>(0x6a);
const char* shaderModelName(D3D_SHADER_MODEL model);
const char* shaderModelProfileSuffix(D3D_SHADER_MODEL model);
D3D_SHADER_MODEL effectiveShaderModel(const DeviceContext::Capabilities& caps);
void queryDeviceCapabilities(ID3D12Device* device, DeviceContext::Capabilities& caps);
std::string capabilitiesText(const DeviceContext::Capabilities& caps);
std::string capabilitiesJson(const DeviceContext::Capabilities& caps);

void probeDxc(DeviceContext::Capabilities& caps);

mcdr::Result<void> compileHlslToDxil(const std::string& source, const std::string& entry,
                                    const std::string& profile, std::vector<uint8_t>& outDxil);
mcdr::Result<void> validateCapturePixelShader(const void* bytecode, uint32_t bytes);
mcdr::Result<void> requireCaptureBarycentrics();

mcdr::Result<void*> createComputePipeline(const void* dxil, int32_t dxilSize);

mcdr::Result<void*> createTerrainTileBinningPipeline(const void* dxil, int32_t dxilSize);

mcdr::Result<void*> createComputePipelineShadingRate(const void* dxil, int32_t dxilSize);
int dispatchComputeShadingRate(CommandContext* ctx, Dx12Object* pipeline, Dx12Object* srvDepth,
                               Dx12Object* srvMotion, Dx12Object* srvPrevious, Dx12Object* image,
                               const int32_t* constants, int32_t constantCount, int32_t x, int32_t y,
                               int32_t z);
mcdr::Result<void*> createShadingRateImage(int32_t width, int32_t height);
int setShadingRateImage(CommandContext* ctx, Dx12Object* image);
int dispatchCompute(CommandContext* ctx, Dx12Object* pipeline, Dx12Object* uavTexture,
                    const int32_t* constants, int32_t constantCount, int32_t x, int32_t y, int32_t z);

DeviceContext& deviceContext();

void flushPendingDeletes(bool force = false);

mcdr::Result<void> writeCBVToDrawHeap(const UINT& slot, const Dx12Object*& buf, const long long& offset,
                                     const long long& size);
mcdr::Result<void> writeTextureSRVToDrawHeap(const UINT& slot, const Dx12Object*& view);
mcdr::Result<void> writeBufferSRVToDrawHeap(UINT slot, Dx12Object* buf, long long offset, long long length,
                                           int texelFormat);

uintptr_t getDeviceHandle();
uintptr_t getQueueHandle();

uintptr_t createHiddenWindow(int width, int height);
void destroyHiddenWindow(uintptr_t hwnd);

mcdr::Result<void*> createTexture(int usage, int format, int width, int height, int depthOrLayers,
                                  int mipLevels);
mcdr::Result<void*> createBuffer(int usage, long long size);
mcdr::Result<void*> createSampler(int addressU, int addressV, int minFilter, int magFilter,
                                  int maxAnisotropy, float maxLod);
mcdr::Result<void*> createSamplerWithMipBias(int addressU, int addressV, int minFilter, int magFilter,
                                             int maxAnisotropy, float maxLod, float mipLodBias);
mcdr::Result<void*> createTextureView(Dx12Object* texture, int baseMipLevel, int mipLevels);
void destroyObject(Dx12Object* obj);

mcdr::Result<void*> mapBuffer(Dx12Object* buffer, long long offset, long long length, bool read, bool write);
void unmapBuffer(Dx12Object* buffer);

struct OitLaneOperationProvenance {
    uint32_t kind = 0;
    uint32_t projectionBinding = UINT32_MAX;
    uint64_t programGeneration = 0;
};

struct Dx12Pipeline {
    struct TileRoute {
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pso;
        uint64_t programGeneration = 0;
        UINT drawParam = UINT_MAX;
        UINT batchParam = UINT_MAX;
        UINT pushConstantParam = UINT_MAX;
        uint32_t bindingCount = 0;
        uint32_t tileStage = UINT32_MAX;
        UINT laneDescriptorParam = UINT_MAX;
        std::array<uint32_t, 5> laneRegisters{};
        std::array<uint32_t, 5> laneBindingSlots{};
        ComPtr<ID3D12DescriptorHeap> lanePlaceholderHeap;
        Dx12Object lanePlaceholder;
        std::array<UINT, 16> vertexStepRates{};
        bool instanceTileBatch = false;
    };
    std::atomic<uint32_t> lifetimeReferences{1};
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> withDepth;
    ComPtr<ID3D12PipelineState> withoutDepth;
    int topology = 4;
    std::unordered_map<int, UINT> vertexStrides;
    bool isMesh = false;
    bool rasterMotion = false;
    bool rasterMotionWritesWorldColor = true;
    UINT colorTargetCount = UINT_MAX;
    std::vector<DXGI_FORMAT> colorTargetFormats;
    DXGI_FORMAT depthTargetFormat = DXGI_FORMAT_UNKNOWN;
    bool depthWrite = false;
    UINT segmentParam = 0;
    UINT geometryParam = 0;
    bool meshVisibleWork = false;
    UINT pushConstantBytes = 0;
    UINT pushConstantParam = UINT_MAX;
    UINT captureParam = UINT_MAX;
    uint64_t captureHash = 0;
    uint64_t captureGeneration = 0;
    uint64_t shaderGeneration = 0;
    std::shared_ptr<void> captureBudget;
    std::shared_ptr<const void> normalizationProvenance;
    OitLaneOperationProvenance oitLaneOperation{};
    std::shared_ptr<TileRoute> tileRoute;
    std::vector<uint8_t> graphicsBindingTypes;
    std::vector<uint8_t> graphicsBindingRegisters;
    bool graphicsBindingLayoutKnown = false;
    std::shared_ptr<void> terrainTileRoute;
};

struct DrawBinding {
    uint8_t type = 0;
    Dx12Object* buffer = nullptr;
    long long offset = 0;
    long long length = 0;
    int texelFormat = 0;
    Dx12Object* view = nullptr;
};

struct GraphicsResourceRange {
    Dx12Object* object = nullptr;
    uint32_t firstSubresource = 0;
    uint32_t subresourceCount = 0;
    uint32_t state = 0;
    uint32_t access = 1;
    std::shared_ptr<void> owner;
    bool sampledWritePrepared = false;
};

struct GraphicsCaptureBinding {
    uint64_t contractHash = 0;
    uint64_t generation = 0;
    uint32_t remainingDraws = 1;
    D3D12_GPU_VIRTUAL_ADDRESS constants = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptors{};
    std::vector<GraphicsResourceRange> resources;
    std::shared_ptr<void> lease;
};

struct GraphicsDrawState {
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    bool viewportValid = false;
    bool scissorValid = false;
    std::array<uint8_t, 128> pushConstants{};
    uint32_t pushConstantBytes = 0;
    ID3D12RootSignature* pushRoot = nullptr;
    bool pushConstantsValid = false;
};
int bindGraphicsCommandObserver(void* context, const mcdr::graphics_recording::Observer& observer);
void unbindGraphicsCommandObserver(void* context);
int prepareGraphicsCommandCapture(void* context, const mcdr::graphics_recording::Command& command,
    std::shared_ptr<const void>* output);
int prepareGraphicsCommandCaptureWithInputs(void* context, const mcdr::graphics_recording::Command& command,
    const DrawBinding* bindings, uint32_t bindingCount, D3D12_GPU_DESCRIPTOR_HANDLE geometry,
    D3D12_GPU_VIRTUAL_ADDRESS globals, const mcdr::graphics_recording::AdapterFacts& adapter,
    std::shared_ptr<const void>* output);
int confirmGraphicsCommandCapture(void* context, const std::shared_ptr<const void>& packet);
bool graphicsCommandPacketMatchesObserver(void* context, const std::shared_ptr<const void>& packet,
    const mcdr::graphics_recording::Observer& observer, uint32_t commandFamily);
int completeGraphicsCommandRecording(void* context, mcdr::graphics_recording::Completion completion);

enum class PacketContentStatus : uint32_t {
    Unfrozen, Frozen, UnsupportedContent, StaleRecording
};
struct PacketContentRange {
    ComPtr<ID3D12Resource> resource;
    uint32_t firstSubresource = 0, subresourceCount = 1;
    uint64_t byteOffset = 0, byteLength = 0;
    bool cpuWritable = false;
    uint32_t initialState = D3D12_RESOURCE_STATE_COMMON;
    Dx12Object* object = nullptr;
    std::shared_ptr<void> immutableOwner;
    ComPtr<ID3D12Resource> sourceResource;
    uint64_t sourceByteOffset = 0;
    Dx12Object* view = nullptr;
};
struct PacketContentTextureView {
    Dx12Object* source = nullptr;
    Dx12Object* frozen = nullptr;
};
struct PacketContentLease {
    PacketContentStatus status = PacketContentStatus::Unfrozen;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::shared_ptr<const void> recordingIdentity;
    ComPtr<ID3D12Fence> fence;
    uint64_t submission = 0;
    std::vector<PacketContentRange> reads;
    std::vector<PacketContentRange> frozenReads;
    std::shared_ptr<void> frozenOwner;
    std::vector<PacketContentTextureView> frozenViews;
};
std::shared_ptr<const void> retainPacketContent(void* context, void* ranges, uint32_t count,
    uint64_t budgetLimit = UINT64_MAX);
PacketContentStatus packetContentStatus(void* context, const std::shared_ptr<const void>& content);
struct GraphicsCommandContentView {
    PacketContentStatus status = PacketContentStatus::Unfrozen;
    const DrawBinding* bindings = nullptr;
    uint32_t bindingCount = 0;
};
mcdr::Result<GraphicsCommandContentView> resolveGraphicsCommandContent(
    void* context, const std::shared_ptr<const void>& packet);

enum class GraphicsPacketInspectionStatus : uint32_t {
    Ready, StaleRecording, Unsupported, RecordingFailed
};
struct GraphicsPacketInspection {
    GraphicsPacketInspectionStatus status = GraphicsPacketInspectionStatus::Unsupported;
    mcdr::graphics_recording::ContentState content = mcdr::graphics_recording::ContentState::Unfrozen;
    mcdr::graphics_recording::AdapterFacts adapter;
    bool confirmed = false;
    bool depthWrite = false;
    bool depthTargetBound = false;
    bool routeBound = false;
    uint64_t opaqueSeedProgramGeneration = 0;
    uint32_t tileStage = UINT32_MAX;
    void* depthObject = nullptr;
    uintptr_t pipelineIdentity = 0;
    uint64_t shaderGeneration = 0;
    uint32_t commandFamily = 0;
    OitLaneOperationProvenance oitLaneOperation{};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    bool fullScreenTriangle = false;
    std::shared_ptr<const void> laneProjection;
    D3D12_CPU_DESCRIPTOR_HANDLE depthTarget{};
    ComPtr<ID3D12PipelineState> pso;
};
GraphicsPacketInspection inspectGraphicsCommandPacket(void* context,
    const std::shared_ptr<const void>& packet);
void cancelOpaqueSeedPacketCapture(const std::shared_ptr<const void>& packet);
mcdr::Result<void> recordTileGraphicsPacket(void* context, const std::shared_ptr<const void>& packet,
    std::span<const D3D12_VIEWPORT> viewports, std::span<const D3D12_RECT> scissors,
    D3D12_GPU_VIRTUAL_ADDRESS batchConstants, std::span<Dx12Object* const> laneReads = {});
enum class GraphicsCommandTextureBinding { Sampled, ColorTarget, DepthTarget, DepthAttachment };
bool matchesGraphicsCommandTexture(const std::shared_ptr<const void>& packet, const void* texture,
    uint32_t subresource, GraphicsCommandTextureBinding binding, uint32_t colorIndex = UINT32_MAX);

enum class SampledTextureContentStatus : uint32_t {
    Ready, UnsupportedView, OrderingUnavailable, BudgetExceeded, StaleRecording, RecordingFailed
};
struct SampledTextureContentCapture {
    SampledTextureContentStatus status = SampledTextureContentStatus::UnsupportedView;
    std::shared_ptr<const void> lease;
    std::shared_ptr<const void> ownerIdentity;
    uint64_t ownerGeneration = 0;
    uint64_t contentVersion = 0;
    uint32_t firstMip = 0, mipCount = 0, firstLayer = 0, layerCount = 0;
};
int captureSampledTextureContentV1(void* context, void* view, uint64_t totalBudgetBytes,
    uint64_t* ownerIdentity, uint64_t* ownerGeneration, uint64_t* contentVersion,
    uint32_t* firstMip, uint32_t* mipCount, uint32_t* firstLayer, uint32_t* layerCount);
struct SampledTextureContentResolution {
    SampledTextureContentStatus status = SampledTextureContentStatus::StaleRecording;
    void* view = nullptr;
};
SampledTextureContentCapture captureSampledTextureContent(void* context, void* view, uint64_t totalBudgetBytes);
void cancelSampledTextureContent(const std::shared_ptr<const void>& lease);
SampledTextureContentResolution resolveSampledTextureContent(void* context, const std::shared_ptr<const void>& lease);
SampledTextureContentStatus prepareSampledTextureWrite(void* context, void* texture, uint32_t firstSubresource, uint32_t subresourceCount);
SampledTextureContentStatus prepareSampledRenderPassWrites(void* context);
struct SampledTextureCpuWrite {
    SampledTextureContentStatus status = SampledTextureContentStatus::UnsupportedView;
    std::shared_ptr<void> lease;
};
SampledTextureCpuWrite prepareSampledTextureCpuWrite(void* texture, uint32_t firstSubresource, uint32_t subresourceCount);

struct GraphicsCaptureShadingRate {
    ComPtr<ID3D12GraphicsCommandList5> commandList;
    ID3D12GraphicsCommandList* recording = nullptr;
    Dx12Object* image = nullptr;
    D3D12_SHADING_RATE rate = D3D12_SHADING_RATE_1X1;
    D3D12_SHADING_RATE_COMBINER combiners[2]{};
};

struct RtvDsvKey {
    ID3D12Resource* resource;
    UINT mip;
    bool operator==(const RtvDsvKey& o) const { return resource == o.resource && mip == o.mip; }
};
struct RtvDsvKeyHash {
    size_t operator()(const RtvDsvKey& k) const {
        return std::hash<void*>()(reinterpret_cast<void*>(k.resource))
               ^ (std::hash<UINT>()(k.mip) * static_cast<size_t>(0x9E3779B97F4A7C15ULL));
    }
};

struct FgPresentTicket {
    uint64_t serial = 0;
    uint64_t realFrameId = 0;
    uint64_t providerGeneration = 0;
    void* context = nullptr;
    UINT64 fenceValue = 0;
    bool operator==(const FgPresentTicket&) const = default;
};

struct DisplayHistoryPublication {
    uint32_t flags = 0;
    uint32_t outputFrameCount = 0;
};

struct SurfacePresentResult {
    uint32_t outcome = MCDR_PRESENT_NOT_CALLED;
    int32_t dxgiStatus = 0;
    uint32_t flags = 0;
    uint32_t outputFrameCount = 0;
};

enum class DisplayPresentPhase { Empty, Awaiting, Terminal, Consumed };

struct DisplayPresentResult {
    DisplayPresentPhase phase = DisplayPresentPhase::Empty;
    mcdr_present_receipt_v1 receipt{};
};

struct CommandContext {
    ComPtr<ID3D12CommandAllocator> allocators[3];
    ComPtr<ID3D12GraphicsCommandList> commandLists[3];
    ComPtr<ID3D12GraphicsCommandList10> workGraphCommandLists[3];
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;
    int listOpen = 0;
    int inRenderPass = 0;

    UINT currentListSlot = 0;
    UINT64 slotFence[3] = {0, 0, 0};
    std::atomic<bool> submissionFailed{false};
    std::atomic<bool> recordingFailed{false};
    std::vector<std::shared_ptr<void>> recordingLeases[3];
    std::shared_ptr<const void> recordingIdentity;
    std::shared_ptr<std::atomic<bool>> recordingDiscarded;
    std::unordered_map<Dx12Pipeline*, bool> retainedGraphicsPipelines;
    std::unordered_map<Dx12Object*, bool> retainedRecordingObjects;
    std::shared_ptr<void> vanillaFrameState;
    uint64_t frameProgramGeneration = 0;
    std::shared_ptr<void> frameprepState;
    std::shared_ptr<void> displayFrameState;
    DisplayPresentResult displayPresentResult;
    std::shared_ptr<void> oitFrameState;
    std::shared_ptr<void> oitHistoryState;
    std::shared_ptr<void> oitPrograms;
    GraphicsCaptureBinding graphicsCapture;
    D3D12_SHADING_RATE recordedShadingRate = D3D12_SHADING_RATE_1X1;
    D3D12_SHADING_RATE_COMBINER recordedShadingCombiners[2]{};
    Dx12Object* shadingRateImageBound = nullptr;
    GraphicsCaptureShadingRate captureShadingRate;
    std::shared_ptr<void> controlIdentity;
    bool graphicsHasDepth = false;
    ComPtr<ID3D12DescriptorHeap> resourceDrawHeap;
    UINT resourceDrawCapacity = 0;
    ComPtr<ID3D12DescriptorHeap> resourceComputeHeap;
    UINT resourceComputeCapacity = 0;
    UINT resourceComputeNext = 0;
    UINT lastSrvCbvCount = 0;
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12GraphicsCommandList10* workGraphCommandList = nullptr;

    std::vector<Dx12Object*> activeColorTargets;
    std::vector<bool> activeColorTargetsTouched;
    bool colorTargetsWritten = false;
    Dx12Object* activeDepthTarget = nullptr;
    std::vector<GraphicsResourceRange> graphicsInputRanges;
    std::vector<GraphicsResourceRange> activeAttachmentRanges;
    std::vector<GraphicsResourceRange> graphicsBindingReadScratch;
    std::vector<GraphicsResourceRange> graphicsBindingReadCache;
    bool graphicsBindingReadCacheValid = false;
    std::vector<GraphicsResourceRange> graphicsBindingPendingScratch;
    std::vector<GraphicsResourceRange> graphicsCollectScratch;
    GraphicsDrawState graphicsDrawState;
    mcdr::graphics_recording::Observer graphicsCommandObserver;
    std::vector<DrawBinding> graphicsBindings;
    bool graphicsBindingsValid = false;
    ID3D12RootSignature* graphicsBindingRoot = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE graphicsBindingTable{};
    Dx12Object* graphicsVertexBuffers[16]{};
    Dx12Object* graphicsIndexBuffer = nullptr;
    D3D12_VERTEX_BUFFER_VIEW graphicsVertexViews[16]{};
    std::array<bool, 16> graphicsVertexSlices{};
    D3D12_INDEX_BUFFER_VIEW graphicsIndexView{};
    Dx12Object* rasterMotionWorldColor = nullptr;
    Dx12Object* rasterMotionWorldDepth = nullptr;
    Dx12Object* rasterMotionTarget = nullptr;
    std::vector<std::shared_ptr<void>> rasterMotionLeases;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> activePassRtvs;
    D3D12_CPU_DESCRIPTOR_HANDLE activePassDsv{};
    D3D12_CPU_DESCRIPTOR_HANDLE activeMotionRtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE additionalColorRtv{};
    Dx12Object* additionalColorTarget = nullptr;
    Dx12Object* activeMotionTarget = nullptr;
    bool rasterMotionSelected = false;
    UINT activePassBoundColorCount = UINT_MAX;

    void* vanillaDepthSeedState = nullptr;
    void (*vanillaDepthSeedClear)(void*, CommandContext*, Dx12Object*, UINT, double,
                                  const D3D12_RECT*, D3D12_CPU_DESCRIPTOR_HANDLE) = nullptr;
    void (*vanillaDepthSeedDraw)(void*, CommandContext*, Dx12Object*, Dx12Pipeline*, uint32_t,
                                 uint64_t, const std::shared_ptr<const void>&) = nullptr;
    uint64_t vanillaDepthSeedOrdinal = 0;

    UINT drawHeapSlotBase = 0;
    UINT nextDrawSlot = 0;

    UINT lastSrvSlotOffset = 0;
    UINT lastSrvCount = 0;
    std::vector<std::pair<Dx12Object*, SIZE_T>> lastSrvViews;

    UINT64 lastSubmitQueueFence = 0;

    UINT64 prevFenceValue = 0;

    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> resourceState;
    std::unordered_map<ID3D12Resource*, std::unordered_map<UINT, D3D12_RESOURCE_STATES>> subresourceState;

    unsigned int frameViewGeneration = 0;

    
    Dx12Pipeline* currentPipeline = nullptr;

    ID3D12PipelineState* cachedPso = nullptr;

    std::unordered_map<RtvDsvKey, D3D12_CPU_DESCRIPTOR_HANDLE, RtvDsvKeyHash> rtvCache;
    std::unordered_map<RtvDsvKey, D3D12_CPU_DESCRIPTOR_HANDLE, RtvDsvKeyHash> dsvCache;

    ComPtr<ID3D12CommandAllocator>& currentAllocator() { return allocators[currentListSlot]; }
};

mcdr::Result<void*> createCommandEncoder();
void destroyCommandEncoder(CommandContext* ctx);
int destroyCommandEncoderChecked(CommandContext* ctx);
int commandRecordingStatus(void* context) noexcept;
enum class CommandContextControlStatus : int {
    Ready,
    Invalid,
    Busy,
    SubmissionUnknown,
};
int validateCommandContextControl(void* context) noexcept;

mcdr::Result<void> beginCommandList(CommandContext* ctx);
mcdr::Result<void> beginCommandListWithWait(CommandContext* ctx, UINT64 waitForValue);
mcdr::Result<void> endCommandList(CommandContext* ctx);

void restoreDrawHeap(CommandContext* ctx);
mcdr::Result<UINT64> submitCommandList(CommandContext* ctx);
int submitCommandListV1(CommandContext* ctx, uint64_t* submittedFence);
int settleCommandSubmissionV1(CommandContext* ctx, uint64_t timeoutMs, uint64_t* submittedFence);
int submitDisplayGuidesAndResume(void* opaqueContext, uint64_t* guideFence);
mcdr::Result<void> abortDisplayFrame(void* opaqueContext);
mcdr::Result<void> waitForFenceValue(CommandContext* ctx, UINT64 value, UINT64 timeoutNs);
UINT64 currentFenceValue(CommandContext* ctx);
mcdr::Result<void> waitForContextFenceValue(CommandContext* ctx, UINT64 value, UINT64 timeoutNs);

bool initAsyncRenderer(UINT workerCount);
bool destroyAsyncRenderer();
bool waitForRenderThreadSubmit();
bool waitForPendingPresents(unsigned timeoutMs = 100);

void logDeviceStatus(const char* tag);
bool deviceRemoved();
mcdr::Result<void> asyncRenderBeginFrame(CommandContext* ctx);
mcdr::Result<void> asyncRenderWaitComplete(CommandContext* ctx, UINT64 timeoutMs);
bool asyncRenderIsRecordingReady(CommandContext* ctx);
bool asyncRenderWaitRecordingReady(CommandContext* ctx, UINT64 timeoutMs);
void asyncSendCommandsReady(CommandContext* ctx);
void clearAsyncRenderCtx();
bool isListOpen(CommandContext* ctx);

UINT currentDrawFrameSlot(CommandContext* ctx);

UINT64 currentQueueFenceValue();
mcdr::Result<void> waitForQueueFenceValue(UINT64 value, UINT64 timeoutNs);

mcdr::Result<long long> getTimestampNow(CommandContext* ctx);

mcdr::Result<void> copyBufferToBuffer(CommandContext* ctx, Dx12Object* src, long long srcOffset, Dx12Object* dst,
                                     long long dstOffset, long long size);

void transitionTextureTo(CommandContext* ctx, Dx12Object* tex, D3D12_RESOURCE_STATES to);

void transitionBufferTo(CommandContext* ctx, Dx12Object* buf, D3D12_RESOURCE_STATES to);

void transitionTo(CommandContext* ctx, ID3D12Resource* res, D3D12_RESOURCE_STATES fromInitial,
                  D3D12_RESOURCE_STATES to);
mcdr::Result<void> transitionResourceRange(void* context, void* resource, uint32_t first,
                                          uint32_t count, uint32_t initialState, uint32_t state);
mcdr::Result<void> adoptResourceRangeState(void* context, void* resource, uint32_t first,
                                          uint32_t count, uint32_t initialState, uint32_t state);
uint32_t resourceSubresourceState(void* context, void* resource, uint32_t subresource,
                                 uint32_t initialState);

mcdr::Result<void> clearColorTexture(CommandContext* ctx, Dx12Object* tex, float r, float g, float b, float a);
mcdr::Result<void> clearDepthTexture(CommandContext* ctx, Dx12Object* tex, double depth);

mcdr::Result<void> clearColorTextureRegion(CommandContext* ctx, Dx12Object* tex, float r, float g, float b,
                                          float a, int x, int y, int w, int h);
mcdr::Result<void> clearDepthTextureRegion(CommandContext* ctx, Dx12Object* tex, double depth, int x, int y,
                                          int w, int h);
mcdr::Result<void> clearStencilTexture(CommandContext* ctx, Dx12Object* tex, int value);

mcdr::Result<void> copyBufferToTexture(CommandContext* ctx, Dx12Object* srcBuf, long long srcOffset,
                                      int srcWidth, int srcHeight, Dx12Object* dstTex, int mip, int layer,
                                      int dstX, int dstY, int w, int h);

mcdr::Result<void> copyTextureToBuffer(CommandContext* ctx, Dx12Object* srcTex, int mip, int layer, int srcX,
                                      int srcY, int w, int h, Dx12Object* dstBuf, long long dstOffset);

mcdr::Result<void> copyTextureToTexture(CommandContext* ctx, Dx12Object* srcTex, Dx12Object* dstTex, int mip,
                                       int layer, int srcX, int srcY, int dstX, int dstY, int w, int h);

unsigned long long getTimestampFrequency();

mcdr::Result<void> beginRenderPass(CommandContext* ctx, Dx12Object* const* colorViews, int colorCount,
                                  const int* colorMips, const int* colorClearFlags, const float* clearColors,
                                  Dx12Object* depthView, int depthMip, int depthClearFlag, double depthClearValue,
                                  int x, int y, int w, int h);
mcdr::Result<void> endRenderPass(CommandContext* ctx);
mcdr::Result<void> setRasterMotionTarget(void* context, void* worldColor, void* worldDepth, void* motion);
mcdr::Result<void> setAdditionalColorTarget(void* context, void* target);

struct QueryPool {
    ComPtr<ID3D12QueryHeap> heap;
    int size = 0;
};

mcdr::Result<void*> createQueryPool(int size);
void destroyQueryPool(QueryPool* pool);
mcdr::Result<void> writeTimestampToPool(CommandContext* ctx, QueryPool* pool, int index);
mcdr::Result<void> readQueryValue(QueryPool* pool, int index, long long& out);
mcdr::Result<void> readQueryValues(QueryPool* pool, int start, int count, long long* out);

DXGI_FORMAT toDxgiFormat(int gpuFormat);
DXGI_FORMAT toDxgiVertexFormat(int gpuFormat);

struct PipelineDesc {
    struct OpaqueSeedShaders {
        std::string_view vertex;
        std::string_view fragment;
        uint64_t programGeneration = 0;
        uint32_t tileStage = UINT32_MAX;
        std::array<uint32_t, 5> laneRegisters{};
    } opaqueSeed;
    uint32_t pushConstantBytes = 0;
    uint64_t captureHash = 0;
    uint64_t captureGeneration = 0;
    std::shared_ptr<const void> normalizationProvenance;
    bool rasterMotion = false;
    struct ColorTarget {
        int format = 0;
        uint8_t writeMask = 0;
        bool blendEnabled = false;
        uint8_t srcColor = 0, dstColor = 0, colorOp = 0;
        uint8_t srcAlpha = 0, dstAlpha = 0, alphaOp = 0;
    };
    struct InputElement {
        int location = 0, binding = 0, format = 0;
        int offset = 0, stride = 0, stepRate = 0;
        std::string semanticName;
    };
    struct Binding {
        uint8_t type = 0;
        uint8_t reg = 0;
    };

    std::vector<uint8_t> vsBytes;
    std::vector<uint8_t> psBytes;
    int colorCount = 0;
    std::vector<ColorTarget> colorTargets;
    bool hasDepth = false;
    int depthFormat = 0;
    bool depthWrite = false;
    int depthCompareOp = 0;
    int topology = 0;
    bool cullEnabled = false;
    int polygonMode = 0;
    std::vector<InputElement> inputElements;
    std::vector<Binding> bindings;
};

mcdr::Result<void*> createGraphicsPipeline(const PipelineDesc& desc);
uint64_t allocateGraphicsPipelineGeneration();
mcdr::Result<void> pushGraphicsConstants(void* context, const void* data, uint32_t byteSize);
void destroyPipeline(Dx12Pipeline* pipeline);

struct BlitPipeline {
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12PipelineState> psoHdr;
    ComPtr<ID3D12Resource> vertBuf;
    ComPtr<ID3D12Resource> idxBuf;
    D3D12_VERTEX_BUFFER_VIEW vbView{};
    D3D12_INDEX_BUFFER_VIEW ibView{};
};

mcdr::Result<void> initBlitPipeline();
const BlitPipeline* getBlitPipeline();

struct SharpenPipeline {
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> vertBuf;
    ComPtr<ID3D12Resource> idxBuf;
    D3D12_VERTEX_BUFFER_VIEW vbView{};
    D3D12_INDEX_BUFFER_VIEW ibView{};
};
mcdr::Result<void> initSharpenPipeline();
const SharpenPipeline* getSharpenPipeline();
mcdr::Result<void> blitBindSourceTexture(CommandContext* ctx, Dx12Object* srcTex, ID3D12GraphicsCommandList* cmd);

int applySmaaToTexture(CommandContext* ctx, Dx12Object* tex);

int applyTaaToTexture(CommandContext* ctx, Dx12Object* scene, Dx12Object* motion, float jitterX,
                      float jitterY, float prevJitterX, float prevJitterY, float blend, int reset,
                      float sharpenAmount);

int applyDlssToTexture(CommandContext* ctx, const mcdr_dlss_input& in);

int queryDlss(char* out, int cap);

enum class SrBackendId : int32_t {
    None = 0,
    Dlss = 1,
    Fsr = 2,
    XeSS = 3,
};

class IUpscaleProvider {
public:
    IUpscaleProvider() = default;
    IUpscaleProvider(const IUpscaleProvider&) = delete;
    IUpscaleProvider& operator=(const IUpscaleProvider&) = delete;
    virtual ~IUpscaleProvider() = default;

    virtual SrBackendId backend() const = 0;
    virtual void fillCaps(mcdr_sr_caps& out) const = 0;
    virtual bool initialize(CommandContext* ctx, const mcdr_sr_dispatch& seed) = 0;
    virtual void onResize(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                          int32_t displayHeight) = 0;
    virtual bool evaluate(CommandContext* ctx, const mcdr_sr_dispatch& desc) = 0;
    virtual std::string failureText() const { return {}; }
    virtual void shutdown() = 0;
};

int32_t srPickBackend(int32_t backendId);
int32_t srSelectedBackend(void);
int32_t srActiveBackend(void);
void srFillCaps(int32_t backendId, mcdr_sr_caps& out);
int32_t srCreateProvider(CommandContext* ctx, int32_t backendId, int32_t maxRenderWidth,
                         int32_t maxRenderHeight, int32_t displayWidth, int32_t displayHeight);
int32_t srResizeProvider(CommandContext* ctx, int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                         int32_t displayHeight);
int32_t srPrepareProvider(CommandContext* ctx, const mcdr_sr_dispatch& description,
    int32_t dlssQuality, int32_t dlssPreset, uint64_t* token);
int32_t srCommitPreparedProvider(uint64_t token);
int32_t srAbortPreparedProvider(uint64_t token);
int32_t srEvaluateFrame(CommandContext* ctx, const mcdr_sr_dispatch& desc);
std::string srLastFailureText();
int32_t srJitterPhaseCount(int32_t backendId, int32_t renderWidth, int32_t displayWidth);
bool srReleaseProvider(void);
std::string srStatusText(void);

std::unique_ptr<IUpscaleProvider> makeFsrProvider();
void fillFsrCaps(mcdr_sr_caps& out);
bool fsrRuntimeAvailable();
std::unique_ptr<IUpscaleProvider> makeXessProvider();
void fillXessCaps(mcdr_sr_caps& out);
bool xessRuntimeAvailable();

enum class FgBackendId : int32_t {
    None = MCDR_FG_BACKEND_NONE,
    DlssG = MCDR_FG_BACKEND_DLSSG,
    FsrFg = MCDR_FG_BACKEND_FSRFG,
    XessFg = MCDR_FG_BACKEND_XESSFG,
};

struct FgFrameInputs {
    ID3D12GraphicsCommandList* cmdList = nullptr;
    void* pColorOutput = nullptr;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    void* pDepth = nullptr;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_READ;
    void* pMotionVectors = nullptr;
    void* pUi = nullptr;
    D3D12_RESOURCE_STATES motionVectorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES uiState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    int32_t renderWidth = 0;
    int32_t renderHeight = 0;
    int32_t displayWidth = 0;
    int32_t displayHeight = 0;
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraVerticalFov = 0.0f;
    float frameTimeDeltaMs = 0.0f;
    float viewSpaceToMeters = 0.0f;
    float cameraPosition[3] = {};
    float cameraRight[3] = {};
    float cameraUp[3] = {};
    float cameraForward[3] = {};
    float currentViewProjection[16] = {};
    float previousViewProjection[16] = {};
    float projection[16] = {};
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float mvScaleX = 0.0f;
    float mvScaleY = 0.0f;
    uint64_t realFrameId = 0;
    bool motionVectorsDilated = false;
    std::shared_ptr<void> resourceLease;
};

enum class FgInputAcceptance : uint32_t { Skipped, Accepted, Failed };

class IFrameGenProvider {
public:
    IFrameGenProvider() = default;
    IFrameGenProvider(const IFrameGenProvider&) = delete;
    IFrameGenProvider& operator=(const IFrameGenProvider&) = delete;
    virtual ~IFrameGenProvider() = default;

    virtual FgBackendId backend() const = 0;
    virtual void fillCaps(mcdr_fg_caps& out) const = 0;
    virtual bool setMode(const mcdr_fg_dispatch& desc) = 0;
    virtual void frameStart() {}
    virtual bool waitForPreviousFrame() { return true; }
    virtual void discardFrameInputs() {}
    virtual void beforePresent() = 0;
    virtual void afterPresent() = 0;
    virtual int32_t presented() const = 0;
    virtual void onResize(int32_t displayWidth, int32_t displayHeight) = 0;
    virtual void shutdown() = 0;

    virtual int32_t wrapSwapChain(ID3D12Device* device, ID3D12CommandQueue* queue,
                                  IDXGISwapChain* appChain, int32_t width, int32_t height,
                                  DXGI_FORMAT format, IDXGISwapChain** outPresentChain) {
        static_cast<void>(device);
        static_cast<void>(queue);
        static_cast<void>(appChain);
        static_cast<void>(width);
        static_cast<void>(height);
        static_cast<void>(format);
        static_cast<void>(outPresentChain);
        return 0;
    }
    virtual void unwrapSwapChain() {}
    virtual bool tryUnwrapSwapChain() { unwrapSwapChain(); return true; }

    virtual void setUiTexture(void* object, std::shared_ptr<void> lease = {}) {
        static_cast<void>(object);
        static_cast<void>(lease);
    }
    virtual void setUiComposition(int32_t enabled) { static_cast<void>(enabled); }
    virtual bool uiCompositionReady() { return false; }
    virtual void setFrameConstants(const mcdr_fg_constants& constants) { (void)constants; }

    virtual FgInputAcceptance tagFrameInputs(const FgFrameInputs& inputs) {
        static_cast<void>(inputs);
        return FgInputAcceptance::Skipped;
    }
    virtual bool historyAcceptedAfterPresent() const { return true; }
};

int32_t fgPickBackend(int32_t backendId);
void fgFillCaps(int32_t backendId, mcdr_fg_caps& out);
int32_t fgEnsureProvider(void);
int32_t fgSetMode(const mcdr_fg_dispatch& desc);
int32_t fgPrepareMode(const mcdr_fg_dispatch& description, uint64_t* token);
int32_t fgCommitPreparedConfiguration(uint64_t token, uint64_t srToken, int32_t antiAliasing);
int32_t fgAbortPreparedMode(uint64_t token);
int beginDisplayFrameRecording(void* context);
mcdr::Result<int32_t> settleDisplayFrame(void* context, uint64_t timeoutMs, void* receipt, uint32_t byteCount);
mcdr::Result<void> beginDisplayPresentReceipt(const FgPresentTicket& ticket);
void attachDisplayPresentIdentity(void* context, const mcdr_frame_identity& identity, uint64_t historyGeneration);
void queueDisplayPresentReceipt(const FgPresentTicket& ticket, uint32_t submissionState);
void failDisplayPresentSubmission(void* context);
void cancelDisplayPresentReceipt(void* context);
void completeDisplayPresentReceipt(const FgPresentTicket& ticket, SurfacePresentResult result,
    uint64_t presentQueueFenceValue, uint32_t retirementState);
int fgAttachWorldFrame(void* context, const mcdr_vanilla_frame_input& frame);
int fgMarkGuiReady(void* context, const mcdr_frame_identity& identity);
bool fgDisplayFrameQueued(void* context);
bool fgDisplayFrameStarted();
FgPresentTicket fgDisplayFrameTicket(void* context);
FgPresentTicket fgCurrentDisplayFrameTicket();
bool fgDisplayFrameCurrent(const FgPresentTicket& ticket);
mcdr::Result<FgPresentTicket> fgQueueDisplayFrame(CommandContext* context, UINT64 fenceValue);
void fgAbortDisplayFrame(void* context);
void fgAbortDisplayFrame(const FgPresentTicket& ticket);
void fgBeforePresent(const FgPresentTicket& ticket);
DisplayHistoryPublication fgAfterPresent(const FgPresentTicket& ticket, int32_t presentSucceeded);
int evaluatePreparedReconstruction(void* context, const void* frameCopy,
    const void* preparedCopy, const void* requestCopy);
void invalidateFrameGenerationHistory(uint32_t reason);
void invalidatePreparedReconstructionHistory();
int32_t fgPresented(void);
void fgOnResize(int32_t displayWidth, int32_t displayHeight);
void fgNoteBackendFrame(uint64_t backendFrameId);
bool fgReleaseProviderWithWait(CommandContext* ctx);
std::string fgStatusText(void);
int32_t fgStatePacked(void);

void fgNoteChainOwner(int32_t owner);
int32_t fgChainOwner(void);
int32_t fgChainOwnerFast(void);

int32_t fgWrapPresentChain(void* device, void* queue, void* appChain, int32_t width, int32_t height, int32_t format,
                           void** outPresentChain);
bool fgUnwrapPresentChain(void);

int checkDisplayFramePreparation(void* context, const void* input, uint32_t bytes);
int skipDisplayFrameGeneration(void* context, const void* input, uint32_t bytes);
int submitDisplayFrameGeneration(void* context, const void* input, uint32_t bytes, const FgFrameInputs& guides);

void fgSetUiTexture(void* object);

void fgSetUiComposition(int32_t enabled);
int32_t fgUiCompositionReady(void);

bool fgChainRebuildPending(void);
void fgNoteChainRebuildDone(int32_t ownerAfterRebuild, int32_t ok, const char* reason);

bool fgAbortWrapAfterFailure(const char* reason);

mcdr::Result<void> rebuildSwapChainForFg(Dx12Surface* s, const char* why, void* recording = nullptr);

std::unique_ptr<IFrameGenProvider> makeDlssGProvider();
void fillDlssGCaps(mcdr_fg_caps& out);

std::unique_ptr<IFrameGenProvider> makeXessFgProvider();
std::unique_ptr<IFrameGenProvider> makeFsrFgProvider();
void fillXessFgCaps(mcdr_fg_caps& out);
void fillFsrFgCaps(mcdr_fg_caps& out);
bool xessFgRuntimeAvailable();
bool fsrFgRuntimeAvailable();

int32_t fgPickedBackend(void);
bool fgWantsAppNativeChain(void);

mcdr::Result<void> setPipeline(CommandContext* ctx, Dx12Pipeline* pipeline, bool hasDepth);
mcdr::Result<void> setScissor(CommandContext* ctx, int x, int y, int w, int h);
mcdr::Result<void> setViewport(CommandContext* ctx, int x, int y, int w, int h);
mcdr::Result<void> setVertexBuffer(CommandContext* ctx, int slot, Dx12Object* buffer, long long offset,
                                   int stride, long long length = 0);
mcdr::Result<void> setIndexBuffer(CommandContext* ctx, Dx12Object* buffer, int indexType);
enum class DescriptorBindPoint { Graphics, Compute, GraphicsDeclared };
mcdr::Result<void> pushDescriptors(CommandContext* ctx, const std::vector<DrawBinding>& bindings,
    DescriptorBindPoint bindPoint = DescriptorBindPoint::Graphics);
struct DrawDescriptorRange {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    UINT offset = 0;
};
mcdr::Result<DrawDescriptorRange> allocateDrawDescriptors(CommandContext* context, UINT count);
mcdr::Result<void> collectGraphicsInputs(void* context, bool indexed, void* indirectCommands);
mcdr::Result<void> appendGraphicsInputBindings(void* context, const void* bindings, uint32_t bindingCount);
mcdr::Result<void> applyGraphicsInputReads(void* context);
mcdr::Result<void> applyGraphicsCapture(void* context);
void restoreGraphicsCaptureRate(void* context);
void invalidateGraphicsInputBindings(void* context, bool invalidateIa);
void restoreGraphicsInputAssembly(void* context);

mcdr::Result<void> drawIndexedInstanced(CommandContext* ctx, UINT indexCount, UINT instanceCount,
                                        INT startIndexLocation, INT baseVertexLocation,
                                        UINT startInstanceLocation);
mcdr::Result<void> drawInstanced(CommandContext* ctx, UINT vertexCount, UINT instanceCount, UINT firstVertex,
                                 UINT startInstanceLocation);
mcdr::Result<void> drawIndexedIndirect(CommandContext* ctx, Dx12Object* commands, long long offset,
                                       UINT drawCount);
mcdr::Result<void> drawIndirect(CommandContext* ctx, Dx12Object* commands, long long offset, UINT drawCount);

mcdr::Result<void> drawMeshlets(CommandContext* ctx, Dx12Object* vertexBuffer, long long vertexOffset,
                                Dx12Object* sections, long long sectionsOffset, long long sectionsLength,
                                Dx12Object* meshlets, long long meshletsOffset, long long meshletsLength,
                                Dx12Object* globals, long long globalsOffset, int meshletCount);

constexpr UINT kSurfaceBufferCount = 3;

struct Dx12Surface {
    std::mutex presentMutex;
    ComPtr<IDXGISwapChain3> swapChain;
    uintptr_t hwnd = 0;
    HWND presentWindow = nullptr;
    void* host = nullptr;
    bool hostRevealed = false;
    UINT width = 1;
    UINT height = 1;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT swapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    int presentMode = 2;
    int currentImageIndex = -1;
    int lastBlitIndex = -1;
    std::atomic<bool> blittedThisFrame{false};
    std::atomic<bool> bufferHeld{false};
    bool suboptimal = false;
    bool exclusive = false;
    bool hdrActive = false;
    float hdrSdrWhiteScale = 1.0f;
    float hdrMaxLuminance = 0.0f;
    bool allowTearing = false;
    bool compositionMode = false;
    ComPtr<IDCompositionDevice> dcompDevice;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;
    std::vector<ComPtr<ID3D12Resource>> backBuffers;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
    std::vector<UINT64> surfaceFences;
    std::vector<UINT64> surfacePresentFences;
    FgPresentTicket frameGenerationTicket;
};

extern Dx12Surface* gActiveSurface;
inline void setActiveSurface(Dx12Surface* s) { gActiveSurface = s; }
inline Dx12Surface* getActiveSurface() { return gActiveSurface; }
extern std::thread gRenderThread;
extern std::atomic<bool> gRenderRunning;
extern std::atomic<CommandContext*> gAsyncRenderCtx;
extern std::atomic<bool> gAsyncOwnsPresent;

mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> allocRtvHandle();

mcdr::Result<void*> createSurface(uintptr_t hwnd);
std::vector<int> surfacePresentModes();
mcdr::Result<void> configureSurface(Dx12Surface* s, int width, int height, int presentMode,
                                    bool force = false);

mcdr::Result<void> setExclusiveFullscreen(Dx12Surface* s, bool on);

void setHdrMode(int32_t mode);
int32_t hdrActiveFlag();
mcdr::Result<void> acquireSurface(Dx12Surface* s);
mcdr::Result<void> prepareDisplayPresentChain(Dx12Surface* s, void* recording);
mcdr::Result<void> blitSurface(CommandContext* ctx, Dx12Surface* s, Dx12Object* srcTex);
mcdr::Result<void> blitTextureToTexture(CommandContext* ctx, Dx12Object* src, Dx12Object* dst);
mcdr::Result<void> sharpenTextureToTexture(CommandContext* ctx, Dx12Object* src, Dx12Object* dst,
                                          float amount);
mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> acquireTextureRtv(CommandContext* ctx, Dx12Object* tex);
SurfacePresentResult presentSurface(Dx12Surface* s, FgPresentTicket ticket = {});
mcdr::Result<void> destroySurface(Dx12Surface* s);
void destroySurfaceNoWaitIdle(Dx12Surface* s);
mcdr::Result<void> readbackSurfacePixels(Dx12Surface* s);
uintptr_t getBackBufferHandle(Dx12Surface* s);
uintptr_t getActiveColorTextureHandle(CommandContext* ctx);
void dbgReadbackTexturePixels(Dx12Object* tex, const char* tag);
void dbgDumpPixelsToFile(const uint8_t* rgba, UINT w, UINT h, UINT64 pitch, const char* tag);
void dbgReadbackBufferBytes(Dx12Object* buf, long long offset, int len, const char* tag);

mcdr::Result<void> deviceWaitIdle();

std::string enumerateAdaptersJson();

std::string adapterInfoJson();
}

namespace d3d12_mc {
void installCrashHandlerOnce();
void dumpPendingDeleteCrashContext();
void dumpDeviceMessages();
void registerDebugMessageCallback(ID3D12InfoQueue1* queue);
int nativeLogLevel();
int loadPixCapturer();
std::string formatBackendResult(HRESULT result);
std::string backendWideToUtf8(const std::wstring& text);
void recordPipelineCreation();
UINT syncDrawDescriptorCapacity();
std::string graphicsShaderModelProfile();
std::string sm69RequirementText();
std::string firstLine(const std::string& text);
std::string errorMessage(const mcdr::Error& error);
FgPresentTicket fgDisplayGuideTicket(void* context);
void fgNoteRecordingSubmitted(CommandContext* context, UINT64 fenceValue);
}
