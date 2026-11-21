#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mcdr_surface_opts_set_allow_tearing(int32_t allow);
int32_t mcdr_surface_opts_allow_tearing(void);

void mcdr_surface_opts_set_force_present_child(int32_t force);
int32_t mcdr_surface_opts_force_present_child(void);

#ifdef __cplusplus
}
#endif
