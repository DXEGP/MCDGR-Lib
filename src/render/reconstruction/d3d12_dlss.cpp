#include <windows.h>

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#if __has_include(<sl.h>)
#include <sl.h>
#include <sl_dlss.h>
#include <sl_reflex.h>
#include <sl_dlss_g.h>
#define MCDR_DLSS_HAVE_STREAMLINE 1
#else
#define MCDR_DLSS_HAVE_STREAMLINE 0
#endif

#include "mcdr_abi.h"
#include "mcdr_dlss_impl.h"
#include "mcdr_hdr_state.h"
extern "C" int32_t mcdr_dlss_invalidate_inputs(void);
import std;

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

#define MCDR_NGX_PROJECT_ID "7c1f4a52-9b3e-4d68-8f21-5a6c0e2d9b74"
#define MCDR_NGX_ENGINE_VERSION "1.0"
#define MCDR_NGX_APPDATA L".\\ngx"

namespace
{
    using LogFn = void (*)(const char* fmt, ...);
    LogFn gLogInfo = nullptr;
    LogFn gLogWarn = nullptr;
    LogFn gLogError = nullptr;

    using CreateTextureFn = int32_t (*)(void*, const DlssTextureDesc*, void**, void**);
    using DestroyTextureFn = void (*)(void*, void*);
    CreateTextureFn gCreateTexture = nullptr;
    DestroyTextureFn gDestroyTexture = nullptr;
    using PinTextureFn = int32_t (*)(void*, void*);
    PinTextureFn gPinTexture = nullptr;
    using ShutdownStreamlineFn = int32_t (*)(void);
    ShutdownStreamlineFn gShutdownStreamline = nullptr;
    std::atomic<bool> gStreamlineInputInvalidationFailed{false};

    void logInfo(const char* fmt, ...) {
        if (gLogInfo == nullptr) {
            return;
        }
        char line[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        gLogInfo("%s", line);
    }
    void logWarn(const char* fmt, ...) {
        if (gLogWarn == nullptr) {
            return;
        }
        char line[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        gLogWarn("%s", line);
    }
    void logError(const char* fmt, ...) {
        if (gLogError == nullptr) {
            return;
        }
        char line[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        gLogError("%s", line);
    }

    const char* ngxResultText(NVSDK_NGX_Result r) {
        switch (r) {
            case NVSDK_NGX_Result_Success: return "success";
            case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "feature not supported";
            case NVSDK_NGX_Result_FAIL_PlatformError: return "platform error";
            case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "feature not found";
            case NVSDK_NGX_Result_FAIL_InvalidParameter: return "invalid parameter";
            case NVSDK_NGX_Result_FAIL_NotInitialized: return "not initialized";
            case NVSDK_NGX_Result_FAIL_Denied: return "denied";
            case NVSDK_NGX_Result_FAIL_OutOfDate: return "out of date";
            case NVSDK_NGX_Result_FAIL_NotImplemented: return "not implemented";
            default: return nullptr;
        }
    }

    std::string ngxResultString(NVSDK_NGX_Result r) {
        char hex[32] = {};
        std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(r));
        if (const char* t = ngxResultText(r)) {
            return std::string(t) + " (" + hex + ")";
        }
        return std::string(hex);
    }
    static const double kTierNominalFactor[5] = {3.0, 2.0, 1.72, 1.5, 1.0};
    struct State
    {
        bool initAttempted = false;
        bool initOk = false;
        bool paramsOk = false;
        bool optimalOk = false;
        bool disabled = false;
        bool featureOk = false;
        bool createdThisFrame = false;
        bool createdWarned = false;
        bool evalOk = false;
        bool evalFailWarned = false;
        bool resetPending = false;
        bool enabled = false;
        uint64_t submitCount = 0;
        bool tooSmallWarned = false;
        ID3D12Device* device = nullptr;
        NVSDK_NGX_Parameter* params = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        uint32_t srViewport = 0;
        int32_t srViewportSlot = -1;
        bool srPrepared = false;
        std::optional<std::string> preparedFailure;
        ID3D12Resource* depthStencil = nullptr;
        ID3D12Resource* dlssOutput = nullptr;
        void* dlssOutputObject = nullptr;
        ID3D12Resource* externalOutput = nullptr;
        UINT dlssOutputWidth = 0;
        UINT dlssOutputHeight = 0;
        DXGI_FORMAT dlssOutputFormat = DXGI_FORMAT_UNKNOWN;
        DlssTransitionFn transitionFn = nullptr;
        void* transitionCtx = nullptr;
        ID3D12Resource* depthSnapshot = nullptr;
        void* depthSnapshotObject = nullptr;
        UINT depthSnapshotWidth = 0;
        UINT depthSnapshotHeight = 0;
        DXGI_FORMAT depthSnapshotFormat = DXGI_FORMAT_UNKNOWN;
        UINT depthSnapshotMips = 0;
        UINT depthSnapshotArray = 0;
        UINT depthSnapshotSamples = 0;
        UINT64 depthSnapshotFrame = 0;
        bool depthSnapshotWarned = false;

        bool slOptsApplied = false;
        int32_t slOptMode = -1;
        uint32_t slOptOutW = 0;
        uint32_t slOptOutH = 0;
        float slOptPreExposure = -1.0f;
        float slOptExposureScale = -1.0f;

        bool slEvalOptsValid = false;
        int32_t slEvalOptMode = -1;
        uint32_t slEvalOptOutW = 0;
        uint32_t slEvalOptOutH = 0;
        float slEvalOptPreExposure = -1.0f;
        float slEvalOptExposureScale = -1.0f;
        uint32_t slEvalOptQuality = 0xFFFFFFFFu;
        int32_t slEvalOptPreset = -1;
        bool slEvalOptHdr = false;

        ID3D12Resource* mvBuffer = nullptr;
        void* mvBufferObject = nullptr;
        void* runtimeModule = nullptr;
        UINT64 scratchBytes = 0;
        UINT32 mvCopyResourceState = D3D12_RESOURCE_STATE_COMMON;
        int32_t renderWidth = 0;
        int32_t renderHeight = 0;
        int32_t targetWidth = 0;
        int32_t targetHeight = 0;
        uint32_t qualityValue = 0;
        int requestedQuality = -1;
        int effectiveQuality = -1;
        int requestedPreset = 0;
        int effectivePreset = 0;
        bool presetRequestLogged = false;
        bool dlaaDowngraded = false;
        bool qualityIsExplicit = false;
        bool qualityRequestLogged = false;
        int32_t featureFlags = 0;
        std::string failure;
        std::string runtimePath;
        std::string optimalText;
        uint32_t optimalRenderW = 0;
        uint32_t optimalRenderH = 0;
        float optimalSharpness = 0.0f;
        int32_t lastRenderW = -1;
        int32_t lastRenderH = -1;
        int32_t lastTargetW = -1;
        int32_t lastTargetH = -1;
        uint32_t lastReset = 0xFFFFFFFFu;
        uint32_t lastFlags = 0xFFFFFFFFu;
        uint32_t lastExposureMode = 0xFFFFFFFFu;
        int32_t lastDepthFormat = 0x7FFFFFFF;
        float lastJitterX = 1e9f;
        float lastJitterY = 1e9f;
        float lastNear = 1e9f;
        float lastFar = 1e9f;
        std::wstring vendorDir;
        bool searchPathSet = false;
        bool envLogged = false;
    };

    State s;

    struct SrCandidate {
        NVSDK_NGX_Parameter* params = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        mcdr_dlss_input input{};
        uint32_t quality = 0;
        int32_t tier = -1;
        int32_t preset = 0;
        int32_t flags = 0;
        uint32_t viewport = 0;
        int32_t viewportSlot = -1;
        bool streamline = false;
        bool viewportTouched = false;
        bool ready = false;
        float preExposure = 1.0f;
        float exposureScale = 1.0f;
        bool hdr = false;
        int32_t mode = -1;
        bool tagsPresent = false;
    };

    bool gSrViewportSlots[3]{};
#if MCDR_DLSS_HAVE_STREAMLINE
    std::optional<sl::Constants> gSrAllocationConstants[3];
#endif
    uint32_t gSrCandidateCount = 0;

    std::wstring moduleDirectoryW() {
        HMODULE self = nullptr;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&moduleDirectoryW), &self) == FALSE ||
            self == nullptr) {
            return {};
        }
        wchar_t path[MAX_PATH * 2] = {};
        const DWORD n = ::GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
        if (n == 0 || n >= std::size(path)) {
            return {};
        }
        std::wstring w(path, n);
        const std::wstring::size_type slash = w.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            return {};
        }
        return w.substr(0, slash + 1);
    }

    std::wstring executableDirectoryW() {
        wchar_t path[MAX_PATH * 2] = {};
        const DWORD n = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (n == 0 || n >= std::size(path)) {
            return {};
        }
        std::wstring w(path, n);
        const std::wstring::size_type slash = w.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            return {};
        }
        return w.substr(0, slash + 1);
    }

    std::wstring currentDirectoryW() {
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = ::GetCurrentDirectoryW(static_cast<DWORD>(std::size(buf)), buf);
        if (n == 0 || n >= std::size(buf)) {
            return {};
        }
        return std::wstring(buf, n);
    }

    std::string narrow(const std::wstring& w) {
        if (w.empty()) {
            return {};
        }
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (need <= 0) {
            return {};
        }
        std::string out(static_cast<std::size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                              nullptr, nullptr);
        return out;
    }

    void widenSearchPath(const std::wstring& dir) {
        if (dir.empty()) {
            logWarn("DLSS vendor directory is empty; the DLL search path is left untouched");
            return;
        }
        const BOOL setDefault = ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                                          LOAD_LIBRARY_SEARCH_USER_DIRS);
        const DWORD setDefaultErr = setDefault ? 0u : ::GetLastError();
        const BOOL setDir = ::SetDllDirectoryW(dir.c_str());
        const DWORD setDirErr = setDir ? 0u : ::GetLastError();
        const DLL_DIRECTORY_COOKIE cookie = ::AddDllDirectory(dir.c_str());
        const DWORD addErr = (cookie != nullptr) ? 0u : ::GetLastError();
        s.searchPathSet = true;
        logInfo("DLSS search path widened for \"%s\": SetDefaultDllDirectories=%s(err=%lu) "
                "SetDllDirectoryW=%s(err=%lu) AddDllDirectory=%s(err=%lu)",
                narrow(dir).c_str(), setDefault ? "ok" : "failed", setDefaultErr,
                setDir ? "ok" : "failed", setDirErr, (cookie != nullptr) ? "ok" : "failed", addErr);
    }

    void logEnvOnce() {
        if (s.envLogged) {
            return;
        }
        s.envLogged = true;
        logInfo("DLSS load environment: moduleDir=\"%s\" cwd=\"%s\" exeDir=\"%s\" vendorDir=\"%s\" "
                "searchPathSet=%d",
                narrow(moduleDirectoryW()).c_str(), narrow(currentDirectoryW()).c_str(),
                narrow(executableDirectoryW()).c_str(), narrow(s.vendorDir).c_str(),
                s.searchPathSet ? 1 : 0);
    }

    HMODULE loadRuntimeDll() {
        std::wstring name = L"nvngx_dlss.dll";
        const std::wstring modDir = moduleDirectoryW();
        const std::wstring cwd = currentDirectoryW();
        const std::wstring exeDir = executableDirectoryW();
        struct Candidate
        {
            const wchar_t* what;
            std::wstring path;
            bool useEx;
            DWORD flags;
        };
        std::vector<Candidate> candidates;
        candidates.push_back({s.searchPathSet ? L"search-path(name)" : L"bare-name", name, false, 0});
        if (!s.vendorDir.empty()) {
            candidates.push_back({L"vendor-dir", s.vendorDir + L"\\" + name, false, 0});
        }
        if (!modDir.empty()) {
            candidates.push_back({L"module-dir", modDir + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        if (!cwd.empty()) {
            candidates.push_back({L"cwd-dxrmc", cwd + L"\\dxrmc\\" + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        if (!exeDir.empty()) {
            candidates.push_back({L"exeDir-dxrmc", exeDir + L"dxrmc\\" + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        if (!cwd.empty()) {
            candidates.push_back({L"cwd", cwd + L"\\" + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }

        HMODULE found = nullptr;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const Candidate& c = candidates[i];
            ::SetLastError(ERROR_SUCCESS);
            HMODULE mod = c.useEx ? ::LoadLibraryExW(c.path.c_str(), nullptr, c.flags)
                                  : ::LoadLibraryW(c.path.c_str());
            const DWORD err = (mod != nullptr) ? 0u : ::GetLastError();
            if (mod != nullptr) {
                found = mod;
                const std::wstring full = c.useEx ? c.path : std::wstring();
                if (!full.empty()) {
                    s.runtimePath = narrow(full);
                } else {
                    wchar_t p[MAX_PATH * 2] = {};
                    const DWORD n = ::GetModuleFileNameW(mod, p, static_cast<DWORD>(std::size(p)));
                    s.runtimePath = narrow(std::wstring(p, n));
                }
                logInfo("nvngx runtime loaded via %s (attempt %llu) -> \"%s\"", c.what,
                        static_cast<unsigned long long>(i + 1), s.runtimePath.c_str());
                break;
            }
            logWarn("nvngx runtime attempt %llu/%llu (%s) failed: \"%s\" -> err=%lu (%s)",
                    static_cast<unsigned long long>(i + 1),
                    static_cast<unsigned long long>(candidates.size()), c.what,
                    narrow(c.useEx ? c.path : std::wstring(name)).c_str(), err,
                    err == ERROR_MOD_NOT_FOUND ? "ERROR_MOD_NOT_FOUND"
                                               : (err == ERROR_FILE_NOT_FOUND ? "ERROR_FILE_NOT_FOUND"
                                                                              : "see winerror.h"));
        }
        if (found == nullptr) {
            logWarn("nvngx_dlss.dll could not be loaded from any candidate; DLSS will report "
                    "itself unavailable. Note: nvngx_dlss.dll imports only system DLLs, so "
                    "ERROR_MOD_NOT_FOUND here points at the path, not at a missing dependency.");
        }
        return found;
    }

    NVSDK_NGX_FeatureCommonInfo* gCommonInfo = nullptr;
    std::wstring* gPathListEntry = nullptr;
    const wchar_t** gPathListArray = nullptr;

    void rebuildCommonInfo(const std::wstring& dir) {
        if (gPathListEntry == nullptr) {
            gPathListEntry = new std::wstring();
        }
        *gPathListEntry = dir;
        if (gPathListArray == nullptr) {
            gPathListArray = new const wchar_t*[1];
        }
        gPathListArray[0] = gPathListEntry->c_str();
        if (gCommonInfo == nullptr) {
            gCommonInfo = new NVSDK_NGX_FeatureCommonInfo();
        }
        *gCommonInfo = NVSDK_NGX_FeatureCommonInfo{};
        gCommonInfo->PathListInfo.Path = gPathListArray;
        gCommonInfo->PathListInfo.Length = dir.empty() ? 0u : 1u;
        gCommonInfo->InternalData = nullptr;
        gCommonInfo->LoggingInfo = NVSDK_NGX_LoggingInfo{};
    }

    NVSDK_NGX_FeatureCommonInfo* commonInfoForNgx() {
        if (gCommonInfo == nullptr || gPathListEntry == nullptr ||
            gCommonInfo->PathListInfo.Length == 0u) {
            return nullptr;
        }
        return gCommonInfo;
    }

    const char* presetHintKey(uint32_t qualityValue) {
        switch (qualityValue) {
            case NVSDK_NGX_PerfQuality_Value_DLAA:
                return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
            case NVSDK_NGX_PerfQuality_Value_MaxQuality:
                return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
            case NVSDK_NGX_PerfQuality_Value_Balanced:
                return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
            case NVSDK_NGX_PerfQuality_Value_MaxPerf:
                return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
            case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
                return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
            case NVSDK_NGX_PerfQuality_Value_UltraQuality:
            default:
                return nullptr;
        }
    }

    std::string presetName(int32_t preset) {
        switch (preset) {
            case NVSDK_NGX_DLSS_Hint_Render_Preset_Default:
                return "Default (NGX picks per tier)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_E:
                return "E (deprecated, CNN)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_F:
                return "F (deprecated, CNN)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_J:
                return "J (transformer, less ghosting / more flicker)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_K:
                return "K (transformer, recommended image quality)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_L:
                return "L (transformer, Ultra Performance default)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_M:
                return "M (transformer, Performance default)";
            default:
                return std::to_string(preset);
        }
    }

    int32_t computeFeatureFlags(const mcdr_dlss_input& in) {
        int32_t flags = 0;
        {
            char ae[8] = {};
            if (in.pInExposure == nullptr && ::GetEnvironmentVariableA("MCDR_DLSS_AUTOEXPOSURE", ae, sizeof(ae)) > 0 &&
                ae[0] == '1') {
                flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
            }
        }
        if (in.depthInverted != 0) {
            flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_DepthInverted);
        }
        auto* colorResource = reinterpret_cast<ID3D12Resource*>(in.pInColor);
        const DXGI_FORMAT colorFormat = colorResource ? colorResource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
        if (colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || colorFormat == DXGI_FORMAT_R32G32B32A32_FLOAT ||
            colorFormat == DXGI_FORMAT_R11G11B10_FLOAT) {
            flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_IsHDR);
        }
        if (in.renderWidth != in.targetWidth || in.renderHeight != in.targetHeight) {
            flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
        }
        return flags;
    }

    std::optional<std::string> preparedSrMismatch(const mcdr_dlss_input& in) {
        if (in.renderWidth != s.renderWidth || in.renderHeight != s.renderHeight)
            return std::format("prepared render mismatch: actual={}x{}, prepared={}x{}",
                in.renderWidth, in.renderHeight, s.renderWidth, s.renderHeight);
        if (in.targetWidth != s.targetWidth || in.targetHeight != s.targetHeight)
            return std::format("prepared target mismatch: actual={}x{}, prepared={}x{}",
                in.targetWidth, in.targetHeight, s.targetWidth, s.targetHeight);
        if (s.requestedQuality != s.effectiveQuality)
            return std::format("prepared tier mismatch: requested={}, prepared={}",
                s.requestedQuality, s.effectiveQuality);
        if (s.requestedPreset != s.effectivePreset)
            return std::format("prepared preset mismatch: requested={}, prepared={}",
                s.requestedPreset, s.effectivePreset);
        const auto flags = computeFeatureFlags(in);
        if (flags != s.featureFlags)
            return std::format("prepared flags mismatch: actual=0x{:X}, prepared=0x{:X}",
                flags, s.featureFlags);
        return std::nullopt;
    }
    void transition(ID3D12Resource* res, D3D12_RESOURCE_STATES after) {
        if (res == nullptr) {
            return;
        }
        if (s.transitionFn == nullptr) {
            return;
        }
        s.transitionFn(s.transitionCtx, reinterpret_cast<void*>(res), static_cast<int32_t>(after));
    }

    bool createTrackedTexture(void* ctx, UINT width, UINT height, DXGI_FORMAT format, UINT mips,
                              UINT arraySize, UINT samples, UINT flags, void** outObject,
                              ID3D12Resource** outResource) {
        if (outObject != nullptr) *outObject = nullptr;
        if (outResource != nullptr) *outResource = nullptr;
        if (outObject == nullptr || outResource == nullptr) return false;
        if (gCreateTexture == nullptr) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                logWarn("the device module injected no texture-create entry — private DLSS "
                        "textures are unavailable (older DLL/JVM pairing); only JVM-supplied "
                        "resources are used");
            }
            return false;
        }
        DlssTextureDesc d;
        d.width = width;
        d.height = height;
        d.dxgiFormat = static_cast<uint32_t>(format);
        d.mipLevels = (mips > 1) ? mips : 1;
        d.arraySize = (arraySize > 1) ? arraySize : 1;
        d.sampleCount = (samples > 0) ? samples : 1;
        d.flags = flags;
        void* obj = nullptr;
        void* res = nullptr;
        if (gCreateTexture(ctx, &d, &obj, &res) != 1 || obj == nullptr || res == nullptr) {
            return false;
        }
        *outObject = obj;
        *outResource = static_cast<ID3D12Resource*>(res);
        return true;
    }

    void destroyTrackedTexture(void* ctx, void** object) {
        if (object != nullptr && *object != nullptr && gDestroyTexture != nullptr) {
            gDestroyTexture(ctx, *object);
        }
        if (object != nullptr) {
            *object = nullptr;
        }
    }

    bool createScratchBuffer(UINT64 bytes) {
        if (s.depthStencil != nullptr && s.scratchBytes >= bytes) {
            return true;
        }
        if (s.depthStencil != nullptr) {
            s.depthStencil->Release();
            s.depthStencil = nullptr;
        }
        s.scratchBytes = 0;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc;
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        const HRESULT hr = s.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&s.depthStencil));
        if (FAILED(hr)) {
            s.depthStencil = nullptr;
            logWarn("DLSS scratch buffer creation failed (%llu bytes, hr=0x%08X); DLSS disabled",
                    static_cast<unsigned long long>(bytes), static_cast<unsigned>(hr));
            return false;
        }
        s.scratchBytes = bytes;
        return true;
    }

    ID3D12Resource* createDlssOutput(UINT width, UINT height, DXGI_FORMAT format) {
        if (s.dlssOutput != nullptr && s.dlssOutputWidth == width && s.dlssOutputHeight == height &&
            s.dlssOutputFormat == format) {
            return s.dlssOutput;
        }
        destroyTrackedTexture(s.transitionCtx, &s.dlssOutputObject);
        s.dlssOutput = nullptr;
        s.dlssOutputWidth = 0;
        s.dlssOutputHeight = 0;
        s.dlssOutputFormat = DXGI_FORMAT_UNKNOWN;
        if (width == 0 || height == 0) {
            return nullptr;
        }
        void* obj = nullptr;
        ID3D12Resource* res = nullptr;
        if (!createTrackedTexture(s.transitionCtx, width, height, format, 1, 1, 1,
                                  static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), &obj,
                                  &res)) {
            logWarn("DLSS private UAV output creation failed (%ux%u fmt=%d) — the device "
                    "module refused or failed to create it; the existing path stays in effect",
                    width, height, static_cast<int>(format));
            return nullptr;
        }
        s.dlssOutputObject = obj;
        s.dlssOutput = res;
        s.dlssOutputWidth = width;
        s.dlssOutputHeight = height;
        s.dlssOutputFormat = format;
        logInfo("DLSS private UAV output created by the device module: %ux%u fmt=%d "
                "(ALLOW_UNORDERED_ACCESS, tracked) — DLSS writes here, then the result is copied "
                "into the JVM's target",
                width, height, static_cast<int>(format));
        return s.dlssOutput;
    }

    void NVSDK_CONV dlssNgxLogCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel,
                                       NVSDK_NGX_Feature sourceComponent) {
        (void)loggingLevel;
        if (message != nullptr) {
            logInfo("NGX[%d] %s", static_cast<int>(sourceComponent), message);
        }
    }

    std::wstring buildAppDataPath() {
        std::wstring own = moduleDirectoryW();
        while (!own.empty() && (own.back() == L'\\' || own.back() == L'/')) {
            own.pop_back();
        }
        if (!own.empty() && ::GetFileAttributesW(own.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return own;
        }
        std::wstring fallback = MCDR_NGX_APPDATA;
        const DWORD attrs = ::GetFileAttributesW(fallback.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            if (::CreateDirectoryW(fallback.c_str(), nullptr) == FALSE &&
                ::GetLastError() != ERROR_ALREADY_EXISTS) {
                logWarn("could not create the NGX application-data directory \"%s\" (err=%lu); "
                        "falling back to the process working directory",
                        narrow(fallback).c_str(), ::GetLastError());
                return L".";
            }
            logInfo("created the NGX application-data directory \"%s\"",
                    narrow(fallback).c_str());
        }
        return fallback;
    }

#if MCDR_DLSS_HAVE_STREAMLINE
    std::wstring dlssModuleDirectory();
#endif

    bool dlssImplStreamline() {
        char buf[16] = {};
        const DWORD n = ::GetEnvironmentVariableA("MCDR_DLSS_IMPL", buf, 16);
        if (n > 0) {
            return _stricmp(buf, "streamline") == 0;
        }
        char legacy[8] = {};
        if (::GetEnvironmentVariableA("MCDR_STREAMLINE", legacy, 8) == 1 && legacy[0] == '1') {
            return true;
        }
#if MCDR_DLSS_HAVE_STREAMLINE
        static bool interposerSeen = false;
        if (!interposerSeen) {
            const std::wstring dir = dlssModuleDirectory();
            if (!dir.empty()) {
                const std::wstring path = dir + L"sl.interposer.dll";
                if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    interposerSeen = true;
                }
            }
        }
        return interposerSeen;
#else
        return false;
#endif
    }

#if MCDR_DLSS_HAVE_STREAMLINE
    using PFn_SlDLSSSetOptions = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSOptions&);

    std::wstring dlssModuleDirectory() {
        HMODULE self = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(&dlssModuleDirectory), &self)) {
            return {};
        }
        wchar_t path[MAX_PATH] = {};
        if (!::GetModuleFileNameW(self, path, MAX_PATH)) return {};
        std::wstring pathStr(path);
        const auto slash = pathStr.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : pathStr.substr(0, slash + 1);
    }

    PFn_SlDLSSSetOptions slDlssSetOptionsFn(bool reset = false) {
        static PFn_SlDLSSSetOptions fn = nullptr;
        static bool resolved = false;
        if (reset) {
            fn = nullptr;
            resolved = false;
            return fn;
        }
        if (resolved) return fn;
        resolved = true;
        fn = []() -> PFn_SlDLSSSetOptions {
            const std::wstring dir = dlssModuleDirectory();
            if (dir.empty()) return nullptr;
            HMODULE sl = ::LoadLibraryW((dir + L"sl.interposer.dll").c_str());
            if (sl == nullptr) return nullptr;
            using PFn_GetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
            auto getFeatureFunction = reinterpret_cast<PFn_GetFeatureFunction>(
                ::GetProcAddress(sl, "slGetFeatureFunction"));
            if (getFeatureFunction == nullptr) return nullptr;
            void* raw = nullptr;
            if (getFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", raw) != sl::Result::eOk) {
                return nullptr;
            }
            return reinterpret_cast<PFn_SlDLSSSetOptions>(raw);
        }();
        return fn;
    }

    int32_t streamlineUpgradeInterface(void** baseInterface) {
        if (baseInterface == nullptr || *baseInterface == nullptr) return 0;
        using PFn_Upgrade = sl::Result (*)(void**);
        static PFn_Upgrade const fn = []() -> PFn_Upgrade {
            const std::wstring dir = dlssModuleDirectory();
            if (dir.empty()) return nullptr;
            HMODULE sl = ::LoadLibraryW((dir + L"sl.interposer.dll").c_str());
            if (sl == nullptr) return nullptr;
            return reinterpret_cast<PFn_Upgrade>(::GetProcAddress(sl, "slUpgradeInterface"));
        }();
        if (fn == nullptr) return 0;
        void* const native = *baseInterface;
        void* upgraded = native;
        if (fn(&upgraded) != sl::Result::eOk || upgraded == nullptr) {
            *baseInterface = native;
            return 0;
        }
        *baseInterface = upgraded;
        return upgraded != native ? 1 : 0;
    }

    uint32_t resolveQualityValue(const mcdr_dlss_input& in);

    std::optional<sl::DLSSOptions> srStreamlineOptions(const mcdr_dlss_input& in,
                                                     uint32_t quality, int32_t preset) {
        if (preset != 0 && preset != 5 && preset != 6 && preset != 10 &&
            preset != 11 && preset != 12 && preset != 13) return std::nullopt;
        auto* color = reinterpret_cast<ID3D12Resource*>(in.pInColor);
        if (color == nullptr) return std::nullopt;
        sl::DLSSOptions options{};
        switch (quality) {
            case NVSDK_NGX_PerfQuality_Value_DLAA: options.mode = sl::DLSSMode::eDLAA; break;
            case NVSDK_NGX_PerfQuality_Value_MaxQuality: options.mode = sl::DLSSMode::eMaxQuality; break;
            case NVSDK_NGX_PerfQuality_Value_Balanced: options.mode = sl::DLSSMode::eBalanced; break;
            case NVSDK_NGX_PerfQuality_Value_UltraPerformance: options.mode = sl::DLSSMode::eUltraPerformance; break;
            default: options.mode = sl::DLSSMode::eMaxPerformance; break;
        }
        options.outputWidth = static_cast<uint32_t>(in.targetWidth);
        options.outputHeight = static_cast<uint32_t>(in.targetHeight);
        options.preExposure = in.preExposure > 0.0f ? in.preExposure : 1.0f;
        options.exposureScale = in.exposureScale > 0.0f ? in.exposureScale : 1.0f;
        const auto selected = static_cast<sl::DLSSPreset>(preset);
        options.dlaaPreset = selected;
        options.qualityPreset = selected;
        options.balancedPreset = selected;
        options.performancePreset = selected;
        options.ultraPerformancePreset = selected;
        options.useAutoExposure = sl::Boolean::eFalse;
        const auto format = color->GetDesc().Format;
        options.colorBuffersHDR = format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
            format == DXGI_FORMAT_R32G32B32A32_FLOAT || format == DXGI_FORMAT_R11G11B10_FLOAT
                ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        options.alphaUpscalingEnabled = sl::Boolean::eFalse;
        return options;
    }

    int32_t streamlineApplyOptions(const mcdr_dlss_input& in) {
        if (s.srPrepared) return 0;
        const PFn_SlDLSSSetOptions fn = slDlssSetOptionsFn();
        if (fn == nullptr) return -1;
        sl::DLSSOptions opt{};
        switch (s.qualityValue) {
            case NVSDK_NGX_PerfQuality_Value_DLAA:       opt.mode = sl::DLSSMode::eDLAA; break;
            case NVSDK_NGX_PerfQuality_Value_MaxQuality: opt.mode = sl::DLSSMode::eMaxQuality; break;
            case NVSDK_NGX_PerfQuality_Value_Balanced:   opt.mode = sl::DLSSMode::eBalanced; break;
            case NVSDK_NGX_PerfQuality_Value_UltraPerformance: opt.mode = sl::DLSSMode::eUltraPerformance; break;
            default:                                     opt.mode = sl::DLSSMode::eMaxPerformance; break;
        }
        opt.outputWidth = static_cast<uint32_t>(in.targetWidth);
        opt.outputHeight = static_cast<uint32_t>(in.targetHeight);
        opt.preExposure = in.preExposure > 0.0f ? in.preExposure : 1.0f;
        opt.exposureScale = in.exposureScale > 0.0f ? in.exposureScale : 1.0f;
        opt.useAutoExposure = sl::Boolean::eFalse;
        const int32_t modeAsInt = static_cast<int32_t>(opt.mode);
        if (s.slOptsApplied && s.slOptMode == modeAsInt && s.slOptOutW == opt.outputWidth &&
            s.slOptOutH == opt.outputHeight && s.slOptPreExposure == opt.preExposure &&
            s.slOptExposureScale == opt.exposureScale) {
            return 0;
        }
        const sl::ViewportHandle viewport(s.srViewport);
        const sl::Result r = fn(viewport, opt);
        if (r != sl::Result::eOk) {
            logWarn("DLSS impl=streamline — slDLSSSetOptions result=0x%X (mode=%d, out=%ux%u, "
                "render=%dx%d)",
                static_cast<unsigned>(r), static_cast<int>(opt.mode), opt.outputWidth,
                opt.outputHeight, static_cast<unsigned>(in.renderWidth),
                static_cast<unsigned>(in.renderHeight));
            return static_cast<int32_t>(r);
        }
        s.slOptsApplied = true;
        s.slOptMode = modeAsInt;
        s.slOptOutW = opt.outputWidth;
        s.slOptOutH = opt.outputHeight;
        s.slOptPreExposure = opt.preExposure;
        s.slOptExposureScale = opt.exposureScale;
        logInfo("slDLSSSetOptions applied (mode=%d, out=%ux%u, preExposure=%.3f, "
                "exposureScale=%.3f; later frames reuse it until one of these changes)",
                modeAsInt, opt.outputWidth, opt.outputHeight, opt.preExposure, opt.exposureScale);
        return 0;
    }
#endif

    bool ensureInit(ID3D12Device* device) {
        if (s.initAttempted) {
            return s.initOk && s.paramsOk;
        }
        s.initAttempted = true;
        s.device = device;
        if (dlssImplStreamline()) {
            logInfo("DLSS impl=streamline — native NGX init skipped "
                    "(Streamline owns the runtime; evaluate lands in phase 2)");
            s.failure = "streamline mode: native NGX init skipped by design";
            return false;
        }

        const std::wstring appDataPath = buildAppDataPath();
        logInfo("NGX Init (sdkVersion=NVSDK_NGX_Version_API=0x%X, projectId=\"%s\", "
                "engineType=%d, engineVersion=\"%s\", appDataPath=\"%s\", featureInfo=%s)",
                static_cast<unsigned>(NVSDK_NGX_Version_API), MCDR_NGX_PROJECT_ID,
                static_cast<int>(NVSDK_NGX_ENGINE_TYPE_CUSTOM), MCDR_NGX_ENGINE_VERSION,
                narrow(appDataPath).c_str(), commonInfoForNgx() != nullptr ? "pathlist" : "null");
        const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(MCDR_NGX_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, MCDR_NGX_ENGINE_VERSION, appDataPath.c_str(), device, commonInfoForNgx(), NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(r)) {
            s.failure = "NVSDK_NGX_D3D12_Init_with_ProjectID";
            logWarn("DLSS unavailable — NVSDK_NGX_D3D12_Init_with_ProjectID failed: %s "
                    "(appDataPath=\"%s\", featureInfo=%s)",
                    ngxResultString(r).c_str(), narrow(appDataPath).c_str(),
                    commonInfoForNgx() != nullptr ? "pathlist" : "null");
            return false;
        }
        s.initOk = true;

        NVSDK_NGX_Parameter* params = nullptr;
        const NVSDK_NGX_Result pr = NVSDK_NGX_D3D12_GetCapabilityParameters(&params);
        if (NVSDK_NGX_FAILED(pr) || params == nullptr) {
            s.failure = "NVSDK_NGX_D3D12_GetCapabilityParameters";
            logWarn("DLSS unavailable — GetCapabilityParameters failed: %s",
                    ngxResultString(pr).c_str());
            return false;
        }
        s.params = params;
        s.paramsOk = true;

        {
            int available = -1, needsDriver = -1, initResult = 0;
            unsigned int minMajor = 0, minMinor = 0;
            const NVSDK_NGX_Result ra = NVSDK_NGX_Parameter_GetI(
                params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
            const NVSDK_NGX_Result rn = NVSDK_NGX_Parameter_GetI(
                params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
            const NVSDK_NGX_Result rmj = NVSDK_NGX_Parameter_GetUI(
                params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
            const NVSDK_NGX_Result rmn = NVSDK_NGX_Parameter_GetUI(
                params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
            const NVSDK_NGX_Result ri = NVSDK_NGX_Parameter_GetI(
                params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
            logInfo("NGX capabilities: available=%d(%s) needsUpdatedDriver=%d(%s) "
                    "minDriverVersion=%u.%u(%s/%s) featureInitResult=0x%08X(%s)",
                    available, ngxResultString(ra).c_str(), needsDriver, ngxResultString(rn).c_str(),
                    minMajor, minMinor, ngxResultString(rmj).c_str(), ngxResultString(rmn).c_str(),
                    static_cast<unsigned>(initResult), ngxResultString(ri).c_str());
            if (available == 0 || needsDriver == 1) {
                logWarn("NGX says DLSS-SR is NOT usable on this system "
                        "(available=%d needsUpdatedDriver=%d minDriverVersion=%u.%u "
                        "featureInitResult=0x%08X)",
                        available, needsDriver, minMajor, minMinor, static_cast<unsigned>(initResult));
            }
        }

        logInfo("DLSS/NGX initialised (project id \"%s\", engine version \"%s\", appdata "
                "\".\\ngx\", SDK API version 0x%08X)",
                MCDR_NGX_PROJECT_ID, MCDR_NGX_ENGINE_VERSION,
                static_cast<unsigned>(NVSDK_NGX_Version_API));
        {
            unsigned int ow = 0, oh = 0, mw = 0, mh = 0, nw = 0, nh = 0;
            float sharp = 0.0f;
            const NVSDK_NGX_Result orr = NGX_DLSS_GET_OPTIMAL_SETTINGS(
                params, 1280, 720, NVSDK_NGX_PerfQuality_Value_MaxQuality, &ow, &oh, &mw, &mh, &nw,
                &nh, &sharp);
            if (!NVSDK_NGX_FAILED(orr)) {
                logInfo("DLSS optimal settings: MaxQuality 1280x720 -> optimal %ux%u "
                        "(range %ux%u .. %ux%u), sharpness %.3f",
                        ow, oh, nw, nh, mw, mh, static_cast<double>(sharp));
            } else {
                logWarn("DLSS optimal-settings query failed: %s", ngxResultString(orr).c_str());
            }
        }
        return true;
    }
    uint32_t chooseQuality(NVSDK_NGX_Parameter* params, uint32_t targetW, uint32_t targetH,
                           uint32_t renderW, uint32_t renderH, int forceQuality = -1) {
        static const NVSDK_NGX_PerfQuality_Value kValues[] = {
            NVSDK_NGX_PerfQuality_Value_MaxPerf,      NVSDK_NGX_PerfQuality_Value_Balanced,
            NVSDK_NGX_PerfQuality_Value_MaxQuality,   NVSDK_NGX_PerfQuality_Value_UltraPerformance,
            NVSDK_NGX_PerfQuality_Value_UltraQuality, NVSDK_NGX_PerfQuality_Value_DLAA,
        };
        static const char* kNames[] = {"MaxPerf",  "Balanced",     "MaxQuality",
                                       "UltraPerformance", "UltraQuality", "DLAA"};
        const float want =
            (targetW > 0) ? (static_cast<float>(renderW) / static_cast<float>(targetW)) : 1.0f;
        const std::size_t forcedIdx =
            (forceQuality >= 0) ? static_cast<std::size_t>(forceQuality) : 0;
        const bool forced = (forceQuality >= 0) && (forcedIdx < std::size(kValues));
        uint32_t best = static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality);
        float bestErr = 1e9f;
        unsigned int bestW = renderW, bestH = renderH;
        float bestSharp = 0.0f;
        std::string table;
        for (std::size_t i = 0; i < std::size(kValues); ++i) {
            unsigned int ow = 0, oh = 0, mw = 0, mh = 0, nw = 0, nh = 0;
            float sharp = 0.0f;
            const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
                params, targetW, targetH, kValues[i], &ow, &oh, &mw, &mh, &nw, &nh, &sharp);
            if (NVSDK_NGX_FAILED(r) || ow == 0 || oh == 0) {
                continue;
            }
            const float ratio = static_cast<float>(ow) / static_cast<float>(targetW);
            const float err = (ratio > want) ? (ratio - want) : (want - ratio);
            char row[96] = {};
            std::snprintf(row, sizeof(row), "%s=%ux%u(%.3f) ", kNames[i], ow, oh,
                          static_cast<double>(ratio));
            table += row;
            if (forced) {
                if (i == forcedIdx) {
                    best = static_cast<uint32_t>(kValues[i]);
                    bestW = ow;
                    bestH = oh;
                    bestSharp = sharp;
                    bestErr = 0.0f;
                }
                continue;
            }
            if (err < bestErr) {
                bestErr = err;
                best = static_cast<uint32_t>(kValues[i]);
                bestW = ow;
                bestH = oh;
                bestSharp = sharp;
            }
        }
        if (!table.empty()) {
            logInfo("DLSS optimal render sizes at %ux%u: %s", targetW, targetH, table.c_str());
        }
        if (forced) {
            logInfo("DLSS quality tier is EXPLICIT (user button): %s -> sdk optimal %ux%u, "
                    "sharpness %.3f (render %ux%u target %ux%u, ratio would have picked "
                    "\"nearest\")",
                    kNames[forcedIdx], bestW, bestH, static_cast<double>(bestSharp), renderW,
                    renderH, targetW, targetH);
        }
        if (bestErr > 1e8f) {
            s.optimalOk = false;
            return static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality);
        }
        s.optimalOk = true;
        s.optimalRenderW = bestW;
        s.optimalRenderH = bestH;
        s.optimalSharpness = bestSharp;
        {
            char text[192] = {};
            std::snprintf(text, sizeof(text),
                          "target %ux%u, render %ux%u -> quality %s (sdk optimal %ux%u, sharpness "
                          "%.3f)",
                          targetW, targetH, renderW, renderH, kNames[best], bestW, bestH,
                          static_cast<double>(bestSharp));
            s.optimalText = text;
        }
        return best;
    }

    uint32_t resolveQualityValue(const mcdr_dlss_input& in) {
        static const uint32_t kTierToNgxShared[5] = {
            static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_UltraPerformance),
            static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxPerf),
            static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_Balanced),
            static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality),
            static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA),
        };
        int resolvedTier = s.requestedQuality;
        if (resolvedTier == 4 &&
            (in.renderWidth != in.targetWidth || in.renderHeight != in.targetHeight)) {
            resolvedTier = 2;
        }
        if (resolvedTier < 0 && in.renderWidth > 0 && in.targetWidth > 0) {
            const double factor = static_cast<double>(in.targetWidth) / static_cast<double>(in.renderWidth);
            double bestErr = 1e9;
            for (int32_t t = 0; t <= 4; ++t) {
                const double err = std::abs(kTierNominalFactor[t] - factor);
                if (err < bestErr) { bestErr = err; resolvedTier = t; }
            }
        }
        if (resolvedTier >= 0 && resolvedTier <= 4 && in.renderWidth > 0 && in.targetWidth > 0) {
            const double factor = static_cast<double>(in.targetWidth) / static_cast<double>(in.renderWidth);
            const double nominal = kTierNominalFactor[resolvedTier];
            if (s.requestedQuality >= 0 && factor > nominal * 1.10) {
                int32_t adopted = resolvedTier;
                for (int32_t t = 0; t <= 4; ++t) {
                    if (kTierNominalFactor[t] >= factor) { adopted = t; break; }
                }
                if (adopted != resolvedTier) {
                    if (!s.tooSmallWarned) {
                        s.tooSmallWarned = true;
                        logError("tier %d (%.2fx nominal) cannot describe render %dx%d -> target %dx%d (%.2fx); asking for tier %d instead",
                                 resolvedTier, nominal, in.renderWidth, in.renderHeight,
                                 in.targetWidth, in.targetHeight, factor, adopted);
                    }
                    resolvedTier = adopted;
                }
            }
            const uint32_t needW = static_cast<uint32_t>(std::ceil(static_cast<double>(in.targetWidth) / kTierNominalFactor[resolvedTier]));
            const uint32_t needH = static_cast<uint32_t>(std::ceil(static_cast<double>(in.targetHeight) / kTierNominalFactor[resolvedTier]));
            const auto tooSmall = [](uint32_t have, uint32_t need) {
                return static_cast<double>(have) < static_cast<double>(need) * 0.995;
            };
            if (tooSmall(static_cast<uint32_t>(in.renderWidth), needW) ||
                tooSmall(static_cast<uint32_t>(in.renderHeight), needH)) {
                if (!s.tooSmallWarned) {
                    s.tooSmallWarned = true;
                    logError("render %ux%u is smaller than tier %d nominal input (%ux%u at %.2fx) — NGX answers with a dynamic scaling error and an empty texture, i.e. a black world with a working GUI; raise the render scale",
                             in.renderWidth, in.renderHeight, resolvedTier, needW, needH,
                             kTierNominalFactor[resolvedTier]);
                }
            } else {
                s.tooSmallWarned = false;
            }
            s.qualityValue = kTierToNgxShared[resolvedTier];
            s.qualityIsExplicit = (s.requestedQuality >= 0);
            return s.qualityValue;
        }
        s.qualityValue = static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality);
        s.qualityIsExplicit = false;
        return s.qualityValue;
    }

    bool destroyFeature() {
        if (s.feature != nullptr) {
            const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_ReleaseFeature(s.feature);
            if (NVSDK_NGX_FAILED(r)) {
                logWarn("NVSDK_NGX_D3D12_ReleaseFeature failed: %s",
                        ngxResultString(r).c_str());
                return false;
            }
            s.feature = nullptr;
        }
        s.featureOk = false;
        s.evalOk = false;
        s.slOptsApplied = false;
        s.slEvalOptsValid = false;
        return true;
    }

    bool ensureFeature(ID3D12GraphicsCommandList* cmd, const mcdr_dlss_input& in) {
        if (s.createdWarned) {
            return false;
        }
        const bool sizeChanged =
            (in.renderWidth != s.renderWidth) || (in.renderHeight != s.renderHeight) ||
            (in.targetWidth != s.targetWidth) || (in.targetHeight != s.targetHeight);
        int resolvedTier = s.requestedQuality;
        const bool dlaaMismatch =
            (resolvedTier == 4) && (in.renderWidth != in.targetWidth ||
                                    in.renderHeight != in.targetHeight);
        if (dlaaMismatch) {
            resolvedTier = 2;
        }
        if (dlaaMismatch != s.dlaaDowngraded) {
            s.dlaaDowngraded = dlaaMismatch;
            if (dlaaMismatch) {
                logError("DLAA (tier 3) requested but render %dx%d != target %dx%d — falling "
                         "back to the Quality tier for this round (accepted as the baseline). Set "
                         "the render scale to 100%% to use DLAA.",
                         in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight);
            } else if (s.requestedQuality == 3) {
                logInfo("DLAA (tier 3) accepted: render %dx%d == target %dx%d",
                        in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight);
            }
        }
        if (resolvedTier >= 0 && resolvedTier <= 4 && in.renderWidth > 0 && in.targetWidth > 0) {
            const double factor = static_cast<double>(in.targetWidth) / static_cast<double>(in.renderWidth);
            const double nominal = kTierNominalFactor[resolvedTier];
            if (factor > nominal * 1.10) {
                int32_t adopted = resolvedTier;
                for (int32_t t = 0; t <= 4; ++t) {
                    if (kTierNominalFactor[t] >= factor) {
                        adopted = t;
                        break;
                    }
                }
                if (adopted != resolvedTier) {
                    logWarn("tier %d (%.2fx nominal) cannot describe render %dx%d -> target %dx%d "
                            "(%.2fx); asking NGX for tier %d instead",
                            resolvedTier, nominal, in.renderWidth, in.renderHeight, in.targetWidth,
                            in.targetHeight, factor, adopted);
                    resolvedTier = adopted;
                }
            }
        }
        if (in.renderWidth > 0 && in.targetWidth > 0 && resolvedTier >= 0 && resolvedTier <= 4) {
            const double nominal = kTierNominalFactor[resolvedTier];
            const uint32_t needW = static_cast<uint32_t>(std::ceil(static_cast<double>(in.targetWidth) / nominal));
            const uint32_t needH = static_cast<uint32_t>(std::ceil(static_cast<double>(in.targetHeight) / nominal));
            const auto tooSmall = [](uint32_t have, uint32_t need) {
                return static_cast<double>(have) < static_cast<double>(need) * 0.995;
            };
            if (tooSmall(static_cast<uint32_t>(in.renderWidth), needW) ||
                tooSmall(static_cast<uint32_t>(in.renderHeight), needH)) {
                if (!s.tooSmallWarned) {
                    s.tooSmallWarned = true;
                    logError("render %ux%u is smaller than tier %d nominal input (%ux%u at %.2fx) — "
                             "NGX answers with a dynamic scaling error and an empty texture, i.e. a black "
                             "world with a working GUI; raise the render scale",
                             in.renderWidth, in.renderHeight, resolvedTier, needW, needH, nominal);
                }
            } else {
                s.tooSmallWarned = false;
            }
        }
        const bool qualityChanged =
            (resolvedTier >= 0) && (resolvedTier != s.effectiveQuality);
        const bool presetChanged = (s.requestedPreset != s.effectivePreset);
        const bool flagsChanged = s.featureFlags != computeFeatureFlags(in);
        if (s.featureOk && !sizeChanged && !qualityChanged && !presetChanged && !flagsChanged) {
            return true;
        }
        if (sizeChanged || qualityChanged || presetChanged || flagsChanged) {
            if (!destroyFeature()) return false;
            s.resetPending = true;
        }
        if (s.params == nullptr) {
            return false;
        }

        if (resolvedTier >= 0) {
            
static const uint32_t kTierToNgx[5] = {
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_UltraPerformance),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxPerf),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_Balanced),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA),
            };
            const int tier = resolvedTier;
            if (tier < 0 || tier > 4) {
                return false;
            }
            s.qualityValue = kTierToNgx[tier];
            s.qualityIsExplicit = true;
            chooseQuality(s.params, static_cast<uint32_t>(in.targetWidth),
                          static_cast<uint32_t>(in.targetHeight),
                          static_cast<uint32_t>(in.renderWidth),
                          static_cast<uint32_t>(in.renderHeight), s.qualityValue);
        } else {
            s.qualityValue = chooseQuality(s.params, static_cast<uint32_t>(in.targetWidth),
                                           static_cast<uint32_t>(in.targetHeight),
                                           static_cast<uint32_t>(in.renderWidth),
                                           static_cast<uint32_t>(in.renderHeight));
            s.qualityIsExplicit = false;
        }
        s.effectiveQuality = resolvedTier;
        s.featureFlags = computeFeatureFlags(in);

        NVSDK_NGX_Parameter_SetUI(s.params, NVSDK_NGX_Parameter_Width,
                                  static_cast<unsigned>(in.renderWidth));
        NVSDK_NGX_Parameter_SetUI(s.params, NVSDK_NGX_Parameter_Height,
                                  static_cast<unsigned>(in.renderHeight));
        NVSDK_NGX_Parameter_SetUI(s.params, NVSDK_NGX_Parameter_OutWidth,
                                  static_cast<unsigned>(in.targetWidth));
        NVSDK_NGX_Parameter_SetUI(s.params, NVSDK_NGX_Parameter_OutHeight,
                                  static_cast<unsigned>(in.targetHeight));
        NVSDK_NGX_Parameter_SetI(s.params, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                                 s.featureFlags);
        NVSDK_NGX_Parameter_SetI(s.params, NVSDK_NGX_Parameter_PerfQualityValue,
                                 static_cast<int>(s.qualityValue));
        if (const char* presetKey = presetHintKey(s.qualityValue)) {
            NVSDK_NGX_Parameter_SetI(s.params, presetKey, s.requestedPreset);
        }
        s.effectivePreset = s.requestedPreset;
        std::size_t scratch = 0;
        const NVSDK_NGX_Result sr =
            NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature_SuperSampling, s.params, &scratch);
        logInfo("DLSS scratch buffer query: result=%s size=%llu bytes",
                ngxResultString(sr).c_str(), static_cast<unsigned long long>(scratch));
        if (NVSDK_NGX_FAILED(sr)) {
            s.createdWarned = true;
            s.failure = "NVSDK_NGX_D3D12_GetScratchBufferSize";
            logWarn("DLSS unavailable — GetScratchBufferSize failed: %s",
                    ngxResultString(sr).c_str());
            return false;
        }
        if (scratch == 0) {
            logInfo("NGX reports a scratch buffer size of 0 (modern DLSS allocates "
                    "internally); allocating a 64 MiB floor regardless");
            scratch = 64ull * 1024ull * 1024ull;
        }
        if (!createScratchBuffer(static_cast<UINT64>(scratch))) {
            s.createdWarned = true;
            s.failure = "scratch buffer creation";
            return false;
        }

        NVSDK_NGX_DLSS_Create_Params cp;
        const bool dlssTierIsDlaa =
            s.qualityValue == static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA);
        cp.Feature.InWidth = static_cast<unsigned>(
            dlssTierIsDlaa ? in.targetWidth : in.renderWidth);
        cp.Feature.InHeight = static_cast<unsigned>(
            dlssTierIsDlaa ? in.targetHeight : in.renderHeight);
        cp.Feature.InTargetWidth = static_cast<unsigned>(in.targetWidth);
        cp.Feature.InTargetHeight = static_cast<unsigned>(in.targetHeight);
        cp.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(s.qualityValue);
        cp.InFeatureCreateFlags = s.featureFlags;
        cp.InEnableOutputSubrects = false;

        NVSDK_NGX_Handle* handle = nullptr;
        const NVSDK_NGX_Result cr = NGX_D3D12_CREATE_DLSS_EXT(
            cmd, 1u, 1u, &handle, s.params, &cp);
        if (NVSDK_NGX_FAILED(cr) || handle == nullptr) {
            s.createdWarned = true;
            s.failure = "NGX_D3D12_CREATE_DLSS_EXT";
            logWarn("DLSS unavailable — CreateFeature failed: %s (render %dx%d, target %dx%d, "
                    "flags=0x%X, quality=%u)",
                    ngxResultString(cr).c_str(), in.renderWidth, in.renderHeight, in.targetWidth,
                    in.targetHeight, static_cast<unsigned>(s.featureFlags), s.qualityValue);
            return false;
        }
        s.feature = handle;
        s.featureOk = true;
        s.renderWidth = in.renderWidth;
        s.renderHeight = in.renderHeight;
        s.targetWidth = in.targetWidth;
        s.targetHeight = in.targetHeight;
        s.createdThisFrame = true;
        const auto bit = [](int32_t flags, NVSDK_NGX_DLSS_Feature_Flags f) {
            return (flags & static_cast<int32_t>(f)) != 0 ? 1 : 0;
        };
        const char* qualityName = "unknown";
        switch (s.qualityValue) {
            case static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxPerf): qualityName = "Performance"; break;
            case static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_Balanced): qualityName = "Balanced"; break;
            case static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality): qualityName = "Quality"; break;
            case static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA): qualityName = "DLAA"; break;
            default: break;
        }
        logInfo("DLSS feature created: render %dx%d -> target %dx%d, quality=%u (%s, %s%s), "
                "flags=0x%X (AutoExposure=%d DepthInverted=%d MVLowRes=%d MVJittered=%d HDR=%d), "
                "scratch=%llu bytes",
                in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight, s.qualityValue,
                qualityName,
                s.qualityIsExplicit ? "explicit tier from the JVM" : "closest to the ratio",
                s.qualityValue == static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA)
                    ? ", render == target as required for DLAA"
                    : "",
                static_cast<unsigned>(s.featureFlags),
                bit(s.featureFlags, NVSDK_NGX_DLSS_Feature_Flags_AutoExposure),
                bit(s.featureFlags, NVSDK_NGX_DLSS_Feature_Flags_DepthInverted),
                bit(s.featureFlags, NVSDK_NGX_DLSS_Feature_Flags_MVLowRes),
                bit(s.featureFlags, NVSDK_NGX_DLSS_Feature_Flags_MVJittered),
                bit(s.featureFlags, NVSDK_NGX_DLSS_Feature_Flags_IsHDR),
                static_cast<unsigned long long>(s.scratchBytes));
        {
            const char* key = presetHintKey(s.qualityValue);
            logInfo("DLSS render preset = %d (%s) on %s", s.effectivePreset,
                    presetName(s.effectivePreset).c_str(), key != nullptr ? key : "(unmapped tier)");
        }
        return true;
    }

    void logFrameChange(const mcdr_dlss_input& in, bool createdThisFrame) {
        const bool changed = createdThisFrame || in.renderWidth != s.lastRenderW ||
                             in.renderHeight != s.lastRenderH || in.targetWidth != s.lastTargetW ||
                             in.targetHeight != s.lastTargetH || in.reset != s.lastReset ||
                             in.depthFormatOrdinal != s.lastDepthFormat ||
                             in.exposureMode != s.lastExposureMode ||
                             in.jitterOffsetX != s.lastJitterX || in.jitterOffsetY != s.lastJitterY ||
                             in.nearZ != s.lastNear || in.farZ != s.lastFar ||
                             static_cast<uint32_t>(s.featureFlags) != s.lastFlags;
        if (!changed) {
            return;
        }
        s.lastRenderW = in.renderWidth;
        s.lastRenderH = in.renderHeight;
        s.lastTargetW = in.targetWidth;
        s.lastTargetH = in.targetHeight;
        s.lastReset = in.reset;
        s.lastDepthFormat = in.depthFormatOrdinal;
        s.lastExposureMode = in.exposureMode;
        s.lastJitterX = in.jitterOffsetX;
        s.lastJitterY = in.jitterOffsetY;
        s.lastNear = in.nearZ;
        s.lastFar = in.farZ;
        s.lastFlags = static_cast<uint32_t>(s.featureFlags);
        logInfo("DLSS input: render %dx%d -> target %dx%d, depthInverted=%u near=%.3f far=%.3f "
                "depthFmt=%d, mvScale=%.2f/%.2f, exposure=%s, jitter(single-sourced from the JVM "
                "Halton by design)=%.3f/%.3f, reset=%u, flags=0x%X, quality=%u",
                in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight, in.depthInverted,
                static_cast<double>(in.nearZ), static_cast<double>(in.farZ), in.depthFormatOrdinal,
                static_cast<double>(in.mvScaleX), static_cast<double>(in.mvScaleY),
                in.exposureMode == MCDR_DLSS_EXPOSURE_AUTO ? "auto-texture" : "constant",
                static_cast<double>(in.jitterOffsetX), static_cast<double>(in.jitterOffsetY), in.reset,
                static_cast<unsigned>(s.featureFlags), s.qualityValue);
    }
}

int32_t mcdr_dlss_streamline_requested(void) { return dlssImplStreamline() ? 1 : 0; }

namespace { std::atomic<int32_t> gSlDlssLoaded{-1}; }
void mcdr_dlss_note_sl_loaded(int32_t loaded) { gSlDlssLoaded.store(loaded ? 1 : 0); }
int32_t mcdr_dlss_available(void) { return gSlDlssLoaded.load() == 0 ? 0 : 1; }

extern "C" {

static bool isMappedReadable(const void* p) {
    if (p == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(p, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFu;
    return prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
           prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_EXECUTE_WRITECOPY;
}

void mcdr_dlss_install_log(void* logInfo, void* logWarn, void* logError) {
    gLogInfo = reinterpret_cast<LogFn>(logInfo);
    gLogWarn = reinterpret_cast<LogFn>(logWarn);
    gLogError = reinterpret_cast<LogFn>(logError);
}

int32_t mcdr_dlss_install_texture_ops(void* createFn, void* destroyFn) {
    gCreateTexture = reinterpret_cast<CreateTextureFn>(createFn);
    gDestroyTexture = reinterpret_cast<DestroyTextureFn>(destroyFn);
    if (gCreateTexture == nullptr || gDestroyTexture == nullptr) {
        logWarn("the device module injected no texture create/destroy entry (create=%p "
                "destroy=%p) — DLSS keeps using only JVM-supplied resources",
                createFn, destroyFn);
        return 0;
    }
    logInfo("B4: DLSS private-texture ops installed (create=%p destroy=%p) — the private UAV and "
            "the depth snapshot are now created by the device module, so their resource states stay "
            "inside its tracked map",
            createFn, destroyFn);
    return 1;
}

void mcdr_dlss_register_runtime_path(const char* path, int32_t cap) {
    if (path == nullptr || cap <= 0) {
        return;
    }
    s.runtimePath = path;
}

int32_t mcdr_dlss_register_vendor_dir(const char* utf8Dir, int32_t cap) {
    if (utf8Dir == nullptr || cap <= 0) {
        return 0;
    }
    const std::string dir(utf8Dir);
    if (dir.empty()) {
        return 0;
    }
    logInfo("DLSS vendor directory from the mod: \"%s\"", dir.c_str());
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()),
                                          nullptr, 0);
    if (need <= 0) {
        logWarn("DLSS vendor directory is not valid UTF-8; ignoring it");
        return 0;
    }
    std::wstring wide(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()), wide.data(), need);
    while (!wide.empty() && (wide.back() == L'\\' || wide.back() == L'/')) {
        wide.pop_back();
    }
    s.vendorDir = wide;
    widenSearchPath(wide);
    rebuildCommonInfo(wide);
    logInfo("NGX feature path list set to \"%s\" (nvngx feature DLLs are looked up there "
            "instead of the application folder, which is the JVM's bin\\ in a dev run)",
            narrow(wide).c_str());
    logEnvOnce();
    return s.searchPathSet ? 1 : 0;
}

void* mcdr_dlss_load_runtime(void) {
    logEnvOnce();
    if (s.runtimeModule != nullptr) {
        return s.runtimeModule;
    }
    HMODULE mod = loadRuntimeDll();
    if (mod == nullptr) {
        return nullptr;
    }
    s.runtimeModule = reinterpret_cast<void*>(mod);
    return s.runtimeModule;
}

const char* mcdr_dlss_runtime_path(void) { return s.runtimePath.c_str(); }

void* mcdr_dlss_ngx_feature_info(void) { return commonInfoForNgx(); }

int32_t mcdr_dlss_can_run(void* device) {
    if (device == nullptr) {
        return 0;
    }
    logEnvOnce();
    if (s.runtimePath.empty()) {
        if (loadRuntimeDll() == nullptr) {
            return 0;
        }
    }
#if MCDR_DLSS_HAVE_STREAMLINE
    if (dlssImplStreamline()) {
        return 1;
    }
#endif
    return ensureInit(static_cast<ID3D12Device*>(device)) ? 1 : 0;
}

void mcdr_dlss_query(char* note, int32_t noteCap, char* optimal, int32_t optimalCap, char* runtime,
                     int32_t runtimeCap, uint32_t* flagsOut) {
    if (flagsOut != nullptr) {
        *flagsOut = 0;
        if (s.initAttempted) *flagsOut |= 1u;
        if (s.initOk) *flagsOut |= 2u;
        if (s.paramsOk) *flagsOut |= 4u;
        if (s.optimalOk) *flagsOut |= 8u;
        if (s.featureOk) *flagsOut |= 16u;
        if (s.disabled) *flagsOut |= 32u;
    }
    if (note != nullptr && noteCap > 0) {
        if (s.srPrepared && s.preparedFailure) {
            std::snprintf(note, static_cast<std::size_t>(noteCap), "DLSS %s",
                s.preparedFailure->c_str());
        } else if (s.srPrepared && dlssImplStreamline()) {
            std::snprintf(note, static_cast<std::size_t>(noteCap),
                "Streamline DLSS prepared: tier=%d preset=%d, render=%dx%d target=%dx%d",
                s.effectiveQuality, s.effectivePreset, s.renderWidth, s.renderHeight,
                s.targetWidth, s.targetHeight);
        } else if (s.disabled) {
            std::snprintf(note, static_cast<std::size_t>(noteCap), "DLSS disabled, %s failed",
                          s.failure.empty() ? "an earlier stage" : s.failure.c_str());
        } else if (!s.initAttempted) {
            std::snprintf(note, static_cast<std::size_t>(noteCap), "DLSS not initialised yet");
        } else if (dlssImplStreamline() && !s.initOk) {
            std::snprintf(note, static_cast<std::size_t>(noteCap),
                          "Streamline owns NGX; DLSS switch is %s; tier=%u (%s), %llu evaluates so far",
                          s.enabled ? "ON" : "OFF", s.qualityValue,
                          s.qualityIsExplicit ? "explicit tier" : "closest to the ratio",
                          static_cast<unsigned long long>(s.submitCount));
        } else if (!s.initOk) {
            std::snprintf(note, static_cast<std::size_t>(noteCap), "NGX init failed (%s)",
                          s.failure.empty() ? "unknown" : s.failure.c_str());
        } else if (!s.enabled) {
            std::snprintf(note, static_cast<std::size_t>(noteCap),
                          "NGX ready, DLSS switch is OFF (featureOk=%d, %llu frames submitted so far)",
                          s.featureOk ? 1 : 0, static_cast<unsigned long long>(s.submitCount));
        } else if (!s.featureOk) {
            std::snprintf(note, static_cast<std::size_t>(noteCap),
                          "NGX ready, DLSS ON but feature not created yet (%llu evaluates so far)",
                          static_cast<unsigned long long>(s.submitCount));
        } else {
            std::snprintf(note, static_cast<std::size_t>(noteCap),
                          "DLSS active, %llu frames submitted, render %dx%d -> target %dx%d, "
                          "quality=%u (%s)",
                          static_cast<unsigned long long>(s.submitCount), s.renderWidth, s.renderHeight,
                          s.targetWidth, s.targetHeight, s.qualityValue,
                          s.qualityIsExplicit ? "explicit tier" : "closest to the ratio");
        }
    }
    if (optimal != nullptr && optimalCap > 0) {
        std::snprintf(optimal, static_cast<std::size_t>(optimalCap), "%s", s.optimalText.c_str());
    }
    if (runtime != nullptr && runtimeCap > 0) {
        std::snprintf(runtime, static_cast<std::size_t>(runtimeCap), "%s", s.runtimePath.c_str());
    }
}

int32_t mcdr_dlss_create(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                         void* tlCtx) {
    if (in == nullptr) {
        return -1;
    }
    if (!std::isfinite(in->preExposure) || in->preExposure < 0.0f ||
        (in->preExposure > 0.0f && !std::isfinite(1.0f / in->preExposure)) ||
        !std::isfinite(in->exposureScale) || in->exposureScale < 0.0f) return -1;
    if (s.srPrepared) {
        s.preparedFailure = preparedSrMismatch(*in);
        return s.preparedFailure ? 0 : 1;
    }
    if (s.disabled) {
        return -1;
    }
    if (!s.initOk || !s.paramsOk) {
#if MCDR_DLSS_HAVE_STREAMLINE
        if (dlssImplStreamline()) {
            (void)resolveQualityValue(*in);
            s.initAttempted = true;
            (void)streamlineApplyOptions(*in);
            return 1;
        }
#endif
        return 0;
    }
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    if (cmd == nullptr) {
        return 0;
    }
    s.transitionFn = tl;
    s.transitionCtx = tlCtx;
    s.createdThisFrame = false;
    if (!ensureFeature(cmd, *in)) {
        return 0;
    }
    logFrameChange(*in, s.createdThisFrame);
    return 1;
}

int32_t mcdr_dlss_snapshot_depth(void* commandList, void* depthResource, DlssTransitionFn tl,
                                 void* tlCtx, DlssQueryStateFn stateOf) {
    if (commandList == nullptr || depthResource == nullptr || tl == nullptr) {
        return 0;
    }
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    auto* depth = reinterpret_cast<ID3D12Resource*>(depthResource);
    if (cmd == nullptr || depth == nullptr || s.device == nullptr) {
        return 0;
    }
    const D3D12_RESOURCE_DESC dd = depth->GetDesc();
    const UINT w = static_cast<UINT>(dd.Width);
    const UINT h = dd.Height;
    if (w == 0 || h == 0 || dd.Format == DXGI_FORMAT_UNKNOWN) {
        return 0;
    }
    const DXGI_FORMAT snapFormat = [](DXGI_FORMAT f) -> DXGI_FORMAT {
        switch (f) {
            case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
            case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;
            case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
            case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            default: return f;
        }
    }(dd.Format);
    if (s.depthSnapshot == nullptr || s.depthSnapshotWidth != w || s.depthSnapshotHeight != h ||
        s.depthSnapshotFormat != snapFormat || s.depthSnapshotMips != dd.MipLevels ||
        s.depthSnapshotArray != dd.DepthOrArraySize ||
        s.depthSnapshotSamples != dd.SampleDesc.Count) {
        destroyTrackedTexture(tlCtx, &s.depthSnapshotObject);
        s.depthSnapshot = nullptr;
        s.depthSnapshotWidth = 0;
        s.depthSnapshotHeight = 0;
        s.depthSnapshotFormat = DXGI_FORMAT_UNKNOWN;
        void* snapObj = nullptr;
        ID3D12Resource* snapRes = nullptr;
        if (!createTrackedTexture(tlCtx, w, h, snapFormat, dd.MipLevels, dd.DepthOrArraySize,
                                  dd.SampleDesc.Count, 0, &snapObj, &snapRes)) {
            s.depthSnapshot = nullptr;
            if (!s.depthSnapshotWarned) {
                s.depthSnapshotWarned = true;
                logWarn("depth snapshot texture creation failed (%ux%u fmt=%d) — the device "
                        "module refused or failed to create it; DLSS falls back to the "
                        "JVM-supplied depth (which is already cleared by then, so the upscale "
                        "loses its depth term)",
                        w, h, static_cast<int>(dd.Format));
            }
            return -1;
        }
        s.depthSnapshotObject = snapObj;
        s.depthSnapshot = snapRes;
        s.depthSnapshotWidth = w;
        s.depthSnapshotHeight = h;
        s.depthSnapshotFormat = snapFormat;
        s.depthSnapshotMips = (dd.MipLevels > 1) ? dd.MipLevels : 1;
        s.depthSnapshotArray = (dd.DepthOrArraySize > 1) ? dd.DepthOrArraySize : 1;
        s.depthSnapshotSamples = dd.SampleDesc.Count;
        logInfo("DLSS depth snapshot texture created by the device module: %ux%u fmt=%d "
                "(private, tracked, COPY_DEST/COPY_SOURCE)",
                w, h, static_cast<int>(dd.Format));
    }
    const int32_t before = (stateOf != nullptr)
                               ? stateOf(tlCtx, depthResource)
                               : static_cast<int32_t>(D3D12_RESOURCE_STATE_COMMON);
    tl(tlCtx, reinterpret_cast<void*>(s.depthSnapshot),
       static_cast<int32_t>(D3D12_RESOURCE_STATE_COPY_DEST));
    {
        D3D12_TEXTURE_COPY_LOCATION src;
        src.pResource = depth;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = s.depthSnapshot;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        const D3D12_BOX box{0, 0, 0, w, h, 1};
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    tl(tlCtx, reinterpret_cast<void*>(s.depthSnapshot),
       static_cast<int32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    tl(tlCtx, depthResource, before);
    ++s.depthSnapshotFrame;
    if (s.depthSnapshotFrame == 1) {
        logInfo("DLSS depth snapshot captured (first): %ux%u fmt=%d from resource %p; it "
                "replaces the JVM's depth at evaluate time",
                w, h, static_cast<int>(dd.Format), depthResource);
    }
    return 1;
}

sl::FrameToken* gLastFrameToken = nullptr;
uint64_t gStreamlineFrameSerial = 0;
uint64_t gWorldConstantsFrame = UINT64_MAX;
uint64_t gDisplayConstantsFrame = UINT64_MAX;
bool gDisplayInputsAccepted = false;

    extern std::atomic<int32_t> gDlssgMode;

    using PFn_SetReflexOptions = sl::Result (*)(const sl::ReflexOptions&);
    using PFn_ReflexSleep = sl::Result (*)(const sl::FrameToken&);
    using PFn_PCLSetMarker = sl::Result (*)(sl::PCLMarker, const sl::FrameToken&);

    namespace {

        struct ReflexApi {
            PFn_SetReflexOptions setOptions = nullptr;
            PFn_ReflexSleep sleep = nullptr;
            PFn_PCLSetMarker marker = nullptr;
            sl::Result (*newFrameToken)(sl::FrameToken*&, const uint32_t*) = nullptr;
            sl::Result (*getState)(sl::ReflexState&) = nullptr;
            uint32_t frameCounter = 0;
            int32_t mode = 1;
            bool resolved = false;
            bool optionsApplied = false;
            bool optionsOk = false;
            uint32_t setOptsFailures = 0;
            uint32_t frameLimitUs = 0;
            uint32_t markerFailures = 0;
            uint32_t sleepFailures = 0;
            bool sleepOkLogged = false;
            int32_t appliedMode = -1;
            uint32_t appliedFrameLimitUs = 0;
        };
        ReflexApi gReflex;
        bool reflexEnabled() {
            static const bool enabled = []() {
                char v[8] = {};
                const DWORD n = ::GetEnvironmentVariableA("MCDR_REFLEX", v, 8);
                return !(n == 1 && v[0] == '0');
            }();
            return enabled;
        }
        void resolveReflexApi() {
            if (gReflex.resolved) return;
            char lim[16] = {};
            const DWORD limLen = ::GetEnvironmentVariableA("MCDR_REFLEX_FRAME_LIMIT_US", lim, 16);
            if (limLen > 0) {
                gReflex.frameLimitUs = static_cast<uint32_t>(std::strtoul(lim, nullptr, 10));
            }
            const std::wstring dir = dlssModuleDirectory();
            if (dir.empty()) return;
            HMODULE sl = ::LoadLibraryW((dir + L"sl.interposer.dll").c_str());
            if (sl == nullptr) return;
            using PFn_GetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
            auto gff = reinterpret_cast<PFn_GetFeatureFunction>(::GetProcAddress(sl, "slGetFeatureFunction"));
            if (gff == nullptr) return;
            void* raw = nullptr;
            const sl::Result setOptsResult = gff(sl::kFeatureReflex, "slReflexSetOptions", raw);
            if (setOptsResult == sl::Result::eOk) {
                gReflex.setOptions = reinterpret_cast<PFn_SetReflexOptions>(raw);
            }
            raw = nullptr;
            if (gff(sl::kFeatureReflex, "slReflexSleep", raw) == sl::Result::eOk) {
                gReflex.sleep = reinterpret_cast<PFn_ReflexSleep>(raw);
            }
            raw = nullptr;
            if (gff(sl::kFeaturePCL, "slPCLSetMarker", raw) == sl::Result::eOk) {
                gReflex.marker = reinterpret_cast<PFn_PCLSetMarker>(raw);
            }
            gReflex.newFrameToken = reinterpret_cast<decltype(gReflex.newFrameToken)>(
                ::GetProcAddress(sl, "slGetNewFrameToken"));
            if (gReflex.setOptions != nullptr && gReflex.sleep != nullptr) {
                gReflex.resolved = true;
            }
            raw = nullptr;
            if (gff(sl::kFeatureReflex, "slReflexGetState", raw) == sl::Result::eOk) {
                gReflex.getState = reinterpret_cast<decltype(gReflex.getState)>(raw);
            }
        }
        bool applyReflexOptions() {
            if (gReflex.setOptions == nullptr) return false;
            const int32_t fgMode = gDlssgMode.load();
            int32_t effective = gReflex.mode;
            if (fgMode != 0 && effective == 0) {
                effective = 1;
            }
            if (gReflex.optionsApplied && effective == gReflex.appliedMode &&
                gReflex.frameLimitUs == gReflex.appliedFrameLimitUs) {
                return gReflex.optionsOk;
            }
            sl::ReflexOptions opt{};
            opt.mode = effective == 2 ? sl::ReflexMode::eLowLatencyWithBoost
                                      : (effective == 1 ? sl::ReflexMode::eLowLatency
                                                        : sl::ReflexMode::eOff);
            opt.frameLimitUs = gReflex.frameLimitUs;
            const sl::Result r = gReflex.setOptions(opt);
            if (r != sl::Result::eOk) {
                gReflex.optionsOk = false;
                gReflex.optionsApplied = false;
                if (gReflex.setOptsFailures < 3) {
                    ++gReflex.setOptsFailures;
                    logWarn("reflex: slReflexSetOptions(mode=%d limit=%uus) failed=0x%X; "
                            "holding slReflexSleep until it succeeds",
                            effective, gReflex.frameLimitUs, static_cast<unsigned>(r));
                }
                return false;
            }
            gReflex.optionsOk = true;
            gReflex.optionsApplied = true;
            gReflex.appliedMode = effective;
            gReflex.appliedFrameLimitUs = gReflex.frameLimitUs;
            return true;
        }
        void emitMarker(sl::PCLMarker marker) {
            if (!gReflex.optionsOk) return;
            if (gReflex.marker == nullptr || gLastFrameToken == nullptr) return;
            const sl::Result r = gReflex.marker(marker, *gLastFrameToken);
            if (r != sl::Result::eOk && gReflex.markerFailures < 3) {
                ++gReflex.markerFailures;
                logWarn("pcl: marker %d result=0x%X", static_cast<int>(marker),
                        static_cast<unsigned>(r));
            }
        }
    }

    void mcdr_reflex_set_frame_limit(uint32_t micros) {
        resolveReflexApi();
        gReflex.frameLimitUs = micros;
        gReflex.optionsApplied = false;
    }

    void mcdr_reflex_frame_begin(void) {
        if (!reflexEnabled()) return;
        resolveReflexApi();
        gLastFrameToken = nullptr;
        ++gStreamlineFrameSerial;
        gDisplayInputsAccepted = false;
        sl::FrameToken* token = nullptr;
        uint32_t frameIndex = ++gReflex.frameCounter;
        if (gReflex.newFrameToken != nullptr &&
            gReflex.newFrameToken(token, &frameIndex) == sl::Result::eOk && token != nullptr) {
            gLastFrameToken = token;
        }
        emitMarker(sl::PCLMarker::eSimulationStart);
    }

    void mcdr_reflex_set_mode(int32_t mode) {
        if (!reflexEnabled()) return;
        const int32_t clamped = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
        if (clamped == gReflex.mode) return;
        gReflex.mode = clamped;
        gReflex.optionsApplied = false;
    }

    int32_t mcdr_reflex_available(void) {
        if (!reflexEnabled()) return 0;
        resolveReflexApi();
        return (gReflex.setOptions != nullptr && gReflex.sleep != nullptr && gReflex.marker != nullptr)
                   ? 1
                   : 0;
    }

    void mcdr_reflex_marker(int32_t marker) {
        if (!reflexEnabled()) return;
        resolveReflexApi();
        emitMarker(static_cast<sl::PCLMarker>(marker));
    }

    void mcdr_reflex_before_present(void) {
        if (!reflexEnabled()) return;
        resolveReflexApi();
        const bool reflexReady = applyReflexOptions();
        if (gLastFrameToken == nullptr && gReflex.newFrameToken != nullptr) {
            sl::FrameToken* token = nullptr;
            uint32_t frameIndex = ++gReflex.frameCounter;
            if (gReflex.newFrameToken(token, &frameIndex) == sl::Result::eOk && token != nullptr) {
                gLastFrameToken = token;
            }
        }
        if (reflexReady) {
            emitMarker(sl::PCLMarker::eRenderSubmitEnd);
            emitMarker(sl::PCLMarker::ePresentStart);
        }
        if (reflexReady && gReflex.sleep != nullptr && gLastFrameToken != nullptr) {
            const sl::Result r = gReflex.sleep(*gLastFrameToken);
            if (r != sl::Result::eOk && gReflex.sleepFailures < 3) {
                ++gReflex.sleepFailures;
                logWarn("reflex: slReflexSleep failed=0x%X (options were applied OK)",
                        static_cast<unsigned>(r));
            } else if (r == sl::Result::eOk && !gReflex.sleepOkLogged) {
                gReflex.sleepOkLogged = true;
                logInfo("reflex: slReflexSleep ok (mode=%d) — Reflex/PCL markers active",
                        gReflex.appliedMode);
            }
        }
    }

    void mcdr_reflex_after_present(void) {
        if (!reflexEnabled()) return;
        emitMarker(sl::PCLMarker::ePresentEnd);
        gLastFrameToken = nullptr;
    }

#if MCDR_DLSS_HAVE_STREAMLINE
    mcdr_dlss_constants gCameraConstants{};
    bool gCameraConstantsValid = false;

    int32_t mcdr_dlss_set_constants(const mcdr_dlss_constants* constants) {
        if (constants == nullptr) return 0;
        gCameraConstants = *constants;
        gCameraConstantsValid = true;
        return 1;
    }

    using PFn_slGetNewFrameToken = sl::Result (*)(sl::FrameToken*&, const uint32_t*);
    using PFn_slSetTagForFrame = sl::Result (*)(const sl::FrameToken&, const sl::ViewportHandle&,
                                                const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
    using PFn_slEvaluateFeature = sl::Result (*)(sl::Feature, const sl::FrameToken&,
                                                 const sl::BaseStructure**, uint32_t,
                                                 sl::CommandBuffer*);
    using PFn_slSetConstants = sl::Result (*)(const sl::Constants&, const sl::FrameToken&,
                                             const sl::ViewportHandle&);
    using PFn_SlDLSSGSetOptions = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSGOptions&);
    using PFn_SlDLSSGGetState = sl::Result (*)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*);

    std::atomic<int32_t> gDlssgMode{0};
    std::atomic<int32_t> gDlssgFrames{1};
    std::atomic<int32_t> gFgPresented{1};
    std::atomic<int32_t> gDlssOutputState{0};

    struct StreamlineEvalApi {
        PFn_SlDLSSSetOptions setOptions = nullptr;
        PFn_SlDLSSGSetOptions setFgOptions = nullptr;
        PFn_SlDLSSGGetState getFgState = nullptr;
        PFn_slGetNewFrameToken newFrameToken = nullptr;
        PFn_slSetTagForFrame setTags = nullptr;
        PFn_slSetConstants setConstants = nullptr;
        PFn_slEvaluateFeature evaluate = nullptr;
    };

    struct StreamlineFgState {
        int32_t mode = -1;
        int32_t frames = -1;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    StreamlineFgState gStreamlineFgState;

    sl::Result clearStreamlineFrameTags(StreamlineEvalApi& api, const sl::FrameToken& token,
                                        const sl::ViewportHandle& viewport) {
        const sl::ResourceTag tags[] = {
            {nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow},
            {nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow},
            {nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow},
            {nullptr, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eOnlyValidNow},
        };
        return api.setTags(token, viewport, tags, static_cast<uint32_t>(std::size(tags)), nullptr);
    }

    sl::Result tagStreamlineFrameInputs(StreamlineEvalApi& api, const sl::FrameToken& token,
                                        const sl::ViewportHandle& viewport,
                                        ID3D12GraphicsCommandList* cmd,
                                        const mcdr_dlss_input& in, sl::Resource& depth,
                                        sl::Resource& motion, sl::Resource& hudless, sl::Resource& ui) {
        const sl::Extent renderExtent{0, 0, static_cast<uint32_t>(in.renderWidth),
                                      static_cast<uint32_t>(in.renderHeight)};
        const sl::Extent displayExtent{0, 0, static_cast<uint32_t>(in.targetWidth),
                                       static_cast<uint32_t>(in.targetHeight)};
        const sl::ResourceTag tags[] = {
            {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent},
            {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow,
             &renderExtent},
            {&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow,
             &displayExtent},
            {&ui, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eOnlyValidNow,
             &displayExtent},
        };
        return api.setTags(token, viewport, tags, static_cast<uint32_t>(std::size(tags)), cmd);
    }

    HMODULE gSlInterposer = nullptr;

    StreamlineEvalApi& streamlineEvalApi(bool reset = false) {
        static StreamlineEvalApi api{};
        static bool resolved = false;
        if (reset) {
            api = {};
            resolved = false;
            gSlInterposer = nullptr;
            return api;
        }
        if (resolved) return api;
        resolved = true;
        api = []() -> StreamlineEvalApi {
            StreamlineEvalApi a{};
            const std::wstring dir = dlssModuleDirectory();
            if (dir.empty()) return a;
            HMODULE sl = ::LoadLibraryW((dir + L"sl.interposer.dll").c_str());
            if (sl == nullptr) return a;
            gSlInterposer = sl;
            a.newFrameToken = reinterpret_cast<PFn_slGetNewFrameToken>(
                ::GetProcAddress(sl, "slGetNewFrameToken"));
            a.setTags = reinterpret_cast<PFn_slSetTagForFrame>(
                ::GetProcAddress(sl, "slSetTagForFrame"));
            a.setConstants = reinterpret_cast<PFn_slSetConstants>(
                ::GetProcAddress(sl, "slSetConstants"));
            a.evaluate = reinterpret_cast<PFn_slEvaluateFeature>(
                ::GetProcAddress(sl, "slEvaluateFeature"));
            using PFn_GetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
            auto gff = reinterpret_cast<PFn_GetFeatureFunction>(
                ::GetProcAddress(sl, "slGetFeatureFunction"));
            if (gff != nullptr) {
                void* raw = nullptr;
                if (gff(sl::kFeatureDLSS, "slDLSSSetOptions", raw) == sl::Result::eOk) {
                    a.setOptions = reinterpret_cast<PFn_SlDLSSSetOptions>(raw);
                }
                raw = nullptr;
                if (gff(sl::kFeatureDLSS_G, "slDLSSGSetOptions", raw) == sl::Result::eOk) {
                    a.setFgOptions = reinterpret_cast<PFn_SlDLSSGSetOptions>(raw);
                }
                raw = nullptr;
                if (gff(sl::kFeatureDLSS_G, "slDLSSGGetState", raw) == sl::Result::eOk) {
                    a.getFgState = reinterpret_cast<PFn_SlDLSSGGetState>(raw);
                }
            }
            return a;
        }();
        return api;
    }

    void mat4Multiply(const float* a, const float* b, float* out) {
        float r[16];
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += a[row * 4 + k] * b[k * 4 + col];
                }
                r[row * 4 + col] = sum;
            }
        }
        std::memcpy(out, r, sizeof(r));
    }

    bool mat4Inverse(const float* m, float* out) {
        double a[4][8] = {};
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) a[row][col] = m[row * 4 + col];
            a[row][4 + row] = 1.0;
        }
        for (int col = 0; col < 4; ++col) {
            int pivot = col;
            for (int row = col + 1; row < 4; ++row) {
                if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) pivot = row;
            }
            if (std::fabs(a[pivot][col]) < 1e-12) return false;
            if (pivot != col) {
                for (int k = 0; k < 8; ++k) std::swap(a[col][k], a[pivot][k]);
            }
            const double inv = 1.0 / a[col][col];
            for (int k = 0; k < 8; ++k) a[col][k] *= inv;
            for (int row = 0; row < 4; ++row) {
                if (row == col) continue;
                const double f = a[row][col];
                if (f == 0.0) continue;
                for (int k = 0; k < 8; ++k) a[row][k] -= f * a[col][k];
            }
        }
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) out[row * 4 + col] = static_cast<float>(a[row][4 + col]);
        }
        return true;
    }
    bool makeStreamlineCamera(const mcdr_dlss_constants& camera, uint32_t dilated, sl::Constants& constants) {
        float inverseCurrent[16], inversePrevious[16], inverseProjection[16];
        if (!mat4Inverse(camera.viewProjection, inverseCurrent) ||
            !mat4Inverse(camera.prevViewProjection, inversePrevious) ||
            !mat4Inverse(camera.projection, inverseProjection)) return false;
        std::memcpy(&constants.cameraViewToClip, camera.projection, sizeof(camera.projection));
        std::memcpy(&constants.clipToCameraView, inverseProjection, sizeof(inverseProjection));
        float clipToPrevious[16], previousToClip[16];
        mat4Multiply(inverseCurrent, camera.prevViewProjection, clipToPrevious);
        mat4Multiply(inversePrevious, camera.viewProjection, previousToClip);
        std::memcpy(&constants.clipToPrevClip, clipToPrevious, sizeof(clipToPrevious));
        std::memcpy(&constants.prevClipToClip, previousToClip, sizeof(previousToClip));
        constants.jitterOffset = {camera.jitterOffsetX, camera.jitterOffsetY};
        constants.mvecScale = {camera.mvecScaleX, camera.mvecScaleY};
        constants.cameraPos = {camera.cameraPosX, camera.cameraPosY, camera.cameraPosZ};
        constants.cameraUp = {camera.cameraUpX, camera.cameraUpY, camera.cameraUpZ};
        constants.cameraRight = {camera.cameraRightX, camera.cameraRightY, camera.cameraRightZ};
        constants.cameraFwd = {camera.cameraFwdX, camera.cameraFwdY, camera.cameraFwdZ};
        constants.cameraNear = camera.cameraNear;
        constants.cameraFar = camera.cameraFar;
        constants.cameraFOV = camera.cameraFovRadians;
        constants.cameraAspectRatio = camera.cameraAspectRatio;
        constants.depthInverted = camera.depthInverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.cameraMotionIncluded = camera.cameraMotionIncluded ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.motionVectors3D = camera.motionVectors3D ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.motionVectorsJittered = camera.motionVectorsJittered ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.reset = camera.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.motionVectorsDilated = dilated != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.orthographicProjection = sl::Boolean::eFalse;
        return true;
    }

    void refreshFgApi(StreamlineEvalApi& a) {
        if (a.setFgOptions != nullptr && a.getFgState != nullptr) return;
        if (gSlInterposer == nullptr) return;
        static uint64_t lastTry = 0;
        const uint64_t now = ::GetTickCount64();
        if (now - lastTry < 500) return;
        lastTry = now;
        using PFn_GetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
        auto gff = reinterpret_cast<PFn_GetFeatureFunction>(
            ::GetProcAddress(gSlInterposer, "slGetFeatureFunction"));
        if (gff == nullptr) return;
        void* raw = nullptr;
        if (gff(sl::kFeatureDLSS_G, "slDLSSGSetOptions", raw) == sl::Result::eOk && raw != nullptr) {
            a.setFgOptions = reinterpret_cast<PFn_SlDLSSGSetOptions>(raw);
        }
        raw = nullptr;
        if (gff(sl::kFeatureDLSS_G, "slDLSSGGetState", raw) == sl::Result::eOk && raw != nullptr) {
            a.getFgState = reinterpret_cast<PFn_SlDLSSGGetState>(raw);
        }

    }

    int32_t streamlineEvaluate(void* commandList, const mcdr_dlss_input& in, DlssTransitionFn tl,
                               void* tlCtx) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
        if (gStreamlineInputInvalidationFailed.load() && mcdr_dlss_invalidate_inputs() != 1)
            return -1;
        if (cmd == nullptr) {
            (void)mcdr_dlss_invalidate_inputs();
            return gStreamlineInputInvalidationFailed.load() ? -1 : 0;
        }
        StreamlineEvalApi& api = streamlineEvalApi();
        if (gDlssgMode.load() != 0) {
            refreshFgApi(api);

        }
        auto* color = reinterpret_cast<ID3D12Resource*>(in.pInColor);
        auto* exposure = reinterpret_cast<ID3D12Resource*>(in.pInExposure);
        auto* depth = reinterpret_cast<ID3D12Resource*>(in.pInDepth);
        auto* mv = reinterpret_cast<ID3D12Resource*>(in.pInMotionVectors);
        auto* out = reinterpret_cast<ID3D12Resource*>(in.pInOutput);
        if (api.setOptions == nullptr || api.newFrameToken == nullptr ||
            api.setTags == nullptr || api.evaluate == nullptr) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                logWarn("streamline evaluate unavailable — interposer exports or "
                        "slDLSSSetOptions missing");
            }
            (void)mcdr_dlss_invalidate_inputs();
            return gStreamlineInputInvalidationFailed.load() ? -1 : 0;
        }
        if (color == nullptr || depth == nullptr || mv == nullptr || out == nullptr) {
            (void)mcdr_dlss_invalidate_inputs();
            return gStreamlineInputInvalidationFailed.load() ? -1 : 0;
        }
        ID3D12Resource* outResource = out;
        bool copyBack = false;
        const bool directOutput = (out->GetDesc().Flags &
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
        const auto fitsTarget = [&in](ID3D12Resource* res) {
            if (res == nullptr) return false;
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            return d.Width == static_cast<UINT64>(in.targetWidth) &&
                   d.Height == static_cast<UINT>(in.targetHeight);
        };
        if (!directOutput && fitsTarget(s.externalOutput)) {
            outResource = s.externalOutput;
            copyBack = true;
        } else if (!directOutput && fitsTarget(s.dlssOutput)) {
            outResource = s.dlssOutput;
            copyBack = true;
        }
        if (outResource == s.dlssOutput &&
            (gPinTexture == nullptr || gPinTexture(commandList, s.dlssOutputObject) != 1)) {
            (void)mcdr_dlss_invalidate_inputs();
            return gStreamlineInputInvalidationFailed.load() ? -1 : 0;
        }
        s.transitionFn = tl;
        s.transitionCtx = tlCtx;
        sl::FrameToken* token = gLastFrameToken;
        if (token == nullptr) {
            uint32_t frameIndex = ++gReflex.frameCounter;
            if (api.newFrameToken(token, &frameIndex) != sl::Result::eOk || token == nullptr) return 0;
            ++gStreamlineFrameSerial;
        }
        const sl::ViewportHandle viewport(s.srViewport);
        gLastFrameToken = token;

        if (!gCameraConstantsValid || !api.setConstants || gWorldConstantsFrame == gStreamlineFrameSerial)
            return -1;
        sl::Constants constants{};
        if (!makeStreamlineCamera(gCameraConstants, 0, constants)) return -1;
        if (s.resetPending || in.reset != 0) constants.reset = sl::Boolean::eTrue;
        gWorldConstantsFrame = gStreamlineFrameSerial;
        if (api.setConstants(constants, *token, viewport) != sl::Result::eOk) return -1;

        transition(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (exposure) transition(exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(outResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        sl::Resource colorRes(sl::ResourceType::eTex2d, color,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource depthRes(sl::ResourceType::eTex2d, depth,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource mvRes(sl::ResourceType::eTex2d, mv,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource outRes(sl::ResourceType::eTex2d, outResource,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        sl::Resource exposureRes(sl::ResourceType::eTex2d, exposure,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const sl::Extent renderExtent{0, 0, static_cast<uint32_t>(in.renderWidth),
                                      static_cast<uint32_t>(in.renderHeight)};
        const sl::Extent displayExtent{0, 0, static_cast<uint32_t>(in.targetWidth),
                                       static_cast<uint32_t>(in.targetHeight)};
        const sl::Extent exposureExtent{0, 0, 1, 1};
        std::array<sl::ResourceTag, 5> tags{};
        uint32_t tagCount = 0;
        tags[tagCount++] = sl::ResourceTag(&colorRes, sl::kBufferTypeScalingInputColor,
                                           sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
        tags[tagCount++] = sl::ResourceTag(&depthRes, sl::kBufferTypeDepth,
                                           sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
        tags[tagCount++] = sl::ResourceTag(&mvRes, sl::kBufferTypeMotionVectors,
                                           sl::ResourceLifecycle::eOnlyValidNow, &renderExtent);
        tags[tagCount++] = sl::ResourceTag(&outRes, sl::kBufferTypeScalingOutputColor,
                                           sl::ResourceLifecycle::eOnlyValidNow, &displayExtent);
        if (exposure) {
            tags[tagCount++] = sl::ResourceTag(&exposureRes, sl::kBufferTypeExposure,
                                               sl::ResourceLifecycle::eOnlyValidNow, &exposureExtent);
        }
        std::array<sl::ResourceTag, 1> absentTags{};
        uint32_t absentTagCount = 0;
        if (!exposure) {
            absentTags[absentTagCount++] = sl::ResourceTag(nullptr, sl::kBufferTypeExposure,
                                                           sl::ResourceLifecycle::eOnlyValidNow);
        }
        const sl::Result tagResult = absentTagCount == 0 ? sl::Result::eOk :
            api.setTags(*token, viewport, absentTags.data(), absentTagCount, cmd);
        if (tagResult != sl::Result::eOk) {
            gStreamlineInputInvalidationFailed.store(true);
            logWarn("Streamline optional SR input invalidation failed: 0x%X", static_cast<unsigned>(tagResult));
            return -1;
        }

        const auto options = srStreamlineOptions(in, s.qualityValue, s.requestedPreset);
        if (!options) return 0;
        const auto& opt = *options;
        const bool hdrInput = opt.colorBuffersHDR == sl::Boolean::eTrue;
        const bool slOptsChanged = !s.slEvalOptsValid || s.slEvalOptMode != static_cast<int32_t>(opt.mode) ||
                                   s.slEvalOptOutW != opt.outputWidth ||
                                   s.slEvalOptOutH != opt.outputHeight ||
                                   s.slEvalOptPreExposure != opt.preExposure ||
                                   s.slEvalOptExposureScale != opt.exposureScale ||
                                   s.slEvalOptQuality != s.qualityValue ||
                                   s.slEvalOptPreset != s.requestedPreset ||
                                   s.slEvalOptHdr != hdrInput;
        sl::Result optResult = sl::Result::eOk;
        if (slOptsChanged) {
            optResult = api.setOptions(viewport, opt);
            if (optResult == sl::Result::eOk) {
                s.slEvalOptsValid = true;
                s.slEvalOptMode = static_cast<int32_t>(opt.mode);
                s.slEvalOptOutW = opt.outputWidth;
                s.slEvalOptOutH = opt.outputHeight;
                s.slEvalOptPreExposure = opt.preExposure;
                s.slEvalOptExposureScale = opt.exposureScale;
                s.slEvalOptQuality = s.qualityValue;
                s.slEvalOptPreset = s.requestedPreset;
                s.slEvalOptHdr = hdrInput;
            }
        }

        std::array<const sl::BaseStructure*, 6> evalInputs{};
        uint32_t evalInputCount = 0;
        evalInputs[evalInputCount++] = &viewport;
        for (uint32_t index = 0; index < tagCount; ++index)
            evalInputs[evalInputCount++] = &tags[index];
        const sl::Result evalResult = optResult == sl::Result::eOk
            ? api.evaluate(sl::kFeatureDLSS, *token, evalInputs.data(),
                           evalInputCount, cmd)
            : optResult;
        ++s.submitCount;

        if (copyBack && outResource != out && evalResult == sl::Result::eOk) {
            transition(outResource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(out, outResource);
        }
        transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);

        gDlssOutputState.store(
            (copyBack && outResource != out && evalResult == sl::Result::eOk)
                ? static_cast<int32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE)
                : 0);

        if (gStreamlineInputInvalidationFailed.load()) return -1;
        if (evalResult != sl::Result::eOk) {
            logWarn("streamline evaluate tags=0x%X options=0x%X evaluate=0x%X "
                    "(mode=%d, out=%ux%u)",
                    static_cast<unsigned>(tagResult), static_cast<unsigned>(optResult),
                    static_cast<unsigned>(evalResult), static_cast<int>(opt.mode),
                    opt.outputWidth, opt.outputHeight);
            return 0;
        }
        s.resetPending = false;
        return 1;
    }
#endif

int32_t mcdr_dlss_prepare_sr(void* commandList, const mcdr_dlss_input* seed,
    const mcdr_raster_camera* camera, int32_t tier, int32_t preset,
    const int32_t resourceStates[5], void** retainedCandidateToken) {
    if (retainedCandidateToken == nullptr) return MCDR_SR_PREPARE_INVALID_ARGUMENT;
    *retainedCandidateToken = nullptr;
    if (commandList == nullptr || seed == nullptr || tier < 0 || tier > 4 ||
        preset < 0 || preset > NVSDK_NGX_DLSS_Hint_Render_Preset_O ||
        seed->renderWidth <= 0 || seed->renderHeight <= 0 || seed->targetWidth <= 0 ||
        seed->targetHeight <= 0 || (tier == 4 && (seed->renderWidth != seed->targetWidth ||
        seed->renderHeight != seed->targetHeight)) ||
        !std::isfinite(seed->preExposure) || seed->preExposure < 0.0f ||
        (seed->preExposure > 0.0f && !std::isfinite(1.0f / seed->preExposure)) ||
        !std::isfinite(seed->exposureScale) || seed->exposureScale < 0.0f)
        return MCDR_SR_PREPARE_INVALID_ARGUMENT;
    static constexpr uint32_t qualities[] = {
        NVSDK_NGX_PerfQuality_Value_UltraPerformance, NVSDK_NGX_PerfQuality_Value_MaxPerf,
        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_DLAA};
    auto candidate = std::make_unique<SrCandidate>();
    candidate->input = *seed;
    candidate->quality = qualities[tier];
    candidate->tier = tier;
    candidate->preset = preset;
    candidate->flags = computeFeatureFlags(*seed);
    candidate->streamline = dlssImplStreamline();
    auto* prepared = candidate.get();
    *retainedCandidateToken = candidate.release();
    ++gSrCandidateCount;
    if (prepared->streamline) {
#if MCDR_DLSS_HAVE_STREAMLINE
        auto& api = streamlineEvalApi();
        using SlAllocateResources = sl::Result (*)(sl::CommandBuffer*, sl::Feature,
            const sl::ViewportHandle&);
        const auto allocate = gSlInterposer == nullptr ? nullptr : reinterpret_cast<SlAllocateResources>(
            ::GetProcAddress(gSlInterposer, "slAllocateResources"));
        if (!allocate || !api.setOptions || !api.setConstants || !api.setTags || !api.newFrameToken)
            return MCDR_SR_PREPARE_REJECTED;
        if (camera == nullptr) return MCDR_SR_PREPARE_NOT_READY;
        if (resourceStates == nullptr) return MCDR_SR_PREPARE_INVALID_ARGUMENT;
        if ((camera->flags & 1u) == 0 || (camera->flags & 2u) != 0 ||
            !std::isfinite(camera->nearPlane) || !std::isfinite(camera->farPlane) ||
            !std::isfinite(camera->verticalFovRadians) || camera->nearPlane <= 0.0f ||
            camera->farPlane <= camera->nearPlane || camera->verticalFovRadians <= 0.0f ||
            camera->verticalFovRadians >= 3.141592654f)
            return MCDR_SR_PREPARE_INVALID_ARGUMENT;
        for (uint32_t index = 0; index < 16; ++index) {
            if (!std::isfinite(camera->projection[index]) || !std::isfinite(camera->viewRotation[index]))
                return MCDR_SR_PREPARE_INVALID_ARGUMENT;
        }
        for (double coordinate : camera->origin) {
            if (!std::isfinite(coordinate) || !std::isfinite(static_cast<float>(coordinate)))
                return MCDR_SR_PREPARE_INVALID_ARGUMENT;
        }
        mcdr_dlss_constants candidateCamera{};
        mat4Multiply(camera->viewRotation, camera->projection, candidateCamera.viewProjection);
        std::memcpy(candidateCamera.prevViewProjection, candidateCamera.viewProjection,
            sizeof(candidateCamera.prevViewProjection));
        std::memcpy(candidateCamera.projection, camera->projection, sizeof(candidateCamera.projection));
        candidateCamera.jitterOffsetX = seed->jitterOffsetX;
        candidateCamera.jitterOffsetY = seed->jitterOffsetY;
        candidateCamera.mvecScaleX = seed->mvScaleX / seed->renderWidth;
        candidateCamera.mvecScaleY = seed->mvScaleY / seed->renderHeight;
        candidateCamera.cameraPosX = static_cast<float>(camera->origin[0]);
        candidateCamera.cameraPosY = static_cast<float>(camera->origin[1]);
        candidateCamera.cameraPosZ = static_cast<float>(camera->origin[2]);
        candidateCamera.cameraRightX = camera->viewRotation[0];
        candidateCamera.cameraRightY = camera->viewRotation[4];
        candidateCamera.cameraRightZ = camera->viewRotation[8];
        candidateCamera.cameraUpX = camera->viewRotation[1];
        candidateCamera.cameraUpY = camera->viewRotation[5];
        candidateCamera.cameraUpZ = camera->viewRotation[9];
        candidateCamera.cameraFwdX = camera->viewRotation[2];
        candidateCamera.cameraFwdY = camera->viewRotation[6];
        candidateCamera.cameraFwdZ = camera->viewRotation[10];
        candidateCamera.cameraNear = camera->nearPlane;
        candidateCamera.cameraFar = camera->farPlane;
        candidateCamera.cameraFovRadians = camera->verticalFovRadians;
        candidateCamera.cameraAspectRatio = static_cast<float>(seed->renderWidth) / seed->renderHeight;
        candidateCamera.depthInverted = (camera->flags & 256u) != 0 ? 1u : 0u;
        candidateCamera.cameraMotionIncluded = 1;
        candidateCamera.reset = 1;
        candidateCamera.renderingGameFrames = 1;
        const auto options = srStreamlineOptions(*seed, prepared->quality, preset);
        sl::Constants constants{};
        if (!options || !makeStreamlineCamera(candidateCamera, 0, constants))
            return MCDR_SR_PREPARE_INVALID_ARGUMENT;
        using SlGetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
        using SlGetOptimalSettings = sl::Result (*)(const sl::DLSSOptions&, sl::DLSSOptimalSettings&);
        const auto getFeatureFunction = reinterpret_cast<SlGetFeatureFunction>(
            ::GetProcAddress(gSlInterposer, "slGetFeatureFunction"));
        void* queryFunction = nullptr;
        if (!getFeatureFunction || getFeatureFunction(sl::kFeatureDLSS,
            "slDLSSGetOptimalSettings", queryFunction) != sl::Result::eOk || !queryFunction)
            return MCDR_SR_PREPARE_REJECTED;
        sl::DLSSOptimalSettings settings{};
        if (reinterpret_cast<SlGetOptimalSettings>(queryFunction)(*options, settings) != sl::Result::eOk ||
            settings.optimalRenderWidth == 0 || settings.optimalRenderHeight == 0 ||
            static_cast<uint32_t>(seed->renderWidth) < settings.renderWidthMin ||
            static_cast<uint32_t>(seed->renderHeight) < settings.renderHeightMin ||
            (settings.renderWidthMax != 0 && static_cast<uint32_t>(seed->renderWidth) > settings.renderWidthMax) ||
            (settings.renderHeightMax != 0 && static_cast<uint32_t>(seed->renderHeight) > settings.renderHeightMax))
            return MCDR_SR_PREPARE_REJECTED;
        ID3D12Resource* resources[] = {
            reinterpret_cast<ID3D12Resource*>(seed->pInColor),
            reinterpret_cast<ID3D12Resource*>(seed->pInDepth),
            reinterpret_cast<ID3D12Resource*>(seed->pInMotionVectors),
            reinterpret_cast<ID3D12Resource*>(seed->pInOutput),
            reinterpret_cast<ID3D12Resource*>(seed->pInExposure)};
        for (uint32_t index = 0; index < 4; ++index) {
            if (!resources[index]) return MCDR_SR_PREPARE_INVALID_ARGUMENT;
            const auto description = resources[index]->GetDesc();
            const auto width = index == 3 ? seed->targetWidth : seed->renderWidth;
            const auto height = index == 3 ? seed->targetHeight : seed->renderHeight;
            if (description.Width < static_cast<UINT64>(width) ||
                description.Height < static_cast<UINT>(height)) return MCDR_SR_PREPARE_INVALID_ARGUMENT;
        }
        if ((resources[3]->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
            return MCDR_SR_PREPARE_INVALID_ARGUMENT;
        for (int32_t slot = 0; slot < 3; ++slot) {
            if (!gSrViewportSlots[slot]) {
                prepared->viewportSlot = slot;
                prepared->viewport = static_cast<uint32_t>(slot + 2);
                gSrViewportSlots[slot] = true;
                prepared->viewportTouched = true;
                break;
            }
        }
        if (prepared->viewportSlot < 0) return MCDR_SR_PREPARE_NOT_READY;
        auto& allocationConstants = gSrAllocationConstants[prepared->viewportSlot];
        if (allocationConstants &&
            (allocationConstants->depthInverted != constants.depthInverted ||
             allocationConstants->motionVectorsJittered != constants.motionVectorsJittered))
            return MCDR_SR_PREPARE_REJECTED;
        if (allocationConstants) constants = *allocationConstants;
        constants.reset = sl::Boolean::eTrue;
        const sl::ViewportHandle viewport(prepared->viewport);
        const auto& option = *options;
        if (api.setOptions(viewport, option) != sl::Result::eOk) return MCDR_SR_PREPARE_REJECTED;
        prepared->mode = static_cast<int32_t>(option.mode);
        prepared->preExposure = option.preExposure;
        prepared->exposureScale = option.exposureScale;
        prepared->hdr = option.colorBuffersHDR == sl::Boolean::eTrue;
        const uint32_t allocationFrame = 0;
        sl::FrameToken* allocationToken = nullptr;
        if (api.newFrameToken(allocationToken, &allocationFrame) != sl::Result::eOk ||
            allocationToken == nullptr) return MCDR_SR_PREPARE_REJECTED;
        const auto constantsResult = api.setConstants(constants, *allocationToken, viewport);
        if (constantsResult == sl::Result::eOk) allocationConstants = constants;
        const sl::Extent renderExtent{0, 0, static_cast<uint32_t>(seed->renderWidth),
            static_cast<uint32_t>(seed->renderHeight)};
        const sl::Extent outputExtent{0, 0, static_cast<uint32_t>(seed->targetWidth),
            static_cast<uint32_t>(seed->targetHeight)};
        const sl::Extent exposureExtent{0, 0, 1, 1};
        const sl::BufferType bufferTypes[] = {sl::kBufferTypeScalingInputColor,
            sl::kBufferTypeDepth, sl::kBufferTypeMotionVectors,
            sl::kBufferTypeScalingOutputColor, sl::kBufferTypeExposure};
        sl::Resource taggedResources[5] = {
            sl::Resource(sl::ResourceType::eTex2d, resources[0]),
            sl::Resource(sl::ResourceType::eTex2d, resources[1]),
            sl::Resource(sl::ResourceType::eTex2d, resources[2]),
            sl::Resource(sl::ResourceType::eTex2d, resources[3]),
            sl::Resource(sl::ResourceType::eTex2d, resources[4])};
        sl::ResourceTag tags[5] = {
            {nullptr, bufferTypes[0], sl::ResourceLifecycle::eValidUntilEvaluate},
            {nullptr, bufferTypes[1], sl::ResourceLifecycle::eValidUntilEvaluate},
            {nullptr, bufferTypes[2], sl::ResourceLifecycle::eValidUntilEvaluate},
            {nullptr, bufferTypes[3], sl::ResourceLifecycle::eValidUntilEvaluate},
            {nullptr, bufferTypes[4], sl::ResourceLifecycle::eValidUntilEvaluate}};
        for (uint32_t index = 0; index < 5; ++index) {
            taggedResources[index] = sl::Resource(sl::ResourceType::eTex2d,
                resources[index], static_cast<D3D12_RESOURCE_STATES>(resourceStates[index]));
            tags[index] = sl::ResourceTag(resources[index] ? &taggedResources[index] : nullptr,
                bufferTypes[index], sl::ResourceLifecycle::eValidUntilEvaluate,
                index == 4 ? &exposureExtent : (index == 3 ? &outputExtent : &renderExtent));
        }
        const uint32_t tagCount = resources[4] ? 5u : 4u;
        sl::Result result = constantsResult;
        if (result == sl::Result::eOk) {
            prepared->tagsPresent = true;
            result = api.setTags(*allocationToken, viewport, tags, tagCount,
                static_cast<sl::CommandBuffer*>(commandList));
        }
        if (result == sl::Result::eOk)
            result = allocate(static_cast<sl::CommandBuffer*>(commandList), sl::kFeatureDLSS, viewport);
        for (auto& tag : tags) tag.resource = nullptr;
        const auto clearResult = api.setTags(*allocationToken, viewport, tags, tagCount,
            static_cast<sl::CommandBuffer*>(commandList));
        if (clearResult == sl::Result::eOk) prepared->tagsPresent = false;
        if (result != sl::Result::eOk || clearResult != sl::Result::eOk) {
            logWarn("DLSS candidate allocation failed: allocate=0x%X clear=0x%X",
                static_cast<unsigned>(result), static_cast<unsigned>(clearResult));
            return MCDR_SR_PREPARE_REJECTED;
        }
        prepared->ready = true;
        return MCDR_SR_PREPARE_READY;
#else
        return MCDR_SR_PREPARE_REJECTED;
#endif
    }
    if (!s.initOk || !s.paramsOk || s.disabled) return MCDR_SR_PREPARE_REJECTED;
    auto result = NVSDK_NGX_D3D12_GetCapabilityParameters(&prepared->params);
    if (NVSDK_NGX_FAILED(result) || !prepared->params) return MCDR_SR_PREPARE_REJECTED;
    unsigned int optimalWidth = 0, optimalHeight = 0;
    unsigned int maximumWidth = 0, maximumHeight = 0, minimumWidth = 0, minimumHeight = 0;
    float sharpness = 0.0f;
    result = NGX_DLSS_GET_OPTIMAL_SETTINGS(prepared->params,
        static_cast<uint32_t>(seed->targetWidth), static_cast<uint32_t>(seed->targetHeight),
        static_cast<NVSDK_NGX_PerfQuality_Value>(prepared->quality), &optimalWidth,
        &optimalHeight, &maximumWidth, &maximumHeight, &minimumWidth, &minimumHeight,
        &sharpness);
    if (NVSDK_NGX_FAILED(result) || optimalWidth == 0 || optimalHeight == 0 ||
        static_cast<uint32_t>(seed->renderWidth) < minimumWidth ||
        static_cast<uint32_t>(seed->renderHeight) < minimumHeight ||
        (maximumWidth != 0 && static_cast<uint32_t>(seed->renderWidth) > maximumWidth) ||
        (maximumHeight != 0 && static_cast<uint32_t>(seed->renderHeight) > maximumHeight))
        return MCDR_SR_PREPARE_REJECTED;
    if (const auto* key = presetHintKey(prepared->quality))
        NVSDK_NGX_Parameter_SetI(prepared->params, key, preset);
    NVSDK_NGX_DLSS_Create_Params parameters{};
    parameters.Feature.InWidth = static_cast<uint32_t>(seed->renderWidth);
    parameters.Feature.InHeight = static_cast<uint32_t>(seed->renderHeight);
    parameters.Feature.InTargetWidth = static_cast<uint32_t>(seed->targetWidth);
    parameters.Feature.InTargetHeight = static_cast<uint32_t>(seed->targetHeight);
    parameters.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(prepared->quality);
    parameters.InFeatureCreateFlags = prepared->flags;
    parameters.InEnableOutputSubrects = false;
    result = NGX_D3D12_CREATE_DLSS_EXT(static_cast<ID3D12GraphicsCommandList*>(commandList),
        1u, 1u, &prepared->feature, prepared->params, &parameters);
    if (NVSDK_NGX_FAILED(result) || !prepared->feature) {
        logWarn("DLSS candidate CreateFeature failed: %s", ngxResultString(result).c_str());
        return MCDR_SR_PREPARE_REJECTED;
    }
    prepared->ready = true;
    return MCDR_SR_PREPARE_READY;
}

void mcdr_dlss_commit_sr(void* candidateToken, void** retiredToken) {
    if (retiredToken == nullptr) return;
    *retiredToken = nullptr;
    auto* candidate = static_cast<SrCandidate*>(candidateToken);
    if (candidate == nullptr || !candidate->ready) return;
    std::swap(candidate->params, s.params);
    std::swap(candidate->feature, s.feature);
    const auto previousViewport = s.srViewport;
    const auto previousSlot = s.srViewportSlot;
    s.srViewport = candidate->viewport;
    s.srViewportSlot = candidate->viewportSlot;
    candidate->viewport = previousViewport;
    candidate->viewportSlot = previousSlot;
    candidate->viewportTouched = candidate->streamline && (s.srPrepared || s.slEvalOptsValid);
    s.renderWidth = candidate->input.renderWidth;
    s.renderHeight = candidate->input.renderHeight;
    s.targetWidth = candidate->input.targetWidth;
    s.targetHeight = candidate->input.targetHeight;
    s.qualityValue = candidate->quality;
    s.requestedQuality = candidate->tier;
    s.effectiveQuality = candidate->tier;
    s.requestedPreset = candidate->preset;
    s.effectivePreset = candidate->preset;
    s.featureFlags = candidate->flags;
    s.qualityIsExplicit = true;
    s.featureOk = true;
    s.paramsOk = candidate->streamline ? s.paramsOk : s.params != nullptr;
    s.createdWarned = false;
    s.evalFailWarned = false;
    s.evalOk = false;
    s.resetPending = true;
    s.createdThisFrame = true;
    s.srPrepared = true;
    s.preparedFailure.reset();
    s.slOptsApplied = false;
    s.slEvalOptsValid = candidate->streamline;
    s.slEvalOptMode = candidate->mode;
    s.slEvalOptOutW = static_cast<uint32_t>(s.targetWidth);
    s.slEvalOptOutH = static_cast<uint32_t>(s.targetHeight);
    s.slEvalOptPreExposure = candidate->preExposure;
    s.slEvalOptExposureScale = candidate->exposureScale;
    s.slEvalOptQuality = candidate->quality;
    s.slEvalOptPreset = candidate->preset;
    s.slEvalOptHdr = candidate->hdr;
    candidate->ready = false;
    *retiredToken = candidate;
}

int32_t mcdr_dlss_abort_sr(void* candidateToken) {
    auto* candidate = static_cast<SrCandidate*>(candidateToken);
    if (candidate == nullptr) return 1;
#if MCDR_DLSS_HAVE_STREAMLINE
    if (candidate->streamline && candidate->viewportTouched) {
        using SlFreeResources = sl::Result (*)(sl::Feature, const sl::ViewportHandle&);
        const auto freeResources = gSlInterposer == nullptr ? nullptr : reinterpret_cast<SlFreeResources>(
            ::GetProcAddress(gSlInterposer, "slFreeResources"));
        if (!freeResources) return 0;
        const sl::ViewportHandle viewport(candidate->viewport);
        if (candidate->tagsPresent) {
            auto& api = streamlineEvalApi();
            if (!api.newFrameToken || !api.setTags) return 0;
            const uint32_t allocationFrame = 0;
            sl::FrameToken* allocationToken = nullptr;
            if (api.newFrameToken(allocationToken, &allocationFrame) != sl::Result::eOk ||
                allocationToken == nullptr) return 0;
            const sl::ResourceTag tags[] = {
                {nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate},
                {nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate},
                {nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate},
                {nullptr, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate},
                {nullptr, sl::kBufferTypeExposure, sl::ResourceLifecycle::eValidUntilEvaluate}};
            const auto clearResult = api.setTags(*allocationToken, viewport, tags,
                static_cast<uint32_t>(std::size(tags)), nullptr);
            if (clearResult != sl::Result::eOk) return 0;
            candidate->tagsPresent = false;
        }
        const auto result = freeResources(sl::kFeatureDLSS, viewport);
        if (result != sl::Result::eOk && result != sl::Result::eErrorInvalidParameter) return 0;
        candidate->viewportTouched = false;
    }
#endif
    if (candidate->feature) {
        const auto result = NVSDK_NGX_D3D12_ReleaseFeature(candidate->feature);
        if (NVSDK_NGX_FAILED(result)) return 0;
        candidate->feature = nullptr;
    }
    if (candidate->params) {
        const auto result = NVSDK_NGX_D3D12_DestroyParameters(candidate->params);
        if (NVSDK_NGX_FAILED(result)) return 0;
        candidate->params = nullptr;
    }
    if (candidate->viewportSlot >= 0) gSrViewportSlots[candidate->viewportSlot] = false;
    delete candidate;
    --gSrCandidateCount;
    return 1;
}

int32_t mcdr_dlss_submit_display_frame(void* commandList, const mcdr_dlss_input* input,
    const mcdr_dlss_constants* camera, int32_t colorState, int32_t depthState,
    int32_t motionState, void* ui, int32_t uiState, uint64_t realFrameId, uint32_t guidesDilated,
    float frameTimeDeltaMs,
    DlssTransitionFn transitionFn, void* transitionContext) {
#if MCDR_DLSS_HAVE_STREAMLINE
    if (!dlssImplStreamline() || !commandList || !input || !camera || !ui || !transitionFn ||
        !input->pInOutput || !input->pInDepth || !input->pInMotionVectors || realFrameId == 0 ||
        input->renderWidth != input->targetWidth || input->renderHeight != input->targetHeight ||
        input->renderWidth <= 0 || input->renderHeight <= 0 ||
        !std::isfinite(frameTimeDeltaMs) || !(frameTimeDeltaMs > 0)) return -1;
    const int32_t mode = gDlssgMode.load();
    if (mode == 0) return mcdr_dlss_invalidate_inputs() == 1 ? 0 : -1;
    auto& api = streamlineEvalApi();
    refreshFgApi(api);
    if (!api.setFgOptions || !api.setConstants || !api.setTags || !api.getFgState ||
        !gLastFrameToken || gDisplayConstantsFrame == gStreamlineFrameSerial ||
        gStreamlineInputInvalidationFailed.load()) return -1;
    const sl::ViewportHandle viewport(1);
    sl::Constants constants{};
    if (!makeStreamlineCamera(*camera, guidesDilated, constants)) return -1;
    gDisplayConstantsFrame = gStreamlineFrameSerial;
    if (api.setConstants(constants, *gLastFrameToken, viewport) != sl::Result::eOk) {
        (void)mcdr_dlss_invalidate_inputs();
        return -1;
    }
    const int32_t maximumFrames = std::clamp<int32_t>(gDlssgFrames.load(), 1, 5);
    const bool dynamic = mode == 2 && gReflex.frameLimitUs == 0;
    int32_t frames = maximumFrames;
    if (mode == 2 && !dynamic) {
        const float targetMs = static_cast<float>(gReflex.frameLimitUs) / 1000.0f;
        frames = std::clamp<int32_t>(static_cast<int32_t>(targetMs / frameTimeDeltaMs) - 1,
            1, maximumFrames);
    }
    const int32_t appliedFrames = dynamic ? -1 : frames;
    auto& state = gStreamlineFgState;
    if (state.mode != mode || state.frames != appliedFrames ||
        state.width != static_cast<uint32_t>(input->targetWidth) ||
        state.height != static_cast<uint32_t>(input->targetHeight)) {
        sl::DLSSGOptions options{};
        options.mode = dynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
        options.numFramesToGenerate = dynamic ? 1u : static_cast<uint32_t>(frames);
        options.mvecDepthWidth = static_cast<uint32_t>(input->renderWidth);
        options.mvecDepthHeight = static_cast<uint32_t>(input->renderHeight);
        options.colorWidth = static_cast<uint32_t>(input->targetWidth);
        options.colorHeight = static_cast<uint32_t>(input->targetHeight);
        if (api.setFgOptions(viewport, options) != sl::Result::eOk) {
            (void)mcdr_dlss_invalidate_inputs();
            return -1;
        }
        state = {mode, appliedFrames, static_cast<uint32_t>(input->targetWidth),
            static_cast<uint32_t>(input->targetHeight)};
    }
    const auto readonly = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for (void* resource : {static_cast<void*>(input->pInOutput), static_cast<void*>(input->pInDepth),
        static_cast<void*>(input->pInMotionVectors), ui})
        transitionFn(transitionContext, resource, static_cast<int32_t>(readonly));
    sl::Resource color(sl::ResourceType::eTex2d, input->pInOutput, readonly);
    sl::Resource depth(sl::ResourceType::eTex2d, input->pInDepth, readonly);
    sl::Resource motion(sl::ResourceType::eTex2d, input->pInMotionVectors, readonly);
    sl::Resource uiResource(sl::ResourceType::eTex2d, ui, readonly);
    const auto result = tagStreamlineFrameInputs(api, *gLastFrameToken, viewport,
        static_cast<ID3D12GraphicsCommandList*>(commandList), *input, depth, motion, color, uiResource);
    transitionFn(transitionContext, input->pInOutput, colorState);
    transitionFn(transitionContext, input->pInDepth, depthState);
    transitionFn(transitionContext, input->pInMotionVectors, motionState);
    transitionFn(transitionContext, ui, uiState);
    if (result != sl::Result::eOk) {
        (void)mcdr_dlss_invalidate_inputs();
        return -1;
    }
    gDisplayInputsAccepted = true;
    return 1;
#else
    (void)commandList; (void)input; (void)camera; (void)colorState; (void)depthState;
    (void)motionState; (void)ui; (void)uiState; (void)realFrameId; (void)guidesDilated;
    (void)frameTimeDeltaMs; (void)transitionFn; (void)transitionContext;
    return -1;
#endif
}

int32_t mcdr_dlss_prepare_fg(int32_t mode, int32_t numFrames, uint32_t displayWidth,
    uint32_t displayHeight, uint32_t guideWidth, uint32_t guideHeight,
    uint32_t* maxGeneratedFrames) {
#if MCDR_DLSS_HAVE_STREAMLINE
    if (maxGeneratedFrames != nullptr) *maxGeneratedFrames = 0;
    if (mode == 0) return MCDR_FG_PREPARE_READY;
    if ((mode != 1 && mode != 2) || numFrames < 1 || numFrames > 5 || displayWidth == 0 ||
        displayHeight == 0 || guideWidth == 0 || guideHeight == 0 ||
        guideWidth != displayWidth || guideHeight != displayHeight)
        return MCDR_FG_PREPARE_INVALID_ARGUMENT;
    if (!dlssImplStreamline() || gSlDlssLoaded.load() != 1 || mcdr_reflex_available() != 1)
        return MCDR_FG_PREPARE_REJECTED;
    auto& api = streamlineEvalApi();
    refreshFgApi(api);
    if (!api.getFgState || !api.setFgOptions) return MCDR_FG_PREPARE_REJECTED;
    sl::DLSSGOptions options{};
    options.mode = mode == 2 ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
    options.numFramesToGenerate = static_cast<uint32_t>(numFrames);
    options.mvecDepthWidth = guideWidth;
    options.mvecDepthHeight = guideHeight;
    options.colorWidth = displayWidth;
    options.colorHeight = displayHeight;
    sl::DLSSGState state{};
    const sl::Result result = api.getFgState(sl::ViewportHandle(1), state, &options);
    if (result != sl::Result::eOk || state.status != sl::DLSSGStatus::eOk ||
        state.minWidthOrHeight > std::min(displayWidth, displayHeight) ||
        state.numFramesToGenerateMax < static_cast<uint32_t>(numFrames) ||
        (mode == 2 && state.bIsDynamicMFGSupported != sl::Boolean::eTrue))
        return MCDR_FG_PREPARE_REJECTED;
    if (state.lastPresentInputsProcessingCompletionFenceValue != 0) {
        auto* fence = static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence);
        if (fence == nullptr) return MCDR_FG_PREPARE_REJECTED;
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) return MCDR_FG_PREPARE_REJECTED;
        if (completed < state.lastPresentInputsProcessingCompletionFenceValue)
            return MCDR_FG_PREPARE_NOT_READY;
    }
    if (maxGeneratedFrames != nullptr) *maxGeneratedFrames = state.numFramesToGenerateMax;
    return MCDR_FG_PREPARE_READY;
#else
    (void)mode; (void)numFrames; (void)displayWidth; (void)displayHeight;
    (void)guideWidth; (void)guideHeight;
    if (maxGeneratedFrames != nullptr) *maxGeneratedFrames = 0;
    return MCDR_FG_PREPARE_REJECTED;
#endif
}

int32_t mcdr_dlss_fg_after_present(void) {
#if MCDR_DLSS_HAVE_STREAMLINE
    if (!gDisplayInputsAccepted) return 0;
    gDisplayInputsAccepted = false;
    auto& api = streamlineEvalApi();
    sl::DLSSGState state{};
    if (!api.getFgState || api.getFgState(sl::ViewportHandle(1), state, nullptr) != sl::Result::eOk ||
        state.status != sl::DLSSGStatus::eOk) return 0;
    gFgPresented.store(static_cast<int32_t>(state.numFramesActuallyPresented));
    return 1;
#else
    return 0;
#endif
}

void mcdr_dlss_clear_camera_constants(void) {
#if MCDR_DLSS_HAVE_STREAMLINE
    gCameraConstantsValid = false;
#endif
}

#if !MCDR_DLSS_HAVE_STREAMLINE
int32_t mcdr_dlss_set_constants(const mcdr_dlss_constants* constants) {
    return constants != nullptr ? 1 : 0;
}
#endif

int32_t mcdr_dlss_install_recording_pin(void* pinFn) {
    gPinTexture = reinterpret_cast<PinTextureFn>(pinFn);
    return 1;
}

int32_t mcdr_dlss_install_streamline_shutdown(void* shutdownFn) {
    gShutdownStreamline = reinterpret_cast<ShutdownStreamlineFn>(shutdownFn);
    return 1;
}

int32_t mcdr_dlss_inputs_ready_for_present(void) {
    return gStreamlineInputInvalidationFailed.load() ? 0 : 1;
}

int32_t mcdr_dlss_invalidate_inputs(void) {
#if MCDR_DLSS_HAVE_STREAMLINE
    if (!dlssImplStreamline() || gSlDlssLoaded.load() != 1) return 1;
    auto& api = streamlineEvalApi();
    const sl::ViewportHandle viewport(1);
    bool ok = true;
    if (gLastFrameToken != nullptr) {
        ok = api.setTags != nullptr &&
             clearStreamlineFrameTags(api, *gLastFrameToken, viewport) == sl::Result::eOk;
        if (s.srViewport != 1) {
            const sl::ViewportHandle srViewport(s.srViewport);
            ok = api.setTags != nullptr &&
                clearStreamlineFrameTags(api, *gLastFrameToken, srViewport) == sl::Result::eOk && ok;
        }
    }
    if (api.setFgOptions != nullptr && gStreamlineFgState.mode != 0) {
        sl::DLSSGOptions off{};
        off.mode = sl::DLSSGMode::eOff;
        ok = (api.setFgOptions(viewport, off) == sl::Result::eOk) && ok;
    } else if (gStreamlineFgState.mode > 0) {
        ok = false;
    }
    if (ok) gStreamlineFgState = {0, 0, 0, 0};
    const bool failedBefore = gStreamlineInputInvalidationFailed.exchange(!ok);
    gFgPresented.store(1);
    gDisplayInputsAccepted = false;
    gDlssOutputState.store(0);
    if (!ok && !failedBefore) logWarn("Streamline input invalidation failed; presentation must be stopped");
    return ok ? 1 : 0;
#else
    return 1;
#endif
}

int32_t mcdr_dlss_evaluate(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                           void* tlCtx) {
    if (in == nullptr) {
        return -1;
    }
    if (!std::isfinite(in->preExposure) || in->preExposure < 0.0f ||
        (in->preExposure > 0.0f && !std::isfinite(1.0f / in->preExposure)) ||
        !std::isfinite(in->exposureScale) || in->exposureScale < 0.0f) return -1;
    if (s.srPrepared) {
        s.preparedFailure = preparedSrMismatch(*in);
        if (s.preparedFailure) return 0;
    }
#if MCDR_DLSS_HAVE_STREAMLINE
    if (dlssImplStreamline()) {
        return streamlineEvaluate(commandList, *in, tl, tlCtx);
    }
#endif
    if (s.disabled) {
        return -1;
    }
    if (!s.featureOk || s.params == nullptr) {
        return 0;
    }
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    if (cmd == nullptr) {
        return 0;
    }
    s.transitionFn = tl;
    s.transitionCtx = tlCtx;
    auto* color = reinterpret_cast<ID3D12Resource*>(in->pInColor);
    auto* exposure = reinterpret_cast<ID3D12Resource*>(in->pInExposure);
    auto* depth = reinterpret_cast<ID3D12Resource*>(in->pInDepth);
    auto* mv = reinterpret_cast<ID3D12Resource*>(in->pInMotionVectors);
    auto* out = reinterpret_cast<ID3D12Resource*>(in->pInOutput);
    if (color == nullptr || depth == nullptr || mv == nullptr || out == nullptr) {
        return 0;
    }
    if (!isMappedReadable(color) || !isMappedReadable(depth) || !isMappedReadable(mv) ||
        !isMappedReadable(out)) {
        if (!s.evalFailWarned) {
            s.evalFailWarned = true;
            s.failure = "invalid texture handle";
            logError("DLSS skipped — a texture handle in mcdr_dlss_input is not a readable "
                     "pointer (color=%p depth=%p mv=%p out=%p). That is a data-plane bug (the JVM "
                     "handed over a stale/invalid handle), not a DLSS failure; DLSS stays enabled "
                     "so the next frame can retry with a valid set.",
                     static_cast<const void*>(color), static_cast<const void*>(depth),
                     static_cast<const void*>(mv), static_cast<const void*>(out));
        }
        return 0;
    }

    ID3D12Resource* mvForDlss = mv;
    const bool mvIsRg16 = (mv->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT);
    if (!mvIsRg16) {
        const D3D12_RESOURCE_DESC md = mv->GetDesc();
        const bool reuse = (s.mvBuffer != nullptr) && (s.mvBuffer->GetDesc().Width == md.Width) &&
                           (s.mvBuffer->GetDesc().Height == md.Height);
        if (!reuse) {
            destroyTrackedTexture(s.transitionCtx, &s.mvBufferObject);
            s.mvBuffer = nullptr;
            void* mvObj = nullptr;
            ID3D12Resource* mvRes = nullptr;
            if (!createTrackedTexture(s.transitionCtx, static_cast<UINT>(md.Width), md.Height,
                                      md.Format, md.MipLevels, md.DepthOrArraySize,
                                      md.SampleDesc.Count, 0, &mvObj, &mvRes)) {
                logError("motion-vector intermediate creation failed (fmt=%d); DLSS disabled",
                         static_cast<int>(md.Format));
                s.disabled = true;
                return -1;
            }
            s.mvBufferObject = mvObj;
            s.mvBuffer = mvRes;
            s.mvCopyResourceState = D3D12_RESOURCE_STATE_COMMON;
        }
        mvForDlss = s.mvBuffer;
        transition(mv, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(mvForDlss, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(mvForDlss, mv);
        transition(mvForDlss, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        s.mvCopyResourceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    transition(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (exposure) transition(exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(mvForDlss, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* ngxTarget = out;
    const bool directOutput = (out->GetDesc().Flags &
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    bool viaHandover = directOutput;
    if (directOutput) transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!directOutput && s.externalOutput != nullptr) {
        const D3D12_RESOURCE_DESC ed = s.externalOutput->GetDesc();
        const bool fits = (ed.Width == static_cast<UINT64>(in->targetWidth)) &&
                          (ed.Height == static_cast<UINT>(in->targetHeight)) &&
                          (ed.Format == out->GetDesc().Format);
        if (fits) {
            ngxTarget = s.externalOutput;
            viaHandover = true;
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(ngxTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else {
            logError("the JVM-supplied DLSS output texture does not fit the target — "
                     "got %llux%u fmt=%d, need %dx%d fmt=%d; falling back to the private UAV "
                     "texture (which this machine cannot write, so DLSS output will be empty)",
                     static_cast<unsigned long long>(ed.Width), ed.Height,
                     static_cast<int>(ed.Format), in->targetWidth, in->targetHeight,
                     static_cast<int>(out->GetDesc().Format));
        }
    }
    if (!directOutput && ngxTarget == out) {
        ID3D12Resource* priv = createDlssOutput(static_cast<UINT>(in->targetWidth),
                                                static_cast<UINT>(in->targetHeight),
                                                out->GetDesc().Format);
        if (priv != nullptr) {
            ngxTarget = priv;
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(priv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    if (!directOutput && ngxTarget == out) {
        logError("no usable DLSS output texture this frame (the hand-over texture was "
                 "rejected and the private one could not be created); skipping the evaluate "
                 "instead of transitioning a non-UAV target to UNORDERED_ACCESS");
        return 0;
    }
    NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = ngxTarget;
    ep.Feature.InSharpness = 0.0f;
    ep.pInDepth = depth;
    ep.pInMotionVectors = mvForDlss;
    ep.pInExposureTexture = exposure;
    ep.InJitterOffsetX = in->jitterOffsetX;
    ep.InJitterOffsetY = in->jitterOffsetY;
    ep.InRenderSubrectDimensions.Width = static_cast<unsigned>(in->renderWidth);
    ep.InRenderSubrectDimensions.Height = static_cast<unsigned>(in->renderHeight);
    ep.InReset = (in->reset != 0 || s.resetPending) ? 1 : 0;
    ep.InMVScaleX = in->mvScaleX;
    ep.InMVScaleY = in->mvScaleY;
    ep.InPreExposure = (in->preExposure != 0.0f) ? in->preExposure : 1.0f;
    ep.InExposureScale = (in->exposureMode == MCDR_DLSS_EXPOSURE_NONE && in->exposureScale != 0.0f)
                             ? in->exposureScale
                             : 1.0f;

    const NVSDK_NGX_Result er = NGX_D3D12_EVALUATE_DLSS_EXT(cmd, s.feature, s.params, &ep);
    if (NVSDK_NGX_FAILED(er)) {
        if (!s.evalFailWarned) {
            s.evalFailWarned = true;
            s.failure = "NGX_D3D12_EVALUATE_DLSS_EXT";
            logError("DLSS Evaluate failed: %s; DLSS disabled for this process "
                     "(the existing linear-blit path stays in effect). Resources that had UAV "
                     "this attempt: %s. Inputs: color=%p depth=%p mv=%p out=%p",
                     ngxResultString(er).c_str(),
                     ngxTarget == out ? "none (JVM target used directly)"
                                      : "the private output texture only",
                     static_cast<const void*>(color), static_cast<const void*>(depth),
                     static_cast<const void*>(mvForDlss), static_cast<const void*>(out));
        }
        s.disabled = true;
        return -1;
    }
    if (ngxTarget != out) {
        transition(ngxTarget, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(out, ngxTarget);
        transition(ngxTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else {
        transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    s.resetPending = false;

    ++s.submitCount;
    if (!s.evalOk) {
        s.evalOk = true;
        logInfo("DLSS Evaluate succeeded on the first frame: render %dx%d -> target %dx%d "
                "(mv=%s)",
                in->renderWidth, in->renderHeight, in->targetWidth, in->targetHeight,
                mvIsRg16 ? "RG16 straight through" : "copied into an RG16 intermediate");
    }
    return viaHandover ? 1 : 2;
}

    int32_t mcdr_dlss_request_quality(int32_t tier) {
    const int normalised = (tier < 0) ? -1 : (tier > 4 ? 4 : tier);
    if (normalised != s.requestedQuality) {
        s.requestedQuality = normalised;
        s.createdWarned = false;
        if (!s.qualityRequestLogged) {
            s.qualityRequestLogged = true;
            logInfo("DLSS quality tier request: %d (%s) — the feature will be rebuilt",
                    normalised,
                    normalised < 0 ? "unset -> closest to the render:target ratio"
                                   : (normalised == 1 ? "Performance"
                                                      : (normalised == 2 ? "Balanced"
                                                                         : (normalised == 3 ? "Quality"
                                                                                            : (normalised == 4 ? "DLAA"
                                                                                                               : "Ultra Performance")))));
        }
    }
    return normalised;
    }

    int32_t mcdr_dlss_request_preset(int32_t preset) {
        if (preset < 0 || preset > NVSDK_NGX_DLSS_Hint_Render_Preset_O) {
            logWarn("DLSS render preset %d is out of range (valid: 0=Default, 5=E, 6=F, "
                    "10=J, 11=K, 12=L, 13=M) — ignored",
                    preset);
            return -1;
        }
        if (preset != s.requestedPreset) {
            s.requestedPreset = preset;
            s.createdWarned = false;
            if (!s.presetRequestLogged) {
                s.presetRequestLogged = true;
                logInfo("DLSS render preset request: %d (%s) — the feature will be rebuilt",
                        preset, presetName(preset).c_str());
            }
        }
        return preset;
    }

int32_t mcdr_dlss_set_output_texture(void* resource) {
    auto* res = static_cast<ID3D12Resource*>(resource);
    if (res == s.externalOutput) {
        return res != nullptr ? 1 : 0;
    }
    if (mcdr_dlss_invalidate_inputs() != 1) return -1;
    s.externalOutput = res;
    if (res != nullptr) {
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        logInfo("DLSS output texture supplied by the JVM: %llux%u fmt=%d flags=0x%X — NGX "
                "will write here instead of this unit's private UAV texture",
                static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format),
                static_cast<unsigned>(d.Flags));
    } else {
        logInfo("DLSS output texture override cleared; back to the private UAV texture");
    }
    return res != nullptr ? 1 : 0;
}

    int32_t mcdr_dlss_set_enabled(int32_t enabled) {
    const bool want = enabled != 0;
    const bool changed = (want != s.enabled);
    s.enabled = want;
    if (changed) {
        logInfo("DLSS switch %s (independent of anti-aliasing; NGX state is kept: "
                "initOk=%d featureOk=%d)",
                want ? "ON" : "OFF", s.initOk ? 1 : 0, s.featureOk ? 1 : 0);
    }
    return want ? 1 : 0;
    }

int32_t mcdr_dlss_shutdown_checked(void) {
    if (gSrCandidateCount != 0) {
        logWarn("DLSS shutdown requires all prepared and retired SR instances to be released");
        return 0;
    }
    if (mcdr_dlss_invalidate_inputs() != 1) return 0;
    if ((s.mvBufferObject != nullptr || s.dlssOutputObject != nullptr ||
         s.depthSnapshotObject != nullptr) && gDestroyTexture == nullptr) {
        logWarn("DLSS texture destroy callback missing; texture ownership must be retained");
        return 0;
    }
    if (!destroyFeature()) return 0;
    if (s.params != nullptr) {
        const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_DestroyParameters(s.params);
        if (NVSDK_NGX_FAILED(r)) {
            logWarn("NVSDK_NGX_D3D12_DestroyParameters failed: %s", ngxResultString(r).c_str());
            return 0;
        }
        s.params = nullptr;
        s.paramsOk = false;
    }
    if (s.initOk) {
        const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Shutdown1(s.device);
        if (NVSDK_NGX_FAILED(r)) {
            logWarn("NVSDK_NGX_D3D12_Shutdown1 failed: %s", ngxResultString(r).c_str());
            return 0;
        }
        s.initOk = false;
    }
    if (gShutdownStreamline != nullptr) {
        if (gShutdownStreamline() != 1) return 0;
    } else if (dlssImplStreamline()) {
        logWarn("Streamline shutdown callback missing; device ownership must be retained");
        return 0;
    }
    destroyTrackedTexture(nullptr, &s.mvBufferObject);
    s.mvBuffer = nullptr;
    destroyTrackedTexture(nullptr, &s.dlssOutputObject);
    s.dlssOutput = nullptr;
    destroyTrackedTexture(nullptr, &s.depthSnapshotObject);
    s.depthSnapshot = nullptr;
    if (s.depthStencil != nullptr) {
        s.depthStencil->Release();
        s.depthStencil = nullptr;
    }
    s = State{};
    for (auto& slot : gSrViewportSlots) slot = false;
#if MCDR_DLSS_HAVE_STREAMLINE
    for (auto& constants : gSrAllocationConstants) constants.reset();
    streamlineEvalApi(true);
    slDlssSetOptionsFn(true);
    const int32_t reflexMode = gReflex.mode;
    const uint32_t reflexFrameLimit = gReflex.frameLimitUs;
    gReflex = ReflexApi{};
    gReflex.mode = reflexMode;
    gReflex.frameLimitUs = reflexFrameLimit;
    gStreamlineFgState = {};
    gSlDlssLoaded.store(-1);
    gStreamlineInputInvalidationFailed.store(false);
    gLastFrameToken = nullptr;
    gCameraConstantsValid = false;
    gWorldConstantsFrame = UINT64_MAX;
    gDisplayConstantsFrame = UINT64_MAX;
    gDisplayInputsAccepted = false;
#endif

    return 1;
}

void mcdr_dlss_shutdown(void) {
    (void)mcdr_dlss_shutdown_checked();
}

int32_t mcdr_sl_upgrade_interface(void** baseInterface) {
#if MCDR_DLSS_HAVE_STREAMLINE
    if (!dlssImplStreamline()) {
        return 0;
    }
    const int32_t fgModeForLog = gDlssgMode.load();
    const int32_t upgraded = streamlineUpgradeInterface(baseInterface);
    if (upgraded != 0) {
        logInfo("DXGI factory upgraded to the Streamline proxy factory "
                "(eUseDXGIFactoryProxy) - swapchain creation now goes through SL (fg=%d)",
                fgModeForLog);
    }
    return upgraded;
#else
    (void)baseInterface;
    return 0;
#endif
}

}

void mcdr_dlss_set_fg_mode(int32_t mode, int32_t numFrames) {
    gDlssgMode.store(mode);
    gDlssgFrames.store(numFrames < 1 ? 1 : numFrames);
    if (mode == 0) {
        gFgPresented.store(1);
    }
}
int32_t mcdr_dlss_fg_presented(void) {
    return gFgPresented.load();
}
int32_t mcdr_dlss_output_state(void) {
    return gDlssOutputState.load();
}
