#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mcdr_rt_state_set_mode(int32_t mode);
int32_t mcdr_rt_state_mode(void);

void mcdr_rt_state_set_gbuffer_ready(int32_t ready);
int32_t mcdr_rt_state_gbuffer_ready(void);

void mcdr_rt_state_set_gbuffer_size(int32_t width, int32_t height);
int32_t mcdr_rt_state_gbuffer_width(void);
int32_t mcdr_rt_state_gbuffer_height(void);

#ifdef __cplusplus
}
#endif
