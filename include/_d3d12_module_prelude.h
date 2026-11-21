#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxcore_interface.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>

#include <array>
#include <atomic>
#include <climits>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef DBG_LOG_DEBUG
#ifdef DX12_DEBUG_VERBOSE
#define DBG_LOG_DEBUG(...) ::d3d12_mc::dbgLogDebug(__VA_ARGS__)
#else
#define DBG_LOG_DEBUG(...) ((void)0)
#endif
#endif
