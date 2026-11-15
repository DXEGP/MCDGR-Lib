# Surface：交换链 / 窗口宿主 / HDR 状态

`mcdr_dx12CreateSurface` 返回的 surface 是 native 侧唯一持有 DXGI swap chain 的对象：一条 flip-model 交换链、三张 back buffer、每张一个 RTV，以及（默认）一个自建宿主子窗口。设备层负责 device / queue / 命令列表与渲染线程，surface 只负责「把一张纹理翻到屏幕上」以及围绕它的窗口、色彩空间与状态位。

相关文档：[设备与命令层](../device/README.md)、[C ABI 约定](../abi/README.md)、[调试与日志](../misc/debug-and-logging.md)。

## 1. 概览

### 1.1 surface 持有什么

| 持有 | 不持有 |
| --- | --- |
| `IDXGISwapChain3` 及其三张 back buffer（`backBuffers`） | `ID3D12Device` / `ID3D12CommandQueue`（属于设备层，`ctx.device` / `ctx.queue`） |
| 每张 buffer 的 RTV 句柄（`rtvHandles`，一次性分配） | 命令列表与 fence 执行流（`ctx`，设备层渲染线程持有） |
| `presentWindow`（宿主子窗口）与 `host`（`MCDR_WindowHost*`） | 消息循环（由游戏 / SDL 跑，surface 只挂 WndProc） |
| 色彩空间与 HDR 派生值（`hdrActive` / `hdrSdrWhiteScale` / `hdrMaxLuminance`） | 设置层的取值（经 `mcdr_hdr_state` / `mcdr_surface_opts_state` 两个进程级 TU 传递） |
| 每张 buffer 的完成 fence（`surfaceFences`）与 present fence（`surfacePresentFences`） | 帧生成 / DLSS 的管线本体（只在建链处介入 wrap） |

字段定义在 `src/module/core/mcdr_device.cppm:1103`（`struct Dx12Surface`），缓冲区数量常量 `kSurfaceBufferCount = 3` 在 `src/module/core/mcdr_device.cppm:1101`。活动 surface 是全局单例：`setActiveSurface` / `getActiveSurface`（`src/module/core/mcdr_device.cppm:1165`），渲染线程的 present 分支直接取它。

### 1.2 与设备层的关系

- 建 surface 前必须先有 device 与 queue，否则 `createSurface` 直接失败（`src/core/d3d12_surface.cpp:536` 的入口检查）。
- 交换链用设备的 direct queue 建（`CreateSwapChainForHwnd(queue, …)`），所以 Present 与命令列表执行在同一队列上排队。
- `createSurface` **不**调用 `setActiveSurface`：必须等 `configureSurface` 建好 back buffer + RTV 之后才注册为活动 surface。提前注册会让渲染线程拿着一张没有 buffer 的 surface 去 acquire，`acquireSurface` 会一直报 `backBuffers empty (surface not configured yet)`。
- 异步渲染开启时（`gAsyncOwnsPresent` / `gAsyncRenderCtx`），`mcdr_dx12PresentSurface` 只清标志并返回，真正的 Present 发生在 `src/core/d3d12_device.cpp` 的渲染线程里（`presentSurface(getActiveSurface())`）。

### 1.3 一条完整路径

```
JVM 主线程                              native（src\core\d3d12_surface.cpp）
    │
    ├─ mcdr_dx12CreateSurface(hwnd) ──────────► createSurface                    :622
    │                                            ├─ HdrProbe → wantHdr / chainFormat
    │                                            ├─ mcdr_host_create（默认建子窗口）
    │                                            ├─ mcdr_host_suspend_gl（摘掉 WGL 上下文）
    │                                            ├─ createSwapChainForWindow（3 档 × 3 pass）
    │                                            ├─ mcdr_host_restore_gl
    │                                            ├─ CheckColorSpaceSupport → SetColorSpace1
    │                                            └─ mcdr_hdr_state_set_active(...)
    │
    ├─ mcdr_dx12ConfigureSurface(w, h, mode) ─► configureSurface                  :945
    │                                            ├─ 必要时 mcdr_host_resize / resizePresentationWindow
    │                                            ├─ waitForPendingPresents(2000) + deviceWaitIdle
    │                                            ├─ ResizeBuffers（最多 10 次，退避 1<<retry ms）
    │                                            │   └─ 失败 → 整链重建（createSwapChainForWindow）
    │                                            ├─ GetBuffer ×3 + CreateRenderTargetView
    │                                            └─ setActiveSurface(s) + fgOnResize
    │
每帧 │
    ├─ mcdr_dx12AcquireSurface ────────────────► acquireSurface                   :1420
    │                                            ├─ 消费 fgChainRebuildPending
    │                                            ├─ waitForPendingPresents(2000)
    │                                            ├─ GetCurrentBackBufferIndex → currentImageIndex
    │                                            └─ blittedThisFrame=false, bufferHeld=true
    ├─ mcdr_dx12BeginCommandList
    ├─ mcdr_dx12BlitSurface(ctx, surface, tex) ► blitSurface                     :1558
    │                                            ├─ back buffer 登记为 PRESENT
    │                                            ├─ PRESENT → RENDER_TARGET
    │                                            ├─ 全屏三角形 draw（SDR / HDR 两套 PSO）
    │                                            ├─ RENDER_TARGET → PRESENT
    │                                            └─ blittedThisFrame=true
    ├─ mcdr_dx12EndCommandList / Submit
    └─ mcdr_dx12PresentSurface ────────────────► presentSurface                  :1878
                                                 ├─ 未 blit 则直接返回
                                                 └─ Present(syncInterval, flags)
```

销毁：

```
mcdr_dx12DestroySurface ─► destroySurface                                        :1996
                            ├─ SetFullscreenState(FALSE)（若在独占全屏）
                            ├─ waitForRenderThreadSubmit + deviceWaitIdle
                            ├─ 等 present fence（上限 250 ms）
                            ├─ flushPendingDeletes + setActiveSurface(nullptr)
                            ├─ delete s
                            └─ mcdr_host_release(host) 或 DestroyWindow(presentWindow)
```

## 2. 交换链创建参数

### 2.1 建链参数

| 字段 | 取值 | 依据 |
| --- | --- | --- |
| `Width` / `Height` | 建链时传入的目标客户区尺寸；小于 1 的值回退为 1 | `src/core/d3d12_surface.cpp:446-447` |
| `Format` | `wantHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM` | `:679-680` |
| `BufferCount` | `kSurfaceBufferCount` = 3 | `src/module/core/mcdr_device.cppm:1101` |
| `SwapEffect` | 档 0/1 `DXGI_SWAP_EFFECT_FLIP_DISCARD`，档 2 `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL` | `:501-509` |
| `Flags` | 档 0 `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`，档 1/2 为 0 | 同上 |
| `Scaling` | `DXGI_SCALING_STRETCH`（画面不随窗口拉伸，客户区 1:1） | `:531` |
| `AlphaMode` | HWND 路径 `DXGI_ALPHA_MODE_UNSPECIFIED`；composition 路径 `DXGI_ALPHA_MODE_PREMULTIPLIED` | `:532`、`:173` |
| `SampleDesc.Count` | 1 | `:528` |
| `Stereo` | FALSE | `:527` |
| `BufferUsage` | `DXGI_USAGE_RENDER_TARGET_OUTPUT` | `:529` |

只支持 flip model（`FLIP_DISCARD` / `FLIP_SEQUENTIAL`）：bitblt 模型不接受 `ALLOW_TEARING`，其 back buffer 也不能作为 D3D12 的渲染目标。

### 2.2 三档回退

| 档 | SwapEffect | Flags | 何时落到这一档 |
| --- | --- | --- | --- |
| 0 | `FLIP_DISCARD` | `ALLOW_TEARING` | 默认首选；`mcdr_surface_opts_allow_tearing()` 为 0 时跳过 |
| 1 | `FLIP_DISCARD` | 0 | 档 0 被拒（驱动 / 远程桌面 / 虚拟机不支持 tearing） |
| 2 | `FLIP_SEQUENTIAL` | 0 | 老驱动不接受 `FLIP_DISCARD` |

每档之间 `pumpWindowMessages(pass * 60)`：第 2 pass 前抽 60 ms 消息，第 3 pass 前抽 120 ms，共 3 个 pass（`src/core/d3d12_surface.cpp:456`）。抽消息是因为部分驱动要等窗口完成一次激活才肯建链。

三档全部失败后回退 DirectComposition：`IDXGIFactory2::CreateSwapChainForComposition` + `dcomp.dll` 的 `DCompositionCreateDevice`（动态解析，不静态链接 `dcomp.lib`），descriptor 里 `SwapEffect = FLIP_SEQUENTIAL`、`AlphaMode = PREMULTIPLIED`、`Flags = 0`（`createCompositionSwapChain`，`:161`）。composition 链不支持 tearing，`bundle.allowTearing` 恒为 false。

成功建链后立即 `MakeWindowAssociation(chainWin, DXGI_MWA_NO_ALT_ENTER)`，只关 Alt+Enter 自动切全屏这一个行为；不要加 `DXGI_MWA_NO_WINDOW_CHANGES`，它会让后续 `SetFullscreenState` 失败并返回 `0x887A0022`。

### 2.3 tearing 的生效条件

四处同时满足才真正 tearing（少一处就退化成同步翻页）：

1. `mcdr_surface_opts_allow_tearing() != 0`（默认 1，设置层可关）；
2. 不是 composition 链（composition 无条件 `Flags = 0`）；
3. 建链落在档 0，即 `s->allowTearing == true`（记录在 `Dx12Surface::allowTearing`）；
4. Present 时 `presentMode == 0 && s->allowTearing && !s->exclusive`。

Present 参数由 `presentSurface` 按同一组条件算（`src/core/d3d12_surface.cpp:1592`）：

| `presentMode` | `syncInterval` | `Present` flags |
| --- | --- | --- |
| 0（IMMEDIATE） | `s->exclusive ? 1 : 0` | `s->allowTearing && !s->exclusive` 时 `DXGI_PRESENT_ALLOW_TEARING`，否则 0 |
| 1（MAILBOX） | 1 | 0 |
| 2（FIFO） | 1 | 0 |
| 3（FIFO_RELAXED） | 1 | 0 |

`mcdr_dx12SurfacePresentModes` 只对外暴露 `{0, 2, 3}`（`surfacePresentModes()`，`:856`）——DXGI flip 模型没有与 `VK_PRESENT_MODE_MAILBOX_KHR` 等价的模式，传 1 进来会落到 `FLAG_FIFO` 的同步行为。

`ResizeBuffers` 的 flags 必须与建链一致（`s->allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0`，composition 为 0），否则 `ResizeBuffers` 返回 `DXGI_ERROR_INVALID_CALL`。

tearing 与 OSD 互斥：RTSS / Afterburner 的文字只在 DWM 合成翻页时画得出来。把 `allow_tearing` 设 0 会让建链跳过档 0，链只能进合成翻页，OSD 恢复但失去 tearing；保持默认（1）则画面可 tearing，OSD 通常不出现。

### 2.4 颜色空间与 HDR 判定

```
hdrMode = mcdr_hdr_state_mode()             // 0 auto / 1 on / 2 off
probe   = probeHdrForWindow(factory, win)   // IDXGIOutput6::GetDesc1
wantHdr = (hdrMode == 1) || (hdrMode == 0 && probe.hdrOn)
chainFormat = wantHdr ? R16G16B16A16_FLOAT : R8G8B8A8_UNORM
```

| 量 | 判定 |
| --- | --- |
| `probe.hdrOn` | 覆盖窗口客户区中心的 `IDXGIOutput6` 的 `ColorSpace` 为 `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` 或 `_STUDIO_G2084_` 变体；找不到输出则为 0 |
| `probe.sdrWhiteNits` | `DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL / 1000`，取不到或 ≤ 1 时回退 200 |
| `s->hdrActive` | `wantHdr` 且 `SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)` 成功（要求 `CheckColorSpaceSupport` 返回 `DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT`） |
| `s->hdrSdrWhiteScale` | `hdrActive ? clamp(sdrWhiteNits / 80, 1.0f, 10.0f) : 1.0f`（scRGB 的 1.0 定为 80 nits） |
| `s->hdrMaxLuminance` | `probe.maxLuminanceNits`，只作记录与日志 |

`SetColorSpace1` 失败时**不重建**链：链保持 `R16G16B16A16_FLOAT`，但按 SDR 值直通写（`hdrActive = false`），日志记 `scRGB color space rejected`。此时 blit 走 SDR 常量分支，画面不暗也不炸，只是没有 HDR 亮度信息。

composition 链走同一段逻辑（没有 `compositionMode` 特判），但 composition 链通常拒绝 `CheckColorSpaceSupport` 的 PRESENT 位，结果仍是 `hdrActive = false`；代码中未明确该路径的实际驱动行为。

### 2.5 独占全屏

`setExclusiveFullscreen`（`src/core/d3d12_surface.cpp:701`）的前置条件是「顶层链」：`compositionMode` 为假，且 `host == nullptr`。动作顺序：

1. `SetFullscreenState(on, nullptr)`；
2. 进入时 `ShowWindow(SW_SHOW)` + `BringWindowToTop` + `SetForegroundWindow`（全屏切换会抢前台）；
3. `s->exclusive = on`，然后无条件 `configureSurface(s, width, height, presentMode, /*force*/ true)`——进入与退出都要重建 back buffer，否则停在切换前那一帧（`src/core/d3d12_surface.cpp`）。

> **缺陷**：`setExclusiveFullscreen` 在 `host != nullptr` 时直接返回错误（`exclusive fullscreen needs a top-level swap chain`）。默认 `force_present_child = 1` 会建宿主子窗口，因此默认配置下独占全屏必然失败；要进独占全屏必须先关掉 `force_present_child` 并重建 surface。

> **缺陷**：独占全屏下无法切换帧生成。`rebuildSwapChainForFg` 在 `s->exclusive` 时直接返回错误（`refused in exclusive fullscreen (rebuild + fullscreen can freeze)`），而 `acquireSurface` 会消费 `fgChainRebuildPending` 并据此返回错误——此时 surface 已被 `backBuffers.clear()` / `swapChain.Reset()`，画面冻结到退出全屏为止。

## 3. blit 通路

### 3.1 谁在写 back buffer

| 场景 | 写入路径 | 是否 present |
| --- | --- | --- |
| 常规帧（游戏 / 后处理 / UI 已合成） | `blitSurface(ctx, s, 源纹理)` | 是（`blittedThisFrame = true`） |
| 纯色（自检、清屏、加载黑屏） | `blitSurface(ctx, s, nullptr)` → `ClearRenderTargetView` | 否（见 §7） |
| 外部直写（拿到 back buffer 句柄自绘） | 外部自己 transition + 绘制 | 否（见 §7） |

`presentSurface` 的第一道判断是 `if (!s->blittedThisFrame || s->currentImageIndex < 0) return;`（`:1898`）。所以只有走过 `blitSurface` 的 draw 分支，帧才会被翻页。

### 3.2 blit 为什么是 draw 而不是拷贝

`blitSurface` 用全屏三角形 + 像素着色器做，而不是 `CopyTextureRegion`，因为一条路径内要同时处理：尺寸不一致（源分辨率 ≠ back buffer）、格式不一致（`R8G8B8A8_UNORM` / `R16G16B16A16_FLOAT` 等）、sRGB → scRGB 的编码差异。`CopyTextureRegion` 三者都不做。

完整步骤（`src/core/d3d12_surface.cpp:1300`）：

| # | 动作 | 备注 |
| --- | --- | --- |
| 1 | 校验 `ctx->commandList` 存在、`currentImageIndex >= 0`、列表已打开 | 报错文案分别是 `no command list` / `no acquired back buffer` / `command list not open` |
| 2 | 把三张 back buffer 登记进 `ctx->resourceState`，状态记为 `PRESENT` | 只在表里没有该资源时插入；状态表跨帧存活，避免重建后状态漂移 |
| 3 | `srcTex != nullptr` 时先把源纹理迁到 `COPY_SOURCE` | 之后由 `blitBindSourceTexture` 建 SRV |
| 4 | 目标 `PRESENT → RENDER_TARGET`，`OMSetRenderTargets` 绑定 RTV | viewport / scissor 取自 back buffer 的 `GetDesc()` |
| 5 | 懒初始化 `initBlitPipeline()`，按 `s->hdrActive` 选 `psoHdr` 或 `pso` | 两个 PSO 共享同一根签名与同一套 VB/IB |
| 6 | 设根签名、写 4 个 32 位根常量到参数 1、绑定源 SRV 描述符表、`DrawIndexedInstanced(6, 1, 0, 0, 0)` | 6 个索引 = 两个三角形 |
| 7 | 目标 `RENDER_TARGET → PRESENT`，`OMSetRenderTargets(0, …)` 解绑，源纹理迁回 `COMMON` | 必须解绑：不解绑时同一条列表里后续的写状态会与 RTV 冲突 |
| 8 | `lastBlitIndex = currentImageIndex`，`blittedThisFrame = true` | 这两个字段决定 present 是否发生 |

`SetGraphicsRoot32BitConstants(1, 4, params, 0)` 写入的四个值是：

| 链类型 | 四元组 |
| --- | --- |
| SDR | `{1.0f, 0.0f, 0.0f, 0.0f}` |
| HDR（scRGB） | `{s->hdrSdrWhiteScale, 1.0f, 0.0f, 0.0f}` |

### 3.3 sRGB 与 HDR10 的格式转换点

转换只发生在像素着色器里，且只在 HDR 链上发生：

| 链格式 | PSO | 行为 |
| --- | --- | --- |
| `R8G8B8A8_UNORM`（SDR） | `pso` | `kBlitPS` 采样直出，值按 sRGB 编码原样写入 UNORM（back buffer **不是** `_SRGB` 格式，硬件不做自动编码） |
| `R16G16B16A16_FLOAT` + scRGB | `psoHdr` | `kBlitPSHdr`：`srgbToLinear(c.rgb) * sdrWhiteScale`，alpha 直通 |

`kBlitPSHdr` 的 cbuffer 是 `register(b0, space0)` 的 4 个 float，与根签名参数 1 的 4 个 32 位常量一一对应；`hdrEnabled < 0.5` 时原样返回，因此同一个 PSO 也能在「float 链但 HDR 关闭」时当直通用（`src/core/d3d12_device.cpp`）。

sRGB 解码是标准分段函数：

```
srgbToLinear(c) = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)
```

采样器是根签名里的静态 sampler（`s0`：`LINEAR` / `CLAMP` / `MinLOD 0`），不占描述符堆。

### 3.4 相关辅助

| 函数 | 用途 | 与 `blitSurface` 的差异 |
| --- | --- | --- |
| `blitTextureToTexture(ctx, src, dst)`（`:1750`） | 纹理到纹理的缩放拷贝（后处理、超分输入） | 不做 back buffer 的状态登记与解绑；常量恒为 SDR 四元组；要求 `dst` 是 `TEXTURE2D` 且 `SampleDesc.Count == 1` |
| `sharpenTextureToTexture(ctx, src, dst, …)`（`:1812`） | 锐化 | 额外传 `{1/w, 1/h, amount, 0}`，前两个分量是纹素尺寸 |
| `getActiveColorTextureHandle(ctx)`（`:1551`） | 取当前 render pass 的第一个颜色附件 | 只在 `ctx->inRenderPass` 且附件非空时有值；对应 ABI `mcdr_dx12GetActiveColorTexture` |

## 4. 窗口宿主

### 4.1 为什么需要

DXGI flip-model 链挂在哪个 HWND 上，决定三件事：

1. **覆盖层能不能画**：注入式叠加层（RTSS / Afterburner）在 GL 的早期加载窗口上能画 OSD，但等 NeoForge 销毁它、改由 SDL 建主窗口、再把交换链挂到那个顶层窗口上之后，OSD 就再也不出现。链挂到自建子窗口上、父窗口仍是那个顶层窗口时，OSD 恢复——这是唯一稳定可复现的差异点（另受 §2.3 的 tearing 取舍影响）。
2. **OpenGL 污染**：游戏窗口可能被 WGL 上下文挂过（早期启动阶段），同一个 HWND 上再建 DXGI 链会被拒绝（`E_ACCESSDENIED`）或黑屏。宿主子窗口是全新的窗口，从未被 GL 碰过。
3. **客户区 1:1**：子窗口 `WS_CHILD | WS_CLIPSIBLINGS`、无边框无标题，客户区就是画面尺寸，捕获软件按客户区取画面即可。

所以在 `createSurface` 里，默认（`force_present_child != 0`）先用 `mcdr_host_create(gameHwnd, w, h)` 拿一个子窗口，链挂在子窗口上；`s->hwnd` 仍记游戏窗口，`s->presentWindow` 记子窗口。

### 4.2 三种窗口归属

| 归属 | 何时 | 特点 |
| --- | --- | --- |
| 宿主子窗口（默认） | `mcdr_surface_opts_force_present_child() != 0` 且 `mcdr_host_create` 成功 | OSD 可观察（受 §2.3 的 tearing 取舍影响）；客户区 1:1；独占全屏不可用 |
| 顶层游戏窗口 | `force_present_child == 0`，或子窗口建链失败后自动回退 | 独占全屏可用；游戏窗口被 GL 挂过时可能建链失败 |
| DirectComposition | 前两者都建链失败 | 不依赖 HWND 的 present 路径；`allowTearing` 恒 false；已建的子窗口会被释放 |

回退顺序在 `createSurface` 里是硬编码的：先子窗口，失败则 `mcdr_host_release` 后在游戏窗口重试，仍失败再走 composition；走 composition 时若建过子窗口，先 `mcdr_host_release` 再把 `presentWindow` 置空。

### 4.3 API（`include/mcdr_window_host.h`）

| 函数 | 作用 | 约束 |
| --- | --- | --- |
| `mcdr_host_create(void* parentHwnd, int32_t w, int32_t h)` | 建宿主子窗口并子类化父窗口 | 同一父窗口复用（引用计数 +1）；**不同**父窗口返回 `nullptr`（进程内只允许一个宿主） |
| `mcdr_host_child(MCDR_WindowHost*)` | 取子窗口 HWND | 建链用这个，不是父窗口 |
| `mcdr_host_resize(host, w, h)` | 子窗口跟随客户区尺寸 | `SetWindowPos` 用 `SWP_NOACTIVATE` / `SWP_NOZORDER` / `SWP_NOCOPYBITS` / `SWP_DEFERERASE` |
| `mcdr_host_reveal(host)` | 首帧上屏后显示子窗口 | `PostMessage(kMsgReveal)`，`SW_SHOWNA`——不抢焦点；幂等 |
| `mcdr_host_minimized(host)` | 父窗口是否最小化 | 目前没有调用方（见 §7） |
| `mcdr_host_suspend_gl` / `mcdr_host_restore_gl` | 建链期间摘掉并恢复调用线程的 WGL 上下文 | 返回值是原 DC；`wglMakeCurrent(nullptr, nullptr)` 之后 DXGI 拿到的 HWND 上一定没有当前 GL 上下文 |
| `mcdr_host_release(host)` | 引用计数 -1，归零时恢复父窗口 WndProc 并销毁子窗口 | 只能释放 `create` 返回的指针 |

### 4.4 谁跑消息循环

宿主**不**创建线程、**不**跑 `GetMessage` 循环：

- 游戏 / SDL 在自己的线程泵消息，宿主只是被泵到的一部分。
- 子窗口的 WndProc 是 `hostChildProc`；父窗口的 WndProc 被 `SetWindowLongPtrW(GWLP_WNDPROC)` 换成 `hostParentProc`，原指针存在父窗口的 `SetPropW` 属性里（属性名 `MCDR_D3D12_SeamlessHostState`）。
- 渲染线程只 `PostMessage`（`kMsgReveal = WM_APP + 0x4D1`），把「可以显示了」这个动作交回窗口所属线程，避免跨线程 `ShowWindow`。

`hostParentProc` 关心的消息：

| 消息 | 处理 |
| --- | --- |
| `WM_SIZE` | `SIZE_MINIMIZED` 置 `minimized = 1`；否则按 `LOWORD/HIWORD` 调 resize |
| `WM_SHOWWINDOW` | `wParam != 0` 清 `minimized`，否则置 1 |
| `WM_NCDESTROY` | `RemovePropW` + 恢复原 WndProc，再链到原处理函数 |
| 其它 | `CallWindowProc(prevProc, …)` 原样转发 |

`hostChildProc` 故意让子窗口对鼠标完全透明，否则它会吃掉游戏的点击：

| 消息 | 返回 |
| --- | --- |
| `WM_NCHITTEST` | `HTTRANSPARENT` |
| `WM_MOUSEACTIVATE` | `MA_NOACTIVATE` |
| `WM_ERASEBKGND` | 1（不擦背景，避免闪白） |
| `WM_PAINT` | `BeginPaint` / `EndPaint` 空转 |
| `kMsgReveal` | `ShowWindow(SW_SHOWNA)` + `SetWindowPos(HWND_TOP, …)`，标志为 `SWP_NOACTIVATE` / `SWP_SHOWWINDOW`；只执行一次 |

窗口类 `L"MCDR_D3D12_SeamlessHost"` 的 style 是 `CS_HREDRAW | CS_VREDRAW`，`hbrBackground = nullptr`，**没有** `CS_OWNDC`。

### 4.5 引用计数与销毁

`mcdr_host_create` 在已有宿主时：父窗口相同则 `refs++` 并 resize，返回同一个指针；父窗口不同则返回 `nullptr`。`mcdr_host_release` 做 `refs.fetch_sub(1, acq_rel)`，大于 1 直接返回，归零才恢复 WndProc、`RemovePropW`、`DestroyWindow(child)`、`delete host`。

因此：**一个进程只有一套宿主窗口**，按父窗口区分。同一父窗口上的第二条 surface（自检、调试）共享同一个子窗口；不同父窗口的第二次 `create` 会拿到 `nullptr`。

### 4.6 隐藏窗口（offscreen）

`mcdr_dx12CreateHiddenWindow(width, height)`（实现在 `src/core/d3d12_device.cpp:2629`）建的是另一种东西：

| 项 | 宿主子窗口 | 隐藏窗口 |
| --- | --- | --- |
| 类名 | `MCDR_D3D12_SeamlessHost` | `Dx12HiddenTest` |
| 窗口样式 | `WS_CHILD` / `WS_CLIPSIBLINGS` | `WS_POPUP` |
| 可见性 | 建时隐藏，首帧 Present 成功后 reveal | 建完立即 `ShowWindow(SW_HIDE)` + `UpdateWindow` |
| WndProc | `hostChildProc`（透明、防抢焦点） | `DefWindowProcW` |
| 归属 | 挂在游戏窗口下 | 独立顶层窗口，不属于任何 surface |
| 用途 | 承载交换链、对齐客户区 | 只需一个 HWND 的场景（能力探测、`CreateSwapChainForHwnd` 参数校验） |

隐藏窗口**不**参与 present：`mcdr_dx12CreateHiddenWindow` 只返回 HWND，不建 surface。用完必须 `mcdr_dx12DestroyHiddenWindow(hwnd)`（内部 `DestroyWindow` + `UnregisterClass`），否则窗口类泄漏。

## 5. HDR 状态与 surface opts 状态机

### 5.1 字段

两个状态都是进程级、无锁 atomic，定义在普通 TU（不是模块单元），因为 DLSS 侧（普通 TU）要读它们。

`include/mcdr_hdr_state.h` + `src/advanced/d3d12_hdr_state.cpp`：

| 字段 | 取值 | 含义 |
| --- | --- | --- |
| `mode` | 0 / 1 / 2 | 0 = auto（按 DXGI 输出色彩空间判定）、1 = 强制开、2 = 强制关；越界值夹到 0 |
| `active` | 0 / 1 | 最近一次建成的链是否真的以 scRGB 呈现 |
| `applied_mode` | -1 / 0 / 1 / 2 | 最近一次建链时读到的 `mode`；-1 表示还没建过链 |

`include/mcdr_surface_opts_state.h` + `src/core/d3d12_surface_opts_state.cpp`：

| 字段 | 默认 | 读取点 |
| --- | --- | --- |
| `allow_tearing` | 1 | 建链档 0 的开关（`src/core/d3d12_surface.cpp:468`）；已建成的链不回读，改值只影响下一次建链 / 重建 |
| `force_present_child` | 1 | `createSurface` 里是否建宿主子窗口（`:697`）；同样只影响下一次建链 |

### 5.2 合法迁移

```
             mcdr_dx12SetHdrMode(0|1|2)
mode  ────────────────────────────────►  只改进程状态，不碰当前链
  │
  └─(createSurface)─► wantHdr = f(mode, probe.hdrOn)
                         │
                         ├─ SetColorSpace1 成功 → active = 1, applied_mode = mode
                         └─ SetColorSpace1 失败 → active = 0, applied_mode = mode
```

| 操作 | 允许 | 效果 |
| --- | --- | --- |
| 任意时刻 `mcdr_dx12SetHdrMode(0/1/2)` | 是 | 写 `mode`，返回后立即读 `mode` 得到新值；`active` / `applied_mode` 不变 |
| 建链时 `mode == 0` 且 `probe.hdrOn` | 是 | `wantHdr = true` |
| 建链时 `mode == 2` | 是 | `wantHdr = false`，即使输出是 HDR |
| 运行中把 `mode` 从 1 改成 2 | 是 | 当前链仍是 float 格式、仍以 scRGB 呈现，直到下一次建链 / 重建 |
| 直接改 `active` | 否 | 无公开接口，只有建链路径写它 |

`applied_mode` 让设置层区分「用户要求 HDR」与「实际生效的 HDR」：`-1` 表示 surface 还没建（设置界面在加载期查询时就是 -1）。

### 5.3 与设备层 / 设置层的交互

```
设置层（JVM）                          native
  │
  ├─ mcdr_dx12SetHdrMode(mode) ───────► mcdr_hdr_state_set_mode  （仅状态）
  ├─ mcdr_dx12SetSwapchainOptions(a,b) ► mcdr_surface_opts_*     （仅状态）
  │
  └─ 下次 mcdr_dx12CreateSurface / ConfigureSurface
                                       ├─ 读 mode + probe → wantHdr / chainFormat
                                       ├─ 读 allow_tearing → 档 0 是否带 ALLOW_TEARING
                                       ├─ 读 force_present_child → 是否建宿主子窗口
                                       ├─ 写 active / applied_mode
                                       └─ 设备层：hdrActiveFlag() = mcdr_hdr_state_active()
                                                  └─ DLSS：sl::DLSSOptions::colorBuffersHDR
```

- 设备层通过 `hdrActiveFlag()`（`src/core/d3d12_surface.cpp:1655`）拿 surface 的结果，不自己探测色彩空间。
- DLSS 在评估前读 `mcdr_hdr_state_active()`；与上次不同则重设 `colorBuffersHDR` 并重建评估选项（`src/advanced/d3d12_dlss.cpp:2300`、`:2758`、`:2770`）。
- 设置层顺序固定：先 `SetHdrMode` / `SetSwapchainOptions`，再建 surface；反序则这一轮仍按旧值建链。

## 6. 像素回读与 back buffer 句柄

### 6.1 回读接口

| ABI | 输出 | 说明 |
| --- | --- | --- |
| `mcdr_dx12ReadbackSurfacePixels(surface, int32_t* out)` | 36 个 int32 | 当前 back buffer 的 3×3 采样（9 像素 × RGBA） |
| `mcdr_dx12ReadbackTexturePixels(tex, out)` | 36 个 int32 | 从 `COMMON` 迁移后读 |
| `mcdr_dx12ReadbackTexturePixelsIn(tex, fromState, out)` | 36 个 int32 | 调用方给出真实的前置状态 |
| `mcdr_dx12DumpTextureToFile(tex, tag)` | 文件 | 纹理必须是 `COMMON` 且不在 render pass 内；落 BMP |

`mcdr_dx12ReadbackSurfacePixels` 的内部步骤（`src/core/d3d12_surface.cpp:1743`）：

1. `deviceWaitIdle()`；
2. 选目标 buffer：`currentImageIndex`，无效则 `lastBlitIndex`，再无效则 `GetCurrentBackBufferIndex()`；
3. 静态 staging 资源（`READBACK` 堆），行距按 256 字节对齐；
4. `COMMON → COPY_SOURCE` 屏障（`PRESENT` 与 `COMMON` 在本 SDK 版本数值都是 0，flip model 下等价）；
5. `CopyTextureRegion`，footprint 固定写 `DXGI_FORMAT_R8G8B8A8_UNORM`；
6. `COPY_SOURCE → PRESENT`，`Close` + `ExecuteCommandLists` + `Signal(++queueFenceValue)` + 等 fence（5 秒超时）；
7. `Map`，按 3×3 网格 `{0, w/2, w-1} × {0, h/2, h-1}` 取值，DEBUG 日志打中心像素；
8. `dbgDumpPixelsToFile(…, "backbuf")` 落 BMP + ASCII 缩略图，`Unmap`。

`ReadbackTexturePixelsIn` 的通用路径（`src/c_abi/abi_surface.cpp` 的 `readbackSamples36`，`bpp` 选择表从 `:329` 起）按格式选字节宽度：

| 源格式 | `bpp` | 复制用格式 |
| --- | --- | --- |
| `R8G8B8A8_*` / `B8G8R8A8_*` | 4 | `R8G8B8A8_UNORM` |
| `R16G16B16A16_*` | 8 | `R16G16B16A16_UNORM` |
| `R32G32B32A32_*` | 16 | `R32G32_FLOAT` |
| `R8_UNORM` 家族 | 1 | 源格式本身 |
| 其它 | 4 | `R8G8B8A8_UNORM` |

### 6.2 调用约束（以代码为准）

| 约束 | 是否有代码强制 |
| --- | --- |
| 必须在 present 之后 | **否**。`acquire` 之后、`present` 之前读的是本帧正在写的那张；`present` 之后 `currentImageIndex` 已置 -1，回退到 `lastBlitIndex`（刚翻走的那张），内容可能已被下一页覆盖 |
| 必须同线程 | **否**。函数内部 `deviceWaitIdle` 并自建列表与 fence，没有线程断言。只要设备上没有别的线程正在 `ExecuteCommandLists`，任何线程都能调 |
| 不能在 render pass 内 | `ReadbackTexturePixelsIn` 依赖调用方给出的 `fromState` 真实；`DumpTextureToFile` 硬性要求 `COMMON` 且不在 pass 内，否则读回未定义（实测恒 0） |
| 尺寸上限 | 是。宽或高为 0 或 > 16384 直接失败 |
| 采样点固定 | 是。3×3 网格，不接受自定义区域 |
| HDR 链 | **不支持**（见 §7） |

### 6.3 back buffer 句柄

`mcdr_dx12GetBackBufferHandle` → `getBackBufferHandle`（`src/core/d3d12_surface.cpp:1285`）返回的是**裸 `ID3D12Resource*`**，不是 `mcdr_object*`；调用方只能把它当不透明句柄转交，不能当纹理对象传给其它 ABI。

| 约束 | 说明 |
| --- | --- |
| 有效窗口 | `acquire` 成功之后、`present` 之前（`0 <= currentImageIndex < backBuffers.size()`），否则返回 0 |
| 所有权 | 不转移。不要 `Release`，不要 `AddRef` 后长期持有 |
| 资源状态 | 处于 `PRESENT`。外部要写它必须先迁到 `RENDER_TARGET`，写完迁回；代码中未明确外部如何迁移（这条路径没有对外暴露状态迁移 ABI） |
| 失效时机 | `present` 把 `currentImageIndex` 置 -1，句柄立刻失效；`configureSurface` 重建 buffer 后旧指针全部失效 |

## 7. 已知限制与缺陷

> **缺陷**：HDR 链下像素回读不可用。`readbackSurfacePixels` 与 `readbackSamples36` 都把行距按 `w * 4`、footprint 按 `DXGI_FORMAT_R8G8B8A8_UNORM` 写死（`src/core/d3d12_surface.cpp:1778`、`src/c_abi/abi_surface.cpp:416`），而 HDR 链是 `R16G16B16A16_FLOAT`（8 字节/像素）。读回得到错位数据且不报错。

> **缺陷**：`blitSurface` 的纯色分支提前返回，不置 `blittedThisFrame`，因此「清一帧黑」不会翻页（`src/core/d3d12_surface.cpp:1351`）。

> **缺陷**：直写 back buffer（`mcdr_dx12GetBackBufferHandle`）的路径没有置 `blittedThisFrame`，`presentSurface` 会静默跳过翻页，画面冻结。

> **缺陷**：默认 `force_present_child = 1` 时独占全屏必然失败（`setExclusiveFullscreen` 要求 `host == nullptr`），且失败只回错误码，界面层没有兜底提示。

> **缺陷**：`configureSurface` 的非法尺寸启发式会误拒合法切换：宽相同且 `height > curH * 2` 时直接返回错误、不改链（`src/core/d3d12_surface.cpp:824`）。例如 854×480 → 854×1080 会被当成无效回调。

> **缺陷**：`ResizeBuffers` 与整链重建各失败一次后，surface 会停在「有 swap chain 但 `backBuffers` 为空」或「`swapChain == nullptr`」的状态，`acquireSurface` 持续报 `backBuffers empty (surface not configured yet)`，代码不会自动重试，只能销毁重建。

> **缺陷**：`destroySurface` 等 present fence 只用 250 ms 超时（`src/core/d3d12_surface.cpp`），超时后照常 `delete`。慢刷新率 / DWM 忙时会在 display controller 仍在读 back buffer 时释放资源。

> **缺陷**：`mcdr_host_minimized` 没有调用方。最小化时仍每帧 `Present`，DXGI 返回 `DXGI_STATUS_OCCLUDED`，`presentSurface` 只把 `suboptimal` 置 1，不节省 GPU 时间。

> **缺陷**：一个进程只允许一套宿主窗口（`mcdr_host_create` 对不同父窗口返回 `nullptr`）。需要第二条独立呈现链、且父窗口不同的场景（第二窗口 / 独立自检窗口）拿不到宿主，只能退回顶层窗口或 composition。

> **缺陷**：`blitSurface` 在 render pass 内被调用时只打一条警告（最多 3 次）然后照常执行，重绑 RTV 会让该 pass 的附件失效，末尾也不恢复原附件。

> **缺陷**：composition 链不支持 tearing，也不允许 `SetFullscreenState`。三条回退路径里它最差，但失败信息只在建链日志里，运行期不再提示。

> **限制**：`s->hdrActive` 只表示「本次建链时 scRGB 是否被接受」。驱动在运行中切换 HDR 开关不会重建链，只有 `IsSurfaceSuboptimal` 或尺寸变化才会。

## 8. 文件导航

路径相对 `MCDR\`。

| 文件 | 内容 |
| --- | --- |
| `src/core/d3d12_surface.cpp` | surface 主体：建链、HDR 探测、configure、acquire、blit、present、readback、destroy |
| `src/core/d3d12_window_host.cpp` | 宿主子窗口实现：窗口类、父窗口子类化、reveal、GL 上下文摘挂、引用计数 |
| `include/mcdr_window_host.h` | 宿主窗口 API 与约束 |
| `src/advanced/d3d12_hdr_state.cpp` | HDR 三字段的进程级状态 |
| `include/mcdr_hdr_state.h` | HDR 状态 API |
| `src/core/d3d12_surface_opts_state.cpp` | `allow_tearing` / `force_present_child` 的进程级状态 |
| `include/mcdr_surface_opts_state.h` | 上者的 API |
| `src/c_abi/abi_surface.cpp` | 全部 surface 相关 ABI 入口与 36 int32 回读实现 |
| `include/mcdr_abi.h` | `mcdr_surface` 句柄、surface 入口声明与约定 |
| `src/module/core/mcdr_device.cppm` | `struct Dx12Surface`、`kSurfaceBufferCount`、活动 surface 单例、blit PSO 创建 |
| `src/core/d3d12_device.cpp` | blit 着色器源码、隐藏窗口、渲染线程内的 Present |
| `docs/surface/README.md` | 本文 |

## 文档维护

1. 改 `DXGI_SWAP_CHAIN_DESC1` 的任一字段（`Format` / `BufferCount` / `SwapEffect` / `Flags` / `Scaling`）时，同步更新 §2.1 表格与 §2.3 的 tearing 条件。
2. 增加或调整回退档位（含 composition 兜底）时，同步更新 §2.2 与 §4.2 的窗口归属表。
3. 修改 blit 的 PSO、根签名或根常量布局时，同步更新 §3.2 的步骤表与 §3.3 的格式转换表。
4. 增删 `mcdr_host_*` 函数、改动 reveal / 最小化行为时，同步更新 §4.3 与 §7 中对应条目。
5. `mcdr_dx12SetHdrMode` / `mcdr_dx12SetSwapchainOptions` 的生效时机若从「下次建链」改为运行中可切换，必须重写 §5.2 的迁移表与 §7 的最后一条。
