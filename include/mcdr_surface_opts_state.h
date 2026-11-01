// mcdr_surface_opts_state.h — 交换链建链选项的进程级状态（普通 TU 定义，理由同
// mcdr_hdr_state.h）。改值只影响下一次建链 / 重建，已建成的链不回读。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0 = 不允许 tearing（只用 FLIP_DISCARD / FLIP_SEQUENTIAL，链进 DWM 合成翻页，
// RTSS/Afterburner 的 OSD 才画得出来），非 0 = 允许（默认）。
void mcdr_surface_opts_set_allow_tearing(int32_t allow);
int32_t mcdr_surface_opts_allow_tearing(void);

// 0 = 建在顶层游戏窗口上，非 0 = 建在自建呈现子窗口上（默认）。
void mcdr_surface_opts_set_force_present_child(int32_t force);
int32_t mcdr_surface_opts_force_present_child(void);

#ifdef __cplusplus
}
#endif
