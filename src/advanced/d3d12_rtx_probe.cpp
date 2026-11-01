// 超分/降噪 SDK 可用性探测实现（普通翻译单元，不是模块实现单元：设备模块要调它，设备模块不能反过来
// import 它）。设计见 include\mcdr_rtx_support.h。
#include <windows.h>
#include <winver.h>  // GetFileVersionInfoW / VerQueryValueW：取 DLL 的文件版本资源

#include "mcdr_rtx_support.h"

// NGX 运行期加载由 d3d12_dlss.cpp 负责（那边才有 dbgLog*）；这里只声明用到的入口，必须 extern "C" 才能跨普通 TU 对上。
extern "C" {
void* mcdr_dlss_load_runtime(void);
const char* mcdr_dlss_runtime_path(void);
// NGX 的 FeatureCommonInfo：PathListInfo 指向厂商目录，否则 NGX 找不到特性 DLL，查询会回 FAIL_InvalidParameter。
void* mcdr_dlss_ngx_feature_info(void);
// 登记厂商目录（同时建好上面的 path list）。**必须在下面的特性查询之前调用**，否则查询拿到 FeatureInfo=NULL。
int32_t mcdr_dlss_register_vendor_dir(const char* utf8Dir, int32_t cap);
}

// JSON 用 vcpkg 的 nlohmann/json。这里拼的是「字段串」（不含外侧花括号），由调用方并入更大的对象。
// 必须放在 import std 之前：该头会 include 标准头，在 import std 之后再包含会被 MSVC 拒绝。
#include <nlohmann/json.hpp>

import std;
// NRD：静态库，直接链进 MCDR.dll，没有运行期 DLL
#if defined(MCDR_HAS_NRD)
#include "NRD.h"
#endif

// DLSS / NGX：导入库只给入口，真正实现是运行期的 nvngx_dlss.dll
#if defined(MCDR_HAS_DLSS)
#include "nvsdk_ngx.h"

// ProjectId 必须是 36 字符 GUID，且与 d3d12_dlss.cpp 的 MCDR_NGX_PROJECT_ID 逐字相同（探测与 Init 用同一个应用身份）。
#define MCDR_NGX_PROJECT_ID "7c1f4a52-9b3e-4d68-8f21-5a6c0e2d9b74"
#define MCDR_NGX_ENGINE_VERSION "1.0"

// 只确认主运行时 nvngx_dlss.dll 能加载：GetFeatureRequirements 自己会去摸它需要的那些。
#define MCDR_NGX_RUNTIME_DLL L"nvngx_dlss.dll"

#define MCDR_NGX_APPDATA L".\\ngx"
#endif

// XeSS（Intel）：头文件只取版本结构体，不用它声明函数。一律 LoadLibrary + GetProcAddress，不改 AdditionalDependencies、
// 缺 libxess.dll 也能加载；xess_version_t 布局照上游（4 个 uint16_t）。
#if defined(MCDR_HAS_XESS)
struct MCDR_XessVersion
{
    unsigned short major;
    unsigned short minor;
    unsigned short patch;
    unsigned short reserved;
};
// xessGetVersion 是全局函数，不需要设备与上下文，加载 DLL 就能问版本（CreateContext 才要设备，本探测不做）。
using mcdr_xessGetVersion_fn = int (*)(MCDR_XessVersion* pVersion);
// xess_result_t: XESS_RESULT_SUCCESS = 0（见 xess.h 的枚举顺序）。
#define MCDR_XESS_RESULT_SUCCESS 0
#endif

// FidelityFX（AMD FSR）：没有导入库，api 就在 DLL 里
// 上游 ffx_api.h 用 FFX_API_ENTRY 导出、ffx_api_loader.h 就是一份 GetProcAddress 加载器，所以直接照抄
// 那 5 个入口的类型（x64 默认 __cdecl，无修饰名）。结构体要用，故包含上游头。
#if defined(MCDR_HAS_FFX)
#include "ffx_api.h"
using mcdr_ffxCreateContext_fn = PfnFfxCreateContext;
using mcdr_ffxDestroyContext_fn = PfnFfxDestroyContext;
using mcdr_ffxConfigure_fn = PfnFfxConfigure;
using mcdr_ffxQuery_fn = PfnFfxQuery;
using mcdr_ffxDispatch_fn = PfnFfxDispatch;
#endif

namespace
{
    std::string hex32(unsigned long long v)
    {
        char b[32] = {};
        std::snprintf(b, sizeof(b), "0x%08llX", v);
        return std::string(b);
    }

    std::string moduleDirectory()
    {
        wchar_t path[MAX_PATH * 2] = {};
        const DWORD n = ::GetModuleFileNameW(nullptr, path, std::size(path));
        if (n == 0 || n >= std::size(path))
        {
            return {};
        }
        std::wstring w(path, n);
        const std::wstring::size_type slash = w.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return {};
        
        w.resize(slash + 1);
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (need <= 0) return {};
        
        std::string out(static_cast<std::size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                              nullptr, nullptr);
        return out;
    }

    // std::wstring -> UTF-8 std::string，非法字符不报错（诊断文字，不值得为它失败）。
    std::string narrow(const std::wstring& w)
    {
        if (w.empty())
        {
            return {};
        }
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (need <= 0)
        {
            return {};
        }
        std::string out(static_cast<std::size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                              nullptr, nullptr);
        return out;
    }

    std::string loadedModulePath(HMODULE mod)
    {
        wchar_t path[MAX_PATH * 2] = {};
        const DWORD n = ::GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path)));
        if (n == 0)
        {
            return {};
        }
        return narrow(std::wstring(path, n));
    }

    // 按「模块同目录优先、其次系统搜索路径」加载运行期 DLL：部署时厂商 DLL 与 MCDR.dll 放在一起，工作目录未必是那里。
    HMODULE loadRuntimeDll(const wchar_t* dllName)
    {
        HMODULE mod = nullptr;
        const std::string dir = moduleDirectory();
        if (!dir.empty())
        {
            const std::wstring wdir(dir.begin(), dir.end());
            mod = ::LoadLibraryW((wdir + dllName).c_str());
        }
        if (mod == nullptr)
        {
            mod = ::LoadLibraryW(dllName);
        }
        return mod;
    }

    // 只查文件版本资源，不加载 DLL：返回形如 "2.3.0.0"，拿不到返回空串。
    std::string fileVersionOf(const std::wstring& fullPath)
    {
        DWORD dummy = 0;
        const DWORD size = ::GetFileVersionInfoSizeW(fullPath.c_str(), &dummy);
        if (size == 0)
        {
            return {};
        }
        std::vector<unsigned char> buf(size);
        if (::GetFileVersionInfoW(fullPath.c_str(), 0, size, buf.data()) == FALSE)
        {
            return {};
        }
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT len = 0;
        if (::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) == FALSE ||
            ffi == nullptr || len < sizeof(VS_FIXEDFILEINFO))
        {
            return {};
        }
        char v[64] = {};
        std::snprintf(v, sizeof(v), "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS),
                      LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS),
                      LOWORD(ffi->dwFileVersionLS));
        return std::string(v);
    }

#if defined(MCDR_HAS_DLSS)
    const char* ngxResultText(NVSDK_NGX_Result r)
    {
        switch (r)
        {
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

    std::string ngxResultString(NVSDK_NGX_Result r)
    {
        if (const char* t = ngxResultText(r))
        {
            return std::string(t) + " (" + hex32(r) + ")";
        }
        return hex32(r);
    }

    // FeatureSupported 是「不支持的原因」位域，0 才表示支持。
    std::string supportBitsText(NVSDK_NGX_Feature_Support_Result bits)
    {
        const unsigned v = static_cast<unsigned>(bits);
        if (v == 0)
        {
            return {};
        }
        std::string s;
        const auto add = [&s](const char* name) {
            if (!s.empty())
            {
                s += "|";
            }
            s += name;
        };
        if (v & NVSDK_NGX_FeatureSupportResult_CheckNotPresent) add("check-not-present");
        if (v & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) add("driver-too-old");
        if (v & NVSDK_NGX_FeatureSupportResult_AdapterUnsupported) add("adapter-unsupported");
        if (v & NVSDK_NGX_FeatureSupportResult_OSVersionBelowMinimumSupported) add("os-too-old");
        if (v & NVSDK_NGX_FeatureSupportResult_NotImplemented) add("not-implemented");
        if (s.empty())
        {
            s = "unknown-bits(" + hex32(v) + ")";
        }
        return s;
    }

    NVSDK_NGX_FeatureDiscoveryInfo makeDiscovery(NVSDK_NGX_Feature feature)
    {
        NVSDK_NGX_FeatureDiscoveryInfo info;
        // SDKVersion 用 0x18，**不要**用头里的 NVSDK_NGX_Version_API（0x15）：运行期 310.x 拿它做门禁，
        // 0x15 时 Init 与 GetFeatureRequirements 一律回 FAIL_InvalidParameter (0xBAD00005)。
        info.SDKVersion = static_cast<NVSDK_NGX_Version>(0x0000018);
        info.FeatureID = feature;
        info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        info.Identifier.v.ProjectDesc.ProjectId = MCDR_NGX_PROJECT_ID;
        info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
        info.Identifier.v.ProjectDesc.EngineVersion = MCDR_NGX_ENGINE_VERSION;
        info.ApplicationDataPath = MCDR_NGX_APPDATA;
        // NGX 默认只在应用目录（开发跑时是 JDK 的 bin\）里找特性 DLL：这张由 d3d12_dlss.cpp 按厂商目录
        // 建好的 path list 才让下面的查询答 Success；NULL 是合法的「还不知道」。
        info.FeatureInfo = static_cast<const NVSDK_NGX_FeatureCommonInfo*>(mcdr_dlss_ngx_feature_info());
        return info;
    }

    // 一个特性的查询结果。不接收适配器：探测阶段设备可能还没建好，给 GetFeatureRequirements 传 nullptr 只会失败。
    struct FeatureProbe
    {
        bool ok = false;
        std::string why;
    };
#endif
}  // namespace

namespace mcdr
{
    std::string RtxSupport::summary() const
    {
        std::string s = "ngx=";
        if (ngxRuntime)
        {
            s += "yes(";
            bool first = true;
            const auto add = [&](const char* n) {
                if (!first)
                {
                    s += ",";
                }
                first = false;
                s += n;
            };
            if (dlssSuperResolution) add("SR");
            if (dlssFrameGeneration) add("FG");
            if (dlssRayReconstruction) add("RR");
            if (first)
            {
                s += "runtime-only";
            }
            s += ")";
        }
        else
        {
            s += "no";
        }

        s += " nrd=";
        if (nrdLinked)
        {
            s += nrdVersion.empty() ? std::string("yes") : nrdVersion;
            s += "[";
            for (std::size_t i = 0; i < nrdDenoisers.size(); ++i)
            {
                if (i)
                {
                    s += ",";
                }
                s += nrdDenoisers[i];
            }
            s += "]";
        }
        else
        {
            s += "off";
        }

        // ---- XeSS ----
        s += " xess=";
        if (xessRuntime)
        {
            // 摘要用文件版本（2.0.2.68）而不是 xessGetVersion 的 2.0.2：能分辨具体构建；两个都在 json 里。
            if (!xessFileVersion.empty())
            {
                s += xessFileVersion;
            }
            else
            {
                s += xessVersion.empty() ? std::string("yes") : xessVersion;
            }
            // 方括号里列的是**运行时文件齐不齐**，不是硬件支不支持。
            std::string caps;
            const auto addCap = [&caps](const char* n) {
                if (!caps.empty())
                {
                    caps += ",";
                }
                caps += n;
            };
            if (xessFrameGenPresent) addCap("fg");
            if (xessLowLatencyPresent) addCap("ll");
            if (!caps.empty())
            {
                s += "[" + caps + "]";
            }
        }
        else
        {
            s += "no";
        }

        // ---- FidelityFX ----
        s += " ffx=";
        if (ffxLoaderLoaded)
        {
            s += ffxLoaderVersion.empty() ? std::string("yes") : ffxLoaderVersion;
            std::string caps;
            const auto addCap = [&caps](const char* n) {
                if (!caps.empty())
                {
                    caps += ",";
                }
                caps += n;
            };
            if (ffxUpscalerPresent) addCap("sr");
            if (ffxFrameGenPresent) addCap("fg");
            if (ffxDenoiserPresent) addCap("dn");
            if (ffxRadianceCachePresent) addCap("rc");
            if (!caps.empty())
            {
                s += "[" + caps + "]";
            }
        }
        else
        {
            s += "no";
        }
        return s;
    }

    // nlohmann 对象 → 「字段串」：`{"a":1,"b":2}` → `"a":1,"b":2`；空对象返回空串，调用方据此跳过逗号。
    static std::string jsonFieldsOf(const nlohmann::json& obj)
    {
        std::string text = obj.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        if (text.size() < 2 || text.front() != '{' || text.back() != '}')
        {
            return std::string();
        }
        text.erase(text.size() - 1);
        text.erase(0, 1);
        return text;
    }

    std::string RtxSupport::jsonFragment() const
    {
        nlohmann::json j;
        j["rtx_dlss_probed"] = dlssProbed;
        j["rtx_dlss_runtime"] = ngxRuntime;
        j["rtx_dlss_sr"] = dlssSuperResolution;
        j["rtx_dlss_fg"] = dlssFrameGeneration;
        j["rtx_dlss_rr"] = dlssRayReconstruction;
        j["rtx_dlss_runtime_path"] = ngxRuntimePath;
        j["rtx_dlss_note"] = ngxNote;
        j["rtx_dlss_sr_why"] = dlssSuperResolutionWhy;
        j["rtx_dlss_fg_why"] = dlssFrameGenerationWhy;
        j["rtx_dlss_rr_why"] = dlssRayReconstructionWhy;

        j["rtx_nrd_linked"] = nrdLinked;
        j["rtx_nrd_version"] = nrdVersion;
        j["rtx_nrd_version_date"] = nrdVersionDate;
        j["rtx_nrd_denoisers"] = nrdDenoisers;

        // ---- XeSS ----
        j["rtx_xess_probed"] = xessProbed;
        j["rtx_xess_runtime"] = xessRuntime;
        j["rtx_xess_version"] = xessVersion;
        j["rtx_xess_file_version"] = xessFileVersion;
        j["rtx_xess_runtime_path"] = xessRuntimePath;
        j["rtx_xess_note"] = xessNote;
        j["rtx_xess_fg_present"] = xessFrameGenPresent;
        j["rtx_xess_low_latency_present"] = xessLowLatencyPresent;

        // ---- FidelityFX ----
        j["rtx_ffx_probed"] = ffxProbed;
        j["rtx_ffx_loader"] = ffxLoaderLoaded;
        j["rtx_ffx_loader_version"] = ffxLoaderVersion;
        j["rtx_ffx_loader_path"] = ffxLoaderPath;
        j["rtx_ffx_note"] = ffxNote;
        j["rtx_ffx_upscaler_present"] = ffxUpscalerPresent;
        j["rtx_ffx_upscaler_version"] = ffxUpscalerVersion;
        j["rtx_ffx_framegen_present"] = ffxFrameGenPresent;
        j["rtx_ffx_framegen_version"] = ffxFrameGenVersion;
        j["rtx_ffx_denoiser_present"] = ffxDenoiserPresent;
        j["rtx_ffx_denoiser_version"] = ffxDenoiserVersion;
        j["rtx_ffx_radiancecache_present"] = ffxRadianceCachePresent;
        j["rtx_ffx_radiancecache_version"] = ffxRadianceCacheVersion;
        return jsonFieldsOf(j);
    }

    namespace
    {
        RtxSupport& cache()
        {
            static RtxSupport s;
            return s;
        }

        void probeNrd(RtxSupport& s)
        {
#if defined(MCDR_HAS_NRD)
            s.nrdLinked = true;

            char v[48] = {};
            std::snprintf(v, sizeof(v), "%u.%u.%u", static_cast<unsigned>(NRD_VERSION_MAJOR),
                          static_cast<unsigned>(NRD_VERSION_MINOR),
                          static_cast<unsigned>(NRD_VERSION_BUILD));
            s.nrdVersion = v;
            s.nrdVersionDate = NRD_VERSION_DATE;

            // GetLibraryDesc() 不需要设备与实例（NRD 唯一的纯查询入口）：supportedDenoisers 就是实际编进静态库的那些。
            if (const nrd::LibraryDesc* d = nrd::GetLibraryDesc(); d != nullptr)
            {
                s.nrdVersion = std::to_string(static_cast<unsigned>(d->versionMajor)) + "." +
                               std::to_string(static_cast<unsigned>(d->versionMinor)) + "." +
                               std::to_string(static_cast<unsigned>(d->versionBuild));
                for (uint32_t i = 0; i < d->supportedDenoisersNum; ++i)
                {
                    const char* name = nrd::GetDenoiserString(d->supportedDenoisers[i]);
                    if (name != nullptr && name[0] != '\0')
                    {
                        s.nrdDenoisers.emplace_back(name);
                    }
                }
            }
            if (s.nrdDenoisers.empty())
            {
                s.nrdVersion += " (desc unavailable)";
            }
#else
            s.nrdLinked = false;
            s.nrdVersion = "not built for this platform";
#endif
        }

        // 非 x64 平台整个特性查询分支会被编译掉，adapter 就没人用；不加 [[maybe_unused]] 会触发 C4100，而 /WX。
        void probeDlss(RtxSupport& s, [[maybe_unused]] IDXGIAdapter* adapter)
        {
            s.dlssProbed = true;
#if defined(MCDR_HAS_DLSS)
            // 第一步：运行期 DLL 在不在。加载与逐条取证在 d3d12_dlss.cpp（探测是普通 TU，拿不到 dbgLog*）；
            // 可执行文件只是入口，缺了 DLL 一切特性都不可用，失败原因绝不能读成「显卡不支持」。
            HMODULE mod = reinterpret_cast<HMODULE>(mcdr_dlss_load_runtime());
            if (mod == nullptr)
            {
                // 兜底：按普通搜索路径与 MCDR.dll 同目录再试一次；语义仍是「找不到」，绝不误报成「不支持」。
                std::wstring dllName = MCDR_NGX_RUNTIME_DLL;
                mod = ::LoadLibraryW(dllName.c_str());
                if (mod == nullptr && !moduleDirectory().empty())
                {
                    const std::string dir = moduleDirectory();
                    std::wstring wdir(dir.begin(), dir.end());
                    mod = ::LoadLibraryW((wdir + dllName).c_str());
                }
            }
            if (mod == nullptr)
            {
                s.ngxRuntime = false;
                s.ngxNote = "nvngx_dlss.dll not found (GetLastError=" + hex32(::GetLastError()) + ")";
                s.dlssSuperResolutionWhy = "runtime missing";
                s.dlssFrameGenerationWhy = "runtime missing";
                s.dlssRayReconstructionWhy = "runtime missing";
                return;
            }
            s.ngxRuntime = true;
            // 路径优先用实现侧记下的那条（只有它知道是哪个候选命中的）；拿不到再回退到 GetModuleFileNameW。
            if (const char* recorded = mcdr_dlss_runtime_path(); recorded != nullptr && recorded[0] != '\0')
            {
                s.ngxRuntimePath = recorded;
            }
            else
            {
                wchar_t path[MAX_PATH * 2] = {};
                const DWORD n = ::GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path)));
                if (n > 0)
                {
                    const int need = ::WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(n),
                                                           nullptr, 0, nullptr, nullptr);
                    if (need > 0)
                    {
                        s.ngxRuntimePath.assign(static_cast<std::size_t>(need), '\0');
                        ::WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(n),
                                              s.ngxRuntimePath.data(), need, nullptr, nullptr);
                    }
                }
            }

            // **必须在下面的特性查询之前**把厂商目录登记给 DLSS 实现：登记同时建好 FeatureCommonInfo 的 PathListInfo，
            // 缺了它 Init 与 GetFeatureRequirements 一律回 FAIL_InvalidParameter (0xBAD00005)；晚登记则查询拿 NULL。
            if (!s.ngxRuntimePath.empty())
            {
                std::wstring wdir;
                {
                    wchar_t dir[MAX_PATH * 2] = {};
                    const DWORD n = ::GetModuleFileNameW(mod, dir, static_cast<DWORD>(std::size(dir)));
                    if (n > 0 && n < std::size(dir))
                    {
                        std::wstring full(dir, n);
                        const std::wstring::size_type slash = full.find_last_of(L"\\/");
                        if (slash != std::wstring::npos)
                        {
                            wdir = full.substr(0, slash);
                        }
                    }
                }
                if (!wdir.empty())
                {
                    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wdir.c_str(),
                                                           static_cast<int>(wdir.size()), nullptr, 0,
                                                           nullptr, nullptr);
                    if (need > 0)
                    {
                        std::string utf8(static_cast<std::size_t>(need), '\0');
                        ::WideCharToMultiByte(CP_UTF8, 0, wdir.c_str(), static_cast<int>(wdir.size()),
                                              utf8.data(), need, nullptr, nullptr);
                        mcdr_dlss_register_vendor_dir(utf8.c_str(), need + 1);
                    }
                }
            }

            // 第二步：逐个特性问硬件/驱动。GetFeatureRequirements 不需要先 NVSDK_NGX_Init，正是探测要的性质。
            // 没有适配器时**不能**拿 nullptr 去问：只会得到 FAIL_InvalidParameter，再把「没适配器」显示成「显卡不支持」；
            // 这种情况只报「运行期在」，查询留到有适配器时。
            // 「查不着」（FAIL_InvalidParameter / FAIL_NotInitialized）同样不是「不支持」：三个支持位留空并写明原因，
            // 真正的门是 NGX 的 Init 是否成功（见 mcdr_dlss_can_run 与 Init 之后的重探）。
            if (adapter == nullptr)
            {
                s.dlssQueried = false;
                s.dlssQueryNote = "no DXGI adapter supplied; feature support not queried yet";
                return;
            }
            s.dlssQueried = true;
            // adapter 必须显式捕获：它是函数参数，lambda 无法隐式捕获它（C3493）。
            const auto ask = [&s, adapter](NVSDK_NGX_Feature feature, bool& outOk, std::string& outWhy) {
                const NVSDK_NGX_FeatureDiscoveryInfo info = makeDiscovery(feature);
                NVSDK_NGX_FeatureRequirement req = {};
                const NVSDK_NGX_Result r =
                    NVSDK_NGX_D3D12_GetFeatureRequirements(adapter, &info, &req);
                if (r != NVSDK_NGX_Result_Success)
                {
                    outOk = false;
                    outWhy = "query failed: " + ngxResultString(r);
                    return;
                }
                if (req.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported)
                {
                    outOk = true;
                    outWhy.clear();
                    return;
                }
                outOk = false;
                outWhy = supportBitsText(req.FeatureSupported);
                if (req.MinOSVersion[0] != '\0')
                {
                    outWhy += " (min os " + std::string(req.MinOSVersion) + ")";
                }
            };
            ask(NVSDK_NGX_Feature_SuperSampling, s.dlssSuperResolution, s.dlssSuperResolutionWhy);
            ask(NVSDK_NGX_Feature_FrameGeneration, s.dlssFrameGeneration,
                s.dlssFrameGenerationWhy);
            ask(NVSDK_NGX_Feature_RayReconstruction, s.dlssRayReconstruction,
                s.dlssRayReconstructionWhy);
            if (s.dlssSuperResolutionWhy.rfind("query failed:", 0) == 0)
            {
                s.dlssQueried = false;
                s.dlssQueryNote = "feature requirement query was inconclusive (" +
                                  s.dlssSuperResolutionWhy + "); the real gate is the NGX init";
            }
#else
            s.ngxRuntime = false;
            s.ngxNote = "not built for this platform (x64 only)";
#endif
        }

        // XeSS：加载 libxess.dll，用 xessGetVersion 拿真实版本号
        void probeXess(RtxSupport& s)
        {
            s.xessProbed = true;
#if defined(MCDR_HAS_XESS)
            constexpr const wchar_t* kRuntime = L"libxess.dll";
            HMODULE mod = loadRuntimeDll(kRuntime);
            if (mod == nullptr)
            {
                s.xessRuntime = false;
                s.xessNote = "libxess.dll not found (GetLastError=" + hex32(::GetLastError()) + ")";
                return;
            }
            s.xessRuntime = true;
            s.xessRuntimePath = loadedModulePath(mod);

            // xessGetVersion 必须解析成功才算「运行时可问」：DLL 在但入口缺失 = 版本对不上，也要报出来。
            const auto getVersion = reinterpret_cast<mcdr_xessGetVersion_fn>(
                reinterpret_cast<void*>(::GetProcAddress(mod, "xessGetVersion")));
            if (getVersion == nullptr)
            {
                s.xessNote = "libxess.dll loaded but xessGetVersion is missing "
                             "(GetLastError=" + hex32(::GetLastError()) + ")";
                return;
            }
            MCDR_XessVersion v{};
            const int rc = getVersion(&v);
            if (rc != MCDR_XESS_RESULT_SUCCESS)
            {
                s.xessNote = "xessGetVersion failed (xess_result_t=" + std::to_string(rc) + ")";
                return;
            }
            char vb[64] = {};
            std::snprintf(vb, sizeof(vb), "%u.%u.%u", static_cast<unsigned>(v.major),
                          static_cast<unsigned>(v.minor), static_cast<unsigned>(v.patch));
            s.xessVersion = vb;
            // 同时取文件版本资源：包名（3.0.2）与运行期自报版本（2.0.2）对不上，两个都报出来。
            if (!s.xessRuntimePath.empty())
            {
                s.xessFileVersion =
                    fileVersionOf(std::wstring(s.xessRuntimePath.begin(), s.xessRuntimePath.end()));
            }

            // 帧生成与低延迟是**另外两个 DLL**：能加载不等于跑得动（那要建上下文 + 真设备），故字段名用 Present。
            if (HMODULE fg = loadRuntimeDll(L"libxess_fg.dll"); fg != nullptr)
            {
                s.xessFrameGenPresent = true;
            }
            if (HMODULE ll = loadRuntimeDll(L"libxell.dll"); ll != nullptr)
            {
                s.xessLowLatencyPresent = true;
            }
#else
            s.xessRuntime = false;
            s.xessNote = "not built for this platform (x64 only)";
#endif
        }

        // FidelityFX：没有导入库，loader DLL 的 5 个入口全靠 GetProcAddress
        void probeFidelityFx(RtxSupport& s)
        {
            s.ffxProbed = true;
#if defined(MCDR_HAS_FFX)
            constexpr auto kLoader = L"amd_fidelityfx_loader_dx12.dll";
            constexpr auto kUpscaler = L"amd_fidelityfx_upscaler_dx12.dll";
            constexpr auto kFrameGen = L"amd_fidelityfx_framegeneration_dx12.dll";
            constexpr auto kDenoiser = L"amd_fidelityfx_denoiser_dx12.dll";
            constexpr auto kRadiance = L"amd_fidelityfx_radiancecache_dx12.dll";

            if (const std::string dir = moduleDirectory(); !dir.empty())
            {
                const std::wstring wdir(dir.begin(), dir.end());
                // 版本号只读文件资源不加载：即使 loader 加载失败也能报出「文件在，是 2.3.0.0」。
                const auto versionIfPresent = [&wdir](const wchar_t* name) {
                    const std::wstring full = wdir + name;
                    return ::GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : fileVersionOf(full);
                };
                if (const std::string v = versionIfPresent(kUpscaler); !v.empty())
                {
                    s.ffxUpscalerPresent = true;
                    s.ffxUpscalerVersion = v;
                }
                if (const std::string v = versionIfPresent(kFrameGen); !v.empty())
                {
                    s.ffxFrameGenPresent = true;
                    s.ffxFrameGenVersion = v;
                }
                if (const std::string v = versionIfPresent(kDenoiser); !v.empty())
                {
                    s.ffxDenoiserPresent = true;
                    s.ffxDenoiserVersion = v;
                }
                if (const std::string v = versionIfPresent(kRadiance); !v.empty())
                {
                    s.ffxRadianceCachePresent = true;
                    s.ffxRadianceCacheVersion = v;
                }
            }

            HMODULE mod = loadRuntimeDll(kLoader);
            if (mod == nullptr)
            {
                s.ffxLoaderLoaded = false;
                s.ffxNote = "amd_fidelityfx_loader_dx12.dll not found (GetLastError=" + hex32(::GetLastError()) + ")";
                return;
            }
            s.ffxLoaderPath = loadedModulePath(mod);
            s.ffxLoaderVersion = fileVersionOf(std::wstring(s.ffxLoaderPath.begin(),s.ffxLoaderPath.end()));

            // 5 个入口必须**全部**解析到，少一个就说明这份 loader 与本 SDK 头不匹配；只报「加载成功」会掩盖错配。
            const mcdr_ffxCreateContext_fn createContext =
                reinterpret_cast<mcdr_ffxCreateContext_fn>(
                    reinterpret_cast<void*>(::GetProcAddress(mod, "ffxCreateContext")));
            const mcdr_ffxDestroyContext_fn destroyContext =
                reinterpret_cast<mcdr_ffxDestroyContext_fn>(
                    reinterpret_cast<void*>(::GetProcAddress(mod, "ffxDestroyContext")));
            const mcdr_ffxConfigure_fn configure = reinterpret_cast<mcdr_ffxConfigure_fn>(
                reinterpret_cast<void*>(::GetProcAddress(mod, "ffxConfigure")));
            const mcdr_ffxQuery_fn query = reinterpret_cast<mcdr_ffxQuery_fn>(
                reinterpret_cast<void*>(::GetProcAddress(mod, "ffxQuery")));
            const mcdr_ffxDispatch_fn dispatch = reinterpret_cast<mcdr_ffxDispatch_fn>(
                reinterpret_cast<void*>(::GetProcAddress(mod, "ffxDispatch")));
            if (createContext == nullptr || destroyContext == nullptr || configure == nullptr ||
                query == nullptr || dispatch == nullptr)
            {
                s.ffxLoaderLoaded = false;
                s.ffxNote = "loader loaded but not all of the 5 ffx entry points resolved";
                return;
            }
            s.ffxLoaderLoaded = true;
#else
            s.ffxLoaderLoaded = false;
            s.ffxNote = "not built for this platform (x64 only)";
#endif
        }
    }  // namespace

    const RtxSupport& probeRtxSupport(IDXGIAdapter* adapter)
    {
        RtxSupport& s = cache();
        // 「探测过没有」记在缓存自身（dlssProbed / nrdLinked），不用函数局部 static —— 那样 resetRtxSupportCache 清不掉。
        if (!s.dlssProbed)
        {
            probeDlss(s, adapter);
        }
        else if (adapter != nullptr && !s.dlssQueried)
        {
            // 首次调用没带适配器（设备还没建好），这次补上特性查询：只重做 DLSS 那一步。
            s.dlssQueried = false;
            s.dlssQueryNote.clear();
            probeDlss(s, adapter);
        }
        if (s.nrdVersion.empty())
        {
            probeNrd(s);
        }
        // XeSS / FidelityFX 都不需要适配器：和 NRD 一样只在没测过时测一次，不因首次没带适配器而重做。
        if (!s.xessProbed)
        {
            probeXess(s);
        }
        if (!s.ffxProbed)
        {
            probeFidelityFx(s);
        }
        return s;
    }

    void resetRtxSupportCache()
    {
        cache() = RtxSupport{};
    }
}  // namespace mcdr