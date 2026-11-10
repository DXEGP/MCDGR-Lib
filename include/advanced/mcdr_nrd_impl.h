// NRD 常驻上下文（S4 4-c）。设备指针由核心模块传进来：d3d12_nrd.cpp 是普通翻译单元，
// 拿不到 gCtx，公开的 mcdr_dx12NrdInit 在 abi_device.cpp 里用 getDeviceHandle() 转一手。
// 见 docs/rt/README.md §22

#ifndef MCDR_NRD_IMPL_H
#define MCDR_NRD_IMPL_H

#include <cstdint>
#include <memory>

enum class McdrNrdRecordingState : int32_t { Pending, Submitted, Cancelled, SubmissionFailed };

using McdrNrdReserveMemory = std::shared_ptr<void> (*)(const void*, bool);
inline constexpr int32_t MCDR_NRD_MEMORY_PRESSURE = -2;

struct McdrNrdDependencies {
    void* paths = nullptr;
    void* changes = nullptr;
    uint64_t worldEpoch = 0;
    uint32_t pathOffset = 0;
    uint32_t changeOffset = 0;
    uint32_t changeCount = 0;
    uint32_t fullDomain = 0;
};

extern "C" int32_t mcdr_nrd_context_init(void* device, int32_t width, int32_t height,
                                         McdrNrdReserveMemory reserveMemory, char* outText, int32_t capacity);
extern "C" int32_t mcdr_nrd_context_shutdown(void);
extern "C" void mcdr_nrd_context_collect(void);

// 逐帧派发用的一帧参数。纹理都是引擎的 ID3D12Resource*；矩阵是列主序、非抖动。
struct mcdr_nrd_frame {
    void* list;               // ID3D12GraphicsCommandList*，必须已经打开
    void* completionFence;
    uint64_t completionValue;
    std::shared_ptr<void> recording;
    McdrNrdRecordingState (*recordingState)(const void*) = nullptr;
    void* motion;             // IN_MV，像素单位
    void* normalRoughness;    // IN_NORMAL_ROUGHNESS，R10G10B10A2 打包值（我们存 RGBA16F）
    void* viewZ;              // IN_VIEWZ，R32_FLOAT，视线投影距离
    void* radianceIn;         // IN_DIFF_RADIANCE_HITDIST
    void* radianceOut;        // OUT_DIFF_RADIANCE_HITDIST
    void* specularIn;
    void* specularOut;
    void* validationOutput;
    float viewToClip[16];
    float viewToClipPrev[16];
    float worldToView[16];
    float worldToViewPrev[16];
    float cameraJitter[2];
    float cameraJitterPrev[2];
    uint32_t frameIndex;
    // 1 = 这一帧丢掉历史（切轨道、改分辨率、首帧），0 = 继续累积。
    uint32_t resetHistory;
    void* specularViewZ = nullptr;
    void* diffuseConfidence = nullptr;
    void* specularConfidence = nullptr;
    void* historyRejection = nullptr;
    bool (*createHistoryRejectionPipeline)(const char*, void*, void**) = nullptr;
    const McdrNrdDependencies* dependencies = nullptr;
};

extern "C" int32_t mcdr_nrd_dispatch(const mcdr_nrd_frame* frame, char* outText,
                                     int32_t capacity);

#endif // MCDR_NRD_IMPL_H
