// mcdr_dlss_impl.h - the only interface between the DLSS implementation unit (d3d12_dlss.cpp) and the
// device module: a header because the device module needs these declarations inside its global module
// fragment (bare extern "C" there is C5202 under /WX). extern "C" subset, void*/POD/char buffers only,
// never std::string (the two TUs do not see the same std::string type).
#ifndef MCDR_DLSS_IMPL_H
#define MCDR_DLSS_IMPL_H

#include "mcdr_abi.h"  // mcdr_dlss_input

struct mcdr_dlssd_frame;

#ifdef __cplusplus
extern "C" {
#endif

// Installs the module's log outlets: the two TUs mangle dbgLogInfo differently. Call once, first.
void mcdr_dlss_install_log(void* logInfo, void* logWarn, void* logError);

// Full path of nvngx_dlss.dll as reported by the RTX probe; copied during the call, not held.
void mcdr_dlss_register_runtime_path(const char* path, int32_t cap);

// 已登记的运行期路径（可能为空串；永不返回 NULL）。
const char* mcdr_dlss_runtime_path(void);

// NGX 初始化（幂等，成败都只做一次）。device 是 ID3D12Device*。返回 1=可用 0=不可用。
int32_t mcdr_dlss_can_run(void* device);

// State query. note/optimal/runtime are caller buffers and may be NULL; flagsOut gets
// 1=INIT_TRIED 2=INIT_OK 4=PARAMS_OK 8=OPTIMAL_OK 16=FEATURE_OK 32=DISABLED.
void mcdr_dlss_query(char* note, int32_t noteCap, char* optimal, int32_t optimalCap, char* runtime,
                     int32_t runtimeCap, uint32_t* flagsOut);

// Announces the vendor runtime directory (`<user.dir>\dxrmc\`) and widens the process DLL search path
// so the probe's bare-name LoadLibraryW succeeds. Idempotent; 1 = widened.
int32_t mcdr_dlss_register_vendor_dir(const char* utf8Dir, int32_t cap);

// Loads nvngx_dlss.dll so every attempt lands in the native log with its own GetLastError.
void* mcdr_dlss_load_runtime(void);

// Explicit DLSS tier: 0=Ultra Performance 1=Performance 2=Balanced 3=Quality 4=DLAA. A value >= 0 makes
// the next `ensureFeature` rebuild (PerfQualityValue is a CreateFeature input); -1 restores the ratio.
int32_t mcdr_dlss_request_quality(int32_t tier);

// DLSS *model* (NGX render preset): 0 = Default (NGX decides), 5 = E / 6 = F (deprecated CNN),
// 10 = J / 11 = K / 12 = L / 13 = M (transformer). A change rebuilds the feature like a tier change.
int32_t mcdr_dlss_request_preset(int32_t preset);

// Hands over the UAV output texture the JVM created through the module's `createTexture` path: the
// private texture this unit creates itself is one the GPU never writes. The resource is NOT owned here.
int32_t mcdr_dlss_set_output_texture(void* resource);

// Reflex: call before Present (slReflexSleep); the interposer is loaded lazily.
void mcdr_reflex_before_present(void);
// Emits the PCL ePresentEnd marker after Present.
void mcdr_reflex_after_present(void);
// Is the Streamline path in use (default auto: the runtime being present enables it).
int32_t mcdr_dlss_streamline_requested(void);

// Records whether the DLSS plugin really loaded in Streamline mode; available() reports "unavailable"
// from it, else NGX refusing to initialise reads as "switch on, every frame skipped".
void mcdr_dlss_note_sl_loaded(int32_t loaded);
int32_t mcdr_dlss_available(void);

// Frame generation (DLSS-G): 0 = off, 1 = on (fixed multiplier), 2 = dynamic. State only; the options are
// pushed by the render thread inside evaluate.
void mcdr_dlss_set_fg_mode(int32_t mode, int32_t numFrames);

// Frames the last Present really submitted (1 = no generation); F3's output reads it.
int32_t mcdr_dlss_fg_presented(void);

// State the DLSS output texture is left in (0 = unknown); a read-back must use it as StateBefore.
int32_t mcdr_dlss_output_state(void);
void mcdr_dlss_invalidate_output(void);

// Reflex mode (0=off 1=eLowLatency 2=eLowLatencyWithBoost) and the availability query.
void mcdr_reflex_set_mode(int32_t mode);
int32_t mcdr_reflex_available(void);

// PCL stats: frame begin (build the token + eSimulationStart) and a generic marker.
void mcdr_reflex_frame_begin(void);
void mcdr_reflex_marker(int32_t marker);

// Reflex frame limiter (microseconds per frame, 0 = off).
void mcdr_reflex_set_frame_limit(uint32_t micros);

// Caches the per-frame camera constants (sl::Constants needs them; mcdr_dlss_constants in mcdr_abi.h).
int32_t mcdr_dlss_set_constants(const mcdr_dlss_constants* constants);

// Tracked transitions: never write a barrier with a guessed "before" state. `endRenderPass` returns every
// attachment to COMMON, so a mismatched Before state is a debug-layer error and a TDR freeze.
using DlssTransitionFn = void (*)(void* ctx, void* resource, int32_t toState);
using DlssQueryStateFn = int32_t (*)(void* ctx, void* resource);
typedef struct DlssSdkResourceOps {
    void* (*acquire)(void* ctx, void* color, void* depth, void* motion, void* output);
    void (*release)(void* scope);
} DlssSdkResourceOps;
// Private textures are created BY the device module (this unit creates none): a resource created here is in
// nobody's state table. Destroy goes through the module's deferred-delete path (a bare Release() with an
// open command list reports "deleted prior to closing the command list"). `flags` is D3D12_RESOURCE_FLAGS
// bits; `dxgiFormat` is the DXGI_FORMAT VALUE, not the JVM format ordinal.
typedef struct DlssTextureDesc {
    uint32_t width;
    uint32_t height;
    uint32_t dxgiFormat;
    uint32_t mipLevels;
    uint32_t arraySize;
    uint32_t sampleCount;
    uint32_t flags;
} DlssTextureDesc;

// 返回 1 = *outObject / *outResource 都已填好；0 = 参数或设备不可用；-1 = 创建失败。
using DlssCreateTextureFn = int32_t (*)(void* ctx, const DlssTextureDesc* desc, void** outObject,
                                       void** outResource);
// `ctx` 只用于把资源从该 ctx 的跟踪表里摘掉，可以为 nullptr（shutdown 时就是这样）。
using DlssDestroyTextureFn = void (*)(void* ctx, void* object);
// 注入创建/销毁入口（模块在 wireDlssImpl() 里调一次）；1 = 两个入口都装上了。
int32_t mcdr_dlss_install_texture_ops(void* createFn, void* destroyFn);

int32_t mcdr_dlss_prepare(const struct mcdr_dlssd_frame* frame, const mcdr_dlss_input* in);
int32_t mcdr_dlss_prepare_shutdown(void);
int32_t mcdr_dlss_create(const struct mcdr_dlssd_frame* frame, const mcdr_dlss_input* in, DlssTransitionFn tl,
                         void* tlCtx);
int32_t mcdr_dlss_evaluate(const struct mcdr_dlssd_frame* frame, const mcdr_dlss_input* in, DlssTransitionFn tl,
                           void* tlCtx, const DlssSdkResourceOps* resourceOps);

// Depth snapshot. Called BEFORE vanilla's clearDepthTexture wipes the depth to 0.0 (reverse-Z "far"); the
// passed resource's state is restored exactly as found.
int32_t mcdr_dlss_snapshot_depth(void* commandList, void* depthResource, DlssTransitionFn tl,
                                 void* tlCtx, DlssQueryStateFn stateOf);
void mcdr_dlss_invalidate_depth_snapshot(void);

// Independent DLSS switch: records the state for the ABI query and the log only, never touching the loaded
// NGX runtime. The gate is the device module's applyDlssToTexture (gCtx.dlssEnabled). Idempotent.
int32_t mcdr_dlss_set_enabled(int32_t enabled);

// 释放特征与自建资源并 Shutdown NGX。必须在**设备还活着**时调用（destroyDevice 里）。
void mcdr_dlss_shutdown(void);

// Upgrades the native DXGI factory to Streamline's proxy factory in place; 1 = it is the proxy. With
// eUseDXGIFactoryProxy the caller must build the swapchain from the proxy (sl_core_api.h:265-275).
int32_t mcdr_sl_upgrade_interface(void** baseInterface);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MCDR_DLSS_IMPL_H
