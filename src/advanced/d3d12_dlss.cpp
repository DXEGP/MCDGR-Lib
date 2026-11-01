// d3d12_dlss.cpp — DLSS 超分（NVSDK_NGX 数据面 + 真正的 Evaluate 调用）。
//
// 独立翻译单元：把厂商类型（ID3D12Resource / NVSDK_NGX_*）藏在极小的 C 接口后面，
// mcdr.d3d12.device 因此永远不 include NGX 头、也不继承 DLSS 的许可面。依赖单向：模块声明
// 这些入口并调用它们，本文件不 import 任何 mcdr.* 模块。
//
// 在帧中的位置：世界渲染之后、GUI 之前，把 render-scaled 的世界层放大到窗口尺寸的目标 ——
// 与线性 blit 相同的位置。DLSS 与 SMAA 是同一个抗锯齿选项的两个取值，绝不并存。
//
// 失败策略：每条失败路径最多打一行并按状态锁存，不逐帧重试；DLSS 永不让整帧失败。
//
// 深度是 REVERSE-Z（投影矩阵已交换 near/far，比较函数 GREATER_EQUAL，深度清成 0 = 远），
// 故必须声明 DepthInverted。MV 是屏幕空间的 **像素** 位移，InMVScaleX/Y 保持 1.0，且不含
// jitter 差（jitter 由 InJitterOffsetX/Y 单独传，故不置 MVJittered）。
//
// 见 docs/advanced/dlss.md
#include <windows.h>

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
// DLSS 实现二选一：Streamline 接管，或直连 NGX（默认 NGX）。
#if __has_include(<sl.h>)
#include <sl.h>
#include <sl_dlss.h>   // sl::DLSSMode / sl::DLSSOptions（sl.h 只含 core）
#include <sl_reflex.h>  // P113：sl::ReflexOptions / sl::ReflexMode
#include <sl_dlss_g.h>
#include <sl_dlss_d.h>  // P213：sl::DLSSGOptions / sl::DLSSGMode（帧生成）
#define MCDR_DLSS_HAVE_STREAMLINE 1
#else
#define MCDR_DLSS_HAVE_STREAMLINE 0
#endif

#include "mcdr_abi.h"
// extern "C" 接口：本单元与设备模块的边界（转换/状态查询回调类型）。
#include "mcdr_dlss_impl.h"
#include "mcdr_dlssd_impl.h"
#include "mcdr_hdr_state.h"  // P197：HDR 激活状态（colorBuffersHDR）
import std;

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
// ProjectId 是与 RTX 探测共享的同一身份（SDK 只认一处应用标识）：一个**稳定的 36 字符 GUID 串**，
// 一经确定不可更改 —— 改了会让 NGX 的 per-app 状态失效。约束是"稳定、非空、GUID 形状"。
#define MCDR_NGX_PROJECT_ID "7c1f4a52-9b3e-4d68-8f21-5a6c0e2d9b74"
#define MCDR_NGX_ENGINE_VERSION "1.0"
#define MCDR_NGX_APPDATA L".\\ngx"
// NGX 运行期发现与诊断。放在这里而不是 d3d12_rtx_probe.cpp：普通 TU 够不到设备模块的 dbgLog*，
// 而"找不到运行期"恰恰是必须逐次记日志的失败。探测通过 mcdr_dlss_load_runtime() 调这里。
//
// nvngx_dlss.dll 是交给 LoadLibraryW 的那一个文件（随 SDK 放在厂商目录）；它只 import
// VERSION/ADVAPI32/USER32/KERNEL32，没有延迟加载表，不存在"缺依赖"。
// nvngx.dll / _nvngx.dll 是 NGX 核心，SDK 不提供：NGX 去**可执行文件**旁边找或走驱动注册表。
// 开发运行时"可执行文件"是 java.exe，所以首选必然落空，最终走注册表 —— 这是 nvngx_dlss.dll
// 存在仍不足以 Init 成功的原因。
// applicationDataPath（Init_with_ProjectID 的第 4 个参数）是 NGX 放日志/缓存的目录，**不是**
// 模型搜索路径；本版 SDK 没有 pathToNGXModel 参数。
namespace
{
    // 日志：格式与级别过滤都由设备模块负责，本单元只调它经 mcdr_dlss_install_log 交过来的
    // dbgLogInfo/dbgLogWarn/dbgLogError，绝不自己拼前缀或时间戳，也绝不在这里命名任何 mcdr.* 实体。
    using LogFn = void (*)(const char* fmt, ...);
    LogFn gLogInfo = nullptr;
    LogFn gLogWarn = nullptr;
    LogFn gLogError = nullptr;
// B4：模块提供的私有纹理创建/销毁；本单元不再自己 CreateCommittedResource —— 创建即登记进跟踪表，销毁走延迟删除。
    using CreateTextureFn = int32_t (*)(void*, const DlssTextureDesc*, void**, void**);
    using DestroyTextureFn = void (*)(void*, void*);
    CreateTextureFn gCreateTexture = nullptr;
    DestroyTextureFn gDestroyTexture = nullptr;

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
// NGX result codes -> readable text.
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
    // 状态：全进程一个 DLSS 实例（设备本身也是单例）。
    // 每档名义上采样倍率（target/render）；NGX 的动态分辨率区间是 [target/nominal, target]，
    // render 比它小时 NGXDLAA::EvaluateFeature 只写一行 dynamic scaling error 并交出空纹理
    // —— 世界全黑、GUI 正常。
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
        // DLSS 是独立开关（权威门在设备模块的 gCtx.dlssEnabled），这里只是镜像，供状态查询与日志用。
        bool enabled = false;
        uint64_t submitCount = 0;     // successful Evaluate calls
        bool tooSmallWarned = false;
        ID3D12Device* device = nullptr;
        NVSDK_NGX_Parameter* params = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        ID3D12Resource* depthStencil = nullptr;  // DLSS scratch buffer, DEFAULT heap
        // DLSS 必须写进带 ALLOW_UNORDERED_ACCESS 的资源：JVM 的 GUI/呈现目标没有 UAV 位，
        // NGX 会回 FAIL_RWFlagMissing(0xBAD00009)。所以写进这张私有的目标尺寸 UAV 纹理，再在
        // 同一条命令列表里拷进真正的输出 —— 不需要改 JVM 侧。
        ID3D12Resource* dlssOutput = nullptr;    // private UAV intermediate
        // B4：纹理由设备模块创建（含深度快照/MV 中间纹理）；本单元只留不透明句柄 + 裸指针，销毁交回 destroyTrackedTexture，绝不 Release。
        void* dlssOutputObject = nullptr;
        // JVM 经模块 createTexture 交付的 UAV 输出，优先于 dlssOutput；nullptr = 退回私有纹理；本单元不持有。
        ID3D12Resource* externalOutput = nullptr;
        UINT dlssOutputWidth = 0;
        UINT dlssOutputHeight = 0;
        DXGI_FORMAT dlssOutputFormat = DXGI_FORMAT_UNKNOWN;
        // 状态转换委托设备模块（见 transition()）。
        DlssTransitionFn transitionFn = nullptr;
        void* transitionCtx = nullptr;
        // 私有 D32 深度快照：存在时替代 JVM 的 pInDepth（DLSS 跑时它已被清空）。
        ID3D12Resource* depthSnapshot = nullptr;
        void* depthSnapshotObject = nullptr;
        UINT depthSnapshotWidth = 0;
        UINT depthSnapshotHeight = 0;
        DXGI_FORMAT depthSnapshotFormat = DXGI_FORMAT_UNKNOWN;
        UINT depthSnapshotMips = 0;      // P108x: a full copy requires the whole shape to agree
        UINT depthSnapshotArray = 0;     // 同上：数组层数也要一起比对，否则全拷贝非法
        UINT depthSnapshotSamples = 0;
        UINT64 depthSnapshotFrame = 0;   // frame counter at the last snapshot
        bool depthSnapshotWarned = false;
// slDLSSSetOptions 只在选项真的变化时下发：Streamline 每次调用都会重建内部 DLSS 资源，
// 实测逐帧调用时进程私有内存 ~800 MB/min、关掉 DLSS 则完全平坦。feature 销毁时清缓存，
// 下次 create 重推。
        bool slOptsApplied = false;
        int32_t slOptMode = -1;
        uint32_t slOptOutW = 0;
        uint32_t slOptOutH = 0;
        float slOptPreExposure = -1.0f;
        float slOptExposureScale = -1.0f;
// 逐帧 Evaluate 路径有自己的 slDLSSSetOptions 缓存：它与 create 路径可能暂时不一致（resize 先进 create），共用一个缓存会让选项再也不重发。
        bool slEvalOptsValid = false;
        int32_t slEvalOptMode = -1;
        uint32_t slEvalOptOutW = 0;
        uint32_t slEvalOptOutH = 0;
        float slEvalOptPreExposure = -1.0f;
        float slEvalOptExposureScale = -1.0f;
        uint32_t slEvalOptQuality = 0xFFFFFFFFu;
        bool slEvalOptHdr = false;

        ID3D12Resource* mvBuffer = nullptr;      // intermediate for a non-RG16 MV input
        void* mvBufferObject = nullptr;
        void* runtimeModule = nullptr;           // HMODULE of nvngx_dlss.dll once loaded
        UINT64 scratchBytes = 0;
        UINT32 mvCopyResourceState = D3D12_RESOURCE_STATE_COMMON;
        // 当前 feature 创建时的尺寸元组；变化即 destroy + recreate（历史重置）。
        int32_t renderWidth = 0;
        int32_t renderHeight = 0;
        int32_t targetWidth = 0;
        int32_t targetHeight = 0;
        uint32_t qualityValue = 0;
        // requestedQuality < 0 = JVM 没指定，按比例选档；>= 0 则原样使用，变化即重建。
        // effectiveQuality 是**被接受的档位**（0..4，-1 = 按比例选），不是 NGX 枚举（DLAA == 5）；
        // 两者绝不能直接比较 —— 那样会逐帧重建 feature。
        int requestedQuality = -1;
        int effectiveQuality = -1;
        // 请求的 NGX render preset（0=Default，10=J，11=K，12=L，13=M）；CreateFeature 输入，变化即重建。effectivePreset 是实际建成的值。
        int requestedPreset = 0;
        int effectivePreset = 0;
        bool presetRequestLogged = false;
        // DLAA 因 render != target 被降级期间为 true —— 一次性 ERROR 的状态门（曾逐帧打印）。
        bool dlaaDowngraded = false;
        bool qualityIsExplicit = false;
        bool qualityRequestLogged = false;
        int32_t featureFlags = 0;
        std::string failure;        // the stage that failed, for the status query
        std::string runtimePath;    // nvngx_dlss.dll as loaded (reported by the RTX probe)
        std::string optimalText;    // "target 1280x720, render 960x540 -> quality Balanced (...)"
        uint32_t optimalRenderW = 0;
        uint32_t optimalRenderH = 0;
        float optimalSharpness = 0.0f;
        // 输入摘要只在值变化时打印。
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
        // JVM 声明的厂商目录、是否已为它放宽进程 DLL 搜索路径、诊断行是否已打印。
        std::wstring vendorDir;
        bool searchPathSet = false;
        bool envLogged = false;
    };

    State s;
// 本模块（MCDR.dll）被加载的目录。不是 GetModuleFileNameW(nullptr)：传 NULL 返回的是**可执行
// 文件**（开发运行时是 java.exe），厂商 DLL 实际与 MCDR.dll 同处 <user.dir>\dxrmc\。
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
// 可执行文件所在目录（开发运行时是 java.exe 的 bin），与 moduleDirectoryW 分开便于诊断对照。
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
// 把 dir 加进进程 DLL 搜索路径：SetDllDirectoryW 与 AddDllDirectory 都要调（后者需要 default-dirs 模式），各自的返回值都记日志。
    void widenSearchPath(const std::wstring& dir) {
        if (dir.empty()) {
            logWarn("DLSS vendor dir empty; search path untouched");
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
        logInfo("DLSS path widen %s: default=%s(%lu) "
                "SetDllDirectoryW=%s(err=%lu) AddDllDirectory=%s(err=%lu)",
                narrow(dir).c_str(), setDefault ? "ok" : "failed", setDefaultErr,
                setDir ? "ok" : "failed", setDirErr, (cookie != nullptr) ? "ok" : "failed", addErr);
    }
// 一行回答"它到底找过哪里"。
    void logEnvOnce() {
        if (s.envLogged) {
            return;
        }
        s.envLogged = true;
        logInfo("DLSS env mod=%s cwd=%s exe=%s vendor=%s "
                "searchPathSet=%d",
                narrow(moduleDirectoryW()).c_str(), narrow(currentDirectoryW()).c_str(),
                narrow(executableDirectoryW()).c_str(), narrow(s.vendorDir).c_str(),
                s.searchPathSet ? 1 : 0);
    }
// 按序尝试每个可能位置，每次失败各自记 GetLastError；全链失败后返回 NULL。
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
        // 1) 裸名字：标准搜索顺序（放宽路径后厂商目录就在其中）。
        candidates.push_back({s.searchPathSet ? L"search-path(name)" : L"bare-name", name, false, 0});
        // 2) JVM 声明的目录（== MCDR.dll 被解出的地方）。
        if (!s.vendorDir.empty()) {
            candidates.push_back({L"vendor-dir", s.vendorDir + L"\\" + name, false, 0});
        }
        // 3) MCDR.dll 自己的目录：extractLibrary() 把 DLL 与厂商库一起写进 <user.dir>\dxrmc\，运行期必然在旁边。
        if (!modDir.empty()) {
            candidates.push_back({L"module-dir", modDir + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        // 4) <cwd>\dxrmc\：模块从别处加载时的兜底。
        if (!cwd.empty()) {
            candidates.push_back({L"cwd-dxrmc", cwd + L"\\dxrmc\\" + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        // 5) <exeDir>\dxrmc\：非开发部署同理。
        if (!exeDir.empty()) {
            candidates.push_back({L"exeDir-dxrmc", exeDir + L"dxrmc\\" + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        // 6) 当前工作目录。
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
            logWarn("nvngx load %llu/%llu (%s) failed: %s err=%lu (%s)",
                    static_cast<unsigned long long>(i + 1),
                    static_cast<unsigned long long>(candidates.size()), c.what,
                    narrow(c.useEx ? c.path : std::wstring(name)).c_str(), err,
                    err == ERROR_MOD_NOT_FOUND ? "ERROR_MOD_NOT_FOUND"
                                               : (err == ERROR_FILE_NOT_FOUND ? "ERROR_FILE_NOT_FOUND"
                                                                              : "see winerror.h"));
        }
        if (found == nullptr) {
            logWarn("nvngx_dlss.dll not in any candidate"
                    "itself unavailable. Note: nvngx_dlss.dll imports only system DLLs, so "
                    "ERROR_MOD_NOT_FOUND here points at the path, not at a missing dependency.");
        }
        return found;
    }
// PathListInfo 必须指向厂商目录：NGX 默认只在**应用目录**里找 feature DLL（开发运行时是 java.exe
// 的 bin），路径表为空时 Init_with_ProjectID 与 GetFeatureRequirements 都回
// FAIL_InvalidParameter(0xBAD00005)。
// 这些缓冲由本单元持有并活到进程结束 —— NGX 保留的是指针（PathListInfo.Path 是
// `wchar_t const* const*`）。
    NVSDK_NGX_FeatureCommonInfo* gCommonInfo = nullptr;
    std::wstring* gPathListEntry = nullptr;    // the one wide string the list points at
    const wchar_t** gPathListArray = nullptr;  // the one-element array of path pointers
// （重）建 common info，让 PathListInfo 指向 dir（dir 为空 => Length 0）。
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
// 交给 NGX 与 RTX 探测的指针；路径表为空时为 NULL。
    NVSDK_NGX_FeatureCommonInfo* commonInfoForNgx() {
        if (gCommonInfo == nullptr || gPathListEntry == nullptr ||
            gCommonInfo->PathListInfo.Length == 0u) {
            return nullptr;
        }
        return gCommonInfo;
    }
// 从帧描述推导 DLSS feature flags：
//   AutoExposure 看环境变量（默认关）；没有 exposure 纹理，声明自动曝光就让 NGX 自己合成。
//   DepthInverted 常开 —— 引擎是 reverse-Z。
//   MVLowRes 只在 render != target 时置位：速度 pass 跑在 render 分辨率上。
//   MVJittered **不置位** —— MV 是真实运动、不含 jitter 差，置位会让 NGX 去减一个不存在的量。
//   IsHDR 关 —— 8 位 UNORM 目标。
// 取消 jitter 只靠 InJitterOffsetX/Y，本版 SDK 没有额外的"MV jitter cancellation"参数。
// render preset（DLSS 模型）只给正在构建的那一档写 hint key，写全部六个只会留下脏值。
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
            // Ultra Quality 在枚举里但没有自己的 hint key，落到 default（不写 = NGX 自选）。
            case NVSDK_NGX_PerfQuality_Value_UltraQuality:
            default:
                return nullptr;
        }
    }
// preset 的可读名（Default / J / K / L / M，保留字母打原值）。
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
        // AutoExposure 由 MCDR_DLSS_AUTOEXPOSURE=1 显式开启（默认关）：声明了却不给 exposure 纹理会移除设备。
        int32_t flags = 0;
        {
            char ae[8] = {};
            if (::GetEnvironmentVariableA("MCDR_DLSS_AUTOEXPOSURE", ae, sizeof(ae)) > 0 &&
                ae[0] == '1') {
                flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
            }
        }
        if (in.depthInverted != 0) {
            flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_DepthInverted);
        }
        if (in.renderWidth != in.targetWidth || in.renderHeight != in.targetHeight) {
            flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
        }
        // MV 是真实运动、不含 jitter 差 => **不置** MVJittered（否则 NGX 会去减一个不存在的 jitter 差）。
        return flags;
    }
    // 资源转换一律走设备模块的跟踪式 transition，绝不在这里手写 barrier：endRenderPass 每趟都会把
    // 附件还原到 COMMON，所以调用方假设的 Before 状态是错的（release 下静默，调试层报 mismatch，
    // 后果是状态损坏 / TDR 冻结）。transitionTextureTo 幂等且更便宜。
    // transitionFn/transitionCtx 由模块逐调用注入；缺席（没开命令列表的诊断路径）就跳过转换。
    void transition(ID3D12Resource* res, D3D12_RESOURCE_STATES after) {
        if (res == nullptr) {
            return;
        }
        if (s.transitionFn == nullptr) {
            return;
        }
        s.transitionFn(s.transitionCtx, reinterpret_cast<void*>(res), static_cast<int32_t>(after));
    }
// 本单元**唯一**的纹理创建入口；失败返回 false，调用方按"没有私有纹理"处理。
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
                logWarn("no texture-create entry; DLSS uses JVM textures only"
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
// 销毁（幂等）；ctx 可为 nullptr，仅用于从该 ctx 的跟踪表摘键。
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
            logWarn("DLSS scratch failed %llu bytes hr=0x%08X; disabled",
                    static_cast<unsigned long long>(bytes), static_cast<unsigned>(hr));
            return false;
        }
        s.scratchBytes = bytes;
        return true;
    }
// DLSS 写进的私有 UAV 纹理；只在目标尺寸或格式变化时重建，失败返回 nullptr。
// 纹理由**设备模块**创建（DlssTextureDesc）：跟踪表以 COMMON 起锚，释放走模块的延迟删除，
// 绝不在这里 Release()。本单元只留不透明句柄 + 交给 NGX 的裸指针。
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
        // 只要 UAV。曾同时带 ALLOW_RENDER_TARGET，结果 GPU 静默不写入（读回全 0）—— UAV|RTV 正是
        // 驱动拒绝无序访问路径却仍报成功的组合。
        void* obj = nullptr;
        ID3D12Resource* res = nullptr;
        if (!createTrackedTexture(s.transitionCtx, width, height, format, 1, 1, 1,
                                  static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), &obj,
                                  &res)) {
            logWarn("private UAV output create failed (%ux%u fmt=%d)"
                    "module refused or failed to create it; the existing path stays in effect",
                    width, height, static_cast<int>(format));
            return nullptr;
        }
        s.dlssOutputObject = obj;
        s.dlssOutput = res;
        s.dlssOutputWidth = width;
        s.dlssOutputHeight = height;
        s.dlssOutputFormat = format;
        logInfo("private UAV output %ux%u fmt=%d (UAV, tracked)"
                "(ALLOW_UNORDERED_ACCESS, tracked) — DLSS writes here, then the result is copied "
                "into the JVM's target",
                width, height, static_cast<int>(format));
        return s.dlssOutput;
    }
// NGX 自己的日志接进 native 日志，Init 失败时能自解释。
    void NVSDK_CONV dlssNgxLogCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel,
                                       NVSDK_NGX_Feature sourceComponent) {
        (void)loggingLevel;
        if (message != nullptr) {
            logInfo("NGX[%d] %s", static_cast<int>(sourceComponent), message);
        }
    }
// NGX 的 applicationDataPath：目录不存在会被回 FAIL_InvalidParameter。优先用本 DLL 所在目录
// （必然存在、且厂商库就在旁边），其次按需创建 .\ngx，最后 "."。
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
                logWarn("no NGX appdata dir %s (err=%lu); using cwd"
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
// 默认走 Streamline：MCDR_DLSS_IMPL 显式指定 > MCDR_STREAMLINE=1 > 按 sl.interposer.dll 是否与 MCDR.dll 同目录判定。
    bool dlssImplStreamline() {
        // 实现选择不能缓存成一次判定：sl.interposer.dll 是 JVM 启动时才解出的，early 判定会把 false
        // 永久锁死（之后每帧静默返回 0，表现是"DLSS 没出图、只剩抖动"）。环境变量每次读；文件探测
        // 只在还没看到时重试，见到才锁存。
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
// 运行时解析 slDLSSSetOptions（不引入静态导入；缺 SL 时返回 nullptr）。
    PFn_SlDLSSSetOptions slDlssSetOptionsFn() {
        static const PFn_SlDLSSSetOptions fn = []() -> PFn_SlDLSSSetOptions {
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
// 把原生 DXGI 工厂原地升级成 SL 代理工厂（唯一正确入口）；未走 SL 时返回 0。
    int32_t streamlineUpgradeInterface(void** baseInterface) {
        if (baseInterface == nullptr || *baseInterface == nullptr) return 0;
        // PFun_slUpgradeInterface 是函数类型别名，要用指针。
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
            *baseInterface = native;  // 失败时把原生指针原样放回去
            return 0;
        }
        // replace in place：成功时调用方那一个引用直接变成代理的引用（不 AddRef/Release）。
        *baseInterface = upgraded;
        return upgraded != native ? 1 : 0;
    }
// Streamline 路径的档位解析（定义在 chooseQuality 之后）。
    uint32_t resolveQualityValue(const mcdr_dlss_input& in);
// 把本帧几何 + 质量档写进 SL 的 DLSS 选项；失败只告警。
    int32_t streamlineApplyOptions(const mcdr_dlss_input& in) {
        const PFn_SlDLSSSetOptions fn = slDlssSetOptionsFn();
        if (fn == nullptr) return -1;
        sl::DLSSOptions opt{};
        switch (s.qualityValue) {
            case NVSDK_NGX_PerfQuality_Value_DLAA:       opt.mode = sl::DLSSMode::eDLAA; break;
            case NVSDK_NGX_PerfQuality_Value_MaxQuality: opt.mode = sl::DLSSMode::eMaxQuality; break;
            case NVSDK_NGX_PerfQuality_Value_Balanced:   opt.mode = sl::DLSSMode::eBalanced; break;
            // 极致性能 = 3x：漏进 default 会按 MaxPerf(2x) 规划特征而喂 3x 输入 => 评价全黑（只有 GUI 正常）。
            case NVSDK_NGX_PerfQuality_Value_UltraPerformance: opt.mode = sl::DLSSMode::eUltraPerformance; break;
            default:                                     opt.mode = sl::DLSSMode::eMaxPerformance; break;
        }
        opt.outputWidth = static_cast<uint32_t>(in.targetWidth);
        opt.outputHeight = static_cast<uint32_t>(in.targetHeight);
        opt.preExposure = in.preExposure > 0.0f ? in.preExposure : 1.0f;
        opt.exposureScale = in.exposureScale > 0.0f ? in.exposureScale : 1.0f;
        opt.useAutoExposure = sl::Boolean::eFalse;
        // 选项只在真的变化时下发：Streamline 每次都重建内部 DLSS 资源，实测 ~800 MB/min 私有内存增长。
        const int32_t modeAsInt = static_cast<int32_t>(opt.mode);
        if (s.slOptsApplied && s.slOptMode == modeAsInt && s.slOptOutW == opt.outputWidth &&
            s.slOptOutH == opt.outputHeight && s.slOptPreExposure == opt.preExposure &&
            s.slOptExposureScale == opt.exposureScale) {
            return 0;
        }
        const sl::ViewportHandle viewport(0);
        const sl::Result r = fn(viewport, opt);
        if (r != sl::Result::eOk) {
            logWarn("slDLSSSetOptions failed 0x%X mode=%d out=%ux%u"
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
        logInfo("slDLSSSetOptions ok mode=%d out=%ux%u preExp=%.3f"
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
            // SL 拥有 NGX：这里绝不能调 NVSDK_NGX_D3D12_Init_*（同进程双 NGX 冲突，0xBAD00005）。
            logInfo("DLSS impl=streamline — native NGX init skipped "
                    "(Streamline owns the runtime; evaluate lands in phase 2)");
            s.failure = "streamline mode: native NGX init skipped by design";
            return false;
        }
// 固定用 SDK 常量 NVSDK_NGX_Version_API(0x15)，不做版本扫描：同代运行库上 0x15 能成功
// Init，0x16+ 只报 out of date。0x15 下的 0xBAD00005 是参数问题，靠能力诊断指名。
        const std::wstring appDataPath = buildAppDataPath();
        logInfo("NGX init 0x%X project=%s "
                "engineType=%d, engineVersion=\"%s\", appDataPath=\"%s\", featureInfo=%s)",
                static_cast<unsigned>(NVSDK_NGX_Version_API), MCDR_NGX_PROJECT_ID,
                static_cast<int>(NVSDK_NGX_ENGINE_TYPE_CUSTOM), MCDR_NGX_ENGINE_VERSION,
                narrow(appDataPath).c_str(), commonInfoForNgx() != nullptr ? "pathlist" : "null");
        // 参数表逐字对齐 nvsdk_ngx.h:247；**必须写在一行**（跨行 MSVC 报 C2065）。
        const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(MCDR_NGX_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, MCDR_NGX_ENGINE_VERSION, appDataPath.c_str(), device, commonInfoForNgx(), NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(r)) {
            s.failure = "NVSDK_NGX_D3D12_Init_with_ProjectID";
            logWarn("DLSS unavailable: NGX init failed %s "
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
// 能力诊断：这四个参数把 0xBAD00005 变成有名有姓的原因（FeatureInitResult / NeedsUpdatedDriver / MinDriverVersion）。
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
            logInfo("NGX caps avail=%d(%s) drv=%d(%s) "
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

        logInfo("NGX ready project=%s engine=%s"
                "\".\\ngx\", SDK API version 0x%08X)",
                MCDR_NGX_PROJECT_ID, MCDR_NGX_ENGINE_VERSION,
                static_cast<unsigned>(NVSDK_NGX_Version_API));
        // 一次性最优设置探测：世界还没加载也能把 SDK 的比例表写进日志。
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
    // 把 render/target 比例映射到最接近的 PerfQuality_Value。渲染比例是独立设置的（25% 步进），
    // 所以"用哪档 DLSS"不是用户选项，而是能复现该分辨率的那一档 —— 顺带避免 DLSS 二次缩放。
    // forceQuality >= 0 时跳过比例选择，只取 SDK 对该档的最优设置（尤其是锐度）。
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
        // forceQuality 是 kValues[] 的下标，先 clamp 保证索引在范围内。
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
            // 日志必须能区分"用户选的档"与"按比例推的档"。
            logInfo("DLSS tier %s sdk %ux%u "
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
// 档位解析抽成两条路径共用。Streamline 模式下 NGX 由 SL 拥有、s.initOk 恒 false，create 会在
// ensureFeature 之前返回 —— 档位解析从未执行，qualityValue 停在 0（= MaxPerf/2x），而 UI 说
// "极致性能"、输入是 3x ⇒ 每帧 dynamic scaling error + 空纹理 = 黑屏。
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
            // 无显式档位时按实际比例挑最接近的一档（Streamline 路径拿不到 NGX 参数表，用名义倍率表）。
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
                        logError("tier %d %.2fx cannot fit %dx%d -> %dx%d %.2fx; tier %d",
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
                    logError("render %ux%u < tier %d input %ux%u (%.2fx); raise scale",
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

    void destroyFeature() {
        if (s.feature != nullptr) {
            const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_ReleaseFeature(s.feature);
            if (NVSDK_NGX_FAILED(r)) {
                logWarn("NVSDK_NGX_D3D12_ReleaseFeature failed: %s",
                        ngxResultString(r).c_str());
            }
            s.feature = nullptr;
        }
        s.featureOk = false;
        s.evalOk = false;
        // feature 已销毁，下次 create 必须重推选项。
        s.slOptsApplied = false;
        s.slEvalOptsValid = false;
    }
// 创建/重建 feature。要求命令列表已打开 —— NGX 在 CreateFeature 里录自己的命令。
    bool ensureFeature(ID3D12GraphicsCommandList* cmd, const mcdr_dlss_input& in) {
        if (s.createdWarned) {
            return false;  // a previous create failed: one warning, never retried
        }
        const bool sizeChanged =
            (in.renderWidth != s.renderWidth) || (in.renderHeight != s.renderHeight) ||
            (in.targetWidth != s.targetWidth) || (in.targetHeight != s.targetHeight);
        // 档位变化必须重建：PerfQualityValue 是 CreateFeature 输入，改 Evaluate 参数没
        // 用。比较对象必须是**本帧会构建的那一档**（含 DLAA 1:1 限制）与 feature 实际建成的档；
        // 绝不能拿 requestedQuality 比 NGX 枚举（DLAA 在 NGX 是 5、本项目是 4，恒不等 => 每帧重建），
        // effectiveQuality 存被接受的档，降级因此成为基线。
        int resolvedTier = s.requestedQuality;  // -1 = no explicit tier: ratio-chosen
        const bool dlaaMismatch =
            (resolvedTier == 4) && (in.renderWidth != in.targetWidth ||
                                    in.renderHeight != in.targetHeight);
        if (dlaaMismatch) {
            resolvedTier = 2;
        }
        if (dlaaMismatch != s.dlaaDowngraded) {
            s.dlaaDowngraded = dlaaMismatch;
            if (dlaaMismatch) {
                // 绝不静默建"假 DLAA"：render != target 时降级为 Quality 并 ERROR 一次，不逐帧报。
                logError("DLAA needs 1:1 (%dx%d vs %dx%d); Quality, set scale 100%%"
                         "back to the Quality tier for this round (accepted as the baseline). Set "
                         "the render scale to 100%% to use DLAA.",
                         in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight);
            } else if (s.requestedQuality == 3) {
                logInfo("DLAA (tier 3) accepted: render %dx%d == target %dx%d",
                        in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight);
            }
        }
        // 显式档位与实际比例差太远（>10%）时改判到能覆盖它的最紧一档；10% 门槛保证只在明显错配时动手。
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
                    logWarn("tier %d %.2fx != %dx%d -> %dx%d "
                            "(%.2fx); asking NGX for tier %d instead",
                            resolvedTier, nominal, in.renderWidth, in.renderHeight, in.targetWidth,
                            in.targetHeight, factor, adopted);
                    resolvedTier = adopted;
                }
            }
        }
        // render 比该档名义输入还小时 NGX 只写一行 dynamic scaling error 再交空纹理（世界全黑、GUI 正常）。
        if (in.renderWidth > 0 && in.targetWidth > 0 && resolvedTier >= 0 && resolvedTier <= 4) {
            const double nominal = kTierNominalFactor[resolvedTier];
            const uint32_t needW = static_cast<uint32_t>(std::ceil(static_cast<double>(in.targetWidth) / nominal));
            const uint32_t needH = static_cast<uint32_t>(std::ceil(static_cast<double>(in.targetHeight) / nominal));
            // 0.5% 容差：实测差 1.1% 被拒并黑屏，差 0.34% 一正常，阈值落在两者之间。
            const auto tooSmall = [](uint32_t have, uint32_t need) {
                return static_cast<double>(have) < static_cast<double>(need) * 0.995;
            };
            if (tooSmall(static_cast<uint32_t>(in.renderWidth), needW) ||
                tooSmall(static_cast<uint32_t>(in.renderHeight), needH)) {
                if (!s.tooSmallWarned) {
                    s.tooSmallWarned = true;
                    logError("render %ux%u < tier %d input %ux%u (%.2fx)"
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
        // render preset 也是 CreateFeature 输入（网络选择被烘进 feature），变化必须像换档一样重建。
        const bool presetChanged = (s.requestedPreset != s.effectivePreset);
        if (s.featureOk && !sizeChanged && !qualityChanged && !presetChanged) {
            return true;
        }
        if (sizeChanged || qualityChanged || presetChanged) {
            destroyFeature();
            // 尺寸变化使历史失效；重建的 feature 本来也没有历史，两种情况都重置。
            s.resetPending = true;
        }
        if (s.params == nullptr) {
            return false;
        }

        if (resolvedTier >= 0) {
            // JVM 显式档位，顺序与 Java 枚举一致：0 -> MaxPerf、1 -> Balanced、2 -> MaxQuality、
            // 3 -> DLAA（1:1，复用整条管线，只是不放大）、4 -> DLAA（Ultra Performance 插到列表
            // 首位后整体后移）。这张表必须与 JVM 的 QUALITY_NAMES 顺序一致，不是 NGX 枚举顺序。
            // DLAA 用真实的亚像素 jitter：投影 UBO 带与 TAA 相同的 Halton 序列，InJitterOffsetX/Y
            // 是渲染像素偏移，MV 不含 jitter 差所以不置 MVJittered。
            
static const uint32_t kTierToNgx[5] = {
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_UltraPerformance),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxPerf),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_Balanced),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA),
            };
            // resolvedTier 已是 0..4 且 DLAA 的 1:1 限制已在上面生效 —— 下面记住的是**被接受**的那一档。
            const int tier = resolvedTier;
            if (tier < 0 || tier > 4) {
                return false;
            }
            s.qualityValue = kTierToNgx[tier];
            s.qualityIsExplicit = true;
            // 最优设置查询同时给出该质量/尺寸下 SDK 建议的锐度，沿用它。
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
// 尺寸依赖的**每一个**输入都必须在查询前写进参数表（Width/Height/OutWidth/OutHeight 不够）：
// 只写这四个时调用回 Success 但 scratch 大小为 0。create flags 与 perf quality 属于同一
// 份描述，也在这里写。
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
        // preset 必须与 PerfQualityValue 同表、且在 CREATE_DLSS_EXT 之前（创建时烘进 feature）。
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
        // 报 0 不是错误（新 DLSS 把 scratch 放在内部显存）；仍按保守下限分配，免得真用缓冲的版本越界。
        if (scratch == 0) {
            logInfo("NGX scratch size 0; using 64 MiB floor"
                    "internally); allocating a 64 MiB floor regardless");
            scratch = 64ull * 1024ull * 1024ull;
        }
        if (!createScratchBuffer(static_cast<UINT64>(scratch))) {
            s.createdWarned = true;
            s.failure = "scratch buffer creation";
            return false;
        }

        NVSDK_NGX_DLSS_Create_Params cp;
        // DLAA = 不放大：feature 一律按 target 尺寸创建。请求档路径已拒绝 render != target，这里是等尺寸不变量的兜底。
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
            logWarn("CreateFeature %s (%dx%d -> %dx%d, "
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
        // 打印每一个 flag 位，不只三位：曾因 AutoExposure/MVLowRes 都是 0 且不可见而查不出问题。
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
        logInfo("DLSS %dx%d->%dx%d q=%u %s%s%s "
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
        // 记下本次 feature 用的 preset 与 hint key（原始数字也打：保留字母映射的数字没有可读名）。
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
        logInfo("dlss %dx%d->%dx%d inv=%u z=%.3f/%.3f "
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
int32_t mcdr_dlss_streamline_requested(void) { return dlssImplStreamline() ? 1 : 0; }
// -1 = 未探测，0 = 探测到未加载，1 = 已加载；只在明确探测到未加载时报不可用，避免误判非 Streamline 的 NGX 路径。
namespace { std::atomic<int32_t> gSlDlssLoaded{-1}; }
void mcdr_dlss_note_sl_loaded(int32_t loaded) { gSlDlssLoaded.store(loaded ? 1 : 0); }
int32_t mcdr_dlss_available(void) { return gSlDlssLoaded.load() == 0 ? 0 : 1; }
// 设备模块用的 C 接口：字符串一律走 (char*, cap) 缓冲 —— std::string 过不了模块边界。
extern "C" {
// p 是否指向已提交可读内存？四个纹理句柄在任何 vtable 调用前都要过这一关（见 evaluate 里的崩溃）。
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
// 注入模块的私有纹理创建/销毁入口；1 = 都装上，0 = 缺一个（此时本单元不创建任何私有纹理）。
int32_t mcdr_dlss_install_texture_ops(void* createFn, void* destroyFn) {
    gCreateTexture = reinterpret_cast<CreateTextureFn>(createFn);
    gDestroyTexture = reinterpret_cast<DestroyTextureFn>(destroyFn);
    if (gCreateTexture == nullptr || gDestroyTexture == nullptr) {
        logWarn("no texture create/destroy entry; JVM resources only"
                "destroy=%p) — DLSS keeps using only JVM-supplied resources",
                createFn, destroyFn);
        return 0;
    }
    logInfo("private-texture ops installed; UAV and depth tracked"
            "the depth snapshot are now created by the device module, so their resource states stay "
            "inside its tracked map",
            createFn, destroyFn);
    return 1;
}
// RTX 探测加载完 nvngx_dlss.dll 后调用 —— 那条路径是 Init 失败时最有用的诊断。
void mcdr_dlss_register_runtime_path(const char* path, int32_t cap) {
    if (path == nullptr || cap <= 0) {
        return;
    }
    s.runtimePath = path;
}
// JVM 声明厂商目录：既放宽进程 DLL 搜索路径，也给下面的加载器一个候选。幂等。
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
    // 告诉 NGX 去哪里找 feature DLL —— 这是 Init/GetFeatureRequirements 从 FAIL_InvalidParameter 变 Success 的关键。
    rebuildCommonInfo(wide);
    logInfo("NGX feature path list \"%s\""
            "instead of the application folder, which is the JVM's bin\\ in a dev run)",
            narrow(wide).c_str());
    logEnvOnce();
    return s.searchPathSet ? 1 : 0;
}
// 加载 nvngx_dlss.dll 并缓存 HMODULE（探测与 NGX 都用它），失败返回 NULL。
// 绝不再从记录的路径字符串反推句柄：路径过期时会拿到非 NULL 的垃圾句柄，探测于是以为运行期
// 可用，自己的兜底再也不跑。
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
// 探测报上来的 nvngx_dlss.dll 路径（未找到前为空串，从不 NULL）。
const char* mcdr_dlss_runtime_path(void) { return s.runtimePath.c_str(); }
// PathListInfo 指向厂商目录的 common info，供 RTX 探测自己调 GetFeatureRequirements；无厂商目录时为 NULL。
void* mcdr_dlss_ngx_feature_info(void) { return commonInfoForNgx(); }

int32_t mcdr_dlss_can_run(void* device) {
    if (device == nullptr) {
        return 0;
    }
    // 先确保运行期 DLL 真加载过（探测可能没跑或失败过）：没有它 Init 只会报 ERROR_MOD_NOT_FOUND，日志看不出是"没加载"还是"不支持"。
    logEnvOnce();
    if (s.runtimePath.empty()) {
        if (loadRuntimeDll() == nullptr) {
            return 0;
        }
    }
#if MCDR_DLSS_HAVE_STREAMLINE
    if (dlssImplStreamline()) {
        // SL 拥有 NGX：既不加载也不初始化 nvngx，对 JVM 报可用，让 CreateDlssFeature 去下发选项。
        return 1;
    }
#endif
    return ensureInit(static_cast<ID3D12Device*>(device)) ? 1 : 0;
}
// flagsOut 位：1 INIT_TRIED 2 INIT_OK 4 PARAMS_OK 8 OPTIMAL_OK 16 FEATURE_OK 32 DISABLED
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
        if (s.disabled) {
            std::snprintf(note, static_cast<std::size_t>(noteCap), "DLSS disabled, %s failed",
                          s.failure.empty() ? "an earlier stage" : s.failure.c_str());
        } else if (!s.initAttempted) {
            std::snprintf(note, static_cast<std::size_t>(noteCap), "DLSS not initialised yet");
        } else if (dlssImplStreamline() && !s.initOk) {
            // Streamline 模式下 NGX 归 SL 所有，initOk 恒 false：状态行必须说清"谁在拥有 NGX"，不能一律报 not initialised yet。
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
            // 报档位及其来源（用户选的还是按比例推的），UI 与日志才能对照。
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
    if (s.disabled) {
        return -1;
    }
    if (!s.initOk || !s.paramsOk) {
#if MCDR_DLSS_HAVE_STREAMLINE
        // streamline 模式下 ensureInit 跳过 NGX：把创建/选项交给 slDLSSSetOptions 并返回 1（返回 0 会让 JVM 跳过 evaluate）。
        if (dlssImplStreamline()) {
            // 先解析档位再下发选项，否则 qualityValue 还是初值 0（= MaxPerf）。
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
    // 为本调用装上模块提供的跟踪式转换回调。
    s.transitionFn = tl;
    s.transitionCtx = tlCtx;
    s.createdThisFrame = false;
    if (!ensureFeature(cmd, *in)) {
        return 0;
    }
    logFrameChange(*in, s.createdThisFrame);
    return 1;
}
// 深度快照。JVM 在 captureMotionVectors() 里调它 —— 此时深度缓冲还是本帧的世界深度，vanilla 的
// clearDepthTexture 会把它清成 0（reverse-Z 的"远"），到 Evaluate 时实时深度已经没了，深度感知
// 的放大因此退化成纯颜色放大。
// 传入的深度资源只在本次调用期间是 COPY_SOURCE：查模块的跟踪状态、转换、拷贝、原样还原。
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
    // 深度建在无类型格式上才能被采样，NGX 只认具体格式，故快照按具体格式建（同族拷贝合法）。
    const DXGI_FORMAT snapFormat = [](DXGI_FORMAT f) -> DXGI_FORMAT {
        switch (f) {
            case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
            case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;
            case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
            case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            default: return f;
        }
    }(dd.Format);
    // 尺寸/格式变化才重建，比较整份拷贝要求（w/h/format/mip/array/sample），否则 CopyResource 静默不落地。
    if (s.depthSnapshot == nullptr || s.depthSnapshotWidth != w || s.depthSnapshotHeight != h ||
        s.depthSnapshotFormat != snapFormat || s.depthSnapshotMips != dd.MipLevels ||
        s.depthSnapshotArray != dd.DepthOrArraySize ||
        s.depthSnapshotSamples != dd.SampleDesc.Count) {
        // B4：由设备模块创建、走延迟删除，绝不 Release；mip/array/sample 对齐源，flags 保持 NONE（只当 COPY_DEST/COPY_SOURCE/只读 SRV）。
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
                logWarn("depth snapshot create failed (%ux%u fmt=%d)"
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
        logInfo("depth snapshot %ux%u fmt=%d (tracked)"
                "(private, tracked, COPY_DEST/COPY_SOURCE)",
                w, h, static_cast<int>(dd.Format));
    }
    // 保存调用方状态 -> 拷贝 -> 复原；stateOf 为 null 时以 COMMON 为锚并还原到 COMMON。
    const int32_t before = (stateOf != nullptr)
                               ? stateOf(tlCtx, depthResource)
                               : static_cast<int32_t>(D3D12_RESOURCE_STATE_COMMON);
    // 用 CopyTextureRegion + 显式子资源 0 的 box，不用 CopyResource（后者要求尺寸/mip/array 完全一致，
    // 不一致时静默不拷贝）。快照自身状态必须**查询**得到，绝不假设。
    // 在 evaluate 时读回（input-depth-after-evaluate）才是可信的判据：更早的时点读到的只能是清空值。
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
        logInfo("depth snapshot first %ux%u fmt=%d"
                "replaces the JVM's depth at evaluate time",
                w, h, static_cast<int>(dd.Format), depthResource);
    }
    return 1;
}
// 最近一帧的 SL frame token（Reflex 的 sleep 要在 Present 前用它）。
sl::FrameToken* gLastFrameToken = nullptr;
// Reflex 低延迟 + PCL 标记 + 帧率限制器（ProgrammingGuideReflex.md §4/§5）；FG 必须开 Reflex。
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
            // Reflex 模式：0=off 1=eLowLatency 2=eLowLatencyWithBoost。
            int32_t mode = 1;
            bool resolved = false;
            bool optionsApplied = false;
            // 只有 slReflexSetOptions 真的成功过才允许 sleep / 发标记，否则 SL 每帧报 feature not yet initialized。
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
            // 只有真的取到函数才 latch：Options.load 阶段（slInit 之前）latch 住就永远解析不到。
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
        // 返回 true = 本帧选项已确认下发成功；失败不要 latch，否则之后每帧短路在无效状态。
        bool applyReflexOptions() {
            if (gReflex.setOptions == nullptr) return false;
            // 帧生成必须开 Reflex（关着 SL 直接拒绝插帧）；FG 期间用户的 OFF 按 ON 生效，FG 关掉后恢复原值。
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
                // 失败不 latch：下一帧重试；期间拦住 sleep/标记，避免 SL 每帧刷同一条错误。
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
            // Reflex/PCL 只在 setOptions 成功后才有效，否则 SL 回 feature not yet initialized。
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
// 每帧开始（世界渲染前）建 SL frame token 并发 eSimulationStart；simulation/submit/present 必须同一帧 token。
    void mcdr_reflex_frame_begin(void) {
        if (!reflexEnabled()) return;
        resolveReflexApi();
        gLastFrameToken = nullptr;
        sl::FrameToken* token = nullptr;
        uint32_t frameIndex = ++gReflex.frameCounter;
        if (gReflex.newFrameToken != nullptr &&
            gReflex.newFrameToken(token, &frameIndex) == sl::Result::eOk && token != nullptr) {
            gLastFrameToken = token;
        }
        emitMarker(sl::PCLMarker::eSimulationStart);
    }
// 通用标记（eSimulationEnd / eRenderSubmitStart 由 JVM 在正确时机调）；界面改的 Reflex 模式下一帧 Present 前重发。
    void mcdr_reflex_set_mode(int32_t mode) {
        if (!reflexEnabled()) return;
        const int32_t clamped = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
        if (clamped == gReflex.mode) return;
        gReflex.mode = clamped;
        gReflex.optionsApplied = false;
    }
// Reflex 可用 = 三个入口都解析到，供界面置灰。
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
        // JVM 的 frame-begin 没跑到时本帧还没有 token；sleep/限帧/标记缺 token 就整条链路静默失效，这里就地补一个。
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
                logInfo("reflex sleep ok mode=%d; PCL markers on",
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
    // JVM 每帧送来的相机常量（sl::Constants 的数据源）；只在渲染线程读写。
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
    using PFn_SlDLSSGGetState = sl::Result (*)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::BaseStructure**);
// FG 开关：0=off 1=on（固定倍率）2=dynamic；由 mcdr_dx12SetDlssgMode 写入，渲染线程读。
    std::atomic<int32_t> gDlssgMode{0};
    std::atomic<int32_t> gDlssgFrames{1};
    std::atomic<int32_t> gFgPresented{1};
    // 上一帧结束时输出纹理的 state（0 = 未知）；JVM 读回必须拿它当 StateBefore，按 COMMON 硬转读回未定义内容。
    std::atomic<int32_t> gDlssOutputState{0};
// Streamline 的每帧调用面（全部运行时解析，不引入静态导入）。
    struct StreamlineEvalApi {
        PFn_SlDLSSSetOptions setOptions = nullptr;
        PFn_SlDLSSGSetOptions setFgOptions = nullptr;
        PFn_SlDLSSGGetState getFgState = nullptr;
        PFn_slGetNewFrameToken newFrameToken = nullptr;
        PFn_slSetTagForFrame setTags = nullptr;
        PFn_slSetConstants setConstants = nullptr;
        PFn_slEvaluateFeature evaluate = nullptr;
    };
// DLSS-G 插件可能比 DLSS 晚加载，解析要能重试（见 refreshFgApi）。
    HMODULE gSlInterposer = nullptr;

    StreamlineEvalApi& streamlineEvalApi() {
        static StreamlineEvalApi api = []() -> StreamlineEvalApi {
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
// 4x4 行主序矩阵工具：SL 要四个矩阵，JVM 只送 viewProj 与 prevViewProj。
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
    // SL 的 DLSS 评价：tag -> options -> evaluate（eDisableCLStateTracking 下状态由我们负责）；DLSS-G 指针延迟解析、500ms 重试。
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
// 把本帧 SR 产物交给 FG provider（XeSS-FG / FSR-FG 这类包装型后端）。
// DLSS 超分走本文件的 mcdr_dlss_evaluate（普通 TU，不 import 设备模块），srEvaluateFrame 的
// 逐帧 tag 点根本不会执行，provider 因此一帧输入都收不到。这里只过 POD 的 C ABI 桥，
// 避免模块私有名的修饰名在链接期对不上。
// 状态：out 刚从 SR 输出恢复成 RENDER_TARGET，深度/MV 是只读 SRV。给错只会让 SDK 记错屏障的
// Before 值，TagFrameResource 会返回错误码。
    void tagFrameGenInputsFromDlss(ID3D12GraphicsCommandList* cmd, const mcdr_dlss_input& in) {
        if (cmd == nullptr) {
            return;
        }
        mcdr_fgTagDlssInputs(cmd, in.pInOutput, static_cast<int32_t>(D3D12_RESOURCE_STATE_RENDER_TARGET),
                             in.pInDepth,
                             static_cast<int32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                             in.pInMotionVectors,
                             static_cast<int32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    }

    int32_t streamlineEvaluate(void* commandList, const mcdr_dlss_input& in, DlssTransitionFn tl,
                               void* tlCtx) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
        if (cmd == nullptr) return 0;
        StreamlineEvalApi& api = streamlineEvalApi();
        if (gDlssgMode.load() != 0) {
            refreshFgApi(api);

        }
        auto* color = reinterpret_cast<ID3D12Resource*>(in.pInColor);
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
            return 0;
        }
        if (color == nullptr || depth == nullptr || mv == nullptr || out == nullptr) return 0;
        // 输出优先用 JVM 交付的 UAV 纹理（externalOutput）：GUI 目标没有 UAV 位（FAIL_RWFlagMissing
        // 0xBAD00009）；而且 DLSS 的 temporal history 以**自己的输出**为反馈源，直接用 GUI 目标会
        // 让历史被 GUI 污染（滑动界面时文字被当成运动物体拖影）。两者都不可用才退化成直写 GUI 目标。
        ID3D12Resource* outResource = out;
        bool copyBack = false;
        const auto fitsTarget = [&in](ID3D12Resource* res) {
            if (res == nullptr) return false;
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            return d.Width == static_cast<UINT64>(in.targetWidth) &&
                   d.Height == static_cast<UINT>(in.targetHeight);
        };
        if (fitsTarget(s.externalOutput)) {
            outResource = s.externalOutput;
            copyBack = true;
        } else if (fitsTarget(s.dlssOutput)) {
            outResource = s.dlssOutput;
            copyBack = true;
        }
        s.transitionFn = tl;
        s.transitionCtx = tlCtx;
        // 资源状态由 SL 管理，宿主只在 evaluate 后恢复；复用 frame_begin 的 token 保证 PCL 标记与 evaluate 同帧。
        sl::FrameToken* token = gLastFrameToken;
        if (token == nullptr) {
            uint32_t frameIndex = static_cast<uint32_t>(s.submitCount);
            if (api.newFrameToken(token, &frameIndex) != sl::Result::eOk || token == nullptr) return 0;
        }
        const sl::ViewportHandle viewport(0);
        gLastFrameToken = token;  // P113：Reflex 在 Present 前要用它
// 相机常量（每帧、尽量早）。没有 JVM 送来的 mcdr_dlss_constants 只能跳过 —— 缺字段 SL 会拒绝该帧。
        if (gCameraConstantsValid) {
            sl::Constants cnst{};
            // 三个传参错误，任何一个都足以让静止画面每帧取错历史：
            //   1) 布局：JVM 送列主序（JOML），sl::Constants 的 float4x4 要**行主序**；直接 memcpy 等于
            //      整组矩阵转置着交给 SL，静止时当前/上一帧只差一个 jitter，转置会把它映射到错误的
            //      clip 轴 ⇒ 每像素每帧约半个像素的错误位移 = 拖影。
            //   2) prevClipToClip 抄成 inverse(prevViewProj)，正确公式见下方。
            //   3) 求逆/相乘用的是本文件的行主序工具，所以必须先转置再算。
            const float* viewProj = gCameraConstants.viewProjection;
            const float* prevViewProj = gCameraConstants.prevViewProjection;
            float curVP[16] = {};
            float prvVP[16] = {};
            // 列主序 -> 行主序：rowMajor[r*4+c] = colMajor[c*4+r]
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    curVP[r * 4 + c] = viewProj[c * 4 + r];
                    prvVP[r * 4 + c] = prevViewProj[c * 4 + r];
                }
            }
            float invCurVP[16] = {};
            float invPrvVP[16] = {};
            const bool invOk = mat4Inverse(curVP, invCurVP);
            const bool invPrevOk = mat4Inverse(prvVP, invPrvVP);
            // cameraViewToClip = **view->clip**，即投影矩阵本身（不是 VP）；塞 VP 会让深度反投影多带一层世界平移 => 静止抖动。
            const float* projColMajor = gCameraConstants.projection;
            float curProj[16] = {};
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    curProj[r * 4 + c] = projColMajor[c * 4 + r];
                }
            }
            std::memcpy(&cnst.cameraViewToClip, curProj, sizeof(curProj));
            {
                float invProj[16] = {};
                if (mat4Inverse(curProj, invProj)) {
                    std::memcpy(&cnst.clipToCameraView, invProj, sizeof(invProj));
                }
            }
            if (invOk) {
                // clipToPrevClip = inverse(curVP) * prevVP
                float clipToPrev[16] = {};
                mat4Multiply(invCurVP, prvVP, clipToPrev);
                std::memcpy(&cnst.clipToPrevClip, clipToPrev, sizeof(clipToPrev));
                if (invPrevOk) {
                    // prevClipToClip 是上面那个的逆：inverse(prevVP) * curVP（不是 inverse(prevVP)）。
                    float prevClipToClip[16] = {};
                    mat4Multiply(invPrvVP, curVP, prevClipToClip);
                    std::memcpy(&cnst.prevClipToClip, prevClipToClip, sizeof(prevClipToClip));
                }
            }
            cnst.jitterOffset = {gCameraConstants.jitterOffsetX, gCameraConstants.jitterOffsetY};
            cnst.mvecScale = {gCameraConstants.mvecScaleX, gCameraConstants.mvecScaleY};
            cnst.cameraPos = {gCameraConstants.cameraPosX, gCameraConstants.cameraPosY,
                              gCameraConstants.cameraPosZ};
            cnst.cameraUp = {gCameraConstants.cameraUpX, gCameraConstants.cameraUpY,
                             gCameraConstants.cameraUpZ};
            cnst.cameraRight = {gCameraConstants.cameraRightX, gCameraConstants.cameraRightY,
                                gCameraConstants.cameraRightZ};
            cnst.cameraFwd = {gCameraConstants.cameraFwdX, gCameraConstants.cameraFwdY,
                              gCameraConstants.cameraFwdZ};
            cnst.cameraNear = gCameraConstants.cameraNear;
            cnst.cameraFar = gCameraConstants.cameraFar;
            cnst.cameraFOV = gCameraConstants.cameraFovRadians;
            cnst.cameraAspectRatio = gCameraConstants.cameraAspectRatio;
            cnst.depthInverted = gCameraConstants.depthInverted ? sl::Boolean::eTrue
                                                                : sl::Boolean::eFalse;
            cnst.cameraMotionIncluded = gCameraConstants.cameraMotionIncluded
                                            ? sl::Boolean::eTrue
                                            : sl::Boolean::eFalse;
            cnst.motionVectors3D =
                gCameraConstants.motionVectors3D ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            cnst.motionVectorsJittered = gCameraConstants.motionVectorsJittered
                                             ? sl::Boolean::eTrue
                                             : sl::Boolean::eFalse;
            cnst.reset = gCameraConstants.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            // 本版 SDK 的 sl::Constants 没有 renderingGameFrames 字段，那一位不用。
            cnst.motionVectorsDilated = sl::Boolean::eFalse;
            cnst.orthographicProjection = sl::Boolean::eFalse;
            (void)api.setConstants(cnst, *token, viewport);
        }
// tag：输入/输出 eOnlyValidNow，深度/MV eValidUntilPresent；只保留输出 barrier，输入侧再切与 SMAA/GUI 冲突。
        transition(outResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        sl::Resource colorRes(sl::ResourceType::eTex2d, color,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource depthRes(sl::ResourceType::eTex2d, depth,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource mvRes(sl::ResourceType::eTex2d, mv,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource outRes(sl::ResourceType::eTex2d, outResource,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // HUD-less 必须与其余 tag 同一次 slSetTagForFrame（分开会互相覆盖，SL 找不到 depth/MV）；outResource == out 会叠 UI，不开 FG。
        const int32_t fgModeEarly = gDlssgMode.load();
        const bool wantFg = (fgModeEarly != 0) && (outResource != out) && (api.setFgOptions != nullptr);
        sl::Resource hudlessRes(sl::ResourceType::eTex2d, outResource,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        sl::ResourceTag tags[5] = {
            sl::ResourceTag(&colorRes, sl::kBufferTypeScalingInputColor,
                            sl::ResourceLifecycle::eOnlyValidNow),
            sl::ResourceTag(&depthRes, sl::kBufferTypeDepth,
                            sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(&mvRes, sl::kBufferTypeMotionVectors,
                            sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(&outRes, sl::kBufferTypeScalingOutputColor,
                            sl::ResourceLifecycle::eOnlyValidNow),
            sl::ResourceTag(&hudlessRes, sl::kBufferTypeHUDLessColor,
                            sl::ResourceLifecycle::eValidUntilPresent),
        };
        const uint32_t tagCount = wantFg ? 5u : 4u;
        const sl::Result tagResult = api.setTags(*token, viewport, tags, tagCount, cmd);
        if (tagResult != sl::Result::eOk) {
            static bool warnedTag = false;
            if (!warnedTag) {
                warnedTag = true;
                logWarn("slSetTagForFrame failed 0x%X frame=%u",
                        static_cast<unsigned>(tagResult), s.submitCount,
                        static_cast<void*>(color), static_cast<void*>(depth),
                        static_cast<void*>(mv), static_cast<void*>(outResource));
            }
        }
        sl::DLSSOptions opt{};
        opt.dlaaPreset = sl::DLSSPreset::ePresetK;
        opt.qualityPreset = sl::DLSSPreset::ePresetK;
        opt.balancedPreset = sl::DLSSPreset::ePresetK;
        opt.performancePreset = sl::DLSSPreset::ePresetM;
        opt.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
        switch (s.qualityValue) {
            case NVSDK_NGX_PerfQuality_Value_DLAA:       opt.mode = sl::DLSSMode::eDLAA; break;
            case NVSDK_NGX_PerfQuality_Value_MaxQuality: opt.mode = sl::DLSSMode::eMaxQuality; break;
            case NVSDK_NGX_PerfQuality_Value_Balanced:   opt.mode = sl::DLSSMode::eBalanced; break;
            // 极致性能 = 3x：漏进 default 会被当成 MaxPerformance(2x)，按 2x 规划特征却喂 3x 输入 => 全黑。
            case NVSDK_NGX_PerfQuality_Value_UltraPerformance: opt.mode = sl::DLSSMode::eUltraPerformance; break;
            default:                                     opt.mode = sl::DLSSMode::eMaxPerformance; break;
        }
        opt.outputWidth = static_cast<uint32_t>(in.targetWidth);
        opt.outputHeight = static_cast<uint32_t>(in.targetHeight);
        opt.preExposure = in.preExposure > 0.0f ? in.preExposure : 1.0f;
        opt.exposureScale = in.exposureScale > 0.0f ? in.exposureScale : 1.0f;
        opt.useAutoExposure = sl::Boolean::eFalse;
        // HDR 开启时 colorBuffersHDR 必须 eTrue：内部主目标仍是 SDR，线性化在 blit PS 里，这里只让 SL 按 HDR 语义处理。
        opt.colorBuffersHDR =
            mcdr_hdr_state_active() != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        opt.alphaUpscalingEnabled = sl::Boolean::eFalse;
        // slDLSSSetOptions 只在配置变化时下发：每帧调用 SL 会重建内部资源（实测 ~750 MB/min 私有内存）。
        const bool slOptsChanged = !s.slEvalOptsValid || s.slEvalOptMode != static_cast<int32_t>(opt.mode) ||
                                   s.slEvalOptOutW != opt.outputWidth ||
                                   s.slEvalOptOutH != opt.outputHeight ||
                                   s.slEvalOptPreExposure != opt.preExposure ||
                                   s.slEvalOptExposureScale != opt.exposureScale ||
                                   s.slEvalOptQuality != s.qualityValue ||
                                   s.slEvalOptHdr != (mcdr_hdr_state_active() != 0);
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
                s.slEvalOptHdr = mcdr_hdr_state_active() != 0;
            }
        }
        const sl::BaseStructure* evalInputs[1] = {&viewport};
        sl::Result evalResult = sl::Result::eOk;
        evalResult = api.evaluate(sl::kFeatureDLSS, *token, evalInputs, 1, cmd);
        ++s.submitCount;
// handover：只有 SL 写的那张纹理不是 GUI 目标时才需要拷回（同尺寸、同格式）。
        if (copyBack && outResource != out && evalResult == sl::Result::eOk) {
            transition(outResource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(out, outResource);
        }
        // 宿主负责恢复：只把输出恢复成 RT —— 输入侧没改状态，也没有要恢复的 barrier。
        transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // SR 已跑完、输出已在 RENDER_TARGET —— 此刻把本帧输入交给 FG provider。
        tagFrameGenInputsFromDlss(cmd, in);
// 帧生成（DLSS-G）：
// - HUD-less 必须是"UI 之前、与 backbuffer 同尺寸"的画面（ProgrammingGuideDLSS_G.md §5.0）：
//   只有 SR 的输出满足（此后只有 GUI 合成会写 out）。outResource == out 那张会叠上 UI，不开 FG。
// - §5.2：tag 的资源在 Present 时才消费，用 eValidUntilPresent。
// - §6.0：slDLSSSetOptions 在下一次 Present 生效 —— 本函数就在渲染线程，紧随其后的那次就是目标。
        const int32_t fgMode = fgModeEarly;
        // appliedFgMode 必须两分支共用：否则关 FG 的分支只能用一次性闩，开过再关就再也不发 eOff。
        static int32_t appliedFgMode = -1;
        static int32_t appliedFgFrames = -1;
        if (wantFg) {
            // 上一步的拷贝把 outResource 留在 COPY_SOURCE；tag 声明的是 UNORDERED_ACCESS，Present 前必须切回去。
            transition(outResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
// 选项只在状态变了时下发（每帧调拖慢 Present 线程）；Resize 后必须重发 options，用输出尺寸当代理。
            static uint32_t appliedFgW = 0;
            static uint32_t appliedFgH = 0;
            // 动态多帧：不用 SL 的 eDynamic（它无视 Reflex 限帧），自己按"上帧时长 : 帧限"算帧数并用 eOn + 显式 numFramesToGenerate。
            const int32_t maxFrames = std::clamp<int32_t>(gDlssgFrames.load(), 1, 5);
            static uint64_t lastFgQpc = 0;
            static int32_t dynFrames = 1;
            static int32_t dynTick = 0;
            int32_t wantFrames = maxFrames;
            if (fgMode == 2) {
                LARGE_INTEGER qpc{};
                LARGE_INTEGER freq{};
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&qpc);
                const uint64_t nowUs = static_cast<uint64_t>(qpc.QuadPart) * 1000000ull /
                                       static_cast<uint64_t>(freq.QuadPart);
                if (lastFgQpc != 0 && nowUs > lastFgQpc) {
                    const uint32_t renderUs = static_cast<uint32_t>(nowUs - lastFgQpc);
                    const uint32_t targetUs = gReflex.frameLimitUs;  // 0 = 不限帧
                    if (targetUs > 0 && renderUs > 0) {
                        int32_t need = static_cast<int32_t>(targetUs / renderUs) - 1;
                        need = std::clamp(need, 1, maxFrames);
                        if (need > dynFrames) {
                            dynFrames += 1;
                        } else if (need < dynFrames) {
                            dynFrames -= 1;
                        }
                        dynFrames = std::clamp(dynFrames, 1, maxFrames);
                    } else {
                        dynFrames = 1;
                    }
                }
                lastFgQpc = nowUs;
                wantFrames = dynFrames;
                if (++dynTick % 120 == 0) {
                    logWarn("fg dynamic: frames=%d max=%d target=%uus",
                            wantFrames, maxFrames, gReflex.frameLimitUs);
                }
            }
            // DYNAMIC 且没有 Reflex 限帧时交回 SL 的 eDynamic（它知道刷新率，会自己追）；有限帧时才用显式帧数。
            const bool fgDynamic = (fgMode == 2) && (gReflex.frameLimitUs == 0);
            // 哨兵 -1 = 帧数不由我们决定：必须存哨兵，否则每帧都判不等 => 每帧一次 slDLSSGSetOptions。
            const int32_t appliedFramesWanted = fgDynamic ? -1 : wantFrames;
            if (appliedFgMode != fgMode || appliedFgFrames != appliedFramesWanted ||
                appliedFgW != in.targetWidth || appliedFgH != in.targetHeight) {
                sl::DLSSGOptions fg{};
                fg.mode = fgDynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
                fg.numFramesToGenerate = fgDynamic ? 1u : static_cast<uint32_t>(wantFrames);

                const sl::Result fgOptResult = api.setFgOptions(viewport, fg);
                if (fgOptResult == sl::Result::eOk) {
                    appliedFgMode = fgMode;
                    appliedFgFrames = appliedFramesWanted;
                    appliedFgW = in.targetWidth;
                    appliedFgH = in.targetHeight;
                    // 只在本函数真的下发过选项时打一行，用来证明开关到过 SL（sl.log 不记 mode/numFrames）。
                    logInfo("fg options -> %s frames=%u (%ux%u)",
                            fgDynamic ? "dynamic" : "on",
                            static_cast<unsigned>(fg.numFramesToGenerate),
                            static_cast<unsigned>(in.targetWidth),
                            static_cast<unsigned>(in.targetHeight));
                } else {
                    static bool warnedFgOpt = false;
                    if (!warnedFgOpt) {
                        warnedFgOpt = true;
                        logWarn("slDLSSGSetOptions failed=0x%X (mode=%d frames=%d)",
                                static_cast<unsigned>(fgOptResult), fgMode, wantFrames);
                    }
                }
            }
            if (api.getFgState != nullptr) {
                sl::DLSSGState stFrame{};
                const sl::Result stRes = api.getFgState(viewport, stFrame, nullptr);
                if (stRes == sl::Result::eOk) {
                    gFgPresented.store(static_cast<int32_t>(stFrame.numFramesActuallyPresented));
                    // 秒级打一次状态，用 numFramesActuallyPresented > 1 证明插帧真的发生（而不是只下发过选项）。
                    static uint64_t fgLogTick = 0;
                    if (++fgLogTick % 600 == 0) {
                        logInfo("fg state: status=%d presented=%u maxGen=%u tick=%llu",
                                static_cast<int>(stFrame.status),
                                static_cast<unsigned>(stFrame.numFramesActuallyPresented),
                                static_cast<unsigned>(stFrame.numFramesToGenerateMax),
                                static_cast<unsigned long long>(fgLogTick));
                    }
                } else {
                    static bool warnedFgState = false;
                    if (!warnedFgState) {
                        warnedFgState = true;
                        logWarn("slDLSSGGetState failed=0x%X", static_cast<unsigned>(stRes));
                    }
                }
            }

        } else if (fgMode == 0 && api.setFgOptions != nullptr) {
            // 关掉 FG 必须真的下发一次 eOff：一次性闩的老 bug 是开过再关就再也不发，SL 仍在插帧。
            if (appliedFgMode != 0) {
                sl::DLSSGOptions fg{};
                fg.mode = sl::DLSSGMode::eOff;
                const sl::Result offResult = api.setFgOptions(viewport, fg);
                if (offResult == sl::Result::eOk) {
                    appliedFgMode = 0;
                    appliedFgFrames = 0;
                    // 关掉时到底有没有真的下发 eOff 只能由我们自己证明（sl.log 不记 mode/numFrames）：打在刚关掉那一帧。
                    logInfo("fg options -> off (applied)");
                } else {
                    // 失败时 appliedFgMode 保持非 0，下一帧还会再试；这里只警告一次。
                    static bool warnedFgOff = false;
                    if (!warnedFgOff) {
                        warnedFgOff = true;
                        logWarn("slDLSSGSetOptions(eOff) failed=0x%X",
                                static_cast<unsigned>(offResult));
                    }
                }
            }
            // 关着时"上一次真正提交的帧数"必须是 1，否则 F3 会把上次开 FG 的 2/3 当成每帧都交 2 帧。
            gFgPresented.store(1);
        }
// 记下本帧结束时输出纹理停在哪个状态，供 JVM 做状态正确的读回（0 = 本帧没有 handover 纹理）。
        gDlssOutputState.store(
            (copyBack && outResource != out && evalResult == sl::Result::eOk)
                ? static_cast<int32_t>(wantFg ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                             : D3D12_RESOURCE_STATE_COPY_SOURCE)
                : 0);

        if (evalResult != sl::Result::eOk) {
            logWarn("streamline evaluate tags=0x%X options=0x%X evaluate=0x%X "
                    "(mode=%d, out=%ux%u)",
                    static_cast<unsigned>(tagResult), static_cast<unsigned>(optResult),
                    static_cast<unsigned>(evalResult), static_cast<int>(opt.mode),
                    opt.outputWidth, opt.outputHeight);
            return 0;
        }
        return 1;
    }
#endif

int32_t mcdr_dlss_evaluate(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                           void* tlCtx) {
    if (in == nullptr) {
        return -1;
    }
#if MCDR_DLSS_HAVE_STREAMLINE
    // 先判 Streamline：s.disabled 是 **NGX 路径**自己的失败状态，排在前面会让 SL 被永久挡在门外
    // （每帧静默 return -1，JVM 只看到 evaluate 连续失败）。
    if (dlssImplStreamline()) {
        // streamline 模式下 NGX 从未 init/创建：直接走 SL 的 tag -> options -> evaluate（状态由内部负责）。
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
    // 为本调用装上模块提供的跟踪式转换回调。
    s.transitionFn = tl;
    s.transitionCtx = tlCtx;
    auto* color = reinterpret_cast<ID3D12Resource*>(in->pInColor);
    auto* depth = reinterpret_cast<ID3D12Resource*>(in->pInDepth);
    auto* mv = reinterpret_cast<ID3D12Resource*>(in->pInMotionVectors);
    auto* out = reinterpret_cast<ID3D12Resource*>(in->pInOutput);
    if (color == nullptr || depth == nullptr || mv == nullptr || out == nullptr) {
        return 0;
    }
    // 非 NULL 但失效的句柄必须**在第一次 vtable 调用之前**挡掉：曾以
    // EXCEPTION_ACCESS_VIOLATION reading 0x50（第一次 mv->GetDesc()）直接把进程干掉。垃圾指针只能
    // 退化成"本帧跳过 DLSS"，绝不杀进程；失败只报一次，带上原始数值。
    if (!isMappedReadable(color) || !isMappedReadable(depth) || !isMappedReadable(mv) ||
        !isMappedReadable(out)) {
        if (!s.evalFailWarned) {
            s.evalFailWarned = true;
            s.failure = "invalid texture handle";
            logError("DLSS skipped: bad texture handle in input"
                     "pointer (color=%p depth=%p mv=%p out=%p). That is a data-plane bug (the JVM "
                     "handed over a stale/invalid handle), not a DLSS failure; DLSS stays enabled "
                     "so the next frame can retry with a valid set.",
                     static_cast<const void*>(color), static_cast<const void*>(depth),
                     static_cast<const void*>(mv), static_cast<const void*>(out));
        }
        return 0;
    }
// MV 必须是 DLSS 能绑的格式：速度 pass 写 RG16_FLOAT（正好），其它格式先拷进同 footprint 的中间纹理。
    ID3D12Resource* mvForDlss = mv;
    const bool mvIsRg16 = (mv->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT);
    if (!mvIsRg16) {
        const D3D12_RESOURCE_DESC md = mv->GetDesc();
        const bool reuse = (s.mvBuffer != nullptr) && (s.mvBuffer->GetDesc().Width == md.Width) &&
                           (s.mvBuffer->GetDesc().Height == md.Height);
        if (!reuse) {
            // B4：由设备模块创建并跟踪，释放走延迟删除；本单元不 CreateCommittedResource。
            destroyTrackedTexture(s.transitionCtx, &s.mvBufferObject);
            s.mvBuffer = nullptr;
            void* mvObj = nullptr;
            ID3D12Resource* mvRes = nullptr;
            if (!createTrackedTexture(s.transitionCtx, static_cast<UINT>(md.Width), md.Height,
                                      md.Format, md.MipLevels, md.DepthOrArraySize,
                                      md.SampleDesc.Count, 0, &mvObj, &mvRes)) {
                logError("mv intermediate create failed fmt=%d; DLSS disabled",
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
// 优先用 vanilla 清深度之前拍的快照：没有它深度项恒为远平面，放大退化成纯颜色；无快照才用 JVM 送的深度。
    ID3D12Resource* depthForDlss = depth;
    if (s.depthSnapshot != nullptr) {
        depthForDlss = s.depthSnapshot;
    }
// 每次 Evaluate 前都设：输入 -> NON_PIXEL_SHADER_RESOURCE，输出 -> UNORDERED_ACCESS。NGX 用
// compute dispatch 评价（输出是 UAV），PIXEL_SHADER_RESOURCE 在 compute 里不可读
// —— 那是 INVALID_CALL，实测 "成功" 后 1 ms 就 GetDeviceRemovedReason=0x887A0001。
// depth 只在它就是交给 NGX 的那张资源时转换（快照已经在 PIXEL_SHADER_RESOURCE 之上）。
    transition(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (depthForDlss == depth) {
        transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    transition(mvForDlss, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // NGX 的输出必须是 UAV 资源，而 JVM 的 GUI 目标是 ALLOW_RENDER_TARGET（FAIL_RWFlagMissing）。
    // 本单元自己 CreateCommittedResource 出来的纹理 GPU 根本不写（同样的拷贝读回全 0，含 alpha），
    // 所以只要 JVM 通过 mcdr_dx12SetDlssOutput 交付了经模块创建的 UAV 纹理就用它，私有纹理只作为
    // 旧 DLL/旧 JVM 配对的兜底。
    ID3D12Resource* ngxTarget = out;
    bool viaHandover = false;
    if (s.externalOutput != nullptr) {
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
            // 格式不一致必须报出来并退回私有纹理（否则拷回的是无意义内容）。
            logError("JVM output does not fit target"
                     "got %llux%u fmt=%d, need %dx%d fmt=%d; falling back to the private UAV "
                     "texture (which this machine cannot write, so DLSS output will be empty)",
                     static_cast<unsigned long long>(ed.Width), ed.Height,
                     static_cast<int>(ed.Format), in->targetWidth, in->targetHeight,
                     static_cast<int>(out->GetDesc().Format));
        }
    }
    if (ngxTarget == out) {
        ID3D12Resource* priv = createDlssOutput(static_cast<UINT>(in->targetWidth),
                                                static_cast<UINT>(in->targetHeight),
                                                out->GetDesc().Format);
        if (priv != nullptr) {
            ngxTarget = priv;
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(priv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
// 绝不让 NGX 写进没按 ALLOW_UNORDERED_ACCESS 建的纹理：把 GUI 目标转成 UAV 会移除设备；两者都不可用就跳过本帧。
    if (ngxTarget == out) {
        logError("no usable DLSS output texture; evaluate skipped"
                 "rejected and the private one could not be created); skipping the evaluate "
                 "instead of transitioning a non-UAV target to UNORDERED_ACCESS");
        return 0;
    }
    NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = ngxTarget;
    // 锐度传 0（不是 SDK 的 optimal）：参考实现注明 DLSS 里锐化已废弃，0.35 是当年查询结果的残留。
    ep.Feature.InSharpness = 0.0f;
    ep.pInDepth = depthForDlss;
    ep.pInMotionVectors = mvForDlss;
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
    // 没有透明遮罩、曝光纹理和 subrect：数据面还不提供这些。

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
    // 结果交回：DLSS 写的是我们的 UAV，拷进 JVM 目标并留在 RENDER_TARGET —— GUI pass 紧接着附着它。
    if (ngxTarget != out) {
        transition(ngxTarget, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(out, ngxTarget);
        transition(ngxTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else {
        transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    // NGX 路径同样在 SR 成功后把本帧输入交给 FG provider（深度/MV 已转成只读）。
    tagFrameGenInputsFromDlss(cmd, *in);
    s.resetPending = false;

    ++s.submitCount;
    if (!s.evalOk) {
        s.evalOk = true;
        logInfo("DLSS evaluate ok %dx%d -> %dx%d"
                "(mv=%s)",
                in->renderWidth, in->renderHeight, in->targetWidth, in->targetHeight,
                mvIsRg16 ? "RG16 straight through" : "copied into an RG16 intermediate");
    }
    // 1 = 结果进了 JVM handover 纹理（或直接进 JVM 目标）；2 = 进了私有纹理，JVM 不能当成"GUI 目标已带世界层"（黑屏路径）。
    return viaHandover ? 1 : 2;
}
// destroyDevice() 里、设备还活着时调用：NGX 必须先释放 feature 再 shutdown。
// setDlssQuality 观察到的档位记在这里：>= 0 让 ensureFeature 下一帧重建（PerfQualityValue 是
// CreateFeature 输入），-1 恢复"按比例选"。
    int32_t mcdr_dlss_request_quality(int32_t tier) {
    const int normalised = (tier < 0) ? -1 : (tier > 4 ? 4 : tier);
    if (normalised != s.requestedQuality) {
        s.requestedQuality = normalised;
        s.createdWarned = false;
        if (!s.qualityRequestLogged) {
            s.qualityRequestLogged = true;
            logInfo("DLSS tier request %d (%s); feature rebuilds",
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
// preset 是 NVSDK_NGX_DLSS_Hint_Render_Preset：0 = Default（NGX 按档自选）、5 = E / 6 = F
// （已废弃的 CNN）、10 = J、11 = K、12 = L、13 = M（transformer）。越界返回 -1 且不改任何东西；
// 变化会重建 feature（preset 是 CreateFeature 输入）。
    int32_t mcdr_dlss_request_preset(int32_t preset) {
        if (preset < 0 || preset > NVSDK_NGX_DLSS_Hint_Render_Preset_O) {
            logWarn("DLSS preset %d out of range (valid 0,5,6,10..13)"
                    "10=J, 11=K, 12=L, 13=M) — ignored",
                    preset);
            return -1;
        }
        if (preset != s.requestedPreset) {
            s.requestedPreset = preset;
            // 同档位：失败的 create 必须能靠换模型重试，否则首帧失败就锁死整个按钮。
            s.createdWarned = false;
            if (!s.presetRequestLogged) {
                s.presetRequestLogged = true;
                logInfo("DLSS preset request %d (%s); feature rebuilds",
                        preset, presetName(preset).c_str());
            }
        }
        return preset;
    }
// JVM 交付的 UAV 输出纹理（经模块 createTexture 创建）。解包由模块做，这里只收裸
// ID3D12Resource* —— 绝不要在这里解释 JVM 句柄（那个错误就是 mv->GetDesc() 的空 vtable 崩溃）。
// resource == nullptr 清掉覆盖、退回私有纹理；本单元不持有该资源。返回 1 = 接受，0 = 清除。
int32_t mcdr_dlss_set_output_texture(void* resource) {
    auto* res = static_cast<ID3D12Resource*>(resource);
    if (res == s.externalOutput) {
        return res != nullptr ? 1 : 0;
    }
    s.externalOutput = res;
    if (res != nullptr) {
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        logInfo("DLSS output texture %llux%u fmt=%d flags=0x%X"
                "will write here instead of this unit's private UAV texture",
                static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format),
                static_cast<unsigned>(d.Flags));
    } else {
        logInfo("DLSS output override cleared; private UAV again");
    }
    return res != nullptr ? 1 : 0;
}
// DLSS 开关与 AA 模式无关，且不动已加载的 runtime/feature（关了再开不该丢 Init）；真正的门是设备模块的 dlssEnabled。
    int32_t mcdr_dlss_set_enabled(int32_t enabled) {
    const bool want = enabled != 0;
    const bool changed = (want != s.enabled);
    s.enabled = want;
    if (changed) {
        logInfo("DLSS switch %s "
                "initOk=%d featureOk=%d)",
                want ? "ON" : "OFF", s.initOk ? 1 : 0, s.featureOk ? 1 : 0);
    }
    return want ? 1 : 0;
    }

    void mcdr_dlss_shutdown(void) {
    destroyFeature();
    // 三张私有纹理归模块所有：设备消失**之前**交回（ctx = nullptr：只做延迟删除，不碰命令列表）。
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
    if (s.initOk) {
        // 只能用 NVSDK_NGX_D3D12_Shutdown1(device)：无参 Shutdown() 需要 NGX_ENABLE_DEPRECATED_SHUTDOWN（未定义，C3861）。
        const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Shutdown1(s.device);
        if (NVSDK_NGX_FAILED(r)) {
            logWarn("NVSDK_NGX_D3D12_Shutdown1 failed: %s", ngxResultString(r).c_str());
        }
        s = State{};
    }

}  // mcdr_dlss_shutdown
// DXGI 工厂升级入口：设备模块建完工厂立刻调一次，否则 DLSS-G 一次都不出帧。
int32_t mcdr_sl_upgrade_interface(void** baseInterface) {
#if MCDR_DLSS_HAVE_STREAMLINE
    if (!dlssImplStreamline()) {
        return 0;
    }
    // 只要实现是 STREAMLINE，链就一律走 SL 代理工厂（不按 FG 开关）：A/B 实测只有代理链让 RTSS 的 OSD 生效。
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
// FG 开关（C++ 链接）：0=off 1=on 2=dynamic（驱动 >= 595.41）；真正下发在 streamlineEvaluate（呈现线程）。
void mcdr_dlss_set_fg_mode(int32_t mode, int32_t numFrames) {
    gDlssgMode.store(mode);
    gDlssgFrames.store(numFrames < 1 ? 1 : numFrames);
    // 关掉时立刻复位"上一次提交的帧数"：F3 每帧都在读它，晚一帧就显示一次 2x 的错数。
    if (mode == 0) {
        gFgPresented.store(1);
    }
}
// 上一次 Present 真正提交的帧数（1 = 没插帧）；F3 用它把 output 累加成 presented fps。
int32_t mcdr_dlss_fg_presented(void) {
    return gFgPresented.load();
}
// DLSS 输出纹理当前的 state（0 = 未知/无 handover 纹理）。读回必须拿它当 StateBefore，否则 barrier 的 Before 错、内容未定义。
int32_t mcdr_dlss_output_state(void) {
    return gDlssOutputState.load();
}

// ---- DLSS-D（Ray Reconstruction）逐帧求值 -------------------------------------
// RR 与 DLSS-SR 是两份插件，函数也从 sl.interposer.dll 现场解析：宁可这里返回 0，
// 也不要在游戏进程里自己调 NVSDK_NGX_D3D12_Init_*（SL 持有 NGX，会撞 0xBAD00005）。
#if MCDR_DLSS_HAVE_STREAMLINE
namespace {

using PFn_GetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);

struct DlssdApi {
    bool resolved = false;
    bool ok = false;
    PFn_GetFeatureFunction getFeatureFunction = nullptr;
    sl::Result (*setTag)(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*,
                         uint32_t, ID3D12GraphicsCommandList*) = nullptr;
    sl::Result (*evaluate)(sl::Feature, const sl::FrameToken&, const sl::BaseStructure* const*,
                           uint32_t, ID3D12GraphicsCommandList*) = nullptr;
    sl::Result (*newFrameToken)(sl::FrameToken*&, const uint32_t*) = nullptr;
    // 缺了常量 DLSS-D 不会报错，只会按「零位移」重投影（画面拖影），所以这条要单独记结果。
    sl::Result (*setConstants)(const sl::Constants&, const sl::FrameToken&,
                               const sl::ViewportHandle&) = nullptr;
    sl::Result (*setOptions)(const sl::ViewportHandle&, const sl::DLSSDOptions&) = nullptr;
    uint32_t optionWidth = 0u;
    uint32_t optionHeight = 0u;
    int32_t optionMode = -1;
    // 一次性诊断：成功只报一条，失败也只报一条（每帧重复没有信息量）。
    bool reportedOk = false;
    bool reportedFail = false;
};

std::wstring dlssdModuleDirectory() {
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&dlssdModuleDirectory), &self)) {
        return {};
    }
    wchar_t path[MAX_PATH] = {};
    if (!::GetModuleFileNameW(self, path, MAX_PATH)) return {};
    std::wstring text(path);
    const auto slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : text.substr(0, slash + 1);
}

DlssdApi& dlssdApi() {
    static DlssdApi api{};
    if (api.resolved) return api;
    api.resolved = true;
    const std::wstring dir = dlssdModuleDirectory();
    if (dir.empty()) return api;
    HMODULE sl = ::LoadLibraryW((dir + L"sl.interposer.dll").c_str());
    if (sl == nullptr) return api;
    api.getFeatureFunction =
        reinterpret_cast<PFn_GetFeatureFunction>(::GetProcAddress(sl, "slGetFeatureFunction"));
    api.setTag = reinterpret_cast<decltype(api.setTag)>(::GetProcAddress(sl, "slSetTagForFrame"));
    api.evaluate = reinterpret_cast<decltype(api.evaluate)>(::GetProcAddress(sl, "slEvaluateFeature"));
    api.newFrameToken =
        reinterpret_cast<decltype(api.newFrameToken)>(::GetProcAddress(sl, "slGetNewFrameToken"));
    api.setConstants =
        reinterpret_cast<decltype(api.setConstants)>(::GetProcAddress(sl, "slSetConstants"));
    if (api.getFeatureFunction == nullptr || api.setTag == nullptr || api.evaluate == nullptr ||
        api.newFrameToken == nullptr) {
        return api;
    }
    void* raw = nullptr;
    if (api.getFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", raw) != sl::Result::eOk ||
        raw == nullptr) {
        return api;
    }
    api.setOptions = reinterpret_cast<decltype(api.setOptions)>(raw);
    api.ok = true;
    return api;
}

void writeText(char* out, int32_t capacity, const char* text) {
    if (out == nullptr || capacity <= 0) return;
    std::snprintf(out, static_cast<size_t>(capacity), "%s", text == nullptr ? "" : text);
}

}  // namespace

extern "C" int32_t mcdr_dlssd_eval(const mcdr_dlssd_frame* frame, char* outText, int32_t capacity) {
    writeText(outText, capacity, "dlssd: frame inputs incomplete");
    if (frame == nullptr) return 0;
    // 位序：1 list、2 radiance、4 output、8 depth、16 motion、32 normal。缺哪个一眼看得出来。
    const unsigned missing =
        (frame->list == nullptr ? 1u : 0u) | (frame->radiance == nullptr ? 2u : 0u) |
        (frame->output == nullptr ? 4u : 0u) | (frame->depth == nullptr ? 8u : 0u) |
        (frame->motion == nullptr ? 16u : 0u) | (frame->normal == nullptr ? 32u : 0u);
    if (missing != 0u) {
        char line[64] = {};
        std::snprintf(line, sizeof(line), "dlssd: frame inputs missing 0x%X", missing);
        writeText(outText, capacity, line);
        return 0;
    }
    DlssdApi& api = dlssdApi();
    if (!api.ok) {
        writeText(outText, capacity, "dlssd: streamline rr api missing");
        return 0;
    }
    auto* list = static_cast<ID3D12GraphicsCommandList*>(frame->list);
    const sl::ViewportHandle viewport(0);

    // 选项只在输出尺寸/模式变化时下发：每帧下发会让 SL 重建内部资源。
    const int32_t mode = static_cast<int32_t>(sl::DLSSMode::eMaxQuality);
    if (api.optionWidth != frame->outputWidth || api.optionHeight != frame->outputHeight ||
        api.optionMode != mode) {
        sl::DLSSDOptions options{};
        options.mode = sl::DLSSMode::eMaxQuality;
        options.outputWidth = frame->outputWidth;
        options.outputHeight = frame->outputHeight;
        options.preExposure = 1.0f;
        options.exposureScale = 1.0f;
        // RR 吃的是线性 HDR 辐亮度（RGBA16F），不是 SDR 颜色。
        options.colorBuffersHDR = sl::Boolean::eTrue;
        options.alphaUpscalingEnabled = sl::Boolean::eFalse;
        options.normalRoughnessMode = frame->roughness != nullptr
                                          ? sl::DLSSDNormalRoughnessMode::eUnpacked
                                          : sl::DLSSDNormalRoughnessMode::ePacked;
        options.dlaaPreset = sl::DLSSDPreset::ePresetF;
        options.qualityPreset = sl::DLSSDPreset::ePresetF;
        options.balancedPreset = sl::DLSSDPreset::ePresetF;
        options.performancePreset = sl::DLSSDPreset::ePresetF;
        const sl::Result r = api.setOptions(viewport, options);
        if (r != sl::Result::eOk) {
            char line[160] = {};
            std::snprintf(line, sizeof(line), "dlssd: setOptions 0x%X out %ux%u",
                          static_cast<unsigned>(r), frame->outputWidth, frame->outputHeight);
            writeText(outText, capacity, line);
            return -1;
        }
        api.optionWidth = frame->outputWidth;
        api.optionHeight = frame->outputHeight;
        api.optionMode = mode;
    }

    sl::FrameToken* token = nullptr;
    const uint32_t index = frame->frameIndex;
    if (api.newFrameToken(token, &index) != sl::Result::eOk || token == nullptr) {
        writeText(outText, capacity, "dlssd: frame token unavailable");
        return 0;
    }

    // slSetConstants：DLSS-D 靠它做重投影。四个矩阵是行主序（SL 的 float4x4 就是 row[4]），
    // 且明确禁止含抖动偏移；Kotlin 侧已按这个约定摊成 72 个浮点。
    sl::Result constantsResult = sl::Result::eErrorMissingInputParameter;
    if (api.setConstants != nullptr && frame->constants != nullptr) {
        const float* m = frame->constants;
        sl::Constants values{};
        for (uint32_t row = 0; row < 4u; ++row) {
            const uint32_t r = row * 4u;
            values.cameraViewToClip.setRow(
                row, sl::float4(m[r], m[r + 1u], m[r + 2u], m[r + 3u]));
            values.clipToCameraView.setRow(
                row, sl::float4(m[16u + r], m[17u + r], m[18u + r], m[19u + r]));
            values.clipToPrevClip.setRow(
                row, sl::float4(m[32u + r], m[33u + r], m[34u + r], m[35u + r]));
            values.prevClipToClip.setRow(
                row, sl::float4(m[48u + r], m[49u + r], m[50u + r], m[51u + r]));
        }
        // 本渲染器不做时间抖动，MV 是像素单位且 +y 朝下，所以 jitter 恒 0、mvecScale 取 1/w、1/h。
        values.jitterOffset = sl::float2(0.0f, 0.0f);
        values.mvecScale =
            sl::float2(frame->width > 0 ? 1.0f / static_cast<float>(frame->width) : 0.0f,
                       frame->height > 0 ? 1.0f / static_cast<float>(frame->height) : 0.0f);
        values.cameraNear = m[64];
        values.cameraFar = m[65];
        values.cameraFOV = m[66];
        values.cameraAspectRatio = m[67];
        values.depthInverted = sl::Boolean::eFalse;
        values.cameraMotionIncluded = sl::Boolean::eTrue;
        values.motionVectors3D = sl::Boolean::eFalse;
        values.orthographicProjection = sl::Boolean::eFalse;
        values.reset = frame->reset != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constantsResult = api.setConstants(values, *token, viewport);
    }

    auto* radiance = static_cast<ID3D12Resource*>(frame->radiance);
    auto* output = static_cast<ID3D12Resource*>(frame->output);
    auto* depth = static_cast<ID3D12Resource*>(frame->depth);
    auto* motion = static_cast<ID3D12Resource*>(frame->motion);
    auto* normal = static_cast<ID3D12Resource*>(frame->normal);
    sl::Resource colorRes(sl::ResourceType::eTex2d, radiance,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource outputRes(sl::ResourceType::eTex2d, output,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource depthRes(sl::ResourceType::eTex2d, depth,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource motionRes(sl::ResourceType::eTex2d, motion,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource normalRes(sl::ResourceType::eTex2d, normal,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource roughnessRes(sl::ResourceType::eTex2d,
                              static_cast<ID3D12Resource*>(frame->roughness),
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // packed 模式下 normal 就是打包后的法线-粗糙度纹理；unpacked 模式多一张粗糙度。
    // 注意枚举名：SL 叫 kBufferTypeNormals（复数），没有 kBufferTypeNormal。
    const sl::BufferType normalType = frame->roughness != nullptr ? sl::kBufferTypeNormals
                                                                 : sl::kBufferTypeNormalRoughness;
    sl::ResourceTag tags[6] = {
        sl::ResourceTag(&colorRes, sl::kBufferTypeScalingInputColor,
                        sl::ResourceLifecycle::eOnlyValidNow),
        sl::ResourceTag(&outputRes, sl::kBufferTypeScalingOutputColor,
                        sl::ResourceLifecycle::eOnlyValidNow),
        sl::ResourceTag(&depthRes, sl::kBufferTypeDepth,
                        sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(&motionRes, sl::kBufferTypeMotionVectors,
                        sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(&normalRes, normalType, sl::ResourceLifecycle::eOnlyValidNow),
        sl::ResourceTag(&roughnessRes, sl::kBufferTypeRoughness,
                        sl::ResourceLifecycle::eOnlyValidNow),
    };
    const uint32_t tagCount = frame->roughness != nullptr ? 6u : 5u;
    const sl::Result tagResult = api.setTag(*token, viewport, tags, tagCount, list);
    if (tagResult != sl::Result::eOk) {
        char line[160] = {};
        std::snprintf(line, sizeof(line), "dlssd: setTag 0x%X tags=%u",
                      static_cast<unsigned>(tagResult), tagCount);
        writeText(outText, capacity, line);
        return -1;
    }

    const sl::BaseStructure* inputs[1] = {&viewport};
    const sl::Result evalResult = api.evaluate(sl::kFeatureDLSS_RR, *token, inputs, 1, list);
    char line[176] = {};
    std::snprintf(line, sizeof(line), "dlssd: eval 0x%X consts=0x%X %ux%u -> %ux%u packed=%d",
                  static_cast<unsigned>(evalResult), static_cast<unsigned>(constantsResult),
                  frame->width, frame->height, frame->outputWidth, frame->outputHeight,
                  frame->roughness == nullptr ? 1 : 0);
    writeText(outText, capacity, line);
    return evalResult == sl::Result::eOk ? 1 : -1;
}

#else

extern "C" int32_t mcdr_dlssd_eval(const mcdr_dlssd_frame*, char* outText, int32_t capacity) {
    if (outText != nullptr && capacity > 0) {
        std::snprintf(outText, static_cast<size_t>(capacity), "%s", "dlssd: streamline not built in");
    }
    return 0;
}

#endif
