#pragma once

// 全局模块片段专用头：每个 .cppm 在 `module;` 之后、`export module` 之前 include 本文件。
// 全局模块片段里的声明归属全局模块，不进任何具名模块的 purview，因此这些头与其中
// 的宏可以被模块接口使用，多份 include 也不产生 ODR 冲突（有 include guard）。
//
// 必须保持「头文件 + #include」，不要改成 import std;：
//   1. 普通 TU 仍在用 #include <...>；同一个 TU 里混用 import std; 与 #include <vector>
//      是非法组合（C2011 / C2953 / 致命错误 C1116），只能整体迁移，不能逐文件切。
//   2. import std; 不能出现在全局模块片段内（C5202），而本文件全部内容都在那里面。
// 两条约束互斥，与是否开启 ISO C++23 标准库模块无关。见 docs/misc/build-and-modules.md。

// 禁用 Windows min/max 宏：必须位于任何 windows.h/d3d12.h 之前。vcxproj 已全局定义
// NOMINMAX，这里再守一道，使本文件脱离属性表单独编译也安全（TreatWarningAsError=true
// 下 min/max 与 std::min 相撞是硬错误 C2589/C2059）。
#ifndef NOMINMAX
#define NOMINMAX
#endif

// ---------------------------------------------------------------------------
// Windows / COM / D3D12 / DXGI
// ---------------------------------------------------------------------------
#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>  // ID3D12InfoQueue / ID3D12Debug
#include <d3dcompiler.h>     // D3DCompile（vs_5_1 / ps_5_1 图形管线）
#include <dcomp.h>           // DirectComposition（composition swapchain 回退路径）
#include <dxcore_interface.h> // IDXCoreAdapterFactory / DXCoreAdapterProperty（adapterInfoJson 用）
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>       // IDXGIFactory6 / DXGI_GPU_PREFERENCE（按 GPU 偏好选适配器）
#include <wrl/client.h>

// ---------------------------------------------------------------------------
// C 运行库
// ---------------------------------------------------------------------------
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>  // mcdr::Result<T>（mcdr_error.cppm）；<string>/<utility> 见下方标准库段

// ---------------------------------------------------------------------------
// C++ 标准库
// ---------------------------------------------------------------------------
#include <array>
#include <atomic>
#include <climits>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 高频调试宏
// ---------------------------------------------------------------------------
// 具名模块不导出宏，所以 DBG_LOG_DEBUG 只能随本前置头走（全局模块片段里的宏对包含
// 它的 TU 全程可见）。唯一使用者是 src\c_abi\abi_command.cpp。
//
// 展开必须写全命名空间 `::d3d12_mc::dbgLogDebug`：该 TU 在 namespace 之外使用宏，
// 少了限定名在 DX12_DEBUG_VERBOSE 打开时报 C3861。
// #ifndef 守卫用于兼容另一种包含顺序（两处定义等价，重定义在 TreatWarningAsError 下是硬错误）。
#ifndef DBG_LOG_DEBUG
#ifdef DX12_DEBUG_VERBOSE
#define DBG_LOG_DEBUG(...) ::d3d12_mc::dbgLogDebug(__VA_ARGS__)
#else
#define DBG_LOG_DEBUG(...) ((void)0)
#endif
#endif
