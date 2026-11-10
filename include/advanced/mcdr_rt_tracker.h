#pragma once

#include <stdint.h>

#include "mcdr_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

void mcdr_rt_track_transition(void* resource, uint32_t declaredBefore, uint32_t after,
                              uint32_t subresource);
void mcdr_rt_track_uav(void* resource);
void mcdr_rt_track_query(mcdr_rt_track_stats* out);
void mcdr_rt_track_reset(void);

#ifdef __cplusplus
}
#endif