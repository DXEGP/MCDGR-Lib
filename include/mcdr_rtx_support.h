#pragma once
//
// MCDR 超分/降噪 SDK 可用性探测（DLSS / NRD / XeSS / FidelityFX）：只回答「这台机器上哪些 SDK
// 装好了、装的是哪个版本」，不做任何渲染；结果由 capabilitiesText / capabilitiesJson 带出去。
//
// 必须是普通头文件而不是模块：设备模块与探测实现单元都要读它，放进模块就成循环 import。
// 「可用」在四家里含义不同，见各字段注释；探测时机与机制见 docs\advanced\rt-probe.md
//
// MCDR_HAS_DLSS / MCDR_HAS_NRD 仅在 x64 定义（见 MCDR.vcxproj）。Win32 下本结构体依然存在、
// 字段全为「不可用」，调用方不需要写 #ifdef。
#include <dxgi.h>  // probeRtxSupport 形参用到 IDXGIAdapter；项目里 dxgi.h 本来就有
#include <string>
#include <vector>

namespace mcdr
{
    // 探测结果。所有字段都是「查到了什么」而不是「能不能渲染」。
    struct RtxSupport
    {
        // ---- DLSS / NGX ----
        bool dlssProbed = false;
        bool dlssQueried = false;    // 已逐特性问过硬件；该查询不需要先 NVSDK_NGX_Init
        std::string dlssQueryNote;   // 没问成时说明原因（此时三个支持位一律为 false）
        bool ngxRuntime = false;
        std::string ngxRuntimePath;
        std::string ngxNote;

        // 三个特性的支持位含义是「这台机器跑得动」，与「MCDR 是否实现了该渲染路径」是两件事。
        bool dlssSuperResolution = false;
        bool dlssFrameGeneration = false;
        bool dlssRayReconstruction = false;
        std::string dlssSuperResolutionWhy;  // 不支持时的原因（支持则为空）
        std::string dlssFrameGenerationWhy;
        std::string dlssRayReconstructionWhy;

        // ---- NRD ----
        bool nrdLinked = false;  // x64 构建已静态链接 NRD
        std::string nrdVersion;
        std::string nrdVersionDate;
        std::vector<std::string> nrdDenoisers;  // 例如 "REBLUR", "RELAX", "SIGMA"

        // ---- XeSS（Intel） ----
        // xessGetVersion 是全局函数，不需要设备 / 上下文，加载 DLL 就能拿到版本；设备能力要到
        // xessD3D12CreateContext 才知道，本探测不做。
        bool xessProbed = false;
        bool xessRuntime = false;
        std::string xessVersion;     // 形如 "2.0.2"，来自 xessGetVersion 的 xess_version_t
        // libxess.dll 的文件版本资源（形如 "2.0.2.68"）。两个版本都要报：SDK 包名
        // XeSS_SDK_3.0.2 与 libxess.dll 自报的 2.0.2 不一致，只报一个会被误读。
        std::string xessFileVersion;
        std::string xessRuntimePath;
        std::string xessNote;
        // 帧生成与低延迟是独立的 DLL（libxess_fg.dll / libxell.dll）：文件齐了不代表跑得动。
        bool xessFrameGenPresent = false;
        bool xessLowLatencyPresent = false;

        // ---- FidelityFX（AMD FSR） ----
        // 插件/加载器架构：loader 只导出 5 个 ffx 入口，上游不提供导入库，一律 GetProcAddress；
        // 版本号取自各 DLL 的文件版本资源。具体效果能枚举哪些版本要 ffxQuery + 真实设备。
        bool ffxProbed = false;
        bool ffxLoaderLoaded = false;  // loader DLL 已加载，且 5 个入口全部解析成功
        std::string ffxLoaderVersion;
        std::string ffxLoaderPath;
        std::string ffxNote;
        // 各效果运行时是否随包部署（只查文件在位，不试创建上下文）：amd_fidelityfx_upscaler_dx12.dll /
        // _framegeneration_ / _denoiser_ / _radiancecache_。
        bool ffxUpscalerPresent = false;
        bool ffxFrameGenPresent = false;
        bool ffxDenoiserPresent = false;
        bool ffxRadianceCachePresent = false;
        // 各自的文件版本（如超分 "4.1.1.0"、帧生成 "4.0.1.0"），便于看出 SDK 里装的是 FSR 几。
        std::string ffxUpscalerVersion;
        std::string ffxFrameGenVersion;
        std::string ffxDenoiserVersion;
        std::string ffxRadianceCacheVersion;

        // 单行摘要，直接拼进 capabilitiesText。形如 "ngx=yes(SR,FG) nrd=4.18.0[REBLUR,RELAX]
        // xess=2.0.2.68[fg,ll] ffx=2.3.0.0[sr,fg,dn,rc]"，不可用段为 "no" / "off"。
        std::string summary() const;

        // JSON 片段：不带外层花括号、不带前导逗号，键名一律 rtx_ 前缀，调用方负责拼接。
        std::string jsonFragment() const;
    };

    // 探测一次并缓存，可重复调用；线程安全由调用方保证（只在设备初始化 / 能力查询路径调）。
    // adapter 必须传设备在用的 DXGI 适配器：NGX 的 GetFeatureRequirements 没有它就答不准。
    // 传 nullptr 时只报「运行期 DLL 在不在」，不会把「没适配器」误报成「不支持」（见 dlssQueried）。
    const RtxSupport& probeRtxSupport(IDXGIAdapter* adapter);

    // 显式放弃缓存，仅供自检使用（如换适配器后重测）。
    void resetRtxSupportCache();
}