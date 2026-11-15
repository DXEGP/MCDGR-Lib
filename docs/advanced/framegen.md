# 帧生成（frame generation）native 实现

帧生成（frame generation，下文 FG）在两次应用渲染帧之间插入 1–N 张由 SDK 生成的中间帧：应用照旧渲染、
照旧 `Present`，插出来的帧由后端在呈现路径上补齐。三条后端是 DLSS-G（NVIDIA Streamline / NGX）、
FSR-FG（AMD FidelityFX）与 XeSS-FG（Intel XeSS-FG + XeLL）。

本文只覆盖 native 侧（`MCDR.dll`）：抽象层、三条 provider、逐帧数据流、资源所有权与失败路径。
JVM 侧如何判断"本帧有没有 3D 世界层"、HUD-less 与 UI 两张目标怎么建，不在本文范围内。

---

## 1. 抽象层是什么、为什么需要

### 1.1 三家的集成形态本来就不同

| 后端 | 谁执行 `Present` | 交换链来自哪里 | 链归属枚举 |
| --- | --- | --- | --- |
| DLSS-G | Streamline 在 `Present` 内部插帧 | 工厂升级成 SL 代理工厂，链由它创建 | `MCDR_FG_CHAIN_SL_PROXY` |
| FSR-FG | FFX 包装后的 proxy 链 | 应用原生链交给 `ffxCreateContextDescFrameGenerationSwapChainWrapDX12` | `MCDR_FG_CHAIN_FSR_WRAP` |
| XeSS-FG | `xefg_swapchain` 包装后的链 | 应用原生链交给 `xefgSwapChainD3D12InitFromSwapChain` | `MCDR_FG_CHAIN_XESS_WRAP` |
| 无 FG | 应用自己的链 | `IDXGIFactory2::CreateSwapChainForHwnd` | `MCDR_FG_CHAIN_APP_NATIVE` |

SL 的 `eUseDXGIFactoryProxy`（"链由我建"）与 FSR/XeSS 的"包装你已有的链"是两种互斥的集成方式。
所以抽象层不能只抽"插帧调用"这一件事，还必须把**链的策略**一起抽出来：谁建链、要不要交给 SDK 包装、
什么时候解包、失败后链归谁。

### 1.2 分层

```
JVM 侧（FFM，只用 mcdr_fg_* / mcdr_dx12Fg*）
   │  Pick / QueryCaps / SetMode / SetConstants / SetUiTexture
   │  SetUiComposition / UiCompositionReady / Presented / Status / State
   ▼
C ABI 适配             src\c_abi\abi_framegen.cpp        ← 不含任何厂商名
   │  fgPickBackend / fgFillCaps / fgSetMode / fgSetFrameConstants / fgTagFrameInputs
   ▼
抽象层注册表           src\advanced\d3d12_framegen.cpp    （FgRegistry + IFrameGenProvider）
   ├─ DlssGFrameGenProvider     同文件            → mcdr_dlss_set_fg_mode / mcdr_dlss_fg_presented
   ├─ FsrFgFrameGenProvider     d3d12_fsffg.cpp   → amd_fidelityfx_framegeneration_dx12.dll
   └─ XessFgFrameGenProvider    d3d12_xessfg.cpp  → libxess_fg.dll + libxell.dll
   ▼
交换链与呈现           src\core\d3d12_surface.cpp
   createDxgiFactory / wrapChainForFrameGen / acquireSurface / rebuildSwapChainForFg / presentSurface
```

`IFrameGenProvider` 声明在模块接口单元 `src\module\core\mcdr_device.cppm:892`（`FgBackendId` 在 :1207，
`FgFrameInputs` 在 :1225）。三个 provider 是同一个模块 `mcdr.d3d12.device` 的三个实现 TU；厂商头
（`ffx_api*.h` / `xefg_swapchain*.h` / `xell*.h`）只在这三个 TU 内可见，不进模块接口。

### 1.3 共同接口

| 能力 | 方法 | 调用点 | 时机 |
| --- | --- | --- | --- |
| 能力查询 | `fillCaps(mcdr_fg_caps&)` | `fgFillCaps`（`fgAfterPresent` 每帧也调） | 任意线程；**不得**建 context |
| 选后端 | `fgPickBackend(int32_t)` | `mcdr_dx12FgPick` | 纯记录，不建不销毁 |
| 模式切换 | `setMode(const mcdr_fg_dispatch&)` | `fgSetMode` | 只记录，不下发 |
| 帧首 | `frameStart()` | `fgFrameStart` ← `acquireSurface` | 链重建消费之后、本帧开画之前 |
| 逐帧输入 | `tagFrameInputs(const FgFrameInputs&)` | `fgTagFrameInputs` | 录制期，命令列表还开着 |
| 帧常量 | `setFrameConstants(const mcdr_fg_constants&)` | `fgSetFrameConstants` | 任意线程，注册表回放 |
| UI 纹理 | `setUiTexture(void* resource)` | `fgSetUiTexture` | 目标创建或重建时一次 |
| UI 合成开关 | `setUiComposition(int32_t)` / `uiCompositionReady()` | `fgSetUiComposition` / `fgUiCompositionReady` | 每帧读，作 latch |
| Present 前 | `beforePresent()` | `fgBeforePresent` | `Present` 之前、Reflex sleep 之前 |
| Present 后 | `afterPresent()` | `fgAfterPresent` | `Present` 返回之后 |
| 插帧结果 | `presented()` | `mcdr_dx12FgPresented` | 任意线程；1 = 没插帧 |
| 包 / 解链 | `wrapSwapChain(...)` / `unwrapSwapChain()` | `fgWrapPresentChain` / `fgUnwrapPresentChain` | 建链 / 销毁链之前 |
| 尺寸变更 | `onResize(w,h)` | `fgOnResize` | `ResizeBuffers` 之后 |
| 销毁 | `shutdown()` | `ensureProviderLocked` / `fgReleaseProviderWithWait` | 释放前 |

四条硬约束：

* provider **不得**自己 `Present`。链的所有权在 `Dx12Surface`，provider 只回答"换成哪条链"
  （`wrapSwapChain` 的出参）和"Present 前后各做什么"。
* `setMode` 只写期望值。真正的 SDK 下发在渲染线程（DLSS-G 在 `beforePresent`，FSR-FG / XeSS-FG 在
  `tagFrameInputs`），因为选项的生效点是"下一次 Present"。
* 同一时刻只有一个 provider（`FgRegistry::provider` 是 `std::unique_ptr<IFrameGenProvider>`）。
* provider 不得假设自己独占 GPU 队列：FSR-FG 的 `WrapDX12` 直接吃 surface 的 `gameQueue`（`d3d12_fsffg.cpp:579-619`）。

### 1.4 两种呈现形态

```
形态 A：DLSS-G —— SL 在 Present 内部插帧
  app render ──► blit ──► Present ──► [ SL：插帧 + 上屏 ]
                             ▲
                             └ 链 = SL 代理工厂建的链（chainOwner = SL_PROXY）

形态 B：FSR-FG / XeSS-FG —— SDK 包装应用链
  app render ──► blit ──► Present ──► [ proxy chain：插帧 + 合成 UI + 上屏 ]
                             ▲
                             └ 链 = SDK 交回的 proxy 链（chainOwner = *_WRAP）
```

`chainOwner` 是四值枚举，与宏一一对应：`MCDR_FG_CHAIN_APP_NATIVE`(0)、`MCDR_FG_CHAIN_SL_PROXY`(1)、
`MCDR_FG_CHAIN_FSR_WRAP`(2)、`MCDR_FG_CHAIN_XESS_WRAP`(3)。写点只有三处：
`createDxgiFactory`（`d3d12_surface.cpp:75-85`，`fgNoteChainOwner`）、
`wrapChainForFrameGen`（`:939`、`:941`）、`fgNoteChainRebuildDone` / `fgAbortWrapAfterFailure`。
provider 改不动它，只能读 `caps.chainOwner`。

---

## 2. 每帧数据流

一帧经过四个时刻，顺序固定：

```
acquireSurface（帧首）                          d3d12_surface.cpp:1211-1225
  1. fgChainRebuildPending()? ──是──► rebuildSwapChainForFg(s, "fg-toggle")
  2. fgNoteChainRebuildDone(fgChainOwner(), ok, why)
  3. fgFrameStart()                              → provider->frameStart()
  4. GetCurrentBackBufferIndex / waitForPendingPresents

录制期
  5. SR 成功后 fgTagFrameInputs(fgMakeFrameInputs(ctx, desc))      d3d12_sr.cpp:361
     或 mcdr_fgTagDlssInputs(...) → fgTagFrameInputs(...)         d3d12_dlss.cpp:2086
     └─ FSR-FG 在这里做 Configure + PrepareV2（命令列表还开着）

presentSurface                                  d3d12_surface.cpp:1592-1627
  6. fgBeforePresent()                           → provider->beforePresent()
  7. mcdr_reflex_before_present()
  8. swapChain->Present(syncInterval, flags)
  9. mcdr_reflex_after_present()
 10. fgAfterPresent()                            → provider->afterPresent() + 注册表记账/回滚
```

第 6–10 步的顺序是刻意排的：`fgBeforePresent` 先下发本帧选项（SL 的生效点是下一次 Present），
Reflex 的 sleep 夹在中间，`fgAfterPresent` 只记账、**不碰资源、不等 GPU**。

### 2.1 Present 之前必须就位的东西

| 输入 | 字段 | 谁准备 | 缺了会怎样 |
| --- | --- | --- | --- |
| 世界层颜色（HUD-less） | `FgFrameInputs::pColorOutput` | JVM（SR 输出或显示分辨率目标） | FSR 打 `fsr-fg: waiting for frame inputs` 并跳过本帧；DLSS-G 从 SL 的 SR 输出取 |
| 深度 | `pDepth` + `depthState` | JVM | 同上（空资源会让 SDK 内部崩） |
| 运动矢量（motion vector） | `pMotionVectors` + `motionVectorState` | JVM | 同上 |
| 相机 | `cameraNear` / `cameraFar` / `cameraVerticalFov` | JVM，逐帧 | FSR 用成员默认值 0.05 / 1024 / 1.0 rad |
| 帧时 | `frameTimeDeltaMs` | JVM | 默认 16.6 |
| jitter / MV 缩放 | `mcdr_fg_constants::jitterX/Y`、`mvScaleX/Y` | JVM | FSR 用 0 与 1.0；XeSS 无帧常量就不 tag |
| View / Projection | `constants.view[16]` / `projection[16]` | JVM | 只有 XeSS-FG 用（见 §4） |
| UI-only 纹理 | `setUiTexture` 的 `object` | JVM（GUI 合成目标） | 只能走"应用自己把 UI 画进 backbuffer"的老路径 |
| 三轨帧号 | `appFrameId` / `backendFrameId` / `presentId` | 注册表 + provider | 跳号会被 SDK 静默丢帧 |

`FgFrameInputs` 还带三个资源**当前状态**（`colorState` / `depthState` / `motionVectorState`），必须填真实值：
SDK 按 `incomingState` 记屏障，写错会让资源实际状态与本仓的跟踪表错位。两条入口给的默认值不同，别混用：

* SR 通用路径 `fgMakeFrameInputs`（`d3d12_framegen.cpp`）：颜色 `RENDER_TARGET`，
  深度 `DEPTH_READ`，MV `NON_PIXEL_SHADER_RESOURCE`。
* DLSS 路径（`d3d12_dlss.cpp:2090-2094`）：颜色 `RENDER_TARGET`，深度与 MV 都是
  `NON_PIXEL_SHADER_RESOURCE`（SR 结束时它们是只读 SRV）。

FSR-FG 对"还没齐"与"句柄已过期"两种情况分开处理，都在 `applyFrameLocked` 里，且各只记一次日志：

| 情况 | 判据 | 动作 | 日志（`dbgLogWarn`，`logOnce`） |
| --- | --- | --- | --- |
| 输入未齐 | `hudlessColor_` / `depth_` / `motionVectors_` 任一为 `nullptr` | `applied_ = false`，跳过 | `fsr-fg: waiting for frame inputs (color=%p depth=%p mv=%p)`（:428） |
| 句柄已释放 | `resourceIsUsable()` 的 `__try/__except` 探测 `GetDesc()` 失败 | 下发 `Configure(false)`，`applied_ = false` | `fsr-fg: frame inputs were released under us (color=%p depth=%p mv=%p bad=%p)`（:462） |
| UI 纹理已释放 | 同一探测 | 丢掉 `uiTexture_`，退回整帧 HUDless 插值，不连累插帧 | `fsr-fg: ui texture %p was released, dropping it`（:444） |

### 2.2 三条后端在哪个时刻"录入"

| 后端 | 录制期（命令列表开着） | Present 前 | Present 后 |
| --- | --- | --- | --- |
| DLSS-G | 不录（SL 自己录） | `mcdr_dlss_set_fg_mode`，仅在变化时 | `mcdr_dlss_fg_presented()` |
| FSR-FG | `Configure` + `PrepareV2`（dilate / optical-flow 的 GPU job 录进**本帧**列表） | 只做 UI 资源注册/撤销 + 停用时收尾 | 空实现（插帧在 SDK 的 Present 里） |
| XeSS-FG | tag 深度 / MV / HUDLESS / UI + `SetEnabled` / `SetNumInterpolatedFrames` | 发 `XELL_RENDERSUBMIT_END` + `XELL_PRESENT_START` | `XELL_PRESENT_END` + `GetLastPresentStatus` |

FSR-FG **不能**把 `PrepareV2` 挪到 `beforePresent`：那时命令列表早已 `Close` + `Execute`，往里录东西非法
（`d3d12_fsffg.cpp` 的注释即此约束）。

### 2.3 插帧完成的同步点

| 同步点 | 触发方式 | 位置 |
| --- | --- | --- |
| FSR-FG 插帧 dispatch | swapchain 在 `Present` 内部回调 `onFrameGenerationDispatch`，把 header 转给 `ffxDispatch` | `d3d12_fsffg.cpp:171`、`:526-528` |
| FSR-FG 呈现排空 | `ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12`，只在销毁 context 时发 | `d3d12_fsffg.cpp:738` |
| FSR-FG 插了几帧 | `params->numGeneratedFrames` 夹到 0..3，`presented_ = generated + 1` | `d3d12_fsffg.cpp:182` |
| XeSS-FG 插了几帧 | `xefgSwapChainGetLastPresentStatus` → `status.framesPresented`（0 时按 1） | `d3d12_xessfg.cpp:330` |
| DLSS-G 插了几帧 | `mcdr_dlss_fg_presented()` | `d3d12_framegen.cpp` |

只有销毁链时才真正等待 GPU（FSR 的 `WaitForPresents`，以及 `fgReleaseProviderWithWait` 的
unwrap → shutdown 顺序，`d3d12_framegen.cpp`）。

---

## 3. 三条后端的差异

### 3.1 运行库与入口

| 项 | DLSS-G | FSR-FG | XeSS-FG |
| --- | --- | --- | --- |
| 运行库 | `nvngx_dlssg.dll`，经 Streamline 的 `sl.interposer` | `amd_fidelityfx_framegeneration_dx12.dll` | `libxess_fg.dll`；低延迟另需 `libxell.dll` |
| 加载 | `mcdr_dlss_available()` / `mcdr_dlss_streamline_requested()` | `LoadLibraryW`：本模块目录 → 裸名 | 同左（`loadLocked`，`d3d12_xessfg.cpp:200`） |
| 必需入口 | 由 `d3d12_dlss.cpp` 解析 | `CreateContext` / `DestroyContext` / `Configure` / `Dispatch` | `createContext` / `buildPipelines` / `initFromSwapChain` / `getSwapChainPtr` / `setEnabled` / `setPresentId` / `destroy` |
| 入口缺失 | `note = "DLSS runtime not loaded"` | `note = "framegeneration dll entry pts"`，日志 `fsr-fg: %s`（:731） | `note = "libxess_fg.dll missing entry points"` |

### 3.2 输入与倍率

| 项 | DLSS-G | FSR-FG | XeSS-FG |
| --- | --- | --- | --- |
| `needsReflex` | 1 | 0 | 0 |
| `needsHudless` | 0（复用 SL 的 SR 输出） | 1 | 1 |
| `needsUiResource` | 0（SL 自己合成） | 1 | 1 |
| `transitionsInputs` | 0（SL 自己转屏障） | 1 | 1 |
| `maxGeneratedFrames` | 5（`slDLSSGGetState.numFramesToGenerateMax`） | 2 | `maxInterpMirror()`，未建 context 时报 1 |
| `latencyProvider` | `MCDR_FG_LATENCY_REFLEX` | `MCDR_FG_LATENCY_ANTILAG2`（架构位，无 DLL） | `MCDR_FG_LATENCY_XELL` |
| 倍率下发 | `mcdr_dlss_set_fg_mode(mode, frames)` | 不设倍率：SDK 自己决定，回调里报 `numGeneratedFrames` | `SetNumInterpolatedFrames(n)`，n 由 `beforePresent` 夹到 `maxInterp_` |
| UI 合成 | SL 在 Present 路径上做 | `RegisterUiResourceDX12` + `flags`（premul alpha + 内部双缓冲，`d3d12_fsffg.cpp:216`） | `SetUiCompositionState(ENABLED)` + tag `RES_UI` |
| 额外前置 | SR 必须是 DLSS 或未选 | `loaded && kWrapWired` | `loaded && kWrapWired && kTaggingWired` + XeLL 可用 |
| 深度约定旗标 | 由 SL 侧处理 | `FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED` | `XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH` |

DLSS-G 的 `available` 还要求 SR 后端是 DLSS 或未选（`fillDlssGCaps`，`d3d12_framegen.cpp`）：
HUD-less 与 `ScalingOutputColor` 都取自 SL 的超分输出，SR 换成 FSR/XeSS 后插帧没有输入。

`caps.note` 只有 28 字节（27 字符 + NUL，`mcdr_abi.h:516-532`），所有原因串必须压到 27 字符以内。
三家的 note 由各自的 `fillXxxCaps` 写，`fillCaps` 层还可能把它换成最近一次失败原因（§5.3）。

---

## 4. 相机、矩阵与转置约定

### 4.1 载荷

`mcdr_fg_constants`（`include\mcdr_abi.h:541-551`）：

```c
typedef struct mcdr_fg_constants
{
    float view[16];
    float projection[16];
    float jitterX;
    float jitterY;
    float mvScaleX;
    float mvScaleY;
    int32_t resetHistory;
    float frameRenderTime;
} mcdr_fg_constants;
```

约定（`mcdr_abi.h`）：

* **行主序（row-major）**，与 XeSS-FG 的 `xefg_swapchain_frame_constant_data_t` 一致；JVM 侧直接交，
  native **不做转置**。
* View / Projection **不含 jitter**；jitter 单独走 `jitterX` / `jitterY`。
* 这套常量归 FG 抽象层自己，不去读 DLSS 的 `mcdr_dlss_constants`（那是另一条通路的私有状态）。

XeSS-FG 的搬运是逐元素拷贝，没有任何数学变换（`TagFrameConstants`，`d3d12_xessfg.cpp`）：

```cpp
frameConstants.viewMatrix[i]       = constants_.view[i];
frameConstants.projectionMatrix[i] = constants_.projection[i];
frameConstants.jitterOffsetX       = constants_.jitterX;
frameConstants.motionVectorScaleX  = constants_.mvScaleX;
frameConstants.resetHistory        = constants_.resetHistory != 0 ? 1u : 0u;
api_.tagFrameConstants(handle_, presentId, &frameConstants);
```

所以矩阵约定的唯一责任方是 JVM：native 既不做 `transpose`，也不做行/列互换。把列主序矩阵塞进来会得到
一份"数值合法、语义错误"的常量 —— SDK 不会报错，只表现为插帧重影。

### 4.2 各后端实际用到哪些字段

| 字段 | DLSS-G | FSR-FG | XeSS-FG |
| --- | --- | --- | --- |
| `view[16]` / `projection[16]` | 不用本结构 | **不用**（`PrepareV2` 无对应字段） | 逐帧 tag |
| `jitterX` / `jitterY` | 不用本结构 | `prep.jitterOffset.x/y` | `jitterOffsetX/Y` |
| `mvScaleX` / `mvScaleY` | 不用本结构 | `prep.motionVectorScale.x/y` | `motionVectorScaleX/Y` |
| `resetHistory` | 不用本结构 | `prep.reset`（发一次后置 0） | `resetHistory` |
| `frameRenderTime` | 不用本结构 | 不用 | `frameRenderTime` |

FSR-FG 的 `setFrameConstants` 只取 jitter 与 MV 缩放（`d3d12_fsffg.cpp:533`），其余字段丢弃。
相机参数走逐帧 `FgFrameInputs`（`d3d12_fsffg.cpp:445-448`）：

```
prep.cameraNear              = cameraNear_
prep.cameraFar               = cameraFar_
prep.cameraFovAngleVertical  = cameraFovVertical_      // 弧度
prep.viewSpaceToMetersFactor = viewSpaceToMeters_
prep.frameTimeDelta          = frameTimeDeltaMs_
```

上游给 0 或非法值时保留成员默认值（`cameraNear_ = 0.05f`、`cameraFar_ = 1024.0f`、
`cameraFovVertical_ = 1.0f`、`viewSpaceToMeters_ = 1.0f`、`frameTimeDeltaMs_ = 16.6f`，
`d3d12_fsffg.cpp:801-810` 与 :702-712 的过滤条件 `cameraNear > 0 && cameraFar > cameraNear`）。

### 4.3 深度约定

本仓 reverse-Z（近 1、远 0）且 Far 面有限，所以：

* FSR-FG：`fgDesc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED`，**不**加
  `..._ENABLE_DEPTH_INFINITE`（`d3d12_fsffg.cpp:579`）。
* XeSS-FG：`kInitFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH | XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV`
  （`d3d12_xessfg.cpp:98`）。`BuildPipelines` 与 `InitFromSwapChain` 必须吃**同一个**常量，两处共用
  `kInitFlags`（:557、:583 附近）。

格式替换只在一处做：FFX 要求深度可建 SRV，而深度资源常是 `R32_TYPELESS`，所以在 `PrepareV2` 前按
`R32_FLOAT` 重新描述（`d3d12_fsffg.cpp`）。XeSS-FG 不做格式替换，tag 调用方给的原始资源
（`d3d12_xessfg.cpp` 的注释记录了这个取舍）。

`renderSize` 以深度/MV 的实际尺寸为准：FFX 要求它与资源描述一致，否则 `PrepareV2` 返回 0x1，
所以代码在 `prep.depth.description` 有效时覆盖 `renderSize`（`d3d12_fsffg.cpp`）。

---

## 5. 动态倍率与状态机

### 5.1 三层状态

| 名字 | 含义 | 写入点 |
| --- | --- | --- |
| `desiredMode` | 上层要什么（0 = 关，其余 = 开） | `fgSetMode` |
| `chainMode` | 链是在哪个 mode 下建成的 | `fgNoteChainRebuildDone` |
| `provider.appliedMode_` / `mode_` | 已下发给 SDK 的值 | `beforePresent`（DLSS）/ `tagFrameInputs`（FSR、XeSS） |

`mcdr_dx12FgState()` 把其中两项与"有没有活着的 provider""重建次数"压进一个 int32
（`fgStatePacked`，`d3d12_framegen.cpp`）：

```
[ 0.. 7] desiredMode   0 = 关，非 0 = 开
[ 8..15] chainMode     链建成时的 mode
[16..23] active        1 = 现在有一个活着的 provider
[24..31] rebuilds      min(重建次数, 255)
```

位布局是契约：改它必须同步 `include\mcdr_abi.h` 的声明与 JVM 侧读法。

### 5.2 一次只推一帧

* `setMode` 只写期望值。下发点分散在渲染线程：DLSS-G 的 `mcdr_dlss_set_fg_mode` 在 `beforePresent`
  且仅在变化时；FSR 的 `Configure` / `PrepareV2` 在 `tagFrameInputs`；XeSS 的 `SetEnabled` /
  `SetNumInterpolatedFrames` 在 `tagFrameInputs` 且用 `appliedMode_` / `appliedFrames_` 门控
  （`d3d12_xessfg.cpp`）。每帧重设会被 SDK 当成状态抖动。
* **顺序不能反**：XeSS 在 `enabled` 之前设的插帧数会被忽略（`d3d12_xessfg.cpp` 的注释）。
* 链重建两段式：`fgSetMode` 只挂 `rebuildPending`（任意线程），`acquireSurface` 帧首消费
  （`d3d12_surface.cpp:1211-1225`）。放进 `Present` 会丢掉刚画完的那一帧。
* 只有**链家族不符**才重建（`d3d12_framegen.cpp`）：

```
needWrap   = wantOn && wantsWrap && (!chainIsWrap || !chainFamilyMatches)
needUnwrap = chainIsWrap && !wantsWrap
```

  关掉开关**不**解包：包装链在关闭态仍合法，SDK 支持整局保持包装、只把插帧关掉。反过来"每次开关都
  重建"会在同一条 HWND 上反复易主。

`mode = 1` 与 `mode = 2` 在 native 侧**走同一条路径**：倍率仍取 `desc.numFrames`（`fgSetMode` 里
`desiredFrames = numFrames < 1 ? 1 : numFrames`）。dynamic 的自动倍率计算不在本层（代码中未明确 native
侧有该算法），上层必须继续逐帧推 `numFrames`。

### 5.3 能力表没有 TTL 缓存

`fillCaps` 每次调用都重算，不缓存 —— 代价只是读几个原子量与运行库句柄。真正的"缓存"是四块
**文件级原子镜像**，写侧在锁内、读侧不加锁（`std::mutex` 不可重入，`fillCaps` 会在持锁路径里被调到）：

| 镜像 | 置真 | 置假 | 用途 |
| --- | --- | --- | --- |
| `gDeviceSupported`（FSR） | `wrapSwapChain` 建成两个 context | `destroyContextsLocked` | `caps.deviceSupported` |
| `contextReady()`（XeSS） | `wrapSwapChain` 末尾 | `unwrapSwapChain` | `caps.deviceSupported` |
| `maxInterpMirror()`（XeSS） | `GetProperties` 拿到 `maxSupportedInterpolations` | `unwrapSwapChain` | `caps.maxGeneratedFrames` |
| `gChainOwnerSnapshot` | `fgNoteChainOwner` / `fgNoteChainRebuildDone` / `fgAbortWrapAfterFailure` | 同上 | `fgChainOwnerFast()` |

失效与覆盖规则：

* `caps.note` 可能被 `lastReason` **替换**（`fgFillCaps`，`d3d12_framegen.cpp`），条件是
  `picked != applied`，或包装型后端的链家族不满足。所以读到的 note 不一定是 `fillXxxCaps` 写的那句。
* 连续 60 帧（`ROLLBACK_FRAMES`）`available = 0` 就回滚：`shutdown()` + 清期望 + 挂链重建。
* 期望"开"但 provider 建不出来时，`fgBeforePresent` 每 30 帧（`ENSURE_RETRY_FRAMES`）重试一次；
  过 300 帧（`USABLE_GRACE_FRAMES`）仍没有 provider 才 warn 一次。

---

## 6. 资源所有权与生命周期

### 6.1 谁创建、谁清屏、谁回收

| 资源 | 创建者 | native 持有形态 | 清屏 | 回收 |
| --- | --- | --- | --- | --- |
| HUD-less 世界层颜色 | JVM（`D3D12GpuTexture`） | 借用 `ID3D12Resource*`，不持有引用 | 不用清（每帧重画） | JVM |
| 深度 / motion vector | JVM | 同上 | 不用清 | JVM |
| UI-only 纹理 | JVM（GUI 合成目标） | 借用同一份句柄 | 清成 `(0,0,0,0)` + 预乘 alpha，由 JVM 负责 | JVM |
| 交换链 | `Dx12Surface` | 包装时 `Detach()` 交出，接管 proxy 的引用 | — | `Dx12Surface` |
| XeSS 临时堆 | `d3d12_xessfg.cpp` 的 `gTempBufferHeap` / `gTempTextureHeap` | 自有 `ComPtr`，所有权不出该 TU | — | 进程退出 |
| XeLL 上下文 | `xellD3D12CreateContext` | 进程级单例 `gXellContext` | — | 不销毁（必须活得比 XeSS-FG context 久） |

native **不创建也不清屏** HUD-less 与 UI 纹理。UI 目标"清成 (0,0,0,0) + 预乘 alpha"是 JVM 侧 GUI 目标的
约定，FSR-FG 只是按这个前提登记 `FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA` 与
`..._ENABLE_INTERNAL_UI_DOUBLE_BUFFERING`（`d3d12_fsffg.cpp:216`）。清屏所在的行不在 native 侧
（代码中未明确）。

### 6.2 句柄解包

JVM 交来的是 `Dx12Object*`（`D3D12GpuTexture.handle()` 的值），不是 `ID3D12Resource*`。唯一解包点
`unwrapDx12Resource`（`d3d12_framegen.cpp`）：

```cpp
auto* wrapper = static_cast<Dx12Object*>(object);
return wrapper != nullptr ? wrapper->resource.Get() : nullptr;
```

注册表存 `Dx12Object*`（期望值），传给 provider 的是解包后的资源指针，两条路径都不能省：
`fgMakeFrameInputs` 解包三个（`:1080-1107`），`ensureProviderLocked` 回放 `uiTexture` 时再解包一次
（`:366-479`）。

### 6.3 交换链写权

* 包装型后端要求"引用计数 = 1"：`wrapChainForFrameGen` 先 `s->swapChain.Detach()` 再交出去
  （`d3d12_surface.cpp:756`）。用 `Get()` 交一条我们仍持有的链会让 `InitFromSwapChain` 失败。
* proxy 链是**另一个对象**，`xefgSwapChainD3D12GetSwapChainPtr` 交出的引用已加过，直接 `Attach`，
  不能再 `QueryInterface` 一次 —— 多出的引用会让 `xefgSwapChainDestroy` 报
  `Swap Chain reference counter is not 0`（`d3d12_surface.cpp` 的注释即此约束）。
* 失败时链已被 SDK 释放，surface 必须按"没有链"处理：`wrapChainForFrameGen` 返回 -1 并打
  `frame generation did not take the app chain; the swap chain is gone and must be rebuilt`（:898），
  调用方随后 `fgAbortWrapAfterFailure` 重建纯应用链（`:1209-1215`、`:1376-1381`）。

### 6.4 销毁顺序

```
fgReleaseProviderWithWait（d3d12_framegen.cpp）
  provider->unwrapSwapChain()   ← 先解包：SDK context 绑在链上
  provider->shutdown()          ← 再关：DLSS 显式下发 eOff，FSR/XeSS 落 enabled=0
  provider.reset()
```

只调 `shutdown` 会把 FFX / xefg 的 context 留在进程里，下一个 provider 再 `CreateContext` 时 SDK 侧还带着
上一条链的 latency-reduction / swapchain 状态。

FSR-FG 内部（`destroyContextsLocked`，`d3d12_fsffg.cpp:738`）：`Configure(enabled=false)` →
`...SwapChainWaitForPresentsDX12` → `DestroyContext(fg)` → `DestroyContext(swap)`。
XeSS-FG（`d3d12_xessfg.cpp:747`）：清 `uiTexture_` / `uiEnabled_` / `maxInterp_` →
`xefgSwapChainDestroy`，且调用点必须先 `swapChain.Reset()` + 清 back buffer 列表（`d3d12_surface.cpp`）。

provider 在 `present` 之后**不回收任何东西**：back buffer 的所有权在 `Present` 返回时已交还 DXGI，
`presentSurface` 自己重置 `currentImageIndex = -1`、`blittedThisFrame = false`、`bufferHeld = false`
（`d3d12_surface.cpp:1592-1627`）。

---

## 7. 失败与降级

### 7.1 返回值约定

| 入口 | 返回 | 含义 |
| --- | --- | --- |
| `mcdr_dx12FgPick` | 1 / 0 | 已记录 / 拒绝（原因进 status；链已被接管型后端占用时为 `restart to switch FG`） |
| `mcdr_dx12FgQueryCaps` | 1 / -1 | 表已填 / 参数非法（`out == nullptr` 或 backend 不在 0..3） |
| `mcdr_dx12FgSetMode` | 1 / 0 / -1 | 已记录 / 后端不可用或参数被注册表拒 / `desc == nullptr`（`abi_framegen.cpp:45`） |
| `mcdr_dx12FgPresented` | ≥1 | 上次 `Present` 真正提交的帧数，1 = 没插帧 |
| `mcdr_dx12FgStatus` | ≥0 / -1 | 写入长度（NUL 结尾）/ 参数非法；`noteCap == 0` 只返回所需长度（`abi_framegen.cpp:58`） |
| `mcdr_dx12FgUiCompositionReady` | 1 / 0 | provider 确认"UI 会被合成" / 拿不准，调用方继续自己合成 UI |
| `fgWrapPresentChain` | 1 / 0 | `*outPresentChain` 是 proxy 链 / 不包装（含不可用、FG 关闭） |

`mcdr_dx12FgSetMode` 的返回值**不代表已生效**：生效看 `mcdr_dx12FgPresented` 与
`mcdr_dx12FgState` 的 `active` 位。

### 7.2 后端不可用

| 场景 | native 行为 | 日志行（原样） |
| --- | --- | --- |
| 能力不足 | provider 不建，`lastReason = caps.note` | `provider %d unavailable: %s`（`d3d12_framegen.cpp:334`） |
| FSR 运行库缺失 | `loadLocked` 返回 0 | `fsr-fg: %s`，problem 为 `framegeneration dll missing`（`d3d12_fsffg.cpp:591`） |
| FSR 入口缺失 | `loadLocked` 返回 0 | 同上，problem 为 `framegeneration dll entry pts` |
| FSR 链不是 `IDXGISwapChain4` | 跳过包装 | `fsr-fg: app chain is not IDXGISwapChain4, skip wrap`（:743） |
| FSR 包链失败 | `Release()` 后返回 0 | `fsr-fg: swapchain wrap failed 0x%X`（:762） |
| FSR FG context 失败 | 销毁 swap context 后返回 0 | `fsr-fg: frame generation context failed 0x%X`（:802） |
| FSR 初始 Configure 失败 | 继续（下一帧会重下发） | `fsr-fg: initial configure failed 0x%X`（:848） |
| XeSS 运行库缺失 | `loadLocked` 返回 0 | `xess-fg: cannot wrap the chain, runtime unavailable: %s`（`d3d12_xessfg.cpp:467`） |
| XeSS `CreateContext` 失败 | 返回 0 | `xess-fg: CreateContext failed result=0x%X`（:552） |
| XeSS `BuildPipelines` 失败 | 销毁 handle 后返回 0 | `xess-fg: BuildPipelines failed result=0x%X`（:559） |
| XeLL 未接 | `SetLatencyReduction` 失败即销毁 | `xess-fg: SetLatencyReduction failed result=0x%X`（:669） |
| XeSS init 失败 | `dumpInfoQueue` 后销毁 | `xess-fg: InitFromSwapChain failed result=0x%X`（:678）+ `iq[init] sev=%d id=%d %.*s` |
| XeSS 取链失败 | 销毁 | `xess-fg: GetSwapChainPtr failed result=0x%X`（:688） |
| 重复包装 | 直接返回 0 | `xess-fg: wrapSwapChain called twice without unwrap (wanted %dx%d)`（:534） |
| 链被包走但后端起不来 | 清期望、链归属记回原生、拆 provider | `frame generation could not take the swap chain and was turned off; …`（`d3d12_surface.cpp:1013-1019`） |
| 期望开但建不出来 | 每 30 帧重试；300 帧后 warn 一次 | `frame generation requested (mode=%d frames=%d) but no provider after %llu frames: %s`（`d3d12_framegen.cpp`） |
| 迟到建成 | 补发 `setMode` 并挂链重建 | `frame generation became usable lazily: backend=%d mode=%d frames=%d …`（:669） |
| 连续 60 帧不可用 | 回滚：关期望 + 挂链重建 | `frame generation rolled back after %llu frames unavailable (%s); …`（:714） |

### 7.3 逐帧失败

| 后端 | 失败 | 动作 | 日志 |
| --- | --- | --- | --- |
| 任意 | 设备已移除 | 跳过 FG 钩子，`Present` 照常（只记一次） | `presentSurface: device removed -> frame generation hooks skipped`（`d3d12_surface.cpp:1606`） |
| FSR-FG | `Configure` 返回非 OK | `applied_ = false`，跳过本帧 | `fsr-fg: configure failed 0x%X (on=%d hudless=%p frame=%llu)`（:531，`logOnce`） |
| FSR-FG | `PrepareV2` 返回非 OK | `applied_ = false`，跳过本帧 | `fsr-fg: prepare failed 0x%X (render=%ux%u depth=%p mv=%p)`（:587，`logOnce`） |
| FSR-FG | `ffxDispatch` 失败（在 SDK 回调里） | `presented_ = 1` | `fsr-fg: dispatch failed 0x%X (frame=%llu, consecutive=%d)`（:264） |
| FSR-FG | UI 纹理换了 | 下一帧先发空注册再注册新的 | `fsr-fg: ui resource unregistered (present = world + ui)`（:337） |
| XeSS-FG | `TagFrameResource` 失败 | 继续（只记第一次） | `xess-fg: TagFrameResource(type=%d) failed result=0x%X`（`d3d12_xessfg.cpp:697`） |
| XeSS-FG | `SetEnabled` 失败 | 继续（`appliedMode_` 已推进） | `xess-fg: SetEnabled(%d) failed result=0x%X`（:852） |
| XeSS-FG | `SetNumInterpolatedFrames` 失败 | 继续 | `xess-fg: SetNumInterpolatedFrames(%d) failed result=0x%X`（:861） |
| XeSS-FG | 请求了插帧但 SDK 报 disabled | 只记一次 | `xess-fg: frame generation requested (frames=%d) but the SDK reports it disabled (frameGenResult=0x%X)`（:430） |
| XeSS-FG | `GetLastPresentStatus` 失败 | `presented_ = 1` | `xess-fg: GetLastPresentStatus failed result=0x%X`（:414） |

### 7.4 可读的状态行

`mcdr_dx12FgStatus` 输出（`fgStatusText`，`d3d12_framegen.cpp`）：

```
[fg] picked=fsr-fg applied=fsr-fg chain=fsr-wrap mode=1/1 rebuilds=0 tracks=1201/1201/1201 presented=2 latency=antilag2(no-dll)
```

`picked` 期望后端（`none` / `dlssg` / `fsr-fg` / `xess-fg`）、`applied` 生效后端（同前）、`chain` 链归属
（`app-native` / `sl-proxy` / `fsr-wrap` / `xess-wrap`）、`mode` = 期望/链建成时的 mode、`rebuilds` 重建次数、
`tracks` = app/backend/present 三轨帧号、`presented` 上次提交帧数（只有 provider 存在时才打）、`latency`
低延迟方案（`none` / `reflex` / `antilag2(no-dll)` / `xell`）、`reason` 最近失败原因（有才打）。

周期快照（每 600 个 present 两行，`fgAfterPresent`，`d3d12_framegen.cpp:574-598`）：

```
fg tracks: app=1201 backend=1201 present=1201 chainOwner=2 mode=1/1
[FG] backend=2 available=1 runtime=1 device=1 chainCompat=1 needsReflex=0 maxGen=2 latency=2 transitions=1 chain=2 rebuilds=1 active=1 note="FSR-FG ready"
```

状态**变化**时另有一行（`logStateChange`，只在变化时打）：

```
[FG] desiredMode 1 -> 0 at present=3402 (picked=2 applied=0 chain=0 rebuilds=1)
```

FSR-FG 与 XeSS-FG 各有一条自己的周期计数（600 / 300 个 present 一次），用来判断"插帧到底有没有在跑"：
FSR 看 `dispatches_per_app`（应 ≈ 1.0），XeSS 看 `presented`。

```
fsr-fg: app_frames=1201 dispatches=1199 dispatches_per_app=1.00 generated_total=1199 generated=2 enabled=1 prepared=1 hudless_ui=1 ui_registered=1 dispatch_failures=0 frame_id=1201
xess-fg: presents=900 enabled=1 presented=2 fgResult=0x0 frame_id=900
```

---

## 8. 已知限制与缺陷

> **缺陷**：XeSS-FG 的 `presented_` 是裸 `int32_t`（`d3d12_xessfg.cpp:824`），在 `afterPresent`（渲染线程）
> 写、在 `mcdr_dx12FgPresented`（JVM 线程）读，没有 `std::atomic`。FSR-FG 同一字段是
> `std::atomic<int32_t>`（`d3d12_fsffg.cpp:825`），两条后端不对称。

> **缺陷**：XeSS-FG 每帧 tag `XEFG_SWAPCHAIN_RES_UI` 前不做存活探测，直接把借用指针交给
> `GetDesc()`（`d3d12_xessfg.cpp:690-694`）。FSR-FG 对同一类指针有 `resourceIsUsable()` 的 `__try/__except`
> 探测（`d3d12_fsffg.cpp:44`），XeSS 侧没有 —— 世界切换那一帧 JVM 对象已 `close()` 时会读野指针。

> **缺陷**：XeSS-FG 的 `uiMode` 只在 `InitFromSwapChain` 时定一次
> （`params.uiMode = (uiTexture_ != nullptr) ? HUDLESS_UITEXTURE : AUTO`，`d3d12_xessfg.cpp`）。
> 若 UI 纹理是在包装**之后**才由 JVM 推来的，声明仍是 `AUTO`，而每帧 tag 已带 `RES_UI`、
> `SetUiCompositionState` 也已置 `ENABLED`（:831-837）—— 声明与实际不一致。SDK 是否容忍这种不一致，
> 代码中未明确。

> **缺陷**：XeSS-FG 的 `initFlags` 没有 `XEFG_SWAPCHAIN_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP`，所以
> `xefgSwapChainD3D12SetDescriptorHeap` 只被 `GetProcAddress` 解析（`d3d12_xessfg.cpp:170`、`:291`）却从不
> 调用，SDK 自管描述符堆。要改成外部堆必须同时改 `BuildPipelines` 与 `InitFromSwapChain` 两处，
> 否则 SDK 拒绝初始化。同处注释写的是"initFlags=0"（:841），与实际的 `kInitFlags` 两位不符 —— 注释已过期。

> **缺陷**：`mcdr_fg_constants` 的 `view` / `projection` 只有 XeSS-FG 使用，FSR-FG 完全忽略
> （`PrepareV2` 无对应字段）。若上层只推帧常量而没推逐帧相机（`FgFrameInputs` 的 near/far/fov），
> FSR 会一直用 0.05 / 1024 / 1.0，插帧质量下降但不报错。

> **缺陷**：`mode = 2`（dynamic）在 native 侧没有自动倍率算法：`desiredFrames` 直接取 `desc.numFrames`，
> 与 `mode = 1` 同路径（`fgSetMode`）。上层必须继续逐帧推 `numFrames`，否则 dynamic 退化成固定倍率。

> **缺陷**：`fgPickBackend` 在链已被接管型后端占用时拒绝改 `picked`（返回 0，reason `restart to switch FG`），
> 且 `fgAbortWrapAfterFailure` 会把 `picked` 清成 `MCDR_FG_BACKEND_NONE`（`d3d12_framegen.cpp`）。
> 前者意味着换后端要重启游戏，后者意味着包装失败后用户要在设置页重新选一次后端。

> **缺陷**：`uiCompositionReady()` 的判据两家不同。FSR-FG 要求"链已包 + UI 纹理在 + 至少发生过一次插帧
> dispatch"（`dispatchCount_ > 0`，`d3d12_fsffg.cpp:525`）；XeSS-FG 只要求
> `uiTexture_ && handle_ && proxyChain_`，不看是否真的插过帧。于是"包装成功但还没插过帧"的窗口期，
> FSR 返回 0 而 XeSS 返回 1。

其它限制（刻意的取舍，不是缺陷）：

* 一条会话只用一个"接管交换链"的后端，换后端要重启。由单 provider 约束与 `fgPickBackend` /
  `ensureProviderLocked` 的拒绝路径共同保证。
* 包装型后端只在应用原生链上工作（`chainCompatible` 恒 1，真正的门控在链家族与 `chainOwner` 上）。
* DLSS-G 的 HUD-less 复用 SL 的超分输出，因此 SR 必须是 DLSS 或未选；换 FSR/XeSS 会走 60 帧回滚。
* FSR-FG 的低延迟方案 Anti-Lag 2 只有头文件、没有 DLL，`latencyProvider` 只是架构位，status 里显示为
  `antilag2(no-dll)`。
* `caps.note` 27 字符上限；超长原因会被截断（`writeString` 的通用行为）。
* FSR-FG 的 UI 资源注册**每帧重发**（`syncUiResourceLocked`，`d3d12_fsffg.cpp:216`），这是 SDK 要求
  的绑定方式，不是冗余调用。

---

## 9. 文件导航

| 路径（相对 `MCDR\`） | 内容 |
| --- | --- |
| `include\mcdr_abi.h` | `mcdr_fg_caps`（:813-831）、`mcdr_fg_constants`（:847-857）、`mcdr_fg_dispatch`（:858-874）、`MCDR_FG_*`（:791-809）、`mcdr_dx12FgState` 位布局（:896-904）、全部 `mcdr_dx12Fg*` 导出 |
| `src\module\core\mcdr_device.cppm` | `FgBackendId`(:1207)、`FgFrameInputs`(:1225)、`IFrameGenProvider`(:1246)、注册表与工厂声明 |
| `src\c_abi\abi_framegen.cpp` | C ABI 适配：只校验与转发，不含厂商名 |
| `src\advanced\d3d12_framegen.cpp` | `FgRegistry`、`DlssGFrameGenProvider`、`fillDlssGCaps`、`fgSetMode`、链重建请求、三轨帧号、`fgStatusText` |
| `src\advanced\d3d12_fsffg.cpp` | `FsrFgFrameGenProvider`、`fillFsrFgCaps`、FFX 包装链、`applyFrameLocked`（Configure + PrepareV2）、UI 资源注册 |
| `src\advanced\d3d12_xessfg.cpp` | `XessFgFrameGenProvider`、`fillXessFgCaps`、`xefg_swapchain` 包装、`tagFrameInputs`、XeLL marker / sleep |
| `src\core\d3d12_surface.cpp` | `createDxgiFactory`(:90-112)、`wrapChainForFrameGen`(:871-944)、`acquireSurface`(:1441-1459)、`presentSurface`(:1916-1964) |
| `src\advanced\d3d12_sr.cpp` | SR 通用路径的逐帧 tag 触发点（:413） |
| `src\advanced\d3d12_dlss.cpp` | DLSS 路径的逐帧 tag 桥（:2505-2514）与 `mcdr_dlss_set_fg_mode` |
| `docs\advanced\framegen.md` | 本文 |

---

## 10. 文档维护

1. 改了 `IFrameGenProvider` 的方法集合，同一次改动里更新 §1.3 的接口表与 §2 的时序，并核对
   `mcdr_device.cppm` 的行号引用。
2. 改了 `mcdr_fg_caps` / `mcdr_fg_constants` / `mcdr_fg_dispatch` 的字段或大小，更新 §5.1 的位布局、
   §4.1 的载荷布局，并同步 `include\mcdr_abi.h` 的注释与 JVM 侧的布局镜像。
3. 改了三家中任一家的 DLL 名、必需入口或默认相机值，更新 §3.1 / §3.2 / §4.2 的差异表。
4. 新增或修改日志行时，同一次改动里更新 §7.2 / §7.3 的表格 —— 日志原文必须与代码逐字一致。
5. 修掉 §8 中任何一条缺陷后删除对应警告块，并把新的降级路径补进 §7；确认不再是缺陷的取舍移到
   §8 末尾的"其它限制"。
