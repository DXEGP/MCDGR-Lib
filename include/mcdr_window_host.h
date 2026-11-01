// mcdr_window_host.h — D3D12 呈现宿主子窗口（C 接口），本层不碰 D3D12。交换链必须建在这个
// 自建、从未被 GL 挂过上下文的子窗口上：OSD 能否画出、独占全屏能否使用都取决于此，见
// docs\surface\README.md §4。普通 C ABI，供模块单元 d3d12_surface.cpp 在全局模块片段里 include。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 不透明句柄。同一父窗口重复 create 复用同一个子窗口（引用计数），不同父窗口返回
// 空指针（进程内只允许一个宿主）。
typedef struct MCDR_WindowHost MCDR_WindowHost;

// 创建（或复用）parentHwnd 的宿主。width/height 为**客户区**尺寸，失败返回空。
MCDR_WindowHost* mcdr_host_create(void* parentHwnd, int32_t width, int32_t height);

// 承载 swapchain 的子窗口 HWND —— 建链用这个，不是父窗口。
void* mcdr_host_child(MCDR_WindowHost* host);

// 同步子窗口几何到客户区尺寸（WM_SIZE 已自动调用一次，configureSurface 再调一次）。
void mcdr_host_resize(MCDR_WindowHost* host, int32_t width, int32_t height);

// 首帧 Present 成功后调用：SW_SHOWNA 显示子窗口（不抢焦点），幂等。
void mcdr_host_reveal(MCDR_WindowHost* host);

// 父窗口是否处于最小化/隐藏状态（Present 应跳过，避免 DXGI 丢弃帧）。目前没有调用方。
int32_t mcdr_host_minimized(MCDR_WindowHost* host);

// 建链前后摘掉/恢复调用线程的 WGL 上下文（有上下文时返回 1）：DXGI 拿到的 HWND 上
// 不能有当前 GL 上下文。
int32_t mcdr_host_suspend_gl(void);
void mcdr_host_restore_gl(void);

// 释放一个引用；最后一个引用归零时恢复父窗口过程并销毁子窗口。
void mcdr_host_release(MCDR_WindowHost* host);

#ifdef __cplusplus
}
#endif
