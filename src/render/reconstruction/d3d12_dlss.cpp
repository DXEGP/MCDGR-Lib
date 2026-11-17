// d3d12_dlss.cpp — P108: DLSS super-resolution over NVSDK_NGX (data plane + the real
// Evaluate call).
//
// WHY THIS IS A PLAIN TRANSLATION UNIT (not a module implementation unit):
//   Same reason as d3d12_rtx_probe.cpp — it hides every vendor type (ID3D12Resource,
//   NVSDK_NGX_*) behind a tiny C interface, so the device module (mcdr.d3d12.device)
//   never includes an NGX header and never inherits the DLSS license surface. The
//   dependency runs one way only: the module declares these entry points itself and
//   calls them; nothing here imports a mcdr.* module.
//
// WHERE IT SITS IN THE FRAME:
//   The Java side calls mcdr_dx12EvaluateDlss at the SAME point where it used to call
//   mcdr_dx12ApplySmaaToTexture — after the world has rendered, before the GUI. DLSS
//   upscales the render-scaled world layer (window * dxrMcRenderScale) into the
//   window-sized target the GUI draws over, which is exactly what the linear blit did.
//   DLSS and SMAA are two values of the same anti-aliasing option, never both.
//
// FAILURE POLICY (mirrors P104b SMAA exactly):
//   Every failure path logs at most ONE line (WARN for "not available", ERROR for
//   "broke while running") and then pins the state, so nothing is retried per frame.
//   DLSS never makes the frame fail: the caller falls back to the plain blit path.
//
// DEPTH CONVENTION (see the P108 report for the full evidence):
//   Minecraft 26.x renders REVERSE-Z. Projection.java:70-74 swaps near/far before
//   building the matrix; DepthStencilState.DEFAULT is GREATER_THAN_OR_EQUAL (mapped by
//   d3d12_device.cpp:3676 to D3D12_COMPARISON_FUNC_GREATER_EQUAL); the depth buffer is
//   cleared to 0.0 == far (GameRenderer.java:443/578, d3d12_device.cpp:3492-3498).
//   DLSS therefore gets NVSDK_NGX_DLSS_Feature_Flags_DepthInverted.
//
// MOTION VECTORS: D3D12Velocity.kt writes screen-space motion in PIXELS
// ((uv - prevUv) * ScreenSize), so InMVScaleX/Y stay 1.0. They are computed from the
// JITTERED current projection, so the jitter is inside them: DLSS is told both that the
// MVs are jittered (MVJittered) and to cancel that jitter
// (motionVectorJitterCancellation), which is the self-consistent combination.
#include <windows.h>

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

// P112 阶段 1：Streamline 接管 DLSS 的实现选择（默认仍是直连 NGX，行为与 P108 一致）。
#if __has_include(<sl.h>)
#include <sl.h>
#include <sl_dlss.h>   // sl::DLSSMode / sl::DLSSOptions（sl.h 只含 core）
#include <sl_reflex.h>  // P113：sl::ReflexOptions / sl::ReflexMode
#include <sl_dlss_g.h>  // P213：sl::DLSSGOptions / sl::DLSSGMode（帧生成）
#define MCDR_DLSS_HAVE_STREAMLINE 1
#else
#define MCDR_DLSS_HAVE_STREAMLINE 0
#endif

#include "mcdr_abi.h"
// P108: the extern "C" interface shared with the device module. It defines the transition
// callback types (DlssTransitionFn / DlssQueryStateFn) this unit stores in its state.
#include "mcdr_dlss_impl.h"
#include "mcdr_hdr_state.h"  // P197：HDR 激活状态（colorBuffersHDR）
extern "C" int32_t mcdr_dlss_invalidate_inputs(void);
import std;

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

// ---------------------------------------------------------------------------
// Project constants (mirrors d3d12_rtx_probe.cpp so the SDK sees one application
// identity from both call sites).
//
// P108: the ProjectId is a **36-character GUID string**, not a short name. Rationale: the
// production integration that ships DLSS against this same 310.x runtime (ETS2/ATS DLAA
// mod, `src/dlaa.cpp`) uses exactly this shape — a fixed GUID with the comment "any stable
// GUID works for unregistered apps; ours, generated for this project". Length/charset is
// not documented as a hard requirement (UE-style integrations use 32 hex chars without
// dashes), so the invariant that matters is: **stable, non-empty, and GUID-shaped**.
// Generated once for DXRMC and pinned here; changing it invalidates NGX's per-app state.
// ---------------------------------------------------------------------------
#define MCDR_NGX_PROJECT_ID "7c1f4a52-9b3e-4d68-8f21-5a6c0e2d9b74"
#define MCDR_NGX_ENGINE_VERSION "1.0"
#define MCDR_NGX_APPDATA L".\\ngx"

// ---------------------------------------------------------------------------
// P108: NGX runtime discovery + diagnostics.
//
// The RTX probe used to do this itself (see d3d12_rtx_probe.cpp), but a plain TU cannot
// reach the device module's dbgLog*, and "the runtime could not be found" is exactly the
// failure that needs a per-attempt log line. So the loading now lives HERE, where the
// module's log pointers are installed, and the probe calls mcdr_dlss_load_runtime().
//
// What NGX actually needs (verified against external\dlss\include\nvsdk_ngx_loader.h and
// the shipped binaries):
//   * `nvngx_dlss.dll` — the feature runtime shipped in external\dlss\bin\x64. This is
//     the one file the loader hands to LoadLibraryW; a missing *dependency* is not
//     possible, because nvngx_dlss.dll imports only VERSION/ADVAPI32/USER32/KERNEL32
//     (checked with a PE import parse) and has no delay-load table at all.
//   * `nvngx.dll` / `_nvngx.dll` — the NGX CORE, which is NOT shipped by the SDK. NGX
//     finds it next to the *application executable* (GetModuleFileNameW(NULL)), or via
//     the driver's registry key. In a Minecraft dev run the executable is the JDK's
//     java.exe, so that lookup cannot succeed and NGX falls back to the registry; this is
//     why `nvngx_dlss.dll` being present is necessary but not sufficient.
//   * `applicationDataPath` (the 4th parameter of Init_with_ProjectID) is a directory for
//     NGX's own logs/cache, NOT a model search path. There is no `pathToNGXModel`
//     parameter in this SDK version.
// ---------------------------------------------------------------------------
namespace
{
    // -----------------------------------------------------------------------
    // Logging. The device module owns the log (level filtering + the line format that
    // mirrors the JVM's log4j pattern), so this unit never formats a prefix or a
    // timestamp of its own: the module hands over its own dbgLogInfo/dbgLogWarn/
    // dbgLogError pointers in mcdr_dlss_install_log (see the bottom of this file).
    // Resolving those names on the module's side is what makes the boundary legal —
    // this TU must never name a mcdr.* module entity itself.
    // -----------------------------------------------------------------------
    using LogFn = void (*)(const char* fmt, ...);
    LogFn gLogInfo = nullptr;
    LogFn gLogWarn = nullptr;
    LogFn gLogError = nullptr;

    // B4：模块提供的私有纹理创建/销毁（见 mcdr_dlss_impl.h 的 DlssTextureDesc）。本单元从此
    // **不再自己 CreateCommittedResource 任何纹理** —— 私有 UAV 与深度快照都从这里建，创建
    // 即登记进模块的跟踪表，销毁回模块的延迟删除路径。两条路留一条，别再走回去。
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

    // -----------------------------------------------------------------------
    // NGX result codes -> readable text.
    // -----------------------------------------------------------------------
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

    // "text (0xXXXXXXXX)" when the code is a known one, plain hex otherwise.
    std::string ngxResultString(NVSDK_NGX_Result r) {
        char hex[32] = {};
        std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(r));
        if (const char* t = ngxResultText(r)) {
            return std::string(t) + " (" + hex + ")";
        }
        return std::string(hex);
    }
    // -----------------------------------------------------------------------
    // State. One process-wide DLSS instance (the device is a singleton too).
    // -----------------------------------------------------------------------
    // P291f（用户 m03752）：每一档名义上的上采样倍数（target/render）。NGX 的动态分辨率区间是
    // [target/nominal, target]；render 比 target/nominal 还小时，NGXDLAA::EvaluateFeature 只写一行
    // 「Dynamic scaling error ... outside of Min ... Max」然后交出一张空纹理 —— 表现是世界全黑、
    // GUI 正常（Illusion\run\dxrmc\sl.log 里 2289 行都是这句：render 844x440 vs Min 1280x667）。
    static const double kTierNominalFactor[5] = {3.0, 2.0, 1.72, 1.5, 1.0};
    struct State
    {
        bool initAttempted = false;   // NGX init was tried (success or not)
        bool initOk = false;          // NVSDK_NGX_D3D12_Init succeeded
        bool paramsOk = false;        // GetCapabilityParameters succeeded
        bool optimalOk = false;       // the optimal-settings query produced dimensions
        bool disabled = false;        // a failure already logged: never retry
        bool featureOk = false;       // a live NVSDK_NGX_Handle exists
        bool createdThisFrame = false;
        bool createdWarned = false;   // a create failure was already reported
        bool evalOk = false;
        bool evalFailWarned = false;  // an evaluate failure was already reported
        bool resetPending = false;    // a reset request is folded into the next Evaluate
        // P108 (user m10920): DLSS is now an INDEPENDENT switch, not an anti-aliasing mode.
        // The authoritative gate lives in the device module (gCtx.dlssEnabled); this mirror
        // only exists so the status query and the log lines can report it.
        bool enabled = false;
        uint64_t submitCount = 0;     // successful Evaluate calls
        bool tooSmallWarned = false;
        ID3D12Device* device = nullptr;
        NVSDK_NGX_Parameter* params = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        uint32_t srViewport = 0;
        int32_t srViewportSlot = -1;
        bool srPrepared = false;
        std::optional<std::string> preparedFailure;
        ID3D12Resource* depthStencil = nullptr;  // DLSS scratch buffer, DEFAULT heap
        // P108: DLSS must write into a resource created with ALLOW_UNORDERED_ACCESS. The
        // JVM's GUI/present target is created with ALLOW_RENDER_TARGET only (that flag is set
        // from usage bit 8 in d3d12_device.cpp and there is no UAV bit at all), and NGX
        // rejects it with `FAIL_RWFlagMissing` (0xBAD00009). Adding a UAV bit to the present
        // target would drag the whole present/blit/barrier path in, so instead DLSS writes
        // into this private, target-sized UAV texture and the result is copied into the real
        // output on the same command list. Self-contained: no JVM change needed.
        ID3D12Resource* dlssOutput = nullptr;    // private UAV intermediate
        // B4：这张纹理现在由**设备模块**创建（见 mcdr_dlss_impl.h 的 DlssTextureDesc）：本单元
        // 只持有一个不透明句柄 + 裸资源指针，销毁必须交回模块（destroyTrackedTexture），
        // **绝不**自己 Release —— 否则就又是一条绕过模块、绕过跟踪表的资源路径。
        void* dlssOutputObject = nullptr;
        // P108 (user m11147 follow-up): the UAV texture the JVM created through the module's
        // `createTexture` path. Preferred over `dlssOutput` for the reasons recorded at the
        // `ngxTarget` selection in mcdr_dlss_evaluate: this unit's own creation path produces a
        // resource the GPU does not write to. Empty (`nullptr`) means "fall back to private".
        // NOT owned by this unit — the module keeps the Dx12Object alive.
        ID3D12Resource* externalOutput = nullptr;
        UINT dlssOutputWidth = 0;
        UINT dlssOutputHeight = 0;
        DXGI_FORMAT dlssOutputFormat = DXGI_FORMAT_UNKNOWN;
        // P108: tracked transitions are delegated to the device module (see transition()).
        DlssTransitionFn transitionFn = nullptr;
        void* transitionCtx = nullptr;
        // P108: the private D32 depth snapshot (see mcdr_dlss_snapshot_depth). When present it
        // replaces the JVM's `pInDepth`, which by the time DLSS runs has already been cleared.
        ID3D12Resource* depthSnapshot = nullptr;
        // B4：同上 —— 深度快照也由设备模块创建，这里只留句柄。
        void* depthSnapshotObject = nullptr;
        UINT depthSnapshotWidth = 0;
        UINT depthSnapshotHeight = 0;
        DXGI_FORMAT depthSnapshotFormat = DXGI_FORMAT_UNKNOWN;
        UINT depthSnapshotMips = 0;      // P108x: a full copy requires the whole shape to agree
        UINT depthSnapshotArray = 0;     // 同上：数组层数也要一起比对，否则全拷贝非法
        UINT depthSnapshotSamples = 0;
        UINT64 depthSnapshotFrame = 0;   // frame counter at the last snapshot
        bool depthSnapshotWarned = false;

        // P199 (user m0412x, "缓慢的内存泄露"): `slDLSSSetOptions` used to be called on EVERY
        // frame, because `mcdr_dlss_create` runs per frame and the streamline branch has nothing
        // to create. Streamline re-derives its internal DLSS resources on every call, and the
        // measured effect was process private bytes growing ~800 MB/min with DLSS on, completely
        // flat with DLSS off. Options only change when the tier / output size / exposure changes,
        // so cache the last pushed set here and skip the call when nothing moved. `slOptsApplied`
        // is cleared whenever the feature is destroyed so the next create re-pushes.
        bool slOptsApplied = false;
        int32_t slOptMode = -1;
        uint32_t slOptOutW = 0;
        uint32_t slOptOutH = 0;
        float slOptPreExposure = -1.0f;
        float slOptExposureScale = -1.0f;

        // P199b: the per-frame evaluate path pushes `slDLSSSetOptions` too (it builds its own
        // `sl::DLSSOptions` for the tags it just set), so it needs its own memory of what was
        // last applied. Keeping the two caches separate is deliberate: the create path and the
        // evaluate path can legitimately disagree in between (a resize lands in the create path
        // first), and sharing one cache made the options silently never re-apply.
        bool slEvalOptsValid = false;
        int32_t slEvalOptMode = -1;
        uint32_t slEvalOptOutW = 0;
        uint32_t slEvalOptOutH = 0;
        float slEvalOptPreExposure = -1.0f;
        float slEvalOptExposureScale = -1.0f;
        uint32_t slEvalOptQuality = 0xFFFFFFFFu;
        int32_t slEvalOptPreset = -1;
        bool slEvalOptHdr = false;

        ID3D12Resource* mvBuffer = nullptr;      // intermediate for a non-RG16 MV input
        // B4：MV 中间纹理也由设备模块创建（同一入口），这里只留句柄。
        void* mvBufferObject = nullptr;
        void* runtimeModule = nullptr;           // HMODULE of nvngx_dlss.dll once loaded
        UINT64 scratchBytes = 0;
        UINT32 mvCopyResourceState = D3D12_RESOURCE_STATE_COMMON;
        // The size tuple the current feature was created for. A change here forces a
        // destroy + recreate on the next Evaluate (and a history reset).
        int32_t renderWidth = 0;
        int32_t renderHeight = 0;
        int32_t targetWidth = 0;
        int32_t targetHeight = 0;
        uint32_t qualityValue = 0;
        // P108 (user m11147): the DLSS tier is now an explicit user choice (Performance /
        // Balanced / Quality) instead of only "closest to the render:target ratio".
        //   requestedQuality < 0  -> the JVM never set one: fall back to chooseQuality()
        //   requestedQuality >= 0 -> use it verbatim, and rebuild the feature when it changes
        // `effectiveQuality` is the ACCEPTED TIER (0..3, -1 = ratio-chosen) the live feature was
        // actually built with — after the DLAA 1:1 restriction turned tier 3 into Quality, it is
        // 2, not the requested 3. `qualityValue` is the NGX enum (DLAA == 5); never compare the
        // two directly (that mistake rebuilt the feature every frame).
        int requestedQuality = -1;
        int effectiveQuality = -1;
        // P108ac (user m08033): the DLSS *model* (NGX "render preset" hint). The JVM turns the
        // model button into one of NVSDK_NGX_DLSS_Hint_Render_Preset values (0 = Default,
        // 10 = J, 11 = K, 12 = L, 13 = M, ...) and it lands here.
        //   requestedPreset >= 0 -> set the hint for the tier being built; it is a CreateFeature
        //   input, so a change rebuilds the feature (same mechanism as the tier).
        // `effectivePreset` is the value the live feature was created with.
        int requestedPreset = 0;
        int effectivePreset = 0;
        bool presetRequestLogged = false;
        // P108aa (user m11796): true while a requested DLAA is being downgraded because
        // render != target. State-change gate for the one-shot ERROR (it used to print per frame).
        bool dlaaDowngraded = false;
        bool qualityIsExplicit = false;
        bool qualityRequestLogged = false;
        int32_t featureFlags = 0;
        // Strings for the status query and the one-time log lines.
        std::string failure;        // the stage that failed, for the status query
        std::string runtimePath;    // nvngx_dlss.dll as loaded (reported by the RTX probe)
        std::string optimalText;    // "target 1280x720, render 960x540 -> quality Balanced (...)"
        uint32_t optimalRenderW = 0;
        uint32_t optimalRenderH = 0;
        float optimalSharpness = 0.0f;
        // Per-frame input summary: log only when a value changes (no per-frame spam).
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
        // P108 blocker-B: the vendor directory the JVM announced (the same `<user.dir>
        // \dxrmc\` MCDR.dll is extracted to), whether the process DLL search path was
        // widened for it, and whether the three descriptive lines were printed yet.
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

    // -----------------------------------------------------------------------
    // Runtime discovery (see the file-header note). Everything here logs through the
    // module's own dbgLog*, so a failure reads like any other native line.
    // -----------------------------------------------------------------------
    // The directory THIS module (MCDR.dll) was loaded from.
    //
    // NOT GetModuleFileNameW(nullptr): passing NULL returns the **executable**, which in a
    // Minecraft dev run is the JDK's java.exe, so every exe-relative guess landed in
    // `C:\Program Files\GraalVM\JDKs\25.1\bin`. That was the actual cause of the whole
    // `ngx=no` / ERROR_MOD_NOT_FOUND investigation: the vendor DLLs sit next to MCDR.dll in
    // `<user.dir>\dxrmc\`, and no candidate ever looked there.
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

    // The directory the *executable* lives in (java.exe's bin during a dev run). Kept
    // separate from moduleDirectoryW so the diagnostics can show both.
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

    // Adds `dir` to the process DLL search path. SetDllDirectoryW alone would be enough
    // (it inserts one directory into the standard search order), but AddDllDirectory needs
    // the default-dirs mode to take effect, so both are issued and each result is logged.
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

    // One line that answers "where was it looking?" for the next failure.
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

    // Tries every plausible location, in order, logging each attempt with its own
    // GetLastError. Returns the module handle, or NULL after logging the whole chain.
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
        // 1) bare name: the standard search order, with a widened path this is where the
        //    vendor directory is found.
        candidates.push_back({s.searchPathSet ? L"search-path(name)" : L"bare-name", name, false, 0});
        // 2) the directory the JVM announced (== where MCDR.dll itself was extracted).
        if (!s.vendorDir.empty()) {
            candidates.push_back({L"vendor-dir", s.vendorDir + L"\\" + name, false, 0});
        }
        // 3) MCDR.dll's own directory. This is the candidate that actually resolves the
        //    shipped layout: JVM extractLibrary() writes MCDR.dll AND the vendor DLLs into
        //    `<user.dir>\dxrmc\`, so the runtime always sits next to this module.
        if (!modDir.empty()) {
            candidates.push_back({L"module-dir", modDir + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        // 4) `<cwd>\dxrmc\` — the extracted vendor directory under the game directory, for
        //    the case where the module was loaded from somewhere else.
        if (!cwd.empty()) {
            candidates.push_back({L"cwd-dxrmc", cwd + L"\\dxrmc\\" + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        // 5) `<exeDir>\dxrmc\` — same idea for a non-dev deployment.
        if (!exeDir.empty()) {
            candidates.push_back({L"exeDir-dxrmc", exeDir + L"dxrmc\\" + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        // 6) cwd, likewise explicit.
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

    // -----------------------------------------------------------------------
    // P108 blocker-B ROOT CAUSE: NVSDK_NGX_FeatureCommonInfo.PathListInfo.
    //
    // `nvsdk_ngx_defs.h:400-409` documents PathListInfo as "List of all paths in descending
    // order of search sequence to locate a feature dll in, other than the default path -
    // application folder"; `:519-526` repeats it for FeatureDiscoveryInfo. NGX's default is
    // the APPLICATION folder, which in a Minecraft dev run is the JDK's `bin\`, never our
    // vendor directory. With an empty path list NGX cannot find nvngx_dlss.dll and answers
    // `FAIL_InvalidParameter` (0xBAD00005) to BOTH Init_with_ProjectID and
    // GetFeatureRequirements.
    //
    // Measured outside the game (PowerShell P/Invoke, real IDXGIAdapter + D3D12 device):
    //   GetFeatureRequirements(adapter, FeatureInfo = vendor path list)
    //       -> 0x00000001 Success, FeatureSupported = 0x0 (supported), MinOSVersion "10.0.0"
    //   In-game probe with FeatureInfo = NULL reported "query failed: invalid parameter
    //       (0xFFFFFFFFBAD00005)", and Init_with_ProjectID likewise returned 0xBAD00005.
    // (Init itself is NOT reachable from PowerShell: nvngx_dlss.dll exports only
    // NVSDK_NGX_CUDA_* and NGX_SNIPPETS_*, so the D3D12 entry points must be exercised
    // in-game.)
    //
    // The buffers are owned here and live for the process lifetime because NGX keeps the
    // pointers (PathListInfo.Path is `wchar_t const* const*`).
    // -----------------------------------------------------------------------
    NVSDK_NGX_FeatureCommonInfo* gCommonInfo = nullptr;
    std::wstring* gPathListEntry = nullptr;    // the one wide string the list points at
    const wchar_t** gPathListArray = nullptr;  // the one-element array of path pointers

    // (Re)builds the common info so PathListInfo holds `dir` (empty -> Length 0).
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

    // The pointer handed to NGX and to the RTX probe's requirement query. NULL only while the
    // path list is still empty (then the probe keeps its old "inconclusive" behaviour).
    NVSDK_NGX_FeatureCommonInfo* commonInfoForNgx() {
        if (gCommonInfo == nullptr || gPathListEntry == nullptr ||
            gCommonInfo->PathListInfo.Length == 0u) {
            return nullptr;
        }
        return gCommonInfo;
    }

    // Builds the DLSS feature flags from the frame description.
    //
    // Re-derived from a WORKING integration shipping against the same 310.x runtime (ETS2/ATS
    // DLAA/DLSS injection mod, `src/dlaa.cpp`). Two of the previous choices were wrong, and both are
    // consistent with "NGX writes nothing / the world comes out black":
    //
    //   AutoExposure  **always on**. Reference: "AutoExposure spares us an exposure texture; it is
    //                 ignored for LDR input anyway." We have no GPU exposure texture and used to pass
    //                 a constant 1.0 with this flag absent — so NGX had neither a texture nor the
    //                 flag that lets it synthesise one.
    //   DepthInverted on — reverse-Z (see the file header).
    //   MVLowRes      **on when render != target**. The bit means "the motion vectors are at the
    //                 (low) render resolution", which is exactly our case: the velocity pass runs at
    //                 render res and writes render-pixel offsets. The old code left it off and even
    //                 documented the opposite, so NGX rescaled them as if they were display-sized.
    //   MVJittered    **on**. D3D12Velocity.kt builds `currViewProj` from the JITTERED projection and
    //                 then adds `PrevJitterPixels`, so both ends of every motion vector sit on the
    //                 jittered grid — the jitter delta is inside them. Declaring that (MVJittered) is
    //                 what lets NGX cancel it with `InJitterOffsetX/Y`; leaving it off made NGX treat
    //                 those deltas as real motion. The old note ("jitter is frozen to 0 anyway") no
    //                 longer holds: P110 removed the freeze, TAA and DLSS now share one Halton(2,3)
    //                 sequence, and the flag is set unconditionally — with jitter 0 it is a no-op.
    //   IsHDR         off — 8-bit UNORM target.
    //
    // Cancelling that jitter needs no extra NGX knob: `MVJittered` plus the `InJitterOffsetX/Y`
    // already passed to `EvaluateFeature` is the whole contract. (This SDK's header has no
    // "motion vector jitter cancellation" parameter key at all — an earlier note here claimed one.)
    // P108ac (user m08033): the DLSS *model* is selected through NGX's "render preset" hint, one
    // parameter key per quality tier. Only the key matching the tier the feature is built with is
    // consulted, so set exactly that one — writing all six would be noise, and a stale value on an
    // unused key could silently apply later when the tier changes without us.
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
            // Ultra Quality exists in this SDK's enum but has no hint key of its own, so it falls
            // through to `default` (no preset written = NGX's own choice for that tier).
            case NVSDK_NGX_PerfQuality_Value_UltraQuality:
            default:
                return nullptr;
        }
    }

    // Human-readable form of a preset value for the log lines ("Default" / "J" / "K" / "L" / "M",
    // or the raw number for the letters NVIDIA marks as reserved).
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
        // P108x (A/B experiment, parent request): AutoExposure is OFF unless asked for via
        // MCDR_DLSS_AUTOEXPOSURE=1. We declare "auto exposure" while passing no `pInExposureTexture`
        // at all, and the device is removed ~1 ms after the first evaluate that has a REAL depth
        // buffer — the earlier build, whose depth snapshot was empty, survived the same call. The
        // reference integration ships this combination, but it is D3D11 on another DLSS build;
        // 310.9.1 may not tolerate it. Default OFF so one run answers "is AutoExposure the trigger?".
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
        // P110: the motion vectors already contain the sub-pixel jitter (see the note in the feature
        // flag comment block above), so tell NGX to cancel it with InJitterOffsetX/Y.
        // P134：MV 改成真实运动后不再置 MVJittered（否则 NGX 会去减一个不存在的 jitter 差）。
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
    // -----------------------------------------------------------------------
    // Resource helpers (this unit owns no device state, so it creates its own scratch
    // resources directly).
    // -----------------------------------------------------------------------
    // P108: resources move through the DEVICE MODULE's tracked transition, never through a
    // barrier written here.
    //
    // History (the bug this replaces): this helper used to take an explicit `before` state and
    // emit the barrier itself. That assumed the previous pass left the resource in
    // RENDER_TARGET/DEPTH_WRITE — but `endRenderPass` (P11) explicitly returns every attachment
    // to COMMON at the end of each pass, so the assumed `before` was wrong. In release that is
    // silently tolerated; the D3D12 debug layer reports "Before state ... does not match", and
    // P11's own notes record that this class of mismatch leads to GPU state corruption and a TDR
    // freeze. The module keeps a per-resource state map (`ctx->resourceState`) and its
    // `transitionTextureTo` is idempotent, so asking it is both correct and cheaper.
    //
    // `s.transitionFn`/`s.transitionCtx` are supplied by the module per call; when absent (a
    // diagnostic path that never opened a command list) transitions are simply skipped.
    void transition(ID3D12Resource* res, D3D12_RESOURCE_STATES after) {
        if (res == nullptr) {
            return;
        }
        if (s.transitionFn == nullptr) {
            return;
        }
        s.transitionFn(s.transitionCtx, reinterpret_cast<void*>(res), static_cast<int32_t>(after));
    }

    // B4：本单元**唯一**的纹理创建入口（模块注入的 create/destroy，见 mcdr_dlss_impl.h）。
    // 成功时 *outObject / *outResource 都填好；失败返回 false，调用方按"没有私有纹理"处理。
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

    // 销毁（幂等，指针一起清空）。`ctx` 只用于从该 ctx 的跟踪表里摘键，可为 nullptr（shutdown）。
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
            return true;  // big enough already
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

    // P108: the private UAV texture DLSS writes into (see the field comment on `dlssOutput`).
    // Recreated only when the target size or format changes. Returns nullptr on failure (the
    // caller then reports the reason and leaves the existing path in place).
    //
    // B4: the texture is created **by the device module** now (DlssTextureDesc, see
    // mcdr_dlss_impl.h). Consequences this function must honour:
    //   * the module owns the resource and seeds its tracked state map with COMMON, so the very
    //     first transition in this list has a real anchor instead of a COMMON guess;
    //   * it is released through the module's deferred-delete path — never `Release()` here;
    //   * this unit keeps two values only: the opaque handle (`dlssOutputObject`) and the raw
    //     pointer NGX is handed (`dlssOutput`).
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
        // P108x: UAV **only**.
        //
        // This used to be `ALLOW_UNORDERED_ACCESS | ALLOW_RENDER_TARGET`. A write-side bisect then
        // showed the texture could not hold a write at all: filling it from the colour input with
        // the COPY ENGINE — same texture, same queue, no shaders, no NGX — read back all-zero both
        // immediately and on the following frame, while the JVM target beside it read back fine
        // (mean 17.6/16.2/11.3, alpha 255). Copying into a resource needs no flag, and NGX is the
        // only consumer here that needs one (`FAIL_RWFlagMissing` = "UAV missing"), so the flag set
        // itself was the problem: `UAV | RTV` is the classic case where a driver declines the
        // unordered-access path while still reporting success.
        // P108x: UAV **only** (see the note above); ALLOW_RENDER_TARGET removed.
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

    // -----------------------------------------------------------------------
    // P108: NGX's own log lines are routed into the native log, so a failing Init explains
    // itself instead of only returning a code. Signature per nvsdk_ngx_defs.h:376-379.
    void NVSDK_CONV dlssNgxLogCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel,
                                       NVSDK_NGX_Feature sourceComponent) {
        (void)loggingLevel;
        if (message != nullptr) {
            logInfo("NGX[%d] %s", static_cast<int>(sourceComponent), message);
        }
    }

    // NGX init. Runs at most once (success or failure); the caller already checked that
    // a device exists.
    // -----------------------------------------------------------------------
    // P108: the NGX application-data directory.
    //
    // NGX writes its own logs/cache there and rejects a path that does not exist with
    // `FAIL_InvalidParameter` — but the reference integration that ships DLSS against this
    // same 310.x runtime (ETS2/ATS DLAA mod, `src/dlaa.cpp`) simply passes **the directory
    // this DLL lives in**, which always exists and never needs creating. Measured order:
    //   1. this module's own directory (MCDR.dll sits in `<user.dir>\dxrmc\` next to the
    //      vendor DLLs, so this is the natural home);
    //   2. `.\ngx`, created on demand (the previous behaviour);
    //   3. `"."` as a last resort.
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
    std::wstring dlssModuleDirectory();  // 定义在下面，auto 探测要先用到
#endif

    // P121：**默认走 Streamline**。
    //   1) `MCDR_DLSS_IMPL` 显式指定优先（`ngx` 强制回退、`streamline` 强制启用）；
    //   2) 否则 `MCDR_STREAMLINE=1` 也算启用（历史开关）；
    //   3) 否则 auto：`sl.interposer.dll` 与 MCDR.dll 同目录就用 Streamline，不在就回退 NGX
    //      —— 发行版按 NVIDIA 许可不带运行库（AGENTS.md「不可再分发」），这条保证那时退回 NGX。
    bool dlssImplStreamline() {
        // P126（用户 m14488「现在已经改到了默认 streamline」之后的实测）：以前整段被
        // `static const bool` 缓存成**一次判定**。而 `sl.interposer.dll` 是 JVM 侧启动时
        // 才解压到 `dxrmc/` 的，native 的第一次调用可能早于解压完成 —— 于是判定成 false 并
        // 永久缓存，后面即使 interposer 已经在目录里也一直走 NGX（NGX 在本机建不出 feature，
        // `0xBAD0000C out of date`，于是每帧静默 return 0，表现就是「DLSS 没出图、只剩抖动」）。
        // 现在：环境变量每次读（显式指定就该立即生效），文件探测只在**还没看到**时重试，
        // 见到之后才锁存 —— 不会再把一次过早的否定结果固定下来。
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

    // MCDR.dll 所在目录（带结尾反斜杠）。
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

    // 运行时取 interposer 的 slDLSSSetOptions（不引入静态导入，缺 SL 时返回 nullptr）。
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

    // P249：把原生 DXGI 工厂原地升级成 SL 的代理工厂（`eUseDXGIFactoryProxy` 模式下建
    // swapchain 的唯一正确入口，语义见 mcdr_dlss_impl.h 的声明处与 sl_core_api.h:265-275）。
    // 未走 streamline / interposer 不在 / 还没 slInit 时返回 0，调用方继续用原生工厂。
    int32_t streamlineUpgradeInterface(void** baseInterface) {
        if (baseInterface == nullptr || *baseInterface == nullptr) return 0;
        // PFun_slUpgradeInterface 在 SDK 里是**函数类型**别名（sl_core_api.h:66），要用指针。
        using PFn_Upgrade = sl::Result (*)(void**);
        static PFn_Upgrade const fn = []() -> PFn_Upgrade {
            const std::wstring dir = dlssModuleDirectory();
            if (dir.empty()) return nullptr;
            HMODULE sl = ::LoadLibraryW((dir + L"sl.interposer.dll").c_str());
            if (sl == nullptr) return nullptr;
            return reinterpret_cast<PFn_Upgrade>(::GetProcAddress(sl, "slUpgradeInterface"));
        }();
        if (fn == nullptr) return 0;
        // 未 slInit 时 SL 只会回 eErrorInitNotCalled，不必自己判断初始化状态。
        void* const native = *baseInterface;
        void* upgraded = native;
        if (fn(&upgraded) != sl::Result::eOk || upgraded == nullptr) {
            *baseInterface = native;  // 失败时把原生指针原样放回去
            return 0;
        }
        // 文档是 replace in place：成功时调用方手里那一个引用直接变成代理的引用
        // （所以这里既不 Release 也不 AddRef，所有权由调用方转移）。
        *baseInterface = upgraded;
        return upgraded != native ? 1 : 0;
    }

    // P291n（用户 m04823）：Streamline 路径的档位解析（定义在 chooseQuality 之后）。
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

    // 把本帧几何 + 质量档写进 SL 的 DLSS 选项；失败时告警。
    int32_t streamlineApplyOptions(const mcdr_dlss_input& in) {
        if (s.srPrepared) return 0;
        const PFn_SlDLSSSetOptions fn = slDlssSetOptionsFn();
        if (fn == nullptr) return -1;
        sl::DLSSOptions opt{};
        switch (s.qualityValue) {
            case NVSDK_NGX_PerfQuality_Value_DLAA:       opt.mode = sl::DLSSMode::eDLAA; break;
            case NVSDK_NGX_PerfQuality_Value_MaxQuality: opt.mode = sl::DLSSMode::eMaxQuality; break;
            case NVSDK_NGX_PerfQuality_Value_Balanced:   opt.mode = sl::DLSSMode::eBalanced; break;
            // P291（用户 m03752）：极致性能 = 3x。它原先落进 default 被当成 MaxPerformance（2x），
            // Streamline 于是按 2x 规划特征，我们却喂 3x 的输入 —— 评价结果全黑（只有 GUI 正常），
            // 而 50% / 58% / 67% 恰好都与 default 的 eMaxPerformance 或已有分支吻合，所以只有这一档坏。
            case NVSDK_NGX_PerfQuality_Value_UltraPerformance: opt.mode = sl::DLSSMode::eUltraPerformance; break;
            default:                                     opt.mode = sl::DLSSMode::eMaxPerformance; break;
        }
        opt.outputWidth = static_cast<uint32_t>(in.targetWidth);
        opt.outputHeight = static_cast<uint32_t>(in.targetHeight);
        opt.preExposure = in.preExposure > 0.0f ? in.preExposure : 1.0f;
        opt.exposureScale = in.exposureScale > 0.0f ? in.exposureScale : 1.0f;
        opt.useAutoExposure = sl::Boolean::eFalse;
        // P199: this used to run on every frame (the streamline branch of `mcdr_dlss_create`
        // has nothing else to do). Streamline re-derives its internal DLSS state per call and
        // the measured cost was ~800 MB/min of process private bytes. Push the options only
        // when something that actually feeds them changed.
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
            // P112：SL 拥有 NGX。这里绝不能调用 NVSDK_NGX_D3D12_Init_*（同进程双 NGX 冲突，
            // P111 实测 sl.log 里 getNGXFeatureRequirements 报 0xbad00005）。
            logInfo("DLSS impl=streamline — native NGX init skipped "
                    "(Streamline owns the runtime; evaluate lands in phase 2)");
            s.failure = "streamline mode: native NGX init skipped by design";
            return false;
        }

        // P108：**固定用 SDK 常量 `NVSDK_NGX_Version_API`(0x15)，不再做版本扫描**。
        // 依据：与我们的 DLL 同代（310.x）的生产实现（ETS2/ATS DLAA mod，src/dlaa.cpp，
        // 作者注明 "verified against the public NVIDIA DLSS SDK headers + DLSS Programming
        // Guide"）在同一个 310.x 运行库上用 **0x15 成功 Init**。所以 0x15 是对的，之前
        // 0x16+ 报 out of date 正是"声明比核心新"；而 0x15 下的 FAIL_InvalidParameter
        // (0xBAD00005) 是**参数**问题，靠下面的能力诊断把原因具名化，而不是靠猜版本。
        const std::wstring appDataPath = buildAppDataPath();
        logInfo("NGX Init (sdkVersion=NVSDK_NGX_Version_API=0x%X, projectId=\"%s\", "
                "engineType=%d, engineVersion=\"%s\", appDataPath=\"%s\", featureInfo=%s)",
                static_cast<unsigned>(NVSDK_NGX_Version_API), MCDR_NGX_PROJECT_ID,
                static_cast<int>(NVSDK_NGX_ENGINE_TYPE_CUSTOM), MCDR_NGX_ENGINE_VERSION,
                narrow(appDataPath).c_str(), commonInfoForNgx() != nullptr ? "pathlist" : "null");
        // 参数表逐字对齐 nvsdk_ngx.h:247；**必须写在一行**（跨行时 MSVC 报 C2065）。
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

        // P108：**能力诊断**（照 ETS2/ATS 那份实现的做法）。这四个参数是把
        // `0xBAD00005` 变成"有名有姓的原因"的关键 —— `FeatureInitResult` 常直接带出
        // 真正的失败码，`NeedsUpdatedDriver` / `MinDriverVersion*` 说明是不是驱动不匹配。
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
            // 这一条单独再报一次：它是"能不能用"的总答案，读到它就不用猜了。
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
        // One-time optimal-settings probe, so the log carries the SDK's own ratio table
        // even before a world is loaded. The query for the REAL target size happens in
        // chooseQuality().
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
    // Maps a render/target ratio onto the closest NVSDK_NGX_PerfQuality_Value and reports
    // what the SDK considers optimal for that quality.
    //
    // WHY THIS EXISTS: the render scale is chosen independently of DLSS (dxrMcRenderScale,
    // 25% steps), so "which DLSS quality" is not a user setting — it is whatever quality
    // reproduces the resolution the user already picked. Choosing the closest one also
    // keeps DLSS from rescaling the input a second time.
    // P108 (user m11147): `forceQuality` >= 0 means the user picked the tier explicitly, so the
    // ratio-based choice is skipped and this call only fetches what the SDK considers optimal
    // for that tier (in particular the sharpness) — and logs the comparison table as usual.
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
        // `forceQuality` is one of kValues[]; clamp so indexing is always in range.
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
                // Self-documenting: this is the tier the user asked for, not the nearest one.
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
            // P108 (user m11147): say plainly that this tier came from the user, not from the
            // ratio — the log must never leave the two indistinguishable.
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

    // P291n（用户 m04823「dlss选项下面写着 dlss not initialized yet」的连带发现）：
    // Streamline 模式下 NGX 由 SL 拥有，`s.initOk` 恒为 false，于是 `mcdr_dlss_create` 在
    // 走到 `ensureFeature` 之前就返回了 —— 档位解析（含 P291f 的两道守卫）从来没执行过，
    // `s.qualityValue` 一直是它的初值 0。而 NGX 的 0 就是 MaxPerf（2x）：UI 说"极致性能"、
    // 交给 SL 的是 2x、我们喂 3x 的输入 → NGX 每帧 `Dynamic scaling error` + 空纹理 = 黑屏。
    // 这里把档位解析抽成两条路径共用的函数；返回 NGX 的 PerfQualityValue。
    uint32_t resolveQualityValue(const mcdr_dlss_input& in) {
        static const uint32_t kTierToNgxShared[5] = {
            static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_UltraPerformance),
            static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxPerf),
            static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_Balanced),
            static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality),
            static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA),
        };
        int resolvedTier = s.requestedQuality;  // -1 = 未指定：按比例挑
        if (resolvedTier == 4 &&
            (in.renderWidth != in.targetWidth || in.renderHeight != in.targetHeight)) {
            resolvedTier = 2;  // DLAA 只在 1:1 有意义
        }
        if (resolvedTier < 0 && in.renderWidth > 0 && in.targetWidth > 0) {
            // 没有显式档位时按实际比例挑最接近的一档（Streamline 路径拿不到 NGX 参数表，
            // 所以用名义倍率表，而不是 NGX_DLSS_GET_OPTIMAL_SETTINGS）。
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
            // P291f：显式档位与实际比例差超过 10% 时，以实际比例为准改判到能覆盖它的最紧一档。
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
            // P291f 尺寸守卫：render 小于该档名义输入时 NGX 只报一行 dynamic scaling error 并交出空纹理。
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
        // P199: the feature is gone, so the next create must re-push the options.
        s.slOptsApplied = false;
        // P199b: same for the per-frame evaluate path's own option cache.
        s.slEvalOptsValid = false;
        return true;
    }

    // Creates (or recreates) the feature. Requires an OPEN command list — NGX records its
    // own commands during CreateFeature.
    bool ensureFeature(ID3D12GraphicsCommandList* cmd, const mcdr_dlss_input& in) {
        if (s.createdWarned) {
            return false;  // a previous create failed: one warning, never retried
        }
        const bool sizeChanged =
            (in.renderWidth != s.renderWidth) || (in.renderHeight != s.renderHeight) ||
            (in.targetWidth != s.targetWidth) || (in.targetHeight != s.targetHeight);
        // P108 (user m11147): a tier change must rebuild too — PerfQualityValue is a
        // CreateFeature input, so it cannot be switched by editing Evaluate parameters alone.
        //
        // P108aa (user m11796, DLAA): resolve the tier THIS frame would build — including the
        // DLAA 1:1 restriction — and compare it against the tier the live feature was actually
        // built with. The old comparison was `requestedQuality != effectiveQuality`, where
        // `effectiveQuality` held the NGX enum, so it was wrong twice:
        //   * after a DLAA downgrade (requested 3 vs effective 2) it stayed true forever and
        //     re-created the feature every frame (480 creates / 24 s, ~30-40 ms each);
        //   * even a successful DLAA build stores NVSDK_NGX_PerfQuality_Value_DLAA == 5, so
        //     3 != 5 would have caused the same per-frame rebuild.
        // `effectiveQuality` now stores the ACCEPTED TIER (0..3, -1 = ratio-chosen), so the
        // downgrade becomes this round's baseline and is not re-detected every frame.
        int resolvedTier = s.requestedQuality;  // -1 = no explicit tier: ratio-chosen
        // Tier 4 = DLAA（2026-10-24：插入 Ultra Performance 之后 DLAA 从 3 挪到 4）。
        const bool dlaaMismatch =
            (resolvedTier == 4) && (in.renderWidth != in.targetWidth ||
                                    in.renderHeight != in.targetHeight);
        if (dlaaMismatch) {
            resolvedTier = 2;
        }
        if (dlaaMismatch != s.dlaaDowngraded) {
            s.dlaaDowngraded = dlaaMismatch;
            if (dlaaMismatch) {
                // Never silently build a "fake DLAA": DLAA is only meaningful at 1:1, and asking
                // for it while the render scale is, say, 80% would make NGX upscale under a DLAA
                // label. State-change only — a per-frame ERROR is how the 480-creates bug was
                // diagnosed, so it must not survive as log spam.
                logError("DLAA (tier 3) requested but render %dx%d != target %dx%d — falling "
                         "back to the Quality tier for this round (accepted as the baseline). Set "
                         "the render scale to 100%% to use DLAA.",
                         in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight);
            } else if (s.requestedQuality == 3) {
                logInfo("DLAA (tier 3) accepted: render %dx%d == target %dx%d",
                        in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight);
            }
        }
        // P291f：显式挡位与实际比例差得太远时（超过 10%），以实际比例为准重选一档 ——
        // 实测用户那次 NGX 是按 MaxPerf（min 1280x667）评价的，而 UI 说的是极致性能（3.0x），
        // 844x440 两边都不满足，于是每帧只拿到一张空纹理。这里把"挡位说不出的比例"改判到
        // 能覆盖它的最紧一档；10% 的门槛保证只在挡位明显错配时动手（33% vs 3.0x 只差 1.1%，
        // 交给下面的尺寸守卫）。
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
        // P291f（用户 m03752）：render 比该档名义输入还小时，NGX 只报一行「Dynamic scaling error ...
        // outside of Min ... Max」然后交出一张空纹理 —— 世界全黑、GUI 正常。JVM 已经把极致性能档
        // 从 33% 改成 34%，这里留一条守卫：同类配置错误必须在日志里一眼可见，而不是又变成一块黑屏。
        if (in.renderWidth > 0 && in.targetWidth > 0 && resolvedTier >= 0 && resolvedTier <= 4) {
            const double nominal = kTierNominalFactor[resolvedTier];
            const uint32_t needW = static_cast<uint32_t>(std::ceil(static_cast<double>(in.targetWidth) / nominal));
            const uint32_t needH = static_cast<uint32_t>(std::ceil(static_cast<double>(in.targetHeight) / nominal));
            // 0.5% 容差：实测 33%（844 vs 需要 854，差 1.1%）会被 NGX 拒绝并给出黑屏，
            // 而 58%（1484 vs 需要 1489，差 0.34%）一直正常 —— 阈值落在两者之间。
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
        // P108ac (user m08033): the render preset (the DLSS "model") is a CreateFeature input as
        // well — NGX bakes the network choice into the feature — so a model change has to rebuild
        // the feature exactly like a tier change. `effectivePreset` is what the live feature got.
        const bool presetChanged = (s.requestedPreset != s.effectivePreset);
        const bool flagsChanged = s.featureFlags != computeFeatureFlags(in);
        if (s.featureOk && !sizeChanged && !qualityChanged && !presetChanged && !flagsChanged) {
            return true;
        }
        if (sizeChanged || qualityChanged || presetChanged || flagsChanged) {
            if (!destroyFeature()) return false;
            // A new size invalidates the history; a pure tier change does not have to, but the
            // rebuilt feature starts with no history anyway, so reset in both cases.
            s.resetPending = true;
        }
        if (s.params == nullptr) {
            return false;
        }

        if (resolvedTier >= 0) {
            // Explicit tier from the JVM: Performance / Balanced / Quality, same order as the
            // Java enum. The mapping is deliberate (0->MaxPerf, 1->Balanced, 2->MaxQuality).
            // P108 (user m11796): tier 3 = DLAA. DLAA reuses this whole pipeline unchanged
            // (depth snapshot, motion vectors, output handover, copy-back); only the
            // PerfQualityValue and the size relationship differ: DLAA means "no upscaling", i.e.
            // render size == target size.
            //
            // P110b: DLAA now gets the real sub-pixel jitter — the projection UBO carries the same
            // Halton sequence TAA uses, InJitterOffsetX/Y is that offset in render pixels, and the
            // MVJittered feature flag tells NGX the motion vectors are already in jittered space.
            // The old note here said the jitter was frozen to 0 because injection "collapsed the
            // frame"; that diagnosis was built on byte-swapped matrix data (see known_bugs.md D.25)
            // and no longer applies.
            // P112 (2026-10-24, user m07529): five tiers — Ultra Performance is now the first
            // entry of `QUALITY_NAMES` on the Java side, so the table order must match that list,
            // not NGX's enum order. 4 is still DLAA (reached through the AA mode, not the list).
            
static const uint32_t kTierToNgx[5] = {
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_UltraPerformance),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxPerf),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_Balanced),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA),
            };
            // `resolvedTier` is already 0..4 and has the DLAA 1:1 restriction applied above;
            // a rejected DLAA was turned into Quality *before* the rebuild decision, so the
            // accepted tier is the one remembered below.
            const int tier = resolvedTier;
            if (tier < 0 || tier > 4) {
                return false;
            }
            s.qualityValue = kTierToNgx[tier];
            s.qualityIsExplicit = true;
            // The optimal-settings query also yields the sharpness the SDK recommends for this
            // quality/size pair; keep using it (it is what the reference integration does).
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
        s.effectiveQuality = resolvedTier;  // accepted tier (0..3), -1 = ratio-chosen
        s.featureFlags = computeFeatureFlags(in);

        // P108: EVERY input the size can depend on must be on the parameter map *before* the
        // query. Width/Height/OutWidth/OutHeight alone are not enough — with them set the call
        // returned Success but a scratch size of 0, which the old code misread as a failure
        // (`P108: DLSS unavailable — GetScratchBufferSize failed: success (0x00000001)`).
        // The create flags and the perf-quality value are part of the same descriptor, so set
        // them here too (they are also what `NGX_D3D12_CREATE_DLSS_EXT` will pass below).
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
        // P108ac (user m08033): the DLSS model. 0 = let NGX pick per tier (the long-standing
        // behaviour, and what an old JVM that never calls the setter gets); otherwise a specific
        // NVSDK_NGX_DLSS_Hint_Render_Preset value. This has to happen on the same parameter map as
        // PerfQualityValue, before NGX_D3D12_CREATE_DLSS_EXT — the preset is baked into the
        // feature at creation, which is why `presetChanged` forces a rebuild above.
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
        // A reported size of 0 is NOT an error: modern DLSS builds answer 0 and place their
        // scratch data in internal allocations. Allocate a conservative floor anyway so a build
        // that does use the buffer cannot read past it, and say so in the log.
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
        // P108 (user m11796): DLAA is defined as "no upscaling", so the feature is created at the
        // TARGET resolution. The requested-tier path already refuses DLAA when render != target
        // (ERROR + fall back to Quality), so this is a belt-and-braces pin of the equal-size
        // invariant: whatever the caller passed, a DLAA feature never upscales.
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
            cmd, 1u /*creation node mask*/, 1u /*visibility node mask*/, &handle, s.params, &cp);
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
        // P108: print EVERY flag bit we pass, not just three of them. The joint debugging of the
        // black-world report needed exactly this: `AutoExposure` and `MVLowRes` were invisible in
        // the old line (they were both 0), so the two wrong bits could not be seen from the log.
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
        // P108ac (user m08033): which model this feature was built with, and on which NGX hint
        // key. The raw number is printed too: the letters NVIDIA reserves (N/O) map to numbers
        // that have no documented name, and "did the value reach NGX at all" is the question this
        // line answers when the picture does not change.
        {
            const char* key = presetHintKey(s.qualityValue);
            logInfo("DLSS render preset = %d (%s) on %s", s.effectivePreset,
                    presetName(s.effectivePreset).c_str(), key != nullptr ? key : "(unmapped tier)");
        }
        return true;
    }

    // Logs the input package ONLY when something changed (a per-frame line would drown
    // the log during world load).
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
}  // namespace

// P118：给设备/交换链模块用的决策出口（外部链接；实现走上面的 dlssImplStreamline）。
int32_t mcdr_dlss_streamline_requested(void) { return dlssImplStreamline() ? 1 : 0; }

// P208：-1 = 还没探测（非 Streamline 路径或 slIsFeatureLoaded 未跑），0 = 探测到未加载，1 = 已加载。
// 只在“明确探测到未加载”时报不可用，避免把非 Streamline 的 NGX 路径误判成坏。
namespace { std::atomic<int32_t> gSlDlssLoaded{-1}; }
void mcdr_dlss_note_sl_loaded(int32_t loaded) { gSlDlssLoaded.store(loaded ? 1 : 0); }
int32_t mcdr_dlss_available(void) { return gSlDlssLoaded.load() == 0 ? 0 : 1; }

// ---------------------------------------------------------------------------
// C interface for the device module. Every string crosses as a (char*, cap) buffer,
// because std::string does not survive a module boundary (the plain TU sees the global
// module's std::string, the module sees import std;'s).
// ---------------------------------------------------------------------------
extern "C" {

// P108: is `p` backed by committed, readable memory? Used to vet the four texture handles in
// mcdr_dlss_input before any vtable call (see the comment in mcdr_dlss_evaluate for the crash
// this exists to prevent). Deliberately cheap: VirtualQuery walks the VAD for the address.
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

// B4：注入模块的私有纹理创建/销毁入口（见 mcdr_dlss_impl.h 的 DlssTextureDesc）。
// 返回 1 = 两个入口都装上了；0 = 缺一个（此时本单元不会创建任何私有纹理）。
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

// Called by the RTX probe once it has loaded nvngx_dlss.dll — that path is the single
// most useful diagnostic when Init fails, and the probe already has it.
void mcdr_dlss_register_runtime_path(const char* path, int32_t cap) {
    if (path == nullptr || cap <= 0) {
        return;
    }
    s.runtimePath = path;
}

// P108 blocker-B: the JVM announces the directory the vendor DLLs live in (the same
// `<user.dir>\dxrmc\` MCDR.dll is extracted to). This both widens the process DLL search
// path — so the bare-name LoadLibraryW inside the probe can succeed — and gives the
// loader below a path to try. Idempotent; returns 1 when the search path was widened.
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
    // P108 blocker-B root cause: tell NGX where to look for its feature DLLs. This is what
    // turns Init/GetFeatureRequirements from FAIL_InvalidParameter into Success.
    rebuildCommonInfo(wide);
    logInfo("NGX feature path list set to \"%s\" (nvngx feature DLLs are looked up there "
            "instead of the application folder, which is the JVM's bin\\ in a dev run)",
            narrow(wide).c_str());
    logEnvOnce();
    return s.searchPathSet ? 1 : 0;
}

// P108: loads nvngx_dlss.dll (the probe calls this instead of doing it itself, so every
// attempt lands in the native log with its own GetLastError). Returns the module handle as
// an opaque pointer, or NULL.
//
// The handle is cached: the module is loaded once and its HMODULE is what both the probe
// and NGX need. An earlier revision tried to re-derive the handle from the recorded path
// string with LoadLibraryW and returned whatever came back — when the path was stale that
// produced a non-NULL junk handle, which made the probe treat the runtime as present and
// its own fallback never ran (observed as `ngxRuntime=no` plus GetLastError=0x7E with no
// P108 attempt lines). Never derive the handle again.
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

// Reads back the nvngx_dlss.dll path the RTX probe reported (never NULL; an empty string
// until the probe has found it). Used for the one-time P108 availability line.
const char* mcdr_dlss_runtime_path(void) { return s.runtimePath.c_str(); }

// P108: the NVSDK_NGX_FeatureCommonInfo whose PathListInfo names the vendor directory, for
// callers that must pass it to NGX themselves (the RTX probe's GetFeatureRequirements).
// Returns NULL while no vendor directory is known.
void* mcdr_dlss_ngx_feature_info(void) { return commonInfoForNgx(); }

int32_t mcdr_dlss_can_run(void* device) {
    if (device == nullptr) {
        return 0;
    }
    // P108 阻塞 B：先确保运行期 DLL 真的加载过。探测通常已经加载（probeDlss 调
    // mcdr_dlss_load_runtime），但若 DLSS 是第一次被用到、或探测那次失败过，这里再试一次；
    // 没有它，ngx 的入口会以 ERROR_MOD_NOT_FOUND 之类的方式失败在 Init 里，日志却看不出
    // 是"没加载"还是"不支持"。
    logEnvOnce();
    if (s.runtimePath.empty()) {
        if (loadRuntimeDll() == nullptr) {
            return 0;
        }
    }
#if MCDR_DLSS_HAVE_STREAMLINE
    if (dlssImplStreamline()) {
        // P112 阶段 1：SL 拥有 NGX —— 既不加载也不初始化 nvngx。对 JVM 报"可用"，
        // 让 CreateDlssFeature 被调用（那里做 slDLSSSetOptions）。
        return 1;
    }
#endif
    return ensureInit(static_cast<ID3D12Device*>(device)) ? 1 : 0;
}

// flagsOut bits, mirrored by the device module's status bits:
//   1 INIT_TRIED  2 INIT_OK  4 PARAMS_OK  8 OPTIMAL_OK  16 FEATURE_OK  32 DISABLED
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
            // P291n：Streamline 模式下 NGX 归 SL 所有，我们自己的 NGX init 从不执行，
            // 所以 `initOk` 永远是 false。旧文案因此对这条路径永远报"not initialised yet"，
            // 而实际上 DLSS 可能正在正常工作 —— 状态行必须说清"谁在拥有 NGX"。
            std::snprintf(note, static_cast<std::size_t>(noteCap),
                          "Streamline owns NGX; DLSS switch is %s; tier=%u (%s), %llu evaluates so far",
                          s.enabled ? "ON" : "OFF", s.qualityValue,
                          s.qualityIsExplicit ? "explicit tier" : "closest to the ratio",
                          static_cast<unsigned long long>(s.submitCount));
        } else if (!s.initOk) {
            std::snprintf(note, static_cast<std::size_t>(noteCap), "NGX init failed (%s)",
                          s.failure.empty() ? "unknown" : s.failure.c_str());
        } else if (!s.enabled) {
            // P108 (user m10920): the switch is its own state, so report it before "active".
            std::snprintf(note, static_cast<std::size_t>(noteCap),
                          "NGX ready, DLSS switch is OFF (featureOk=%d, %llu frames submitted so far)",
                          s.featureOk ? 1 : 0, static_cast<unsigned long long>(s.submitCount));
        } else if (!s.featureOk) {
            std::snprintf(note, static_cast<std::size_t>(noteCap),
                          "NGX ready, DLSS ON but feature not created yet (%llu evaluates so far)",
                          static_cast<unsigned long long>(s.submitCount));
        } else {
            // P108 (user m11147): report the tier and where it came from, so the UI and the log
            // can be compared without guessing whether the user picked it or MCDR derived it.
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
        // P112 阶段 1：streamline 模式下 ensureInit 主动跳过 NGX，因此必然走到这里；
        // 把创建/选项交给 SL 的 slDLSSSetOptions，然后返回 0（evaluate 迁到阶段 2 才出图）。
        // P112：NGX 在 streamline 模式下从未 init，这里把创建/选项交给 SL 并返回 1 ——
        // 返回 1 表示"特征已就绪"，JVM 才会继续调用 mcdr_dx12EvaluateDlss；返回 0 会被
        // 当成"特征未就绪"而跳过 evaluate（P112 实测）。
        if (dlssImplStreamline()) {
            // P291n：先解析档位再下发选项 —— 否则 s.qualityValue 还是初值 0（=MaxPerf）。
            (void)resolveQualityValue(*in);
            s.initAttempted = true;  // 状态串用：SL 拥有 NGX，我们自己的 init 永远不会跑
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
    // Install the module-provided tracked-transition callback for this call (see transition()).
    s.transitionFn = tl;
    s.transitionCtx = tlCtx;
    s.createdThisFrame = false;
    if (!ensureFeature(cmd, *in)) {
        return 0;
    }
    logFrameChange(*in, s.createdThisFrame);
    return 1;
}

// P108: the depth snapshot (see mcdr_dlss_impl.h for why this exists).
//
// The JVM calls this from `DeepLearningSuperSampling.captureMotionVectors()`, i.e. at the point
// in the frame where the depth buffer still holds this frame's world depth — before vanilla's
// `clearDepthTexture` (GameRenderer.render) wipes it to 0.0 (reverse-Z "far"). By the time DLSS
// is evaluated the live depth is already clear, which is why the depth-aware upscale had
// degenerated into a colour-only one.
//
// The passed-in depth resource is COPY_SOURCE only for the duration of this call: we ask the
// module for its tracked state, transition, copy, and put it back exactly as it was, so the
// JVM's state machine around the world render is untouched.
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
    // P76f：设备模块现在把深度纹理建在**无类型**格式上（`R32_TYPELESS` 等），否则它无法被
    // 采样（有类型资源建别的格式的 SRV 是非法的，采样恒返回 0 —— 那是速度 pass 读到全 0
    // 深度的原因）。但 NGX 只认具体的有类型格式，所以快照这一侧仍按具体格式建；拷贝
    // 在同一类型族内的 TYPELESS ↔ typed 之间是合法的。
    const DXGI_FORMAT snapFormat = [](DXGI_FORMAT f) -> DXGI_FORMAT {
        switch (f) {
            case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
            case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;
            case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
            case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            default: return f;
        }
    }(dd.Format);
    // (Re)create the private snapshot texture when the size/format changed.
    // P108x: compare EVERYTHING a full-resource copy requires to agree, not just w/h/format. The
    // debug layer caught the old check being too loose:
    //   [D3D12 ERROR #944] CopyResource: Source and Destination texture resource must have equivalent
    //   dimensions, including width, height, depth, mip levels, and array size.
    // The copy therefore never landed and the snapshot stayed all-zero (a dead depth term for NGX).
    if (s.depthSnapshot == nullptr || s.depthSnapshotWidth != w || s.depthSnapshotHeight != h ||
        s.depthSnapshotFormat != snapFormat || s.depthSnapshotMips != dd.MipLevels ||
        s.depthSnapshotArray != dd.DepthOrArraySize ||
        s.depthSnapshotSamples != dd.SampleDesc.Count) {
        // B4: created by the device module (see DlssTextureDesc in mcdr_dlss_impl.h) and released
        // through its deferred-delete path — never Release() here. Mirror the source's
        // mip/array/sample shape so a full copy is legal at every level. Flags stay NONE: this
        // texture is only ever COPY_DEST / COPY_SOURCE / NON_PIXEL_SHADER_RESOURCE.
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
    // Save the caller's state, copy, restore. `stateOf` may be null (older module), in which
    // case COMMON is used as the tracked anchor and restored to COMMON.
    const int32_t before = (stateOf != nullptr)
                               ? stateOf(tlCtx, depthResource)
                               : static_cast<int32_t>(D3D12_RESOURCE_STATE_COMMON);
    // P108x: the JVM depth is deliberately NOT dumped here any more.
    //
    // The helper drains the GPU queue up to the point of insertion, so it does read real bytes —
    // but at this call site the interesting content is not there yet: this frame's depth is
    // written by commands the *current, still-open* command list has not submitted, so the read
    // can only ever show the cleared value. That made the label misleading ("the source is
    // empty") when the truth was "this reading cannot see frame-in-flight content".
    //
    // The trustworthy judgement is the depth actually handed to NGX at evaluate time, which is
    // dumped as `input-depth-after-evaluate` (the snapshot when one exists). The build that moved
    // the snapshot hook to the end of the frame's main pass reports e.g.
    //   input-depth-after-evaluate 683x384 fmt=40 mean=14.6/16.4/13.5 max=255/254/255
    // i.e. real depth. Anything read at an earlier point is uninformative and is omitted.
    // P108x: `CopyTextureRegion` with an explicit box on subresource 0, NOT `CopyResource`.
    // `CopyResource` demands identical width/height/depth/mip/array (#944 above) and silently does
    // nothing when they differ; an explicit box copies the top mip of slice 0 regardless of the rest
    // of the shape, which is all DLSS samples anyway.
    // The snapshot's own state is QUERIED, never assumed — the debug layer also flagged a barrier
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
    tl(tlCtx, depthResource, before);  // hand the depth back exactly as it was found
    ++s.depthSnapshotFrame;
    if (s.depthSnapshotFrame == 1) {
        logInfo("DLSS depth snapshot captured (first): %ux%u fmt=%d from resource %p; it "
                "replaces the JVM's depth at evaluate time",
                w, h, static_cast<int>(dd.Format), depthResource);
    }
    return 1;
}

// P113：最近一帧的 SL frame token（Reflex 的 slReflexSleep 要在 Present 前用它）。
sl::FrameToken* gLastFrameToken = nullptr;
uint64_t gStreamlineFrameSerial = 0;
uint64_t gWorldConstantsFrame = UINT64_MAX;
uint64_t gDisplayConstantsFrame = UINT64_MAX;
bool gDisplayInputsAccepted = false;


    // P113/P115：Reflex 低延迟 + PCL 延迟标记 + 帧率限制器。
    // 文档：ProgrammingGuideReflex.md §4（选项）/§5（Present 前 sleep）、ProgrammingGuidePCL.md（标记）。
    // 定义在文件后部的帧生成状态；Reflex 的“FG 必须开 Reflex”联动要用它。
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
            // P122：Reflex 模式（0=off 1=eLowLatency 2=eLowLatencyWithBoost），默认低延迟。
            int32_t mode = 1;
            bool resolved = false;
            bool optionsApplied = false;
            // P247：slReflexSetOptions 上一次是否真的成功。只有成功才允许 slReflexSleep /
            // PCL 标记 —— 否则 SL 每帧都会打 "Reflex feature not yet initialized; call
            // slReflexSetOptions() first"，而且那本来就是必然失败的调用。
            bool optionsOk = false;
            uint32_t setOptsFailures = 0;
            uint32_t frameLimitUs = 0;
            uint32_t markerFailures = 0;
            uint32_t sleepFailures = 0;
            bool sleepOkLogged = false;
            // P213d：上一次真正下发的生效模式与限帧，用来判断“要不要重发”。
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
            // 只有真的取到函数才 latch：`Options.load` 阶段（slInit 之前）也会走到这里，
            // 那时 `slGetFeatureFunction` 还没准备好，latch 住就永远解析不到了。
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
        // P247：返回 true 表示“本帧 Reflex 选项已确认下发成功”，调用方据此决定要不要
        // slReflexSleep / 发 PCL 标记。改动前这里无条件把 optionsApplied 置 true，
        // 早期（slInit/slSetDevice 完成前）那次失败的调用就把状态 latch 住了，
        // 之后每帧都走“已下发”的短路分支，SL 收到的永远是无效状态。
        bool applyReflexOptions() {
            if (gReflex.setOptions == nullptr) return false;
            // P213d：帧生成必须开 Reflex —— Reflex 关闭时 Streamline 直接拒绝插帧
            // （sl.log 里是 "DLSS-G disabled"）。用户选的 OFF 在 FG 打开期间按 ON 生效，
            // FG 关掉后自动回到用户原值。
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
            opt.frameLimitUs = gReflex.frameLimitUs;  // 0 = 不限帧（文档：与 mode 独立）
            const sl::Result r = gReflex.setOptions(opt);
            if (r != sl::Result::eOk) {
                // 失败不要 latch：下一帧重试（设备/交换链就绪后通常会成功），
                // 期间把 sleep/标记拦住，避免 SL 侧每帧刷同一条错误。
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
            // P247：Reflex/PCL 只有在 slReflexSetOptions 成功之后才有效，否则 SL 同样会
            // 回 “feature not yet initialized” 并刷日志。
            if (!gReflex.optionsOk) return;
            if (gReflex.marker == nullptr || gLastFrameToken == nullptr) return;
            const sl::Result r = gReflex.marker(marker, *gLastFrameToken);
            if (r != sl::Result::eOk && gReflex.markerFailures < 3) {
                ++gReflex.markerFailures;
                logWarn("pcl: marker %d result=0x%X", static_cast<int>(marker),
                        static_cast<unsigned>(r));
            }
        }
    }  // namespace

    void mcdr_reflex_set_frame_limit(uint32_t micros) {
        resolveReflexApi();
        gReflex.frameLimitUs = micros;
        gReflex.optionsApplied = false;  // 下次 Present 前重新下发
    }

    // P117：每帧开始（世界渲染之前）—— 建本帧的 SL frame token 并发 eSimulationStart。
    // 文档 ProgrammingGuidePCL.md:144/152：simulation/submit/present 标记必须用**同一帧**的 token。
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

    // P117：通用标记（eSimulationEnd / eRenderSubmitStart 等由 JVM 在正确时机调）。
    // P122：界面上的 Reflex 模式（0=off 1=on 2=on+boost），下一帧 Present 前重新下发。
    void mcdr_reflex_set_mode(int32_t mode) {
        if (!reflexEnabled()) return;
        // 只存值：`Options.load` 阶段调到这里时 SL 还没 init，不能在这里解析函数指针。
        const int32_t clamped = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
        if (clamped == gReflex.mode) return;
        gReflex.mode = clamped;
        gReflex.optionsApplied = false;
    }

    // P122：Reflex 是否可用（三个入口都解析到才算），供界面置灰。
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
        // P247：只有 setOptions 确认成功后才有资格 sleep / 发标记。
        const bool reflexReady = applyReflexOptions();
        // P131：JVM 的 frame-begin 钩子没跑到时（例如 DLSS 没在渲染），这一帧还没有 token。
        // Reflex 的 sleep/限帧/标记全都要 token，缺了就整条链路静默失效；这里就地补一个。
        if (gLastFrameToken == nullptr && gReflex.newFrameToken != nullptr) {
            sl::FrameToken* token = nullptr;
            uint32_t frameIndex = ++gReflex.frameCounter;
            if (gReflex.newFrameToken(token, &frameIndex) == sl::Result::eOk && token != nullptr) {
                gLastFrameToken = token;
            }
        }
        if (reflexReady) {
            emitMarker(sl::PCLMarker::eRenderSubmitEnd);  // 本帧所有提交已完成，接着 Present
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
        gLastFrameToken = nullptr;  // 下一帧由 mcdr_reflex_frame_begin() 重新建
    }

#if MCDR_DLSS_HAVE_STREAMLINE
    // P112：JVM 每帧送来的相机常量（sl::Constants 的数据源）。单线程（渲染线程）写入/读取。
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
    // ProgrammingGuide.md §2.11.1：slSetConstants(constants, frame, viewport)
    using PFn_slSetConstants = sl::Result (*)(const sl::Constants&, const sl::FrameToken&,
                                             const sl::ViewportHandle&);
    // P213：帧生成的选项入口（ProgrammingGuideDLSS_G.md §6.0）。
    using PFn_SlDLSSGSetOptions = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSGOptions&);
    // P213：帧生成的状态查询（读 numFramesActuallyPresented / status，用来验证是否真的在插帧）。
    using PFn_SlDLSSGGetState = sl::Result (*)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*);

    // P213 阶段 2：帧生成的开关状态。0 = off，1 = on（固定倍率），2 = dynamic。
    // 由 ABI `mcdr_dx12SetDlssgMode` 写入，渲染线程每帧读取。
    std::atomic<int32_t> gDlssgMode{0};
    std::atomic<int32_t> gDlssgFrames{1};
    // P213c：最近一次 Present 真正提交了几帧（1 = 只有应用帧，2+ = 插帧生效）。给 F3 的 output 用。
    std::atomic<int32_t> gFgPresented{1};
    // P291e：上一帧结束时输出纹理停在哪个 D3D12_RESOURCE_STATES（0 = 未知/没有输出纹理）。
    // JVM 侧的黑帧采样要拿它做"已知起始状态"的读回 —— 按 COMMON 硬转是非法 barrier，
    // 读回来的是未定义内容（实测恒 0，于是 DLSS 明明正常却被熔断成永久线性 blit）。
    std::atomic<int32_t> gDlssOutputState{0};

    // P112 阶段 2：Streamline 的每帧调用面（全部运行时解析，不引入静态导入）。
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
        // SR must already have written HUDless. These calls record SDK-owned copies on cmd;
        // wrapper pins must survive execution of cmd, while SL owns the later Present reads.
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

    // P213：DLSS-G 插件可能比 DLSS 晚加载，所以解析要能重试（见 refreshFgApi）。
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
                // P213：帧生成的选项函数在 DLSS-G 插件里，插件没加载时这里就是 nullptr。
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

    // P112：4x4 行主序矩阵工具（sl::Constants 需要 cameraViewToClip / clipToCameraView /
    // clipToPrevClip / prevClipToClip 四个矩阵，而 JVM 只送 viewProj 与 prevViewProj）。
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

    // Gauss-Jordan 求逆（失败返回 false，out 保持原样）。
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

    // SL 的 DLSS 评价：按 ProgrammingGuideDLSS.md 的顺序 —— tag 资源 → 设选项 → evaluate。
    // eDisableCLStateTracking 下资源状态由我们负责（这里沿用 NGX 路径的同一组状态）。
    // P213：DLSS-G 的函数指针是延迟解析的。插件在第一次 evaluate 之后才 load 时，
    // 之前的 nullptr 会被永久缓存，帧生成就此静默失效——这里按 500ms 节奏重试。
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
        // P112：输出目标优先用 JVM 交付的 UAV 纹理（s.externalOutput，见 mcdr_dlss_set_output_texture）。
        // 直接把 GUI 目标（无 UAV 位）标成 kBufferTypeScalingOutputColor，NGX 会报
        // FAIL_RWFlagMissing(0xBAD00009)（SL 日志里是 "[sl.dlss] NGX evaluate feature failed"）。
        // P114（用户报告的 GUI 文字被当作运动物体）：DLSS 的 temporal history 以**自己的输出**
        // 为反馈源。如果直接把 GUI 目标（每帧还要被 GUI 画一遍）标成 ScalingOutputColor，
        // 历史缓冲会被 GUI 污染 —— 滑动界面时文字就被当成运动物体放大/拖出框外。
        // NGX 路径从来没这个现象，因为它写的是私有 UAV / handover 纹理再拷回目标。
        // 这里照做：优先 externalOutput（JVM 交付的 UAV），否则用模块私有的 dlssOutput，
        // 只有两者都不可用才退化成直接写 GUI 目标。
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
        // 文档（ProgrammingGuideDLSS.md:155）：**资源状态由 SL 管理，宿主不要手动 transition**；
        // 宿主的责任是 evaluate 之后恢复命令列表状态（:242-253，配合 eDisableCLStateTracking）。
        // P117：token 由 mcdr_reflex_frame_begin() 在帧开始建好；这里复用，保证 PCL 标记与
        // DLSS evaluate 落在**同一帧**（文档要求）。没有 frame_begin（例如 Reflex 关闭）时兜底自建。
        sl::FrameToken* token = gLastFrameToken;
        if (token == nullptr) {
            uint32_t frameIndex = ++gReflex.frameCounter;
            if (api.newFrameToken(token, &frameIndex) != sl::Result::eOk || token == nullptr) return 0;
            ++gStreamlineFrameSerial;
        }
        const sl::ViewportHandle viewport(s.srViewport);
        gLastFrameToken = token;  // P113：Reflex 在 Present 前要用它

        // 1) 相机常量（每帧、尽量早；ProgrammingGuide.md §2.11.1）。没有 JVM 送来的
        //    `mcdr_dlss_constants` 时只能跳过 —— slSetConstants 缺字段 DLSS 会拒绝该帧。
        if (!gCameraConstantsValid || !api.setConstants || gWorldConstantsFrame == gStreamlineFrameSerial)
            return -1;
        sl::Constants constants{};
        if (!makeStreamlineCamera(gCameraConstants, 0, constants)) return -1;
        if (s.resetPending || in.reset != 0) constants.reset = sl::Boolean::eTrue;
        gWorldConstantsFrame = gStreamlineFrameSerial;
        if (api.setConstants(constants, *token, viewport) != sl::Result::eOk) return -1;

        // Local SR tags do not publish FG inputs or copy the as-yet unwritten SR output.
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

        // 3) 选项（:170-186：各档 preset + 模式 + 输出尺寸 + 非 HDR/非自动曝光）
        const auto options = srStreamlineOptions(in, s.qualityValue, s.requestedPreset);
        if (!options) return 0;
        const auto& opt = *options;
        const bool hdrInput = opt.colorBuffersHDR == sl::Boolean::eTrue;
        // P199：`slDLSSSetOptions` 是"配置变化时"的调用，不是每帧调用。26.3 起我们每帧都会走到
        // 这里，而 SL 的 DLSS 插件在 SetOptions 里会重建内部资源（实测每帧调用时进程私有提交
        // 内存约 750 MB/min 线性上涨，关闭 DLSS 则完全平坦）。只在参数真的变了才下发。
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


        // 4.5) handover：只有当 SL 写的那张纹理不是 GUI 目标时才需要拷回（同尺寸、同格式）。
        // P115：`outResource == out` 说明两者已经是同一张纹理，这一拷贝可以直接省掉。
        if (copyBack && outResource != out && evalResult == sl::Result::eOk) {
            transition(outResource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(out, outResource);
        }
        // 5) 宿主负责恢复（:242-253 + eDisableCLStateTracking）：只恢复输出到 RT。
        // 输入侧我们不再改状态，所以也没有要恢复的 barrier。
        transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);

        // P291e：记下这一帧结束时"那张被 DLSS 写过的输出纹理"停在哪个状态，
        // 供 JVM 做状态正确的读回（0 = 本帧没有 handover 纹理，别采样）。
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
    // Invalidate only the current CPU frame. Older copies may still be used on SDK queues.
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
    // P127（p93 实测：native 已自报 impl=streamline，但 streamlineEvaluate 一次都没被调用）：
    // `s.disabled` 是 **NGX 路径**自己的失败状态（建不出 feature / MV 中间纹理失败时置位）。
    // 它原来排在这个分支之前，于是启动阶段只要 NGX 探测失败，Streamline 就被永久挡在门外，
    // native 每帧静默 return -1，JVM 只看到 "failed to evaluate 30 frames in a row"。
    // Streamline 模式下 NGX 的状态与它无关，所以必须先判 SL。
    if (dlssImplStreamline()) {
        // P112 阶段 2：streamline 模式下 NGX 从未 init/创建，这里直接走 SL 的
        // tag → options → evaluate 链（资源状态由 streamlineEvaluate 内部负责）。
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
    // Install the module-provided tracked-transition callback for this call (see transition()).
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
    // P108 (2026-10-16 crash, hs_err_pid52140): a NON-NULL but invalid handle passed the
    // `== nullptr` check above and faulted on the very first vtable call —
    // `EXCEPTION_ACCESS_VIOLATION reading 0x50`, native frames
    // `mcdr_dlss_evaluate+0xba (d3d12_dlss.cpp:1125)` -> `applyDlssToTexture+0x31d` ->
    // `mcdr_dx12EvaluateDlss+0x13`, i.e. the first `mv->GetDesc()`. Reject a dangling or
    // truncated handle HERE instead of dereferencing it: a garbage pointer must degrade to
    // "DLSS skipped this frame", never to a process kill. Reported once, with raw values.
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

    // Motion vectors must be in a format DLSS can bind. The velocity pass writes
    // RG16_FLOAT, which is exactly what NGX wants; anything else is copied into an
    // intermediate of the same footprint first.
    ID3D12Resource* mvForDlss = mv;
    const bool mvIsRg16 = (mv->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT);
    if (!mvIsRg16) {
        const D3D12_RESOURCE_DESC md = mv->GetDesc();
        const bool reuse = (s.mvBuffer != nullptr) && (s.mvBuffer->GetDesc().Width == md.Width) &&
                           (s.mvBuffer->GetDesc().Height == md.Height);
        if (!reuse) {
            // B4: created by the device module (verified/tracked there), released through its
            // deferred-delete path — no CreateCommittedResource in this unit.
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
    // P108: NGX's output must be a UAV resource. The JVM's target is created with
    // ALLOW_RENDER_TARGET only, which NGX rejects with `FAIL_RWFlagMissing` (0xBAD00009), so DLSS
    // has to write into something else.
    //
    // HISTORY — why the JVM now supplies that something (user m11147 follow-up, control-group
    // evidence): our own `CreateCommittedResource` texture turned out to be unwritable. With the
    // IDENTICAL source, call site and command list, copying the colour input into the
    // module-created target read back `mean=17.8/16.5/11.6/255.0`, while the same copy into this
    // unit's private texture read `mean=0.0/0.0/0.0/0.0` — all zero, alpha included — both
    // immediately and on the following frame. So the ad-hoc creation path produces a resource the
    // GPU will not write; the module's `createTexture` path does not.
    //
    // Therefore: if the JVM hands us a UAV texture it created through the module (see
    // `mcdr_dx12SetDlssOutput`), use it and never build a private one. The private texture stays
    // only as a fallback for older JVM builds / older DLL pairings.
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
            // Never silent: a mismatch means the copy back would be nonsense, so say so and fall
            // back to the private texture (the old, still-broken-on-this-machine path).
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
            // `out` only has to be a copy destination until the copy below.
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(priv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    // P291b: never ask NGX to write into a texture this module did not create with
    // ALLOW_UNORDERED_ACCESS. The JVM GUI target has no UAV flag, so transitioning it to
    // UNORDERED_ACCESS is what removes the device (user m04140: turning the hand-over off
    // crashed the game). If neither the hand-over texture nor a private one is usable, skip
    // the frame instead of removing the device.
    if (!directOutput && ngxTarget == out) {
        logError("no usable DLSS output texture this frame (the hand-over texture was "
                 "rejected and the private one could not be created); skipping the evaluate "
                 "instead of transitioning a non-UAV target to UNORDERED_ACCESS");
        return 0;
    }
    NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = ngxTarget;
    // P108: 0, not the SDK's "optimal" sharpness. The reference integration sets 0 with the note
    // "sharpening is deprecated in DLSS"; the value we used to pass (0.35 here) is a leftover from
    // the optimal-settings query and is not what a working integration asks for.
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
    // P108: hand the result back. DLSS wrote into our UAV texture, so copy it into the JVM's
    // target and leave that target in RENDER_TARGET — the GUI render pass attaches it right
    // after this returns (the contract this entry point has always had).
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
    // P291b: 1 = the result went into the JVM hand-over texture (or straight into the JVM
    // target); 2 = it went into the module private texture, which the JVM must NOT treat as
    // "the GUI target already carries the world layer" - that is the black-screen path.
    return viaHandover ? 1 : 2;
}

    // Called from destroyDevice() BEFORE the D3D12 device goes away: NGX must release its
    // feature and shut down while the device is still alive.
    // P108 (user m11147): record the tier the user picked. Called by the device module whenever
    // `setDlssQuality` observes a change. A value >= 0 makes `ensureFeature` rebuild the feature on
    // the next frame (PerfQualityValue is a CreateFeature input); -1 restores the "closest to the
    // render:target ratio" behaviour for old DLL/old JVM combinations.
    int32_t mcdr_dlss_request_quality(int32_t tier) {
    const int normalised = (tier < 0) ? -1 : (tier > 4 ? 4 : tier);
    if (normalised != s.requestedQuality) {
        s.requestedQuality = normalised;
        // Allow a rebuild even if a previous create failed, so switching tiers can recover.
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

    // P108ac (user m08033): the DLSS *model* (NGX render preset). The user's report was that the
    // remaining noise/flicker is inherent to DLSS and that the usual remedy is swapping the model,
    // so this exposes exactly that knob instead of guessing a better default ourselves.
    //
    // `preset` is a NVSDK_NGX_DLSS_Hint_Render_Preset value: 0 = Default (NGX decides per tier),
    // 5 = E and 6 = F (deprecated CNN presets), 10 = J, 11 = K, 12 = L, 13 = M (transformer).
    // Out-of-range values are rejected with -1 and change nothing. A change makes `ensureFeature`
    // rebuild the feature on the next frame, because the preset is a CreateFeature input.
    int32_t mcdr_dlss_request_preset(int32_t preset) {
        if (preset < 0 || preset > NVSDK_NGX_DLSS_Hint_Render_Preset_O) {
            logWarn("DLSS render preset %d is out of range (valid: 0=Default, 5=E, 6=F, "
                    "10=J, 11=K, 12=L, 13=M) — ignored",
                    preset);
            return -1;
        }
        if (preset != s.requestedPreset) {
            s.requestedPreset = preset;
            // Same recovery rule as the tier: a failed create must be retryable by changing the
            // model, otherwise a first-frame failure locks the button out for the whole session.
            s.createdWarned = false;
            if (!s.presetRequestLogged) {
                s.presetRequestLogged = true;
                logInfo("DLSS render preset request: %d (%s) — the feature will be rebuilt",
                        preset, presetName(preset).c_str());
            }
        }
        return preset;
    }

    // P108 (user m11147 follow-up): the JVM hands us the UAV texture it created through the module's
// `createTexture` path, to be used as NGX's output instead of this unit's private texture.
//
// The module does the unwrapping (it is the side that knows `Dx12Object`'s layout) and passes the
// raw `ID3D12Resource*` as an opaque pointer — never reinterpret a JVM handle here, that mistake
// caused the `mv->GetDesc()` null-vtable crash recorded in mcdr_dlss_evaluate.
//
// `resource == nullptr` clears the override and restores the private fallback. The resource is NOT
// owned by this unit; the module keeps the Dx12Object alive. Returns 1=accepted 0=cleared.
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

// P108 (user m10920): the DLSS on/off switch. Independent of the anti-aliasing mode, and
    // deliberately does NOT touch the loaded NGX runtime / feature handle — flipping the switch
    // off and on again must not throw away a successful Init/CreateFeature. The real gate is
    // the device module's applyDlssToTexture (gCtx.dlssEnabled); this mirror feeds the status
    // query and the log.
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
    // B4: the three private textures are module-owned now. Hand them back **before** the device
    // goes away (ctx = nullptr: no command list to touch, just the deferred delete). They used to
    // be leaked here (`s = State{}` dropped the pointers) or Release()d inline.
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

// P249：DXGI 工厂升级入口。设备模块建工厂后立刻调一次，把 swapchain 交给 SL 的代理工厂
// （否则 DLSS-G 一次都不出帧；见 mcdr_dlss_impl.h 里那段说明）。
int32_t mcdr_sl_upgrade_interface(void** baseInterface) {
#if MCDR_DLSS_HAVE_STREAMLINE
    if (!dlssImplStreamline()) {
        return 0;
    }
    // P253（2026-10-26，用户 m04053 的对照实验）：**只要 DLSS 实现是 STREAMLINE，链就一律走
    // SL 代理工厂**，不再按帧生成开关决定。依据是同一台机器上的 A/B：
    //   dev（FG 开 ⇒ 代理链，Present 槽 owner=sl.interposer.dll）: RTSS OSD 正常；
    //   用户 Prism 实例（FG 关 ⇒ P250 保持原生链，owner=DXGI.DLL+jmp）: OSD 不显示。
    // 两边的 RTSSHooks64 基址、两个 D3D12Core、swapchain 描述、窗口模式全部一致，唯一测到的
    // 差异就是链是否被 SL 代理 —— 所以先按“代理链才让 RTSS 的 OSD 生效”验证。
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


}  // extern "C"

// P213：帧生成的开关（C++ 链接，与 mcdr_dlss_impl.h 的声明一致）。
// 0 = off，1 = on（固定倍率），2 = dynamic（多帧，驱动 ≥ 595.41）。
// 真正的下发在 streamlineEvaluate() 里（那里才有 frame token 与 viewport，且必须在呈现线程上调用）。
void mcdr_dlss_set_fg_mode(int32_t mode, int32_t numFrames) {
    gDlssgMode.store(mode);
    gDlssgFrames.store(numFrames < 1 ? 1 : numFrames);
    // P213c（用户 m01219）：关掉时立刻把“上一次提交的帧数”复位，不等下一帧 evaluate ——
    // F3 每帧都在读它，晚一帧就会显示一次 2× 的错数。
    if (mode == 0) {
        gFgPresented.store(1);
    }
}
// P213c：上一次 Present 真正提交的帧数（1 = 没插帧）。JVM 的 F3 用它把 output 累加成
// “presented fps”，否则一次 slPresent 只被 JVM 看到一次，两个数字永远相等。
int32_t mcdr_dlss_fg_presented(void) {
    return gFgPresented.load();
}
// P291e：DLSS 输出纹理当前所处的 D3D12_RESOURCE_STATES（0 = 未知/没有 handover 纹理）。
// 读回采样必须带这个状态，否则 barrier 的 StateBefore 是错的、内容未定义。
int32_t mcdr_dlss_output_state(void) {
    return gDlssOutputState.load();
}
