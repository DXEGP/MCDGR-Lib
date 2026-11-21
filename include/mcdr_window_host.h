#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MCDR_WindowHost MCDR_WindowHost;

MCDR_WindowHost* mcdr_host_create(void* parentHwnd, int32_t width, int32_t height);

void* mcdr_host_child(MCDR_WindowHost* host);

void mcdr_host_resize(MCDR_WindowHost* host, int32_t width, int32_t height);

void mcdr_host_reveal(MCDR_WindowHost* host);

int32_t mcdr_host_prepare_release(MCDR_WindowHost* host);

int32_t mcdr_host_minimized(MCDR_WindowHost* host);

int32_t mcdr_host_suspend_gl(void);
void mcdr_host_restore_gl(void);

void mcdr_host_release(MCDR_WindowHost* host);

#ifdef __cplusplus
}
#endif
