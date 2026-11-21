#include <windows.h>
#include <winver.h>

#include "mcdr_rtx_support.h"

extern "C" {
void* mcdr_dlss_load_runtime(void);
const char* mcdr_dlss_runtime_path(void);
void* mcdr_dlss_ngx_feature_info(void);
int32_t mcdr_dlss_register_vendor_dir(const char* utf8Dir, int32_t cap);
}

#include <nlohmann/json.hpp>

import std;
#if defined(MCDR_HAS_NRD)
#include "NRD.h"
#endif

#if defined(MCDR_HAS_DLSS)
#include "nvsdk_ngx.h"

#define MCDR_NGX_PROJECT_ID "7c1f4a52-9b3e-4d68-8f21-5a6c0e2d9b74"
#define MCDR_NGX_ENGINE_VERSION "1.0"

#define MCDR_NGX_RUNTIME_DLL L"nvngx_dlss.dll"

#define MCDR_NGX_APPDATA L".\\ngx"
#endif

#if defined(MCDR_HAS_XESS)
struct MCDR_XessVersion
{
    unsigned short major;
    unsigned short minor;
    unsigned short patch;
    unsigned short reserved;
};
using mcdr_xessGetVersion_fn = int (*)(MCDR_XessVersion* pVersion);
#define MCDR_XESS_RESULT_SUCCESS 0
#endif

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
        if (slash == std::wstring::npos)
        {
            return {};
        }
        w.resize(slash + 1);
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
        info.SDKVersion = static_cast<NVSDK_NGX_Version>(0x0000018);
        info.FeatureID = feature;
        info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        info.Identifier.v.ProjectDesc.ProjectId = MCDR_NGX_PROJECT_ID;
        info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
        info.Identifier.v.ProjectDesc.EngineVersion = MCDR_NGX_ENGINE_VERSION;
        info.ApplicationDataPath = MCDR_NGX_APPDATA;
        info.FeatureInfo = static_cast<const NVSDK_NGX_FeatureCommonInfo*>(mcdr_dlss_ngx_feature_info());
        return info;
    }

    struct FeatureProbe
    {
        bool ok = false;
        std::string why;
    };
#endif
}

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

        s += " xess=";
        if (xessRuntime)
        {
            if (!xessFileVersion.empty())
            {
                s += xessFileVersion;
            }
            else
            {
                s += xessVersion.empty() ? std::string("yes") : xessVersion;
            }
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

        j["rtx_xess_probed"] = xessProbed;
        j["rtx_xess_runtime"] = xessRuntime;
        j["rtx_xess_version"] = xessVersion;
        j["rtx_xess_file_version"] = xessFileVersion;
        j["rtx_xess_runtime_path"] = xessRuntimePath;
        j["rtx_xess_note"] = xessNote;
        j["rtx_xess_fg_present"] = xessFrameGenPresent;
        j["rtx_xess_low_latency_present"] = xessLowLatencyPresent;

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

        void probeDlss(RtxSupport& s, [[maybe_unused]] IDXGIAdapter* adapter)
        {
            s.dlssProbed = true;
#if defined(MCDR_HAS_DLSS)
            HMODULE mod = reinterpret_cast<HMODULE>(mcdr_dlss_load_runtime());
            if (mod == nullptr)
            {
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

            if (adapter == nullptr)
            {
                s.dlssQueried = false;
                s.dlssQueryNote = "no DXGI adapter supplied; feature support not queried yet";
                return;
            }
            s.dlssQueried = true;
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
            if (!s.xessRuntimePath.empty())
            {
                s.xessFileVersion =
                    fileVersionOf(std::wstring(s.xessRuntimePath.begin(), s.xessRuntimePath.end()));
            }

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

        void probeFidelityFx(RtxSupport& s)
        {
            s.ffxProbed = true;
#if defined(MCDR_HAS_FFX)
            constexpr const wchar_t* kLoader = L"amd_fidelityfx_loader_dx12.dll";
            constexpr const wchar_t* kUpscaler = L"amd_fidelityfx_upscaler_dx12.dll";
            constexpr const wchar_t* kFrameGen = L"amd_fidelityfx_framegeneration_dx12.dll";
            constexpr const wchar_t* kDenoiser = L"amd_fidelityfx_denoiser_dx12.dll";
            constexpr const wchar_t* kRadiance = L"amd_fidelityfx_radiancecache_dx12.dll";

            if (const std::string dir = moduleDirectory(); !dir.empty())
            {
                const std::wstring wdir(dir.begin(), dir.end());
                const auto versionIfPresent = [&wdir](const wchar_t* name) {
                    const std::wstring full = wdir + name;
                    return ::GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES
                               ? std::string()
                               : fileVersionOf(full);
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
                s.ffxNote = "amd_fidelityfx_loader_dx12.dll not found (GetLastError=" +
                            hex32(::GetLastError()) + ")";
                return;
            }
            s.ffxLoaderPath = loadedModulePath(mod);
            s.ffxLoaderVersion = fileVersionOf(std::wstring(s.ffxLoaderPath.begin(),
                                                            s.ffxLoaderPath.end()));

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
    }

    const RtxSupport& probeRtxSupport(IDXGIAdapter* adapter)
    {
        RtxSupport& s = cache();
        if (!s.dlssProbed)
        {
            probeDlss(s, adapter);
        }
        else if (adapter != nullptr && !s.dlssQueried)
        {
            s.dlssQueried = false;
            s.dlssQueryNote.clear();
            probeDlss(s, adapter);
        }
        if (s.nrdVersion.empty())
        {
            probeNrd(s);
        }
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
}
