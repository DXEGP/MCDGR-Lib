// NRD 常驻上下文（S4 4-c）。设备指针由核心模块传进来：d3d12_nrd.cpp 是普通翻译单元，
// 拿不到 gCtx，公开的 mcdr_dx12NrdInit 在 abi_device.cpp 里用 getDeviceHandle() 转一手。
// 见 docs/rt/README.md §22

#ifndef MCDR_NRD_IMPL_H
#define MCDR_NRD_IMPL_H

#include <cstdint>

extern "C" int32_t mcdr_nrd_context_init(void* device, int32_t width, int32_t height,
                                         char* outText, int32_t capacity);
extern "C" int32_t mcdr_nrd_context_shutdown(void);

// 逐帧派发用的一帧参数。纹理都是引擎的 ID3D12Resource*；矩阵是列主序、非抖动。
struct mcdr_nrd_frame {
    void* list;               // ID3D12GraphicsCommandList*，必须已经打开
    void* motion;             // IN_MV，像素单位
    void* normalRoughness;    // IN_NORMAL_ROUGHNESS，R10G10B10A2 打包值（我们存 RGBA16F）
    void* viewZ;              // IN_VIEWZ，R32_FLOAT，视线投影距离
    void* radianceIn;         // IN_DIFF_RADIANCE_HITDIST
    void* radianceOut;        // OUT_DIFF_RADIANCE_HITDIST
    float viewToClip[16];
    float viewToClipPrev[16];
    float worldToView[16];
    float worldToViewPrev[16];
    uint32_t frameIndex;
    // 1 = 这一帧丢掉历史（切轨道、改分辨率、首帧），0 = 继续累积。
    uint32_t resetHistory;
};

extern "C" int32_t mcdr_nrd_dispatch(const mcdr_nrd_frame* frame, char* outText,
                                     int32_t capacity);

#endif // MCDR_NRD_IMPL_H
