// d3d12_xessfg.cpp — XeSS-FG（XeSS 帧生成）provider 的实现单元（模块 mcdr.d3d12.device）
//
// XeSS-FG 是**包装交换链**型后端：init 之后所有 Present 必须走 `xefgSwapChainD3D12GetSwapChainPtr`
// 换回来的那条链，重链之前必须先 Destroy；因此它要求链是**应用原生**的（MCDR_FG_CHAIN_APP_NATIVE），
// SL 代理工厂建的链交不进来。见 docs/advanced/framegen.md

module;

#include "_d3d12_module_prelude.h"

// 必须放在全局模块片段：模块 purview 内文本包含头文件会触发 C5244，/WX 直接失败。
#include "mcdr_abi.h"

// 厂商头不进模块接口 —— 别的单元想用也用不了，依赖方向只有一个。
#include <xess_fg/xefg_swapchain.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell.h>
#include <xell/xell_d3d12.h>

module mcdr.d3d12.device;

namespace d3d12_mc {

namespace {

constexpr const char* kXessFgDll = "libxess_fg.dll";
constexpr const char* kXellDll = "libxell.dll";

// XeSS-FG 硬性要求一条 XeLL 低延迟通路（缺了 InitFromSwapChain 直接返回 -15
// LATENCY_REDUCTION_UNSUPPORTED）。上下文按进程单例持有：XeLL 的语义是"一条低延迟通路"，
// 入口每帧要用，不能每帧 GetProcAddress。
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
        // `bLowLatencyMode = 1` 是启用 latency reduction 的开关，必须在 SetEnabled 之前设一次；
        // `minimumIntervalUs = 0` 表示不做限帧。
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
// 深度是 reverse-Z（GREATER_THAN_OR_EQUAL、清 0 = 远），MV 含 jitter（与 DLSS 侧同时置
// MVJittered + motionVectorJitterCancellation 一致）。头文件要求 BuildPipelines 的 initFlags
// 与初始化用的值完全一致，所以只留一个常量。
constexpr uint32_t kInitFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH | XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;

constexpr bool kTaggingWired = true;

// 置 false 只 tag MV + 深度（最小契约），怀疑某条资源时不用改代码结构。
constexpr bool kTagHudless = true;

// caps 是自由函数、且会在持锁路径上被调用：镜像一律用文件级原子。模块内 `std::mutex` 不可
// 重入 —— 持锁时不能回调 fgFillCaps / fgChainOwner。
ComPtr<ID3D12Heap> gTempBufferHeap;
ComPtr<ID3D12Heap> gTempTextureHeap;
UINT64 gTempBufCapacity = 0;   // 已建堆的容量，尺寸变大才重建
UINT64 gTempTexCapacity = 0;
int32_t gWrapCount = 0;         // 第几次 wrap（多尺寸重建会多次进来）
// XeLL 的 frame_id 必须**单调递增**，换链也不回到 0（上下文是进程单例、记着用过的号），
// 所以计数器放文件级、只增不减；presentId 与它共用一个号。
std::atomic<uint32_t>& markerFrameCounter() {
    static std::atomic<uint32_t> counter{0};
    return counter;
}
std::atomic<bool>& contextReady() {
    static std::atomic<bool> ready{false};
    return ready;
}

// init 时 SDK 报的最大插帧数的镜像（caps 看不到 provider 实例，同 contextReady()）。
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

// 本模块所在目录：`GetModuleFileNameA(nullptr)` 拿到的是宿主 exe（java.exe），必须用
// GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 问自己。发行包里厂商 DLL 与本 DLL 同目录。
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

// 从 DLL 里取到的入口：`ready()` 里那几条是必需的，其余有就用。用 `decltype(&...)` 而不是
// 手写签名 —— 头文件是唯一类型来源，改 SDK 版本时编译器替我们检查。
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

// 进程级只尝试加载一次：`LoadLibraryW` 失败路径会走一遍搜索（可能很慢），而 fillCaps 每
// 600 帧被调用一次。失败原因留在 problem，caps 的 note 透给 JVM。
struct XessFgRuntime {
    std::mutex lock;
    bool attempted = false;
    XessFgApi api{};
    std::string problem;  // 空 = 运行库与必需入口都在
};

XessFgRuntime& runtime() {
    static XessFgRuntime instance;
    return instance;
}

// 搜索顺序：本模块目录 → 裸 DLL 名。返回 1 = 已载入且必需入口齐备。
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
    return 1;
}

// XeSS-FG 的 provider。`presented()` 与 DLSS-G 对齐：返回上一次 Present 真正提交的帧数
//（1 = 没插帧），取自 xefgSwapChainGetLastPresentStatus 的 framesPresented；context 没建时恒 1。
class XessFgFrameGenProvider final : public IFrameGenProvider {
public:
    FgBackendId backend() const override { return FgBackendId::XessFg; }

    void fillCaps(mcdr_fg_caps& out) const override { fillXessFgCaps(out); }

    bool setMode(const mcdr_fg_dispatch& desc) override {
        // setMode 只记录（可能来自 JVM 设置线程），真正下发在下一次 beforePresent。
        desiredMode_ = desc.mode;
        desiredFrames_ = desc.numFrames < 1 ? 1 : desc.numFrames;
        return true;
    }

    void frameStart() override {
        // XeLL 帧首：`xellSleep` 必须是带新 frame_id 的第一个 XeLL 调用、且在发 marker 之前返回，
        // 随后 `XELL_SIMULATION_START` 必须是本帧第一条 marker —— 顺序反了 addMarkerData 返回 -4。
        // 模拟段压成零长：钩子挂在 acquireSurface，真正的模拟段在 JVM 侧（只让耗时估计偏低）。
        if (handle_ == nullptr || xell_ == nullptr || mode_ == 0) {
            return;
        }
        if (markerOpen_) {
            // 上一帧的 PRESENT_END 没发出去就补一条，否则 XeLL 的 present 窗口会一直开着。
            noteMarker(XELL_PRESENT_END, frameId_, "PRESENT_END(late)");
        }
        frameId_ = ++markerFrameCounter();
        markerOpen_ = true;
        noteSleep();
        noteMarker(XELL_SIMULATION_START, frameId_, "SIMULATION_START");
        noteMarker(XELL_SIMULATION_END, frameId_, "SIMULATION_END");
        noteMarker(XELL_RENDERSUBMIT_START, frameId_, "RENDERSUBMIT_START");
    }

    void beforePresent() override {
        // `desc.numFrames` 是**额外帧数**（1 = 多插 1 张 = 2x），与 XeSS 的"插几帧"同一口径。
        // 每次 Present 前重算（设置线程随时可能改），真正的 SDK 下发在 tagFrameInputs。
        if (desiredMode_ <= 0) {
            mode_ = 0;
            frames_ = 0;
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
        // 帧尾两段（顺序同 guide）：RENDERSUBMIT_END → PRESENT_START，PRESENT_END 在 afterPresent。
        // frameId_ == 0 = 本帧还没走 frameStart（开关刚打开），此时 marker 会被 XeLL 拒掉，干脆不发。
        if (xell_ != nullptr && frameId_ != 0) {
            noteMarker(XELL_RENDERSUBMIT_END, frameId_, "RENDERSUBMIT_END");
            noteMarker(XELL_PRESENT_START, frameId_, "PRESENT_START");
        }
    }

    void afterPresent() override {
        // PRESENT_END 先发 —— 它属于上一次 Present，不能因为下面读状态失败就被漏掉。
        if (xell_ != nullptr && markerOpen_) {
            noteMarker(XELL_PRESENT_END, frameId_, "PRESENT_END");
            markerOpen_ = false;
        }
        // 插帧在 SDK 的 Present 内部完成，这一侧只能读结果。
        if (handle_ == nullptr || api_.getLastPresentStatus == nullptr) {
            presented_ = 1;
            return;
        }
        xefg_swapchain_present_status_t status{};
        const xefg_swapchain_result_t r = api_.getLastPresentStatus(handle_, &status);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            presented_ = 1;
            if (presentStatusFails_++ == 0) {
                dbgLogWarn("xess-fg: present status failed 0x%X",
                           static_cast<unsigned>(r));
            }
            return;
        }
        presented_ = status.framesPresented > 0 ? static_cast<int32_t>(status.framesPresented) : 1;
        // SDK 只在状态里回答"到底开没开"；每 300 帧留一行，判断 tag 都通了但 SDK 仍 disabled。
        if (++statusPolls_ % 300 == 0) {
            dbgLogInfo("xess-fg: presents=%llu on=%u shown=%d result=0x%X frame=%u",
                       static_cast<unsigned long long>(statusPolls_),
                       static_cast<unsigned>(status.isFrameGenEnabled), presented_,
                       static_cast<unsigned>(status.frameGenResult), frameId_);
        }
        if (mode_ != 0 && status.isFrameGenEnabled == 0 && !notedDisabled_) {
            notedDisabled_ = true;
            dbgLogWarn("xess-fg: %d frames requested, SDK disabled (0x%X)",
                       frames_, static_cast<unsigned>(status.frameGenResult));
        }
        if (statusLogCounter_++ % 120 == 0) {
            // DEBUG：每 120 帧一行，放 INFO 就是刷屏。
            dbgLogDebug("xess-fg: shown=%d on=%u result=0x%X mode=%d frames=%d",
                        presented_, status.isFrameGenEnabled, static_cast<unsigned>(status.frameGenResult),
                        mode_, frames_);
        }
    }

    int32_t presented() const override { return presented_; }

    void onResize(int32_t displayWidth, int32_t displayHeight) override {
        // 链重建后必须重新 InitFromSwapChain：context 绑在链上，尺寸变了旧 context 就作废。
        width_ = displayWidth;
        height_ = displayHeight;
    }

    void shutdown() override {
        // 顺序：先 SetEnabled(0) 让 SDK 停止插帧，再 Destroy（链引用由 surface 持有）；没 context 时幂等。
        // 状态全部归零，否则下一个实例会带着"已下发过"的假象跳过 SetEnabled。
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
        // marker 状态跟着归零；XeLL 上下文是进程单例，不在这里销毁（它必须活得更久）。
        xell_ = nullptr;
        frameId_ = 0;
        markerOpen_ = false;
        markerFails_ = 0;
    }

// `ERROR_HRESULT_FAILURE (-17)` 只说"某个 D3D12 调用失败"；具体证据在调试层的 InfoQueue 里。
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
            dbgLogWarn("xess-fg: chain already wrapped, wanted %dx%d", width, height);
            return 0;
        }

        XessFgRuntime& rt = runtime();
        XessFgApi api{};
        {
            std::lock_guard<std::mutex> lk(rt.lock);
            if (loadLocked(rt) != 1) {
                dbgLogWarn("xess-fg: no wrap, runtime unavailable: %s", rt.problem.c_str());
                return 0;
            }
            api = rt.api;
        }

        xefg_swapchain_handle_t handle = nullptr;
        xefg_swapchain_result_t result = api.createContext(device, &handle);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || handle == nullptr) {
            dbgLogWarn("xess-fg: CreateContext failed result=0x%X", static_cast<unsigned>(result));
            return 0;
        }
        // blocking=1：pipeline 编译完再往下走 —— 把卡顿留在链重建这次（已等过 GPU 空闲）。
        result = api.buildPipelines(handle, nullptr, 1, kInitFlags);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            dbgLogWarn("xess-fg: BuildPipelines failed result=0x%X", static_cast<unsigned>(result));
            api.destroy(handle);
            return 0;
        }

        xefg_swapchain_d3d12_init_params_t params{};
        params.pApplicationSwapChain = appChain;
        params.initFlags = kInitFlags;
        // 这里给的是"上限"而不是"当前值"：`SetNumInterpolatedFrames` 只能在 init 之后调，
        // 且不能超过 init 时声明的上限。
        params.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
        params.creationNodeMask = 0;
        params.visibleNodeMask = 0;
        params.pTempBufferHeap = nullptr;
        params.bufferHeapOffset = 0;
        params.pTempTextureHeap = nullptr;
        params.textureHeapOffset = 0;
        params.pPipelineLibrary = nullptr;
        // uiMode 必须与"实际会 tag 什么"一致：声明 HUDLESS_UITEXTURE 就是承诺每帧都有 UI 纹理进来，
        // 没有时 SDK 判 -14 INCORRECT_INPUT_RESOURCES；所以有纹理才用，否则 AUTO。
        params.uiMode = (uiTexture_ != nullptr) ? XEFG_SWAPCHAIN_UI_MODE_HUDLESS_UITEXTURE
                                                : XEFG_SWAPCHAIN_UI_MODE_AUTO;

        {
            xefg_swapchain_properties_t props{};
            const xefg_swapchain_result_t pr =
                api.getProperties(handle, &params, 0, 0, DXGI_FORMAT_UNKNOWN, &props);
            dbgLogWarn("xess-fg: stage1 props=0x%X tempHeap=%llu texHeap=%llu maxInterp=%u",
                       static_cast<unsigned>(pr),
                       static_cast<unsigned long long>(props.tempBufferHeapSize),
                       static_cast<unsigned long long>(props.tempTextureHeapSize),
                       props.maxSupportedInterpolations);
        }

        // InitFromSwapChain 要求应用提供两块临时堆，尺寸由 GetProperties 给出；堆的所有权留在本模块内。
        {
            xefg_swapchain_properties_t heaps{};
            const xefg_swapchain_result_t pr2 =
                api.getProperties(handle, &params, static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<DXGI_FORMAT>(format), &heaps);
            dbgLogInfo("xess-fg: stage1 props result=0x%X buf=%llu tex=%llu max=%u",
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
                // 插帧数上限只在这里能问到（init 之前），记下来给 SetNumInterpolatedFrames 与 caps 用。
                maxInterp_ = static_cast<int32_t>(heaps.maxSupportedInterpolations);
                maxInterpMirror().store(maxInterp_);
            }
        }
    // 顺序：注册日志回调 → SetLatencyReduction → InitFromSwapChain（让 SDK 自己说 init 失败的原因）。
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
            dbgLogWarn("xess-fg: SetLoggingCallback failed 0x%X", static_cast<unsigned>(logged));
        }
    }
    if (api.setLatencyReduction != nullptr)
    {
        void* xell = xellContext(device);
        const xefg_swapchain_result_t latency = api.setLatencyReduction(handle, xell);
        if (latency != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            dbgLogWarn("xess-fg: SetLatencyReduction failed 0x%X", static_cast<unsigned>(latency));
            api.destroy(handle);
            return 0;
        }
    }

    result = api.initFromSwapChain(handle, queue, &params);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        dbgLogWarn("xess-fg: InitFromSwapChain failed 0x%X", static_cast<unsigned>(result));
        dumpInfoQueue(device, "init");
        api.destroy(handle);
        return 0;
    }

    void* presentChain = nullptr;
    result = api.getSwapChainPtr(handle, __uuidof(IDXGISwapChain4), &presentChain);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || presentChain == nullptr)
    {
        dbgLogWarn("xess-fg: GetSwapChainPtr failed result=0x%X", static_cast<unsigned>(result));
        api.destroy(handle);
        return 0;
    }

        handle_ = handle;
        api_ = api;
        contextReady().store(true);
        width_ = width;
        height_ = height;
        // SDK 要求 HUD-less / UI 的格式与 back buffer 完全一致。
        {
            ComPtr<IDXGISwapChain1> sc1;
            DXGI_SWAP_CHAIN_DESC1 scDesc{};
            if (SUCCEEDED(static_cast<IDXGISwapChain*>(presentChain)->QueryInterface(IID_PPV_ARGS(&sc1))) &&
                sc1 != nullptr && SUCCEEDED(sc1->GetDesc1(&scDesc))) {
            }
        }
        // 新链 = 新会话：SDK 侧状态回到初始值，本地记账必须跟着清，否则会跳过 SetEnabled。
        appliedMode_ = -1;
        appliedFrames_ = -1;
        uiEnabled_ = false;
        tagFailLogged_ = false;
        notedDisabled_ = false;
        presentStatusFails_ = 0;
        presentId_ = 0;
        presented_ = 1;
        // XeLL 上下文是进程单例，换链不换它；帧号也**不能**归零。
        xell_ = xellContext(device);
        markerOpen_ = false;
        markerFails_ = 0;
        dbgLogInfo("xess-fg: chain wrapped %dx%d ui=hudless interp=%d",
                   width, height, maxInterp_);
        *outPresentChain = static_cast<IDXGISwapChain*>(presentChain);
        return 1;
    }

    void setFrameConstants(const mcdr_fg_constants& constants) override
    {
        constants_ = constants;
        hasConstants_ = true;
    }

    void setUiTexture(void* object) override {
        // UI 纹理 = GUI 合成目标；第一次拿到时顺带打开 SetUiCompositionState（init 的 uiMode 只是声明）。
        uiTexture_ = object;
    }

    void tagFrameInputs(const FgFrameInputs& in) override {
        // 逐帧 tag 的序列，顺序不能颠倒：
        //   1) SetPresentId（严格 +1，跳号会被 SDK 静默丢帧）+ 帧常量；2) 四条 TagFrameResource；
        //   3) UI 合成开关（拿到 UI 纹理时一次）；4) SetEnabled / SetNumInterpolatedFrames（仅变化时）。
        ++tagCalls_;
        if (handle_ == nullptr || in.cmdList == nullptr) {
            return;
        }
        // presentId 与 XeLL marker 共用 frameId_（由 frameStart 推进）：自增会让 marker 的 id 慢一帧。
        // frameId_ == 0 只可能出现在 frameStart 没被调用的路径上，退回自增保底。
        const uint32_t presentId = frameId_ != 0 ? frameId_ : ++presentId_;
        presentId_ = presentId;
        api_.setPresentId(handle_, presentId);
            // TagFrameConstants 是必需输入；presentId 必须与下面 TagFrameResource 用同一个，否则常量
            // 会算到另一帧上。
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
                api_.tagFrameConstants(handle_, presentId, &frameConstants);
            }
        // incomingState 必须是调用方给的**当前真实**状态（SDK 按它记录屏障，写错就与跟踪表错位）。
        // 没有 BACKBUFFER 那条：它只在 letterboxing 时定义插帧矩形，官方 sample 也只 tag HUDLESS/MV/DEPTH。
        // `lazy=true` → RV_UNTIL_NEXT_PRESENT 且不传命令列表（SDK 在 Present 时自己拷），只对 HUDLESS 用。
        auto tagResource = [&](xefg_swapchain_resource_type_t type, void* resource, D3D12_RESOURCE_STATES state,
                               bool lazy = false) {
            if (api_.tagFrameResource == nullptr) {
                return;
            }
            if (resource == nullptr) {
                return;
            }
            xefg_swapchain_d3d12_resource_data_t data{};
            data.type = type;
            data.validity = lazy ? XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT : XEFG_SWAPCHAIN_RV_ONLY_NOW;
            data.pResource = static_cast<ID3D12Resource*>(resource);
            data.incomingState = state;
            // `resourceSize` 必须显式给：全零在 SDK 眼里是 0x0 的合法区域，按它分配/拷贝会失败（每帧 -14）。
            // 约束：HUDLESS / UI / back buffer 同尺寸；MV 与 depth 同尺寸。
            const D3D12_RESOURCE_DESC desc =
                static_cast<ID3D12Resource*>(resource)->GetDesc();
            data.resourceBase = {0, 0};
            data.resourceSize = {static_cast<uint32_t>(desc.Width), desc.Height};
            const xefg_swapchain_result_t tagged = api_.tagFrameResource(handle_, lazy ? nullptr : in.cmdList, presentId, &data);
            if (tagged != XEFG_SWAPCHAIN_RESULT_SUCCESS && !tagFailLogged_) {
                tagFailLogged_ = true;
                dbgLogWarn("xess-fg: TagFrameResource(%d) failed 0x%X",
                           static_cast<int>(type), static_cast<unsigned>(tagged));
            }
        };
        tagResource(XEFG_SWAPCHAIN_RES_DEPTH, in.pDepth, in.depthState);
        tagResource(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, in.pMotionVectors, in.motionVectorState);
        // 世界层（无 HUD）就是 SR 的输出，用懒拷贝交给 SDK，避免在本帧渲染列表里插拷贝。
        if (kTagHudless) {
            tagResource(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, in.pColorOutput, in.colorState, /*lazy=*/true);
        }
        // init 的 uiMode 只是声明，合成开关初始是 DISABLED，必须显式打开；否则 UI 纹理不参与合成，
        // 插出来的帧会把 HUD 当世界层一起插（重影）。没有 UI 纹理就不打开。
        if (uiTexture_ != nullptr && !uiEnabled_ && api_.setUiCompositionState != nullptr) {
            const xefg_swapchain_result_t ui =
                api_.setUiCompositionState(handle_, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
            uiEnabled_ = (ui == XEFG_SWAPCHAIN_RESULT_SUCCESS);
            dbgLogInfo("xess-fg: ui composition %s result=0x%X %p",
                       uiEnabled_ ? "enabled" : "rejected", static_cast<unsigned>(ui), uiTexture_);
        }
        tagResource(XEFG_SWAPCHAIN_RES_UI, uiTexture_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // 描述符堆：只有 initFlags 要了 XEFG_SWAPCHAIN_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP 才需要
        // setDescriptorHeap；我们没要，SDK 自管。开关与倍率只在**生效值**变化时下发（每帧重设会被
        // SDK 当成状态抖动），且顺序不能反：enabled 之前设的插帧数会被忽略。
        if (appliedMode_ != mode_) {
            const xefg_swapchain_result_t on =
                api_.setEnabled(handle_, mode_ != 0 ? 1u : 0u);
            appliedMode_ = mode_;
            if (on != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                dbgLogWarn("xess-fg: SetEnabled(%d) failed result=0x%X",
                           mode_ != 0 ? 1 : 0, static_cast<unsigned>(on));
            }
        }
        if (mode_ != 0 && appliedFrames_ != frames_) {
            const xefg_swapchain_result_t nf =
                api_.setNumInterpolatedFrames(handle_, static_cast<uint32_t>(frames_ < 1 ? 1 : frames_));
            appliedFrames_ = frames_;
            if (nf != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                dbgLogWarn("xess-fg: SetNumInterpolatedFrames(%d) failed 0x%X",
                           frames_, static_cast<unsigned>(nf));
            }
        }
        if (tagCalls_ == 1) {
            dbgLogInfo("xess-fg: tag cmd=%p color=%p depth=%p mv=%p ui=%p mode=%d frames=%d",
                       static_cast<void*>(in.cmdList),
                       static_cast<const void*>(in.pColorOutput), static_cast<const void*>(in.pDepth),
                       static_cast<const void*>(in.pMotionVectors), uiTexture_, mode_, frames_);
        }
    }

    void unwrapSwapChain() override {
        if (handle_ == nullptr) {
            return;
        }
        // 销毁前先放掉自己这边的引用。真正的交换链引用在 surface 侧 —— 调用点必须先
        // `swapChain.Reset()` + `backBuffers.clear()`，否则 SDK 看到引用计数 ≠ 0。
        uiTexture_ = nullptr;
        uiEnabled_ = false;
        if (api_.destroy != nullptr) {
            api_.destroy(handle_);
        }
        handle_ = nullptr;
        api_ = XessFgApi{};
        contextReady().store(false);
        maxInterpMirror().store(0);
        maxInterp_ = 0;
        dbgLogInfo("xess-fg: released the wrapped chain");
    }

private:
    // XeLL 的 sleep：低延迟模式下由库算"这一帧该等多久"，必须是本帧第一条带新 frame_id 的调用。
    // 失败也继续 —— 少睡一帧只影响延迟，不影响正确性；前几次失败落日志。
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
    void* xell_ = nullptr;           // XeLL 上下文（进程单例，不持有所有权）
    uint32_t frameId_ = 0;           // XeLL marker 与 SetPresentId 共用的帧号（guide :240）
    bool markerOpen_ = false;        // 本帧的 RENDERSUBMIT_START 已发、PRESENT_END 未发
    int32_t markerFails_ = 0;
    int32_t sleepFails_ = 0;
    int32_t tagCalls_ = 0;
    uint32_t presentId_ = 0;
    int32_t mode_ = 0;
    int32_t frames_ = 1;
    int32_t maxInterp_ = 0;          // init 时 SDK 报的最大插帧数
    bool uiEnabled_ = false;         // SetUiCompositionState(ENABLED) 是否已成功下发
    bool tagFailLogged_ = false;
    bool notedDisabled_ = false;
    int32_t presentStatusFails_ = 0;
    uint64_t statusPolls_ = 0;
    uint32_t statusLogCounter_ = 0;
    int32_t appliedMode_ = -1;
    int32_t appliedFrames_ = -1;
    void* uiTexture_ = nullptr;
    mcdr_fg_constants constants_ = {};
    bool hasConstants_ = false;
    XessFgApi api_{};
    int32_t desiredMode_ = 0;
    int32_t desiredFrames_ = 1;
    int32_t width_ = 0;
    int32_t height_ = 0;
    int32_t presented_ = 1;
};

}  // namespace

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
    // deviceSupported 由 CreateContext(device) 检查；caps 只如实报告"现在有没有 context"。
    out.deviceSupported = contextReady().load() ? 1 : 0;
    static_cast<void>(0);
    // 链兼容：包装型后端只能在**应用原生链**上工作（`eUseDXGIFactoryProxy` 与"包装别人的链"互斥）。
    // 真正的"现在这条链能不能用"由 chainOwner 字段与 available 表达，这里恒 1。
    out.chainCompatible = 1;
    // XeSS-FG 不依赖 Reflex：低延迟走 XeLL（可选 DLL），所以 needsReflex = 0 是结构事实。
    out.needsReflex = 0;
    // needsHudless / needsUiResource = 1 的含义是"调用方必须逐帧喂这两份资源"。
    out.needsHudless = 1;
    out.needsUiResource = 1;
    // XeSS-FG 不替我们转屏障：`incomingState` 是我们声明"交给它时在哪个状态"，转换由我们发
    //（与 FSR-FG 同一条契约，DLSS-G 相反）。
    out.transitionsInputs = 1;
    // `GetProperties` 才给出真实上限；context 未建时按常见的 1 报（不虚报）。
    const int32_t knownMax = maxInterpMirror().load();
    out.maxGeneratedFrames = knownMax > 0 ? knownMax : 1;
    out.latencyProvider = MCDR_FG_LATENCY_XELL;
    out.reserved = 0;
    // available = "接线完成 + 运行库在"，不代表此刻已有 context（包装发生在链重建时）。
    out.available = (loaded && kWrapWired && kTaggingWired) ? 1 : 0;

    if (!loaded) {
        // note 只有 28 B：下面每句原因串必须压到 27 字符以内。
        writeCapsNote(out.note, sizeof(out.note), problem.empty() ? "libxess_fg.dll missing" : problem.c_str());
    } else if (out.chainCompatible == 0) {
        writeCapsNote(out.note, sizeof(out.note), "needs app-native chain");
    } else if (!kWrapWired) {
        writeCapsNote(out.note, sizeof(out.note), "XeSS-FG tags not wired");
    } else {
        writeCapsNote(out.note, sizeof(out.note), "XeSS-FG ready");
    }

    // 探测结果只在这里能看到（界面读 caps 但不落日志）：一次性打一行，三家后端格式对齐。
    static std::atomic<bool> loggedCaps{false};
    if (!loggedCaps.exchange(true)) {
        dbgLogInfo("xess-fg: caps run=%d dev=%d chain=%d ok=%d lat=%d \"%s\"",
                   out.runtimeLoaded, out.deviceSupported, out.chainCompatible, out.available, out.latencyProvider,
                   out.note);
    }
}

std::unique_ptr<IFrameGenProvider> makeXessFgProvider() {
    return std::make_unique<XessFgFrameGenProvider>();
}

}  // namespace d3d12_mc
