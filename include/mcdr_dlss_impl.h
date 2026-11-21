#ifndef MCDR_DLSS_IMPL_H
#define MCDR_DLSS_IMPL_H

#include "mcdr_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

void mcdr_dlss_install_log(void* logInfo, void* logWarn, void* logError);

void mcdr_dlss_register_runtime_path(const char* path, int32_t cap);

const char* mcdr_dlss_runtime_path(void);

int32_t mcdr_dlss_can_run(void* device);

void mcdr_dlss_query(char* note, int32_t noteCap, char* optimal, int32_t optimalCap, char* runtime,
                     int32_t runtimeCap, uint32_t* flagsOut);

int32_t mcdr_dlss_register_vendor_dir(const char* utf8Dir, int32_t cap);

void* mcdr_dlss_load_runtime(void);

int32_t mcdr_dlss_request_quality(int32_t tier);

int32_t mcdr_dlss_request_preset(int32_t preset);

int32_t mcdr_dlss_set_output_texture(void* resource);

void mcdr_reflex_before_present(void);
void mcdr_reflex_after_present(void);
int32_t mcdr_dlss_streamline_requested(void);

void mcdr_dlss_note_sl_loaded(int32_t loaded);
int32_t mcdr_dlss_available(void);

void mcdr_dlss_set_fg_mode(int32_t mode, int32_t numFrames);
int32_t mcdr_dlss_prepare_fg(int32_t mode, int32_t numFrames, uint32_t displayWidth,
    uint32_t displayHeight, uint32_t guideWidth, uint32_t guideHeight,
    uint32_t* maxGeneratedFrames);
int32_t mcdr_dlss_fg_after_present(void);

int32_t mcdr_dlss_fg_presented(void);

int32_t mcdr_dlss_output_state(void);

void mcdr_reflex_set_mode(int32_t mode);
int32_t mcdr_reflex_available(void);

void mcdr_reflex_frame_begin(void);
void mcdr_reflex_marker(int32_t marker);

void mcdr_reflex_set_frame_limit(uint32_t micros);

int32_t mcdr_dlss_set_constants(const mcdr_dlss_constants* constants);
void mcdr_dlss_clear_camera_constants(void);

using DlssTransitionFn = void (*)(void* ctx, void* resource, int32_t toState);
using DlssQueryStateFn = int32_t (*)(void* ctx, void* resource);
int32_t mcdr_dlss_submit_display_frame(void* commandList, const mcdr_dlss_input* input,
    const mcdr_dlss_constants* camera, int32_t colorState, int32_t depthState,
    int32_t motionState, void* ui, int32_t uiState, uint64_t realFrameId, uint32_t guidesDilated,
    float frameTimeDeltaMs,
    DlssTransitionFn transitionFn, void* transitionContext);
typedef struct DlssTextureDesc {
    uint32_t width;
    uint32_t height;
    uint32_t dxgiFormat;
    uint32_t mipLevels;
    uint32_t arraySize;
    uint32_t sampleCount;
    uint32_t flags;
} DlssTextureDesc;

using DlssCreateTextureFn = int32_t (*)(void* ctx, const DlssTextureDesc* desc, void** outObject,
                                       void** outResource);
using DlssDestroyTextureFn = void (*)(void* ctx, void* object);
int32_t mcdr_dlss_install_texture_ops(void* createFn, void* destroyFn);
int32_t mcdr_dlss_install_recording_pin(void* pinFn);
int32_t mcdr_dlss_install_streamline_shutdown(void* shutdownFn);
int32_t mcdr_dlss_invalidate_inputs(void);
int32_t mcdr_dlss_inputs_ready_for_present(void);

int32_t mcdr_dlss_create(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                         void* tlCtx);
int32_t mcdr_dlss_prepare_sr(void* commandList, const mcdr_dlss_input* seed,
    const mcdr_raster_camera* camera, int32_t tier, int32_t preset,
    const int32_t resourceStates[5], void** retainedCandidateToken);
void mcdr_dlss_commit_sr(void* candidateToken, void** retiredToken);
int32_t mcdr_dlss_abort_sr(void* candidateToken);
int32_t mcdr_dlss_evaluate(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                           void* tlCtx);

int32_t mcdr_dlss_snapshot_depth(void* commandList, void* depthResource, DlssTransitionFn tl,
                                 void* tlCtx, DlssQueryStateFn stateOf);

int32_t mcdr_dlss_set_enabled(int32_t enabled);

void mcdr_dlss_shutdown(void);
int32_t mcdr_dlss_shutdown_checked(void);

int32_t mcdr_sl_upgrade_interface(void** baseInterface);

#ifdef __cplusplus
}
#endif

#endif

