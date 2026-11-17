#ifndef MCDR_OIT_NORMALIZATION_H
#define MCDR_OIT_NORMALIZATION_H

#include <stdint.h>

typedef struct mcdr_oit_normalization_pipeline_v1 {
    uint32_t schemaVersion, structSize, certificationVersion, targetCount;
    uint64_t programGeneration;
    uint8_t fragmentSourceHash[32], vertexSourceHash[32], contractHash[32];
    uint64_t reserved[2];
} mcdr_oit_normalization_pipeline_v1;

MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateOitNormalizationPipelineV1(
    const mcdr_pipeline_desc* descriptor, const mcdr_oit_normalization_pipeline_v1* certification);

typedef struct mcdr_oit_normalization_program_v1 {
    uint32_t schemaVersion, structSize, targetCount, reserved;
    uint64_t programGeneration;
    const void* laneDxil;
    uint64_t laneDxilBytes;
    const void* averageDxil;
    uint64_t averageDxilBytes;
} mcdr_oit_normalization_program_v1;

typedef struct mcdr_oit_normalization_region_v1 {
    uint32_t originX, originY, width, height;
} mcdr_oit_normalization_region_v1;

typedef struct mcdr_oit_normalization_batch_v1 {
    uint32_t schemaVersion, structSize;
    mcdr_frame_identity identity;
    uint64_t frameSerial, historyGeneration, programGeneration, totalBudgetBytes;
    uint32_t outputWidth, outputHeight, residentCount, reserved;
    mcdr_oit_normalization_region_v1 regions[16];
} mcdr_oit_normalization_batch_v1;

typedef struct mcdr_oit_normalization_lane_v1 {
    uint32_t schemaVersion, structSize, laneIndex, reserved;
    mcdr_prepared_binding q;
    mcdr_prepared_binding coefficients[4];
} mcdr_oit_normalization_lane_v1;

typedef struct mcdr_oit_normalization_info_v1 {
    uint32_t schemaVersion, structSize, recordedLaneMask, averageRecorded;
    uint32_t availability, unavailableReason, residentCount, targetCount;
    uint64_t frameSerial, programGeneration, publicationToken, reserved;
    mcdr_prepared_binding normalized, corrected;
} mcdr_oit_normalization_info_v1;

#define MCDR_OIT_NORMALIZATION_SUCCESS 1
#define MCDR_OIT_NORMALIZATION_RECORDED_UNVERIFIED 2
#define MCDR_OIT_NORMALIZATION_INVALID_REQUEST (-1)
#define MCDR_OIT_NORMALIZATION_UNSUPPORTED (-2)
#define MCDR_OIT_NORMALIZATION_BUDGET_EXCEEDED (-3)
#define MCDR_OIT_NORMALIZATION_BUSY (-4)
#define MCDR_OIT_NORMALIZATION_STALE (-5)
#define MCDR_OIT_NORMALIZATION_SUBMISSION_UNKNOWN (-6)
#define MCDR_OIT_NORMALIZATION_BACKEND_FAILURE (-7)
#define MCDR_OIT_NORMALIZATION_RECORDING_FAILED (-8)
#define MCDR_OIT_NORMALIZATION_UNAVAILABLE 0u
#define MCDR_OIT_NORMALIZATION_UNAVAILABLE_STAGE_EXECUTION 1u

#ifdef __cplusplus
static_assert(sizeof(mcdr_oit_normalization_pipeline_v1) == 136);
static_assert(sizeof(mcdr_oit_normalization_program_v1) == 56);
static_assert(sizeof(mcdr_oit_normalization_region_v1) == 16);
static_assert(sizeof(mcdr_oit_normalization_batch_v1) == 344);
static_assert(sizeof(mcdr_oit_normalization_lane_v1) == 96);
static_assert(sizeof(mcdr_oit_normalization_info_v1) == 96);
#endif

#endif
