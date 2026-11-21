module;

#include "_d3d12_module_prelude.h"
#include <algorithm>

#include "mcdr_abi.h"

#include <xess_fg/xefg_swapchain.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell.h>
#include <xell/xell_d3d12.h>

module mcdr.d3d12.device;

namespace d3d12_mc {

namespace {

constexpr const char* kXessFgDll = "libxess_fg.dll";
constexpr const char* kXellDll = "libxell.dll";

struct XellApi {
    HMODULE module = nullptr;
    decltype(&xellSetSleepMode) setSleepMode = nullptr;
    decltype(&xellSleep) sleep = nullptr;
    decltype(&xellAddMarkerData) addMarkerData = nullptr;

    bool ready() const { return sleep != nullptr && addMarkerData != nullptr; }
};

std::mutex gXellMutex;
void* gXellContext = nullptr;
bool gXellAttempted = false;

XellApi& xellApi() {
    static XellApi api;
    return api;
}

void* xellContext(ID3D12Device* device)
{
    std::lock_guard<std::mutex> lk(gXellMutex);
    if (gXellContext != nullptr || gXellAttempted)
    {
        return gXellContext;
    }
    gXellAttempted = true;
    HMODULE module = ::LoadLibraryA("libxell.dll");
    if (module == nullptr)
    {
        return nullptr;
    }
    auto create = reinterpret_cast<decltype(&xellD3D12CreateContext)>(::GetProcAddress(module, "xellD3D12CreateContext"));
    if (create == nullptr)
    {
        return nullptr;
    }
    XellApi& xell = xellApi();
    xell.module = module;
    xell.setSleepMode = reinterpret_cast<decltype(xell.setSleepMode)>(::GetProcAddress(module, "xellSetSleepMode"));
    xell.sleep = reinterpret_cast<decltype(xell.sleep)>(::GetProcAddress(module, "xellSleep"));
    xell.addMarkerData =
        reinterpret_cast<decltype(xell.addMarkerData)>(::GetProcAddress(module, "xellAddMarkerData"));
    xell_context_handle_t context = nullptr;
    const xell_result_t created = create(device, &context);
    if (created == 0)
    {
        gXellContext = context;
        if (xell.ready())
        {
            xell_sleep_params_t params{};
            params.bLowLatencyMode = 1;
            const xell_result_t sleepMode = xell.setSleepMode(context, &params);
            if (sleepMode != 0)
            {
                dbgLogWarn("xell: SetSleepMode(lowLatency=1) failed 0x%X",
                           static_cast<unsigned>(sleepMode));
            }
        }
    }
    return gXellContext;
}
constexpr bool kWrapWired = true;
constexpr uint32_t kInitFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;

constexpr bool kTaggingWired = true;

constexpr bool kTagHudless = true;

ComPtr<ID3D12Heap> gTempBufferHeap;
ComPtr<ID3D12Heap> gTempTextureHeap;
UINT64 gTempBufCapacity = 0;
UINT64 gTempTexCapacity = 0;
int32_t gWrapCount = 0;
std::atomic<uint32_t>& markerFrameCounter() {
    static std::atomic<uint32_t> counter{0};
    return counter;
}
std::atomic<bool>& contextReady() {
    static std::atomic<bool> ready{false};
    return ready;
}

std::atomic<int32_t>& maxInterpMirror() {
    static std::atomic<int32_t> value{0};
    return value;
}

void writeCapsNote(char* dst, size_t cap, const char* text) {
    if (dst == nullptr || cap == 0) {
        return;
    }
    size_t i = 0;
    if (text != nullptr) {
        for (; i + 1 < cap && text[i] != '\0'; ++i) {
            dst[i] = text[i];
        }
    }
    dst[i] = '\0';
}

std::wstring selfDirectory() {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&selfDirectory), &self) == FALSE ||
        self == nullptr || ::GetModuleFileNameW(self, path, MAX_PATH) == 0) {
        return {};
    }
    std::wstring dir(path);
    const std::wstring::size_type slash = dir.find_last_of(L"\\/");
    dir.resize(slash == std::wstring::npos ? 0 : slash);
    return dir;
}

struct XessFgApi {
    HMODULE module = nullptr;
    decltype(&xefgSwapChainGetVersion) getVersion = nullptr;
    decltype(&xefgSwapChainD3D12CreateContext) createContext = nullptr;
    decltype(&xefgSwapChainD3D12BuildPipelines) buildPipelines = nullptr;
    decltype(&xefgSwapChainD3D12GetProperties) getProperties = nullptr;
    decltype(&xefgSwapChainD3D12InitFromSwapChain) initFromSwapChain = nullptr;
    decltype(&xefgSwapChainD3D12GetSwapChainPtr) getSwapChainPtr = nullptr;
    decltype(&xefgSwapChainSetLatencyReduction) setLatencyReduction = nullptr;
    decltype(&xefgSwapChainSetLoggingCallback) setLoggingCallback = nullptr;
    decltype(&xefgSwapChainD3D12TagFrameResource) tagFrameResource = nullptr;
    decltype(&xefgSwapChainD3D12SetDescriptorHeap) setDescriptorHeap = nullptr;
    decltype(&xefgSwapChainTagFrameConstants) tagFrameConstants = nullptr;
    decltype(&xefgSwapChainSetEnabled) setEnabled = nullptr;
    decltype(&xefgSwapChainSetPresentId) setPresentId = nullptr;
    decltype(&xefgSwapChainSetNumInterpolatedFrames) setNumInterpolatedFrames = nullptr;
    decltype(&xefgSwapChainSetUiCompositionState) setUiCompositionState = nullptr;
    decltype(&xefgSwapChainGetLastPresentStatus) getLastPresentStatus = nullptr;
    decltype(&xefgSwapChainDestroy) destroy = nullptr;

    bool ready() const {
        return createContext != nullptr && buildPipelines != nullptr && initFromSwapChain != nullptr &&
               getSwapChainPtr != nullptr && setEnabled != nullptr && setPresentId != nullptr &&
               destroy != nullptr;
    }
};

struct XessFgRuntime {
    std::mutex lock;
    bool attempted = false;
    XessFgApi api{};
    std::string problem;
};

XessFgRuntime& runtime() {
    static XessFgRuntime instance;
    return instance;
}

int32_t loadLocked(XessFgRuntime& rt) {
    if (rt.attempted) {
        return rt.problem.empty() ? 1 : 0;
    }
    rt.attempted = true;

    std::wstring candidate = selfDirectory();
    HMODULE module = nullptr;
    if (!candidate.empty()) {
        candidate += L"\\";
        candidate += std::wstring(kXessFgDll, kXessFgDll + std::strlen(kXessFgDll));
        module = ::LoadLibraryW(candidate.c_str());
    }
    if (module == nullptr) {
        module = ::LoadLibraryA(kXessFgDll);
    }
    if (module == nullptr) {
        rt.problem = "libxess_fg.dll not found";
        return 0;
    }

    XessFgApi& api = rt.api;
    api.module = module;
    api.getVersion = reinterpret_cast<decltype(api.getVersion)>(
        ::GetProcAddress(module, "xefgSwapChainGetVersion"));
    api.createContext = reinterpret_cast<decltype(api.createContext)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12CreateContext"));
    api.buildPipelines = reinterpret_cast<decltype(api.buildPipelines)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12BuildPipelines"));
    if (api.buildPipelines == nullptr) {
        rt.problem = "xefgSwapChainD3D12BuildPipelines missing";
        return 0;
    }
    api.getProperties = reinterpret_cast<decltype(api.getProperties)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12GetProperties"));
    api.initFromSwapChain = reinterpret_cast<decltype(api.initFromSwapChain)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChain"));
    api.getSwapChainPtr = reinterpret_cast<decltype(api.getSwapChainPtr)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12GetSwapChainPtr"));
    api.setLatencyReduction = reinterpret_cast<decltype(api.setLatencyReduction)>(
        ::GetProcAddress(module, "xefgSwapChainSetLatencyReduction"));
    api.setLoggingCallback = reinterpret_cast<decltype(api.setLoggingCallback)>(
::GetProcAddress(module, "xefgSwapChainSetLoggingCallback"));
    api.tagFrameResource = reinterpret_cast<decltype(api.tagFrameResource)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12TagFrameResource"));
    api.setDescriptorHeap = reinterpret_cast<decltype(api.setDescriptorHeap)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12SetDescriptorHeap"));
    api.tagFrameConstants = reinterpret_cast<decltype(api.tagFrameConstants)>(
        ::GetProcAddress(module, "xefgSwapChainTagFrameConstants"));
    api.setEnabled =
        reinterpret_cast<decltype(api.setEnabled)>(::GetProcAddress(module, "xefgSwapChainSetEnabled"));
    api.setPresentId = reinterpret_cast<decltype(api.setPresentId)>(
        ::GetProcAddress(module, "xefgSwapChainSetPresentId"));
    api.setNumInterpolatedFrames = reinterpret_cast<decltype(api.setNumInterpolatedFrames)>(
        ::GetProcAddress(module, "xefgSwapChainSetNumInterpolatedFrames"));
    api.setUiCompositionState = reinterpret_cast<decltype(api.setUiCompositionState)>(
        ::GetProcAddress(module, "xefgSwapChainSetUiCompositionState"));
    api.getLastPresentStatus = reinterpret_cast<decltype(api.getLastPresentStatus)>(
        ::GetProcAddress(module, "xefgSwapChainGetLastPresentStatus"));
    api.destroy =
        reinterpret_cast<decltype(api.destroy)>(::GetProcAddress(module, "xefgSwapChainDestroy"));

    if (!api.ready()) {
        rt.problem = "libxess_fg.dll missing entry points";
        return 0;
    }
    xefg_swapchain_version_t version{};
    if (!api.getVersion || api.getVersion(&version) != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        rt.problem = "XeSS-FG runtime version unavailable";
        return 0;
    }
    dbgLogInfo("xess-fg: runtime=%u.%u.%u display guides; HIGH_RES_MV is deprecated",
        version.major, version.minor, version.patch);
    return 1;
}

class XessFgFrameGenProvider final : public IFrameGenProvider {
public:
    FgBackendId backend() const override { return FgBackendId::XessFg; }

    void fillCaps(mcdr_fg_caps& out) const override { fillXessFgCaps(out); }

    bool setMode(const mcdr_fg_dispatch& desc) override {
        desiredMode_ = desc.mode;
        desiredFrames_ = desc.numFrames < 1 ? 1 : desc.numFrames;
        return true;
    }

    void frameStart() override {
        taggedThisFrame_ = false;
        if (handle_ == nullptr || xell_ == nullptr || desiredMode_ == 0) {
            return;
        }
        if (markerOpen_) {
            noteMarker(XELL_PRESENT_END, frameId_, "PRESENT_END(late)");
        }
        frameId_ = ++markerFrameCounter();
        markerOpen_ = true;
        noteSleep();
        noteMarker(XELL_SIMULATION_START, frameId_, "SIMULATION_START");
        noteMarker(XELL_SIMULATION_END, frameId_, "SIMULATION_END");
        noteMarker(XELL_RENDERSUBMIT_START, frameId_, "RENDERSUBMIT_START");
    }

    void discardFrameInputs() override {
        taggedThisFrame_ = false;
        historyAccepted_ = false;
        hasConstants_ = false;
        if (handle_ && api_.setEnabled && appliedMode_ != 0 &&
            api_.setEnabled(handle_, 0u) == XEFG_SWAPCHAIN_RESULT_SUCCESS) appliedMode_ = 0;
    }

    void beforePresent() override {
        if (desiredMode_ <= 0 || !taggedThisFrame_) {
            mode_ = 0;
            frames_ = 0;
            if (handle_ && api_.setEnabled && appliedMode_ != 0 &&
                api_.setEnabled(handle_, 0u) == XEFG_SWAPCHAIN_RESULT_SUCCESS) appliedMode_ = 0;
            return;
        }
        mode_ = 1;
        const int32_t cap = maxInterp_ > 0 ? maxInterp_ : 1;
        int32_t interp = desiredFrames_;
        if (interp < 1) {
            interp = 1;
        }
        if (interp > cap) {
            interp = cap;
        }
        frames_ = interp;
        if (xell_ != nullptr && frameId_ != 0) {
            noteMarker(XELL_RENDERSUBMIT_END, frameId_, "RENDERSUBMIT_END");
            noteMarker(XELL_PRESENT_START, frameId_, "PRESENT_START");
        }
    }

    void afterPresent() override {
        historyAccepted_ = false;
        taggedThisFrame_ = false;
        if (xell_ != nullptr && markerOpen_) {
            noteMarker(XELL_PRESENT_END, frameId_, "PRESENT_END");
            markerOpen_ = false;
        }
        if (handle_ == nullptr || api_.getLastPresentStatus == nullptr) {
            presented_ = 1;
            return;
        }
        xefg_swapchain_present_status_t status{};
        const xefg_swapchain_result_t r = api_.getLastPresentStatus(handle_, &status);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            presented_ = 1;
            if (presentStatusFails_++ == 0) {
                dbgLogWarn("xess-fg: GetLastPresentStatus failed result=0x%X",
                           static_cast<unsigned>(r));
            }
            return;
        }
        presented_ = status.framesPresented > 0 ? static_cast<int32_t>(status.framesPresented) : 1;
        historyAccepted_ = mode_ != 0 && status.isFrameGenEnabled != 0 &&
            status.frameGenResult == XEFG_SWAPCHAIN_RESULT_SUCCESS;
        if (++statusPolls_ % 300 == 0) {
            dbgLogInfo("xess-fg: presents=%llu enabled=%u presented=%d fgResult=0x%X frame_id=%u",
                       static_cast<unsigned long long>(statusPolls_),
                       static_cast<unsigned>(status.isFrameGenEnabled), presented_,
                       static_cast<unsigned>(status.frameGenResult), frameId_);
        }
        if (mode_ != 0 && status.isFrameGenEnabled == 0 && !notedDisabled_) {
            notedDisabled_ = true;
            dbgLogWarn("xess-fg: frame generation requested (frames=%d) but the SDK reports it "
                       "disabled (frameGenResult=0x%X)",
                       frames_, static_cast<unsigned>(status.frameGenResult));
        }
        if (statusLogCounter_++ % 120 == 0) {
            dbgLogDebug("xess-fg: presented=%d enabled=%u fgResult=0x%X (mode=%d frames=%d)",
                        presented_, status.isFrameGenEnabled, static_cast<unsigned>(status.frameGenResult),
                        mode_, frames_);
        }
    }

    int32_t presented() const override { return presented_; }
    bool historyAcceptedAfterPresent() const override { return historyAccepted_; }

    void onResize(int32_t displayWidth, int32_t displayHeight) override {
        width_ = displayWidth;
        height_ = displayHeight;
    }

    void shutdown() override {
        desiredMode_ = 0;
        desiredFrames_ = 1;
        mode_ = 0;
        frames_ = 0;
        maxInterp_ = 0;
        appliedMode_ = -1;
        appliedFrames_ = -1;
        uiEnabled_ = false;
        notedDisabled_ = false;
        presentId_ = 0;
        presented_ = 1;
        xell_ = nullptr;
        frameId_ = 0;
        markerOpen_ = false;
        markerFails_ = 0;
    }

void xefgLogCallback(const char* message, xefg_swapchain_logging_level_t level, void* userData)
{
    (void)userData;
    if (message != nullptr) {
        dbgLogInfo("sdk[%d] %.*s", static_cast<int>(level), 420, message);
    }
}

void dumpInfoQueue(ID3D12Device* device, const char* tag)
{
    if (device == nullptr) {
        return;
    }
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr) {
        return;
    }
    const UINT64 stored = infoQueue->GetNumStoredMessages();
    const UINT64 first = stored > 8 ? stored - 8 : 0;
    for (UINT64 i = first; i < stored; ++i)
    {
        SIZE_T size = 0;
        if (FAILED(infoQueue->GetMessage(i, nullptr, &size)) || size == 0)
        {
            continue;
        }
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(infoQueue->GetMessage(i, message, &size)))
        {
            continue;
        }
        dbgLogWarn("iq[%s] sev=%d id=%d %.*s", tag,
                   static_cast<int>(message->Severity), static_cast<int>(message->ID),
                   static_cast<int>(message->DescriptionByteLength > 240 ? 240 : message->DescriptionByteLength),
                   message->pDescription != nullptr ? message->pDescription : "");
    }
    infoQueue->ClearStoredMessages();
}

    int32_t wrapSwapChain(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* appChain,
                          int32_t width, int32_t height, DXGI_FORMAT format,
                          IDXGISwapChain** outPresentChain) override {
        if (outPresentChain != nullptr) {
            *outPresentChain = nullptr;
        }
        if (!kWrapWired) {
            return 0;
        }
        if (device == nullptr || queue == nullptr || appChain == nullptr || outPresentChain == nullptr) {
            return 0;
        }
        if (handle_ != nullptr) {
            dbgLogWarn("xess-fg: wrapSwapChain called twice without unwrap (wanted %dx%d)", width, height);
            return 0;
        }

        XessFgRuntime& rt = runtime();
        XessFgApi api{};
        {
            std::lock_guard<std::mutex> lk(rt.lock);
            if (loadLocked(rt) != 1) {
                dbgLogWarn("xess-fg: cannot wrap the chain, runtime unavailable: %s", rt.problem.c_str());
                return 0;
            }
            api = rt.api;
        }

        xefg_swapchain_handle_t handle = nullptr;
        xefg_swapchain_result_t result = api.createContext(device, &handle);
        handle_ = handle;
        api_ = api;
        handleInitialized_ = false;
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || handle == nullptr) {
            dbgLogWarn("xess-fg: CreateContext failed result=0x%X", static_cast<unsigned>(result));
            (void)tryUnwrapSwapChain();
            return 0;
        }
        result = api.buildPipelines(handle, nullptr, 1, kInitFlags);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            dbgLogWarn("xess-fg: BuildPipelines failed result=0x%X", static_cast<unsigned>(result));
            (void)tryUnwrapSwapChain();
            return 0;
        }

        xefg_swapchain_d3d12_init_params_t params{};
        params.pApplicationSwapChain = appChain;
        params.initFlags = kInitFlags;
        params.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
        params.creationNodeMask = 0;
        params.visibleNodeMask = 0;
        params.pTempBufferHeap = nullptr;
        params.bufferHeapOffset = 0;
        params.pTempTextureHeap = nullptr;
        params.textureHeapOffset = 0;
        params.pPipelineLibrary = nullptr;
        params.uiMode = (uiTexture_ != nullptr) ? XEFG_SWAPCHAIN_UI_MODE_HUDLESS_UITEXTURE
                                                : XEFG_SWAPCHAIN_UI_MODE_AUTO;

        {
            xefg_swapchain_properties_t props{};
            const xefg_swapchain_result_t pr =
                api.getProperties(handle, &params, 0, 0, DXGI_FORMAT_UNKNOWN, &props);
            dbgLogWarn("xess-fg: stage1 GetProperties result=0x%X tempBufferHeapSize=%llu "
                       "tempTextureHeapSize=%llu maxSupportedInterpolations=%u",
                       static_cast<unsigned>(pr),
                       static_cast<unsigned long long>(props.tempBufferHeapSize),
                       static_cast<unsigned long long>(props.tempTextureHeapSize),
                       props.maxSupportedInterpolations);
        }

        {
            xefg_swapchain_properties_t heaps{};
            const xefg_swapchain_result_t pr2 =
                api.getProperties(handle, &params, static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<DXGI_FORMAT>(format), &heaps);
            dbgLogInfo("stage1: GetProperties result=0x%X bufHeap=%llu texHeap=%llu maxInterp=%u",
                       static_cast<unsigned>(pr2),
                       static_cast<unsigned long long>(heaps.tempBufferHeapSize),
                       static_cast<unsigned long long>(heaps.tempTextureHeapSize),
                       heaps.maxSupportedInterpolations);
            if (pr2 == XEFG_SWAPCHAIN_RESULT_SUCCESS && device != nullptr) {
                if (heaps.tempBufferHeapSize > gTempBufCapacity) {
                    gTempBufferHeap.Reset();
                    D3D12_HEAP_DESC desc{};
                    desc.SizeInBytes = heaps.tempBufferHeapSize;
                    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
                    const HRESULT hr = device->CreateHeap(&desc, IID_PPV_ARGS(&gTempBufferHeap));
                    if (SUCCEEDED(hr)) { gTempBufCapacity = heaps.tempBufferHeapSize; }
                }
                if (heaps.tempTextureHeapSize > gTempTexCapacity) {
                    gTempTextureHeap.Reset();
                    D3D12_HEAP_DESC desc{};
                    desc.SizeInBytes = heaps.tempTextureHeapSize;
                    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
                    const HRESULT hr = device->CreateHeap(&desc, IID_PPV_ARGS(&gTempTextureHeap));
                    if (SUCCEEDED(hr)) { gTempTexCapacity = heaps.tempTextureHeapSize; }
                }
                params.pTempBufferHeap = gTempBufferHeap.Get();
                params.bufferHeapOffset = 0;
                params.pTempTextureHeap = gTempTextureHeap.Get();
                params.textureHeapOffset = 0;
                maxInterp_ = static_cast<int32_t>(heaps.maxSupportedInterpolations);
                maxInterpMirror().store(maxInterp_);
            }
        }
    if (api.setLoggingCallback != nullptr)
    {
        const xefg_swapchain_result_t logged = api.setLoggingCallback(
            handle, XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG,
            [](const char* message, xefg_swapchain_logging_level_t level, void* userData)
            {
                (void)userData;
                dbgLogInfo("sdk[%d] %.*s", static_cast<int>(level), 420,
                           message != nullptr ? message : "");
            },
            nullptr);
        if (logged != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            dbgLogWarn("xess-fg: SetLoggingCallback failed result=0x%X", static_cast<unsigned>(logged));
        }
    }
    if (api.setLatencyReduction != nullptr)
    {
        void* xell = xellContext(device);
        const xefg_swapchain_result_t latency = api.setLatencyReduction(handle, xell);
        if (latency != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            dbgLogWarn("xess-fg: SetLatencyReduction failed result=0x%X", static_cast<unsigned>(latency));
            (void)tryUnwrapSwapChain();
            return 0;
        }
    }

    result = api.initFromSwapChain(handle, queue, &params);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        dbgLogWarn("xess-fg: InitFromSwapChain failed result=0x%X", static_cast<unsigned>(result));
        dumpInfoQueue(device, "init");
        (void)tryUnwrapSwapChain();
        return 0;
    }
    handleInitialized_ = true;

    void* presentChain = nullptr;
    result = api.getSwapChainPtr(handle, __uuidof(IDXGISwapChain4), &presentChain);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || presentChain == nullptr)
    {
        dbgLogWarn("xess-fg: GetSwapChainPtr failed result=0x%X", static_cast<unsigned>(result));
        (void)tryUnwrapSwapChain();
        return 0;
    }

        handle_ = handle;
        api_ = api;
        contextReady().store(true);
        width_ = width;
        height_ = height;
        {
            ComPtr<IDXGISwapChain1> sc1;
            DXGI_SWAP_CHAIN_DESC1 scDesc{};
            if (SUCCEEDED(static_cast<IDXGISwapChain*>(presentChain)->QueryInterface(IID_PPV_ARGS(&sc1))) &&
                sc1 != nullptr && SUCCEEDED(sc1->GetDesc1(&scDesc))) {
            }
        }
        appliedMode_ = -1;
        appliedFrames_ = -1;
        uiEnabled_ = false;
        tagFailLogged_ = false;
        notedDisabled_ = false;
        presentStatusFails_ = 0;
        presentId_ = 0;
        presented_ = 1;
        xell_ = xellContext(device);
        markerOpen_ = false;
        markerFails_ = 0;
        dbgLogInfo("xess-fg: wrapped the app chain %dx%d (ui=HUDLESS_UITEXTURE maxInterp=%d)",
                   width, height, maxInterp_);
        *outPresentChain = static_cast<IDXGISwapChain*>(presentChain);
        return 1;
    }

    void setFrameConstants(const mcdr_fg_constants& constants) override
    {
        constants_ = constants;
        hasConstants_ = true;
    }

    void setUiTexture(void* object, std::shared_ptr<void> lease = {}) override {
        uiTextureHold_ = static_cast<ID3D12Resource*>(object);
        uiLease_ = std::move(lease);
        uiTexture_ = uiTextureHold_.Get();
        if (uiTexture_ == nullptr && uiEnabled_ && handle_ && api_.setUiCompositionState) {
            const auto result = api_.setUiCompositionState(handle_, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED);
            if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS) uiEnabled_ = false;
        }
    }

    FgInputAcceptance tagFrameInputs(const FgFrameInputs& in) override {
        taggedThisFrame_ = false;
        historyAccepted_ = false;
        if (desiredMode_ <= 0) return FgInputAcceptance::Skipped;
        if (!api_.setPresentId || !api_.tagFrameConstants || !api_.tagFrameResource ||
            !api_.setEnabled || !api_.setNumInterpolatedFrames || !api_.setUiCompositionState || !hasConstants_) {
            return FgInputAcceptance::Failed;
        }
        ++tagCalls_;
        if (handle_ == nullptr || in.cmdList == nullptr || !in.resourceLease) {
            return FgInputAcceptance::Failed;
        }
        struct DisableRejectedFrame {
            XessFgFrameGenProvider* provider;
            bool accepted = false;
            ~DisableRejectedFrame() {
                if (!accepted && provider->api_.setEnabled(provider->handle_, 0u) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
                    provider->appliedMode_ = 0;
            }
        } rejection{this};
        auto* ctx = static_cast<CommandContext*>(recordingContextForCommandList(in.cmdList));
        if (ctx == nullptr) return FgInputAcceptance::Failed;
        if (!in.pDepth || !in.pMotionVectors || !in.pUi || !in.motionVectorsDilated ||
            in.renderWidth != in.displayWidth || in.renderHeight != in.displayHeight ||
            in.displayWidth <= 0 || in.displayHeight <= 0 || (kTagHudless && !in.pColorOutput)) {
            if (api_.setEnabled(handle_, 0u) == XEFG_SWAPCHAIN_RESULT_SUCCESS) appliedMode_ = 0;
            return FgInputAcceptance::Failed;
        }
        for (void* resource : {in.pDepth, in.pMotionVectors, in.pColorOutput, in.pUi}) {
            const auto description = static_cast<ID3D12Resource*>(resource)->GetDesc();
            if (description.Width != static_cast<uint32_t>(in.displayWidth) ||
                description.Height != static_cast<uint32_t>(in.displayHeight)) {
                if (api_.setEnabled(handle_, 0u) == XEFG_SWAPCHAIN_RESULT_SUCCESS) appliedMode_ = 0;
                return FgInputAcceptance::Failed;
            }
        }
        mode_ = desiredMode_ > 0 ? 1 : 0;
        frames_ = (std::min)((std::max)(desiredFrames_, 1), (std::max)(maxInterp_, 1));
        if (frameId_ == 0 || !markerOpen_) return FgInputAcceptance::Failed;
        const uint32_t presentId = frameId_;
        presentId_ = presentId;
        if (api_.setPresentId(handle_, presentId) != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            return FgInputAcceptance::Failed;
        }
            if (api_.tagFrameConstants != nullptr && hasConstants_)
            {
                xefg_swapchain_frame_constant_data_t frameConstants = {};
                for (int32_t i = 0; i < 16; ++i)
                {
                    frameConstants.viewMatrix[i] = constants_.view[i];
                    frameConstants.projectionMatrix[i] = constants_.projection[i];
                }
                frameConstants.jitterOffsetX = constants_.jitterX;
                frameConstants.jitterOffsetY = constants_.jitterY;
                frameConstants.motionVectorScaleX = constants_.mvScaleX;
                frameConstants.motionVectorScaleY = constants_.mvScaleY;
                frameConstants.resetHistory = constants_.resetHistory != 0 ? 1u : 0u;
                frameConstants.frameRenderTime = constants_.frameRenderTime;
                if (api_.tagFrameConstants(handle_, presentId, &frameConstants) !=
                    XEFG_SWAPCHAIN_RESULT_SUCCESS) return FgInputAcceptance::Failed;
            }
        bool tagsOk = true;
        auto tagResource = [&](xefg_swapchain_resource_type_t type, void* resource, D3D12_RESOURCE_STATES initial) {
            if (api_.tagFrameResource == nullptr) {
                return;
            }
            if (resource == nullptr) {
                return;
            }
            xefg_swapchain_d3d12_resource_data_t data{};
            data.type = type;
            data.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
            data.pResource = static_cast<ID3D12Resource*>(resource);
            const auto state = static_cast<D3D12_RESOURCE_STATES>(resourceSubresourceState(ctx, resource, 0, initial));
            transitionTo(ctx, data.pResource, initial, state);
            data.incomingState = state;
            data.resourceBase = {0, 0};
            data.resourceSize = {static_cast<uint32_t>(in.displayWidth), static_cast<uint32_t>(in.displayHeight)};
            const xefg_swapchain_result_t tagged = api_.tagFrameResource(handle_, in.cmdList, presentId, &data);
            if (tagged != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                tagsOk = false;
                if (!tagFailLogged_) {
                    tagFailLogged_ = true;
                    dbgLogWarn("xess-fg: TagFrameResource(type=%d) failed result=0x%X",
                               static_cast<int>(type), static_cast<unsigned>(tagged));
                }
            }
        };
        tagResource(XEFG_SWAPCHAIN_RES_DEPTH, in.pDepth, in.depthState);
        tagResource(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, in.pMotionVectors, in.motionVectorState);
        if (kTagHudless) {
            tagResource(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, in.pColorOutput, in.colorState);
        }
        if (uiTexture_ != nullptr && !uiEnabled_ && api_.setUiCompositionState != nullptr) {
            const xefg_swapchain_result_t ui =
                api_.setUiCompositionState(handle_, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
            uiEnabled_ = (ui == XEFG_SWAPCHAIN_RESULT_SUCCESS);
            dbgLogInfo("xess-fg: UI composition %s (result=0x%X texture=%p)",
                       uiEnabled_ ? "enabled" : "rejected", static_cast<unsigned>(ui), uiTexture_);
            if (!uiEnabled_) return FgInputAcceptance::Failed;
        }
        if (!uiEnabled_) return FgInputAcceptance::Failed;
        tagResource(XEFG_SWAPCHAIN_RES_UI, in.pUi, in.uiState);
        if (!tagsOk) {
            taggedThisFrame_ = false;
            if (api_.setEnabled(handle_, 0u) == XEFG_SWAPCHAIN_RESULT_SUCCESS) appliedMode_ = 0;
            return FgInputAcceptance::Failed;
        }
        if (appliedMode_ != mode_) {
            const xefg_swapchain_result_t on =
                api_.setEnabled(handle_, mode_ != 0 ? 1u : 0u);
            if (on == XEFG_SWAPCHAIN_RESULT_SUCCESS) appliedMode_ = mode_;
            if (on != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                dbgLogWarn("xess-fg: SetEnabled(%d) failed result=0x%X",
                           mode_ != 0 ? 1 : 0, static_cast<unsigned>(on));
                return FgInputAcceptance::Failed;
            }
        }
        if (mode_ != 0 && appliedFrames_ != frames_) {
            const xefg_swapchain_result_t nf =
                api_.setNumInterpolatedFrames(handle_, static_cast<uint32_t>(frames_ < 1 ? 1 : frames_));
            if (nf == XEFG_SWAPCHAIN_RESULT_SUCCESS) appliedFrames_ = frames_;
            if (nf != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                dbgLogWarn("xess-fg: SetNumInterpolatedFrames(%d) failed result=0x%X",
                           frames_, static_cast<unsigned>(nf));
                return FgInputAcceptance::Failed;
            }
        }
        taggedThisFrame_ = true;
        rejection.accepted = true;
        if (tagCalls_ == 1) {
            dbgLogInfo("xess-fg: tagging through the wrapped chain cmdList=%p color=%p depth=%p "
                       "mv=%p ui=%p mode=%d frames=%d",
                       static_cast<void*>(in.cmdList),
                       static_cast<const void*>(in.pColorOutput), static_cast<const void*>(in.pDepth),
                       static_cast<const void*>(in.pMotionVectors), uiTexture_, mode_, frames_);
        }
        return FgInputAcceptance::Accepted;
    }

    void unwrapSwapChain() override {
        (void)tryUnwrapSwapChain();
    }

    bool tryUnwrapSwapChain() override {
        if (handle_ == nullptr) {
            return true;
        }
        if (api_.destroy == nullptr) return false;
        if (handleInitialized_ && api_.setEnabled != nullptr &&
            api_.setEnabled(handle_, 0u) != XEFG_SWAPCHAIN_RESULT_SUCCESS) return false;
        const auto destroyed = api_.destroy(handle_);
        if (destroyed != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            dbgLogWarn("xess-fg: Destroy failed result=0x%X; keeping context and leases",
                       static_cast<unsigned>(destroyed));
            return false;
        }
        uiEnabled_ = false;
        handle_ = nullptr;
        handleInitialized_ = false;
        api_ = XessFgApi{};
        contextReady().store(false);
        maxInterpMirror().store(0);
        maxInterp_ = 0;
        dbgLogInfo("xess-fg: released the wrapped chain");
        return true;
    }

private:
    void noteSleep()
    {
        XellApi& xell = xellApi();
        if (xell_ == nullptr || !xell.ready())
        {
            return;
        }
        const xell_result_t r = xell.sleep(static_cast<xell_context_handle_t>(xell_), frameId_);
        if (r != 0 && sleepFails_ < 4)
        {
            ++sleepFails_;
        }
    }

    void noteMarker(xell_latency_marker_type_t marker, uint32_t frameId, const char* name)
    {
        XellApi& xell = xellApi();
        if (xell_ == nullptr || !xell.ready())
        {
            return;
        }
        const xell_result_t r = xell.addMarkerData(static_cast<xell_context_handle_t>(xell_), frameId, marker);
        if (r != 0 && markerFails_ < 4)
        {
            ++markerFails_;
        }
    }

    xefg_swapchain_handle_t handle_ = nullptr;
    bool handleInitialized_ = false;
    void* xell_ = nullptr;
    uint32_t frameId_ = 0;
    bool markerOpen_ = false;
    int32_t markerFails_ = 0;
    int32_t sleepFails_ = 0;
    int32_t tagCalls_ = 0;
    uint32_t presentId_ = 0;
    int32_t mode_ = 0;
    int32_t frames_ = 1;
    int32_t maxInterp_ = 0;
    bool uiEnabled_ = false;
    bool taggedThisFrame_ = false;
    bool historyAccepted_ = false;
    bool tagFailLogged_ = false;
    bool notedDisabled_ = false;
    int32_t presentStatusFails_ = 0;
    uint64_t statusPolls_ = 0;
    uint32_t statusLogCounter_ = 0;
    int32_t appliedMode_ = -1;
    int32_t appliedFrames_ = -1;
    void* uiTexture_ = nullptr;
    ComPtr<ID3D12Resource> uiTextureHold_;
    std::shared_ptr<void> uiLease_;
    mcdr_fg_constants constants_ = {};
    bool hasConstants_ = false;
    XessFgApi api_{};
    int32_t desiredMode_ = 0;
    int32_t desiredFrames_ = 1;
    int32_t width_ = 0;
    int32_t height_ = 0;
    int32_t presented_ = 1;
};

}

bool xessFgRuntimeAvailable() {
    XessFgRuntime& rt = runtime();
    std::lock_guard<std::mutex> lk(rt.lock);
    return loadLocked(rt) == 1;
}

void fillXessFgCaps(mcdr_fg_caps& out) {
    out.backend = MCDR_FG_BACKEND_XESSFG;

    XessFgRuntime& rt = runtime();
    bool loaded = false;
    std::string problem;
    {
        std::lock_guard<std::mutex> lk(rt.lock);
        loaded = loadLocked(rt) == 1;
        problem = rt.problem;
    }

    out.runtimeLoaded = loaded ? 1 : 0;
    out.deviceSupported = contextReady().load() ? 1 : 0;
    static_cast<void>(0);
    out.chainCompatible = 1;
    out.needsReflex = 0;
    out.needsHudless = 1;
    out.needsUiResource = 1;
    out.transitionsInputs = 1;
    const int32_t knownMax = maxInterpMirror().load();
    out.maxGeneratedFrames = knownMax > 0 ? knownMax : 1;
    out.latencyProvider = MCDR_FG_LATENCY_XELL;
    out.reserved = 0;
    out.available = (loaded && kWrapWired && kTaggingWired) ? 1 : 0;

    if (!loaded) {
        writeCapsNote(out.note, sizeof(out.note), problem.empty() ? "libxess_fg.dll missing" : problem.c_str());
    } else if (out.chainCompatible == 0) {
        writeCapsNote(out.note, sizeof(out.note), "needs app-native chain");
    } else if (!kWrapWired) {
        writeCapsNote(out.note, sizeof(out.note), "XeSS-FG tags not wired");
    } else {
        writeCapsNote(out.note, sizeof(out.note), "XeSS-FG ready");
    }

    static std::atomic<bool> loggedCaps{false};
    if (!loggedCaps.exchange(true)) {
        dbgLogInfo("xess-fg caps: runtime=%d device=%d chainCompat=%d available=%d latency=%d note=\"%s\"",
                   out.runtimeLoaded, out.deviceSupported, out.chainCompatible, out.available, out.latencyProvider,
                   out.note);
    }
}

std::unique_ptr<IFrameGenProvider> makeXessFgProvider() {
    return std::make_unique<XessFgFrameGenProvider>();
}

}

