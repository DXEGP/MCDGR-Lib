// DLSS-D（Ray Reconstruction）逐帧求值的 C 边界。
//
// 为什么放在这里：NGX 在游戏进程里由 Streamline 持有（见 d3d12_dlss.cpp 顶部注释），所以 RR 只能走
// slSetTagForFrame + slEvaluateFeature；这两个入口与 frame token 都住在 d3d12_dlss.cpp 里，核心模块
// （d3d12_device.cpp）只负责把引擎纹理换成 ID3D12Resource* 并在调用前后管理资源状态。
#pragma once

#include <cstdint>

// 一帧 RR 的输入。roughness 为 null 时按 packed 处理（normal 指向打包后的法线-粗糙度纹理），
// 非空时按 unpacked 处理（normal 与 roughness 两张独立纹理）。
struct mcdr_dlssd_frame {
    void* list = nullptr;      // ID3D12GraphicsCommandList*
    void* radiance = nullptr;  // 带噪辐亮度（= RR 的 color 输入）
    void* output = nullptr;    // RR 输出颜色
    void* depth = nullptr;     // 正的线性 viewZ，kBufferTypeLinearDepth
    void* motion = nullptr;    // 运动矢量（像素单位）
    void* normal = nullptr;    // 法线；packed 模式下是法线-粗糙度打包纹理
    void* roughness = nullptr; // 粗糙度；null 表示 packed 模式
    void* diffuseAlbedo = nullptr;
    void* specularAlbedo = nullptr;
    void* specularHitDistance = nullptr;
    void* exposure = nullptr;
    void* completionFence = nullptr;
    uint64_t completionValue = 0;
    float preExposure = 1.0f;
    float exposureScale = 1.0f;
    const float* constants = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t frameIndex = 0;
    int32_t reset = 0;         // 非 0 时丢历史（切轨道、分辨率变化、进程前几帧）
};

// 返回 1 表示 SL 接受了这次求值；0 表示未执行（SL 缺函数、参数不全）；-1 表示求值失败。
// outText 最多写 capacity 字节，内容是一行诊断（成功与失败都会写，便于一次性看清原因）。
extern "C" int32_t mcdr_dlssd_eval(const mcdr_dlssd_frame* frame, char* outText, int32_t capacity);
extern "C" int32_t mcdr_dlss_sr_eval(const mcdr_dlssd_frame* frame, char* outText, int32_t capacity);
extern "C" int32_t mcdr_rt_prepare_exposure(int32_t rr, int32_t manual, void* completionFence,
    uint64_t completionValue, char* outText, int32_t capacity);
extern "C" void mcdr_rt_reset_exposure();
