#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mcdr_hdr_state_set_mode(int32_t mode);
int32_t mcdr_hdr_state_mode(void);

void mcdr_hdr_state_set_active(int32_t active);
int32_t mcdr_hdr_state_active(void);

void mcdr_hdr_state_set_applied_mode(int32_t mode);
int32_t mcdr_hdr_state_applied_mode(void);

#ifdef __cplusplus
}
#endif
