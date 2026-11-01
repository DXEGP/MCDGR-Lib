# 超分后端（SR / FSR / XeSS）

本文讲 MCDR 的**超分（Super Resolution，SR）抽象层**：一张与厂商无关的能力表、一份逐帧载荷、
九个 C ABI 入口，以及 FSR 与 XeSS 两条「整条自己干」的后端实现。DLSS 的实现留在
`src\advanced\d3d12_sr.cpp` 的 `DlssSrProvider`（转发给 `applyDlssToTexture`），细节见
`docs\advanced\dlss.md`，本文只在差异表里列出它。

读这份文档能解决三类问题：

- 想知道某个后端为什么「不可用」、日志里那行 `sr active=… picked=… eval=…` 怎么读；
- 要新增第四条后端，需要实现哪些虚函数、填哪些 caps 字段；
- 每帧 dispatch 的某张纹理格式/状态要求是什么（输出没有 `ALLOW_UNORDERED_ACCESS` 会直接炸）。

## 1. 这一层做什么

三家的调用形态完全不同：

| 后端 | 引入方式 | 上下文 | 每帧入口 | 谁转屏障 |
| --- | --- | --- | --- | --- |
| DLSS（Streamline / NGX） | 导入库 + 运行期 `nvngx_dlss.dll` | NGX feature（延迟到首帧建） | `applyDlssToTexture` | Streamline 自己 |
| FSR（FidelityFX `ffx_api`） | 运行期 `LoadLibrary` | `ffxContext` | `ffxDispatch` | **我们自己** |
| XeSS（Intel XeSS SDK） | 导入库 + 运行期 `libxess.dll` | `xess_context_handle_t` | `xessD3D12Execute` | **我们自己**（输入输出） |

抽象层把「哪家后端、能力是什么、怎么翻译参数」从 C ABI 层搬到设备模块里，于是
`src\c_abi\abi_sr.cpp` 里**不出现任何厂商名**：加 FSR / XeSS 时那个文件一行都没改。

判定规则：

- 后端编号：`MCDR_SR_BACKEND_NONE=0`、`_DLSS=1`、`_FSR=2`、`_XESS=3`；0 是合法选择（不超分），不是错误。
- 掩码能力位取并集：`MCDR_SR_MASK_REACTIVE=1`（reactive mask）、`MCDR_SR_MASK_RESPONSIVE=2`
  （responsive pixel mask），具体怎么喂由各 Provider 决定。
- 资源指针一律是 **JVM 创建的纹理句柄**（`Dx12Object*`，即 `D3D12GpuTexture.handle()` 的值），
  不是 `ID3D12Resource*`；传 0 表示「这张资源没有」。解包只在设备模块内做。
- 任何一帧失败都不让帧失败：调用方退回「没有超分」的老路径。

## 2. 架构与数据流

```
JVM (SuperResolution.kt)
  │  SrPick / SrQueryCaps / SrCreate / SrResize / SrEvaluate / SrJitterPhaseCount
  ▼
src\c_abi\abi_sr.cpp            参数校验 + 结构体填充（无厂商名）
  ▼
src\advanced\d3d12_sr.cpp       注册表：gSrPicked / gSrActive / gSrProvider
  │  ensureProvider() 懒建或切后端（切之前 releaseProviderWithWait）
  ▼
IUpscaleProvider（mcdr.d3d12.device 模块内，签名只用 POD）
  ├─ DlssSrProvider  → applyDlssToTexture(ctx, toDlssInput(desc))   （d3d12_sr.cpp 内）
  ├─ FsrSrProvider   → ffxCreateContext / ffxDispatch               （d3d12_fsr.cpp）
  └─ XessSrProvider  → xessD3D12CreateContext / xessD3D12Execute    （d3d12_xess.cpp）
        │
        └─ transitionTextureTo()：输入 → NON_PIXEL_SHADER_RESOURCE
                                 输出 → UNORDERED_ACCESS → 回 RENDER_TARGET
```

`IUpscaleProvider` 的虚函数（声明在 `src\module\core\mcdr_device.cppm`）：

| 虚函数 | 语义 | 允许失败 |
| --- | --- | --- |
| `SrBackendId backend() const` | 实现属于哪一家 | 否 |
| `void fillCaps(mcdr_sr_caps&) const` | 填能力表，**不建上下文** | 否（`available=0` 表达不可用） |
| `bool initialize(CommandContext*, const mcdr_sr_dispatch& seed)` | 建/唤醒上下文，命令列表必须已打开 | 是 |
| `void onResize(rw, rh, dw, dh)` | 只记账，真正重建推迟到下一次 `evaluate` | 否 |
| `bool evaluate(CommandContext*, const mcdr_sr_dispatch&)` | 提交一帧 | 是（每类失败最多一行日志） |
| `void shutdown()` | 释放上下文；调用方负责先等 fence | 否 |

`initialize` 收到的是「种子载荷」：`srCreateProvider` 用 `seedFromSizes()` 造，只有尺寸，
资源指针为空。要在建上下文时就知道 HDR / 深度格式的后端，得让种子多带字段。

## 3. ABI 契约

### 3.1 `mcdr_sr_caps`（`sizeof = 80`、`align = 4`）

Kotlin 侧镜像在 `MCDR_Layout.kt` 的 `SR_CAPS`。`note` 是给 UI 与日志的短原因（UTF-8，NUL 结尾），
不要拿它做机器判断——机器判断只看 `available`。

| 偏移 | 字段 | 取值 |
| --- | --- | --- |
| 0 | `backend` | `MCDR_SR_BACKEND_*` |
| 4 | `available` | 1 = 本机此刻可用 |
| 8 | `coupledDenoise` | 1 = 该后端自带降噪（DLSS-D / FSR4 ML）；三条已实现后端都报 0 |
| 12 | `needsExposure` | 1 = 需要曝光纹理 |
| 16 | `takesMasks` | `MCDR_SR_MASK_*` 位集 |
| 20 | `externalHeap` | 1 = 接受调用方提供的临时堆 |
| 24 | `jitterPhaseCount` | 0 = 无意见（沿用 `D3D12Temporal` 的 8） |
| 28 / 32 | `minRenderWidth` / `minRenderHeight` | 低于此值后端给不出有效结果 |
| 36 / 40 | `maxRenderWidth` / `maxRenderHeight` | 上界 |
| 44 | `nativeAA` | 1 = 允许 1.0x（DLAA 式） |
| 48 | `reserved` | 必须为 0 |
| 52 | `note[28]` | 短原因（52 + 28 = 80） |

### 3.2 `mcdr_sr_dispatch`（`sizeof = 136`、`align = 8`）

Kotlin 侧镜像在 `MCDR_Layout.kt` 的 `SR_DISPATCH`。约定（与 `mcdr_dlss_input` 一致，别改）：

- `renderWidth/Height` 是**渲染目标**尺寸，`displayWidth/Height` 是**窗口**尺寸；
- `jitterOffsetX/Y` 是**渲染空间像素**，不归一化、不取负；
- `motionVectorScaleX/Y` 在「MV 已是像素位移」时给 1.0；0 一律解释为「未填 → 按 1.0」；
- `cameraVerticalFov` 是弧度（等于 FSR 的 `cameraFovAngleVertical`），取自 `GameRenderer.getFov`
  的实际值，不要用视场设置的标称值；
- 可选字段填 0 = 未填，后端按各自安全默认处理（`cameraAspectRatio` → `displayWidth/displayHeight`，
  `viewSpaceToMetersFactor` → 1.0）。

| 偏移 | 字段 | 说明 |
| --- | --- | --- |
| 0 / 4 | `renderWidth` / `renderHeight` | 渲染尺寸 |
| 8 / 12 | `displayWidth` / `displayHeight` | 显示尺寸（= 窗口） |
| 16 / 20 | `jitterOffsetX` / `jitterOffsetY` | 渲染空间像素 |
| 24 / 28 | `motionVectorScaleX` / `motionVectorScaleY` | 像素位移时给 1.0 |
| 32 | `preExposure` | 0 → 按 1.0 |
| 36 | `frameTimeDeltaMs` | 0 → 按 16.7 |
| 40 / 44 | `cameraNear` / `cameraFar` | 世界单位 |
| 48 | `cameraVerticalFov` | 弧度，0 → 1.2217 |
| 52 | `sharpness` | 0 = 不锐化，否则夹到 [0,1] |
| 56 | `reset` | 1 = 丢弃历史 |
| 60 | `hdrInput` | 1 = 输入是 HDR 线性色 |
| 64 | `backend` | 0 = 用 `SrPick` 记下的那个 |
| 68 | `flags` | 保留，必须为 0 |
| 72 | `pColorInput` | 渲染尺寸颜色 |
| 80 | `pColorOutput` | 显示尺寸颜色（UAV 写） |
| 88 | `pDepth` | 渲染尺寸深度 |
| 96 | `pMotionVectors` | 渲染尺寸 RG16F |
| 104 | `pReactiveMask` | R8，可 0 |
| 112 | `pExposure` | R32F，可 0 |
| 120 | `depthFormatOrdinal` | 与 `mcdr_dlss_input` 同名字段同义 |
| 124 | `cameraAspectRatio` | 显示尺寸宽高比（可选） |
| 128 | `viewSpaceToMetersFactor` | 1.0 = 一个世界单位算 1 米；0 = 未填 |
| 132 | `_pad2` | 补到 136 |

### 3.3 入口函数

| 函数 | 返回 | 约束 |
| --- | --- | --- |
| `mcdr_dx12SrPick(int32_t backend)` | 1 已记录 / 0 编号未知 / -1 非法 | **纯记录**，不建不销毁；重建发生在下一帧帧首 |
| `mcdr_dx12SrQueryCaps(int32_t, mcdr_sr_caps*)` | 1 已填 / -1 非法 | `out` 非空；`*out` 先清零再填；会触发一次 `LoadLibrary` 尝试 |
| `mcdr_dx12SrCreate(ctx, backend, maxRw, maxRh, dw, dh)` | 1 就绪 / 0 不可用 / -1 非法 | 命令列表必须打开；同一后端重复调用幂等（只更新尺寸） |
| `mcdr_dx12SrResize(ctx, rw, rh, dw, dh)` | 1 已接受 / 0 无生效后端 / -1 非法 | 当前实现只记账，`ctx` 未使用 |
| `mcdr_dx12SrEvaluate(ctx, const mcdr_sr_dispatch*)` | 1 已执行 / 0 跳过 / -1 非法 | 命令列表必须打开；`desc->backend=0` 时用 `SrPick` 的值 |
| `mcdr_dx12SrJitter(frameIndex, float* x, float* y)` | 恒 0 | 刻意「无意见」：亚像素 jitter 只有一个源（`D3D12Temporal` 的 Halton(2,3)） |
| `mcdr_dx12SrJitterPhaseCount(backend, rw, dw)` | 相位长度 / -1 非法 | 0 = 不适用；见 §4.6 |
| `mcdr_dx12SrDestroy()` | — | 帧首、无打开命令列表时调用；重复调用安全；同时把期望后端清成 NONE |
| `mcdr_dx12SrQuery(char*, int32_t)` | 1 已写 / 0 无内容 / -1 非法 | `(nullptr, 0)` 只问长度，与 `mcdrLastError` 约定一致 |

### 3.4 一帧的调用时序

```
帧首
  SrDestroy()            （可选：彻底释放；之后必须重新 Create）
  SrPick(backend)        （可选：JVM 在 load / 切档时推一次）
世界层画完、GUI 之前、命令列表已打开
  SrCreate(ctx, …)       幂等：同后端只更新尺寸
  SrEvaluate(ctx, &desc)  ├─ ensureProvider：需要时懒建 / 切后端
                          ├─ BeginEvent("SR:<backend> <rw>x<rh>-><dw>x<dh>")
                          ├─ provider->evaluate
                          ├─ EndEvent
                          └─ restoreDrawHeap(ctx)（无论成败）
```

窗口 / 渲染比例变化

```
  SrResize(ctx, …)  → provider->onResize（只记账）
  下一次 Evaluate 内部的 ensureContext 才真正重建（那时能等 fence）
```

## 4. 注册表、生命周期与诊断（`src\advanced\d3d12_sr.cpp`）

### 4.1 状态

| 变量 | 含义 |
| --- | --- |
| `gSrPicked` | 用户**选**的那一家（`SrPick` 写入，`SrDestroy` 清成 NONE） |
| `gSrActive` | **真正在跑**的那一家（`initialize` 成功后写入，`evaluate` 成功后再确认一次） |
| `gSrProvider` | 当前 Provider 实例（`std::unique_ptr<IUpscaleProvider>`） |
| `gSrNote` | 最近一次失败 / 降级原因；`evaluate` 成功时清空 |
| `gSrEvalOk` / `gSrEvalFail` | 逐帧成功 / 失败计数（只给 `SrQuery` 的文本用） |
| `gSrLastRenderWidth` … | 上一帧的尺寸，`size=` 段用 |

`gSrPicked` 与 `gSrActive` 不是一回事：JVM 只调 `SrCreate` 而不调 `SrPick` 时 `gSrPicked`
仍是 NONE，需要「超分现在跑的是哪家」的判断（例如帧生成的门控）必须问 `gSrActive`。

### 4.2 懒建与热切换

`ensureProvider(ctx, backendId, seed)`：

1. `gSrProvider != nullptr && gSrActive == backendId` → 直接 `true`；
2. 否则先 `releaseProviderWithWait(ctx)`，打一行 `SR provider switch <old> -> <new>`；
3. `makeProvider(id)`：`Dlss` 直接 `new DlssSrProvider`，`Fsr` / `XeSS` 走各自工厂
   （运行库不在时工厂返回 `nullptr`，原因由工厂自己打日志）；
4. `provider->initialize(ctx, seed)` 失败 → 返回 `false`（`initialize` 已写 `gSrNote`）；
5. 成功 → 写 `gSrActive`、打 `SR provider ready: <name> (render WxH display WxH)`。

### 4.3 销毁前必须等 GPU

`releaseProviderWithWait(ctx)`：`ctx != nullptr && ctx->fenceValue > 0` 时
`waitForFenceValue(ctx, ctx->fenceValue, 5'000'000'000ULL)`，超时只 WARN 然后照常释放。

- 只等 `ctx->fenceValue`、**不等 `+1`**：切换发生在帧内，本帧还没提交，等 `+1` 等于等一个
  永远不会被 Signal 的值。
- 不等就释放的典型表现：设备移除，或切换那一帧整块变黑（`ffxDestroyContext` 与
  `xessDestroyContext` 都会整块释放上下文持有的 GPU 资源）。

`srReleaseProvider()`（`SrDestroy` 走这条）没有命令上下文，改用队列级栅栏：
`currentQueueFenceValue()` + `waitForQueueFenceValue(queueValue, 5'000'000'000ULL)`。

### 4.4 每帧的收尾动作（必须保留）

`srEvaluateFrame` 在 provider 返回后**无论成败**都做三件事：

1. `restoreDrawHeap(ctx)`：三家 SDK 的 Evaluate 内部都可能 `SetDescriptorHeaps`，而堆绑定是
   命令列表级状态、不随根签名 / PSO 复位。不绑回来，后续任何走瞬时 SRV 的绘制都会撞
   `descriptor heap containing handle is different from currently set descriptor heap`。
2. 计数与 `gSrNote` 维护（成功清空，失败写 `<name> evaluate failed`）。
3. 成功时 `fgTagFrameInputs(fgMakeFrameInputs(ctx, desc))`：包装型帧后端要求输入 tag 落在
   **这一帧的命令列表**上；DLSS-G 不覆写这个钩子，对它是一次空调用。

### 4.5 诊断行格式

`mcdr_dx12SrQuery` 输出一行：

```
sr active=fsr picked=fsr eval=912/3 size=1280x720->1920x1080 caps[ok masks=1 jitter=8 max=7680x4320] why=… last=…
```

- `active` / `picked` 不一致 = 切换失败或被拒；
- `eval=ok/fail` 逐帧计数：能区分「建起来了但每帧都失败」；
- `caps[...]` 是**生效后端**自己的能力面（`masks` / `jitter` / `max`）；
- `why=` 只在 `available=0` 或 note 不是 `off` 时出现（NONE 档的 `off` 是正常状态）；
- `last=` 是最近一次降级原因。

这一步会去问运行库（FSR / XeSS 是一次 `LoadLibrary` 尝试，DLSS 走 `queryDlss`），只该在诊断 /
界面刷新时调，不要挂到每帧路径。

### 4.6 jitter 相位长度

| 后端 | 值 | 依据 |
| --- | --- | --- |
| `NONE` | 0 | 没有超分在累积，jitter 与它无关 |
| `DLSS` | 8 | Streamline 的 8 相位，与 `D3D12Temporal` 的 Halton(2,3) 对齐 |
| `FSR` | `8 * (displayWidth / renderWidth)^2`，四舍五入后夹到 `[1, 256]` | 官方公式的近似；接真后端后应改用 `ffxQueryDescUpscaleGetJitterPhaseCount` 问运行库 |
| `XeSS` | 8 | SDK 自己按 `xessGetJitterScale` 缩放我们给的像素偏移，序列长度不变 |

`renderWidth` 或 `displayWidth` ≤ 0 时 FSR 分支返回 8（保守值）。

## 5. 每帧 dispatch：纹理与格式约束

### 5.1 输入输出

| 字段 | 内容 | 尺寸 | 建议格式 | 本层发出的状态 |
| --- | --- | --- | --- | --- |
| `pColorInput` | 世界层颜色 | 渲染 | 由 JVM 决定（HDR 时置 `hdrInput=1`） | `NON_PIXEL_SHADER_RESOURCE` |
| `pDepth` | 深度 | 渲染 | reverse-Z（`D32_FLOAT` 系） | `NON_PIXEL_SHADER_RESOURCE` |
| `pMotionVectors` | 运动矢量 | 渲染 | `RG16F` | `NON_PIXEL_SHADER_RESOURCE` |
| `pColorOutput` | 升采样结果 | 显示 | **必须带 `ALLOW_UNORDERED_ACCESS`** | `UNORDERED_ACCESS` → 返回前 `RENDER_TARGET` |
| `pReactiveMask` | 可选掩码 | 渲染 | R8 | 有资源时 `NON_PIXEL_SHADER_RESOURCE` |
| `pExposure` | 可选曝光 | — | R32F | 三条后端当前都不接（`needsExposure=0`） |

约束（违反就出错，不是建议）：

- `pColorOutput` 必须是 UAV 纹理。拿普通 RenderTarget 的 GUI 目标当输出，`UNORDERED_ACCESS`
  屏障会在 `Close()` 时被 D3D12 拒成 `E_INVALIDARG`（`0x80070057`）。JVM 侧要交给
  `DLSSOutput` 那张 `USAGE_STORAGE` 纹理再 blit。
- 输出在返回前必须交回 `RENDER_TARGET`：后面 GUI 还要按渲染目标用它。
- 输入 / 输出 / 深度 / MV 缺任何一张，Provider 不做猜测，直接返回 `false` 让调用方退回老路径。
- 句柄在但 `Dx12Object::resource` 为空（JVM 侧还没建好）等同「没有这张资源」；把半个对象
  交给 SDK 会让它自己发的屏障打到空指针上。

### 5.2 尺寸与能力下限

三条后端共用的窗口：

| 项 | 值 | 行为 |
| --- | --- | --- |
| 渲染尺寸下限 | `320 x 180` | 低于此值**跳过**本帧 dispatch，只记一次 `dbgLogError`（提示抬渲染比例） |
| 硬上限 | 每边 `8192` | 超过直接拒绝（`problem_ = "size beyond 8k"`） |
| caps 报告的上界 | `7680 x 4320` | UI 侧据此置灰 |

低于下限时 FFX 只会给出无效结果，DLSS 那边表现为「动态缩放错误 + 空纹理」（世界全黑、
GUI 正常），所以这里是明确拒绝而不是硬跑。

## 6. FSR 后端（`src\advanced\d3d12_fsr.cpp`）

### 6.1 运行库

- DLL：`amd_fidelityfx_upscaler_dx12.dll`（**不含** loader DLL：本后端直接向 effect DLL 要入口）。
- 搜索顺序：`<MCDR.dll 所在目录>\amd_fidelityfx_upscaler_dx12.dll` → 裸 DLL 名（标准搜索路径）。
  模块目录恰好是 JVM 侧 `extractVendorResources()` 解出厂商库的目录 `<user.dir>\dxrmc\`，
  所以不需要额外的开关。
- 必需入口（缺任一即不可用）：`ffxCreateContext`、`ffxDestroyContext`、`ffxQuery`、`ffxDispatch`。
- 进程内只尝试加载一次，失败原因留在 `FfxRuntime::problem`，由 `caps.note` 透给 JVM。
- 取自身模块目录用 `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)`：
  `GetModuleFileNameA(nullptr)` 拿到的是宿主 `java.exe`。

### 6.2 内存：两套分配器

| 分配器 | 用途 | 实现 |
| --- | --- | --- |
| `ffxAllocationCallbacks`（`alloc` / `dealloc`） | FFX 的普通 CPU 侧分配 | `std::malloc` / `std::free` |
| `PfnFfxHeapAllocatorFunc` / `PfnFfxHeapDeallocatorFunc` | FFX 要建的 `ID3D12Heap` | `fsrHeapAlloc` / `fsrHeapFree` + `FsrHeapBroker` |

堆中介的规则（只接管 `D3D12_HEAP_TYPE_DEFAULT` 且 `!aliasable` 的请求）：

1. 第一次请求时建一块共享堆，容量 = `max(align64K(need) * 4, 96 MB)`，64 KB 对齐
   （`D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT`），`ALLOW_ALL_BUFFERS_AND_TEXTURES`；
2. 之后按 bump 游标切段，`cursor + need > capacity` 时退回「临时堆」（`loose`，由 deallocator 回收）；
3. `ffxDestroyContext` 后游标归零，但**共享堆留着** —— 换后端、改窗口时省掉一次分配；
4. 只接管 DEFAULT 非 aliasable：UPLOAD / READBACK 必须 CPU 可见，aliasable 交给设备自建更稳。

### 6.3 常量缓冲与 shader 资源

本工程**不直接创建** FSR 的常量缓冲和 shader 资源：`ffxCreateContextDescUpscale` 只给
`maxRenderSize` / `maxUpscaleSize` 与 `fpMessage`，内部资源（常量缓冲、内部纹理、pipeline）
由运行库按这两个尺寸分配，分配走上面那两套回调。每帧只有 FFX 内部常量通过
`ffxDispatchDescUpscale` 的字段更新（见 §6.5）。要确认这份 SDK 实际建了哪些资源，只能靠
RenderDoc / PIX 抓帧——代码中未明确。

### 6.4 上下文描述链

```
ffxCreateContextDescUpscale   (type = ..._TYPE_UPSCALE, pNext = ↓)
  └─ ffxCreateBackendDX12AllocationCallbacksDesc
        (type = ..._TYPE_BACKEND_DX12_ALLOCATION_CALLBACKS, pNext = ↓,
         pfnFfxHeapAllocator = fsrHeapAlloc, pfnFfxHeapDeallocator = fsrHeapFree,
         pfnFfxResourceAllocator / pfnFfxResourceDeallocator /
         pfnFfxConstantBufferAllocator = nullptr)
       └─ ffxCreateBackendDX12Desc (type = ..._TYPE_BACKEND_DX12, device = ID3D12Device*)
```

`upscaleDesc.flags` 在建 / 重建时算一次（每帧不变）：

| 位 | 何时置 | 理由 |
| --- | --- | --- |
| `FFX_UPSCALE_ENABLE_DEPTH_INVERTED` | 恒置 | 26.3 是 reverse-Z |
| `FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` | 恒置 | MV 由 jittered 投影算出，要让 FSR 把 jitter 差当相机抖动扣掉 |
| `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE` | `seed.hdrInput != 0` | 输入是 HDR 线性色 |
| `FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS` | **不置** | MV 是渲染分辨率的像素位移 |
| `FFX_UPSCALE_ENABLE_DEPTH_INFINITE` | **不置** | 26.3 的远平面有限 |
| `FFX_UPSCALE_ENABLE_AUTO_EXPOSURE` | **不置** | 曝光由 `preExposure` 标量给；纹理后续再接 |

### 6.5 每帧 `ffxDispatchDescUpscale` 映射

| desc 字段 | 来源 |
| --- | --- |
| `header.type` | `FFX_API_DISPATCH_DESC_TYPE_UPSCALE` |
| `commandList` | `ctx->commandList`（FFX 的 `FfxCommandList` 就是 `ID3D12GraphicsCommandList*`，不做额外转换） |
| `color` / `depth` / `motionVectors` / `reactive` | `ffxApiGetResourceDX12(res, FFX_API_RESOURCE_STATE_COMPUTE_READ)` |
| `exposure` / `transparencyAndComposition` | `ffxApiGetResourceDX12(nullptr)`（= 没有这张纹理） |
| `output` | `ffxApiGetResourceDX12(res, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)` |
| `jitterOffset` | `jitterOffsetX/Y`（原样） |
| `motionVectorScale` | `0 → 1.0f` |
| `renderSize` / `upscaleSize` | 本帧渲染 / 显示尺寸 |
| `enableSharpening` / `sharpness` | `sharpness > 0` / `clamp(sharpness, 0, 1)` |
| `frameTimeDelta` | `0 → 16.7f` |
| `preExposure` | `0 → 1.0f` |
| `reset` | `dispatch.reset != 0` |
| `cameraNear` / `cameraFar` | 原样 |
| `cameraFovAngleVertical` | `0 → 1.2217f`（≈70°） |
| `viewSpaceToMetersFactor` | `0 → 1.0f`（MC 的世界单位就是米） |
| `flags` | 0（每帧不加 flag） |

`ffxApiGetResourceDX12(res, state)` 的 state 是「进入 dispatch 时该资源所处的状态」，必须与
我们刚发的 `transitionTextureTo` 结果一致（`NON_PIXEL_SHADER_RESOURCE` ⇔ `COMPUTE_READ`）。

### 6.6 上下文尺寸与重建

- 上下文按 **max** 建、按**实际** dispatch：`ensureContext` 只在「首次」或「尺寸变大超出已建上界」
  时重建；已建上界之内直接复用。
- 重建前若 `ctx->fenceValue > 0`，先 `waitForFenceValue(ctx, ctx->fenceValue, 2'000'000'000ULL)`：
  旧上下文的内部资源可能还在飞，不等就销毁会导致设备移除或整帧变黑。
- `onResize` 只记账，重建发生在下一帧 `evaluate`（那时才有能等的命令上下文）。
- 建失败只 WARN 一次（`FSR context creation failed: <rc> (render=… display=…)`）。

### 6.7 caps

`available` 只取决于运行库能否加载；`note` 为 `fidelityfx runtime ok` 或具体失败原因。
`minRender = 320x180`、`maxRender = 7680x4320`、`jitterPhaseCount = 8`、`takesMasks = REACTIVE`、
`nativeAA = 1`、`coupledDenoise = 0`、`needsExposure = 0`、`externalHeap = 0`。

## 7. XeSS 后端（`src\advanced\d3d12_xess.cpp`）

### 7.1 运行库与入口

- DLL：`libxess.dll`；搜索顺序与 FSR 相同（模块目录 → 裸名）。
- 入口（`XessApi`）：`xessD3D12CreateContext`、`xessD3D12Init`、`xessD3D12Execute`、
  `xessDestroyContext`、`xessGetVersion`、`xessGetOptimalInputResolution` 为**必需**；
  `xessD3D12BuildPipelines`、`xessIsOptimalDriver`、`xessGetProperties` 有就用。
- `ready()` 只要求前六个：`buildPipelines` 只为省 `init` 的停顿，`isOptimalDriver` 只为日志，
  `getProperties` 只为外部堆尺寸。
- 版本字符串来自 `xessGetVersion`（`xess_version_t` → `major.minor.patch`），`caps.note` 形如
  `libxess 3.0.2`。

### 7.2 质量档：按「输入区间」挑，不按比例

XeSS 的 `qualitySetting` 在 `init` 时就定死，而本工程的渲染比例是滑块，输入尺寸每帧都可能是
新值。SDK 的 Dynamic Input Resolution 允许在 `[min, max]` 内自由变化（由
`xessGetOptimalInputResolution` 给出），所以：

候选档顺序（`kCandidates`）：

```
QUALITY → BALANCED → ULTRA_QUALITY → PERFORMANCE → ULTRA_QUALITY_PLUS → ULTRA_PERFORMANCE → AA
```

`pickQualityForSize` 对每档问一次
`getOptimalInputResolution(handle, &output, candidate, &optimal, &minSize, &maxSize)`：

1. 若 `minSize ≤ (renderW, renderH) ≤ maxSize` → 选中该档，记下 `inputMin_` / `inputMax_`；
2. 全都不容纳时，按 `displayW / optimal.x` 与目标比例的距离取最近的一档，边界同样记下。

只按比例取最近档会踩坑：某些档的区间下界（例如 1.7x 档的 `503x283`）高于 75% 渲染比例给出的
`495x278`，于是每帧判越界、每帧重建上下文。**不要**改成按比例选档。

### 7.3 上下文与 init 参数

| init 参数 | 值 |
| --- | --- |
| `outputResolution` | `{displayW, displayH}` |
| `qualitySetting` | §7.2 挑出的档 |
| `initFlags` | `XESS_INIT_FLAG_INVERTED_DEPTH \| XESS_INIT_FLAG_JITTERED_MV`，掩码启用时再加 `XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK` |
| `creationNodeMask` / `visibleNodeMask` | 0（单 GPU） |
| `pTempBufferHeap` / `pTempTextureHeap` | 外部临时堆（见 §7.4），建堆失败时置空由 SDK 自建 |
| `bufferHeapOffset` / `textureHeapOffset` | 0 |
| `pPipelineLibrary` | `nullptr` |

不置的 flag 及理由：`USE_NDC_VELOCITY`（MV 是像素位移）、`HIGH_RES_MV`（MV 与颜色同为渲染
尺寸）、`EXPOSURE_SCALE_TEXTURE`（不接曝光纹理，`exposureScale` 按 `preExposure` 直传）。

建上下文流程：`xessD3D12CreateContext` →（`isOptimalDriver` 报 `XESS_RESULT_WARNING_OLD_DRIVER`
时 WARN）→ `pickQualityForSize` → `xessD3D12BuildPipelines(handle, nullptr, false, flags)`
（非阻塞预建，`init` 会等它）→ `ensureTempHeaps` → `xessD3D12Init`。

### 7.4 外部临时堆

- 尺寸**唯一来源**是 `xessGetProperties(handle, &outputResolution, &props)` 的
  `tempBufferHeapSize` / `tempTextureHeapSize`（`requiredDescriptorCount` 只进日志）。
- 两个 `D3D12_HEAP_TYPE_DEFAULT` 堆，大小按 64 KB 向上取整，
  `CreationNodeMask = VisibleNodeMask = 1`，`ALLOW_ALL_BUFFERS_AND_TEXTURES`（XeSS 在里面放哪
  一类资源不该由我们假设）。
- 建堆失败 → 只 WARN 一次，退回 `nullptr` 让 SDK 自建，不致命。
- 建一次复用：尺寸只取决于输出分辨率，而重建条件里已包含它。
- 释放顺序：先 `xessDestroyContext`，再 `Reset()` 两个堆（SDK 在上下文生命周期内持有堆上的资源）。

### 7.5 每帧 `xess_d3d12_execute_params_t` 映射

| 参数 | 来源 |
| --- | --- |
| `pColorTexture` | `pColorInput` |
| `pVelocityTexture` | `pMotionVectors` |
| `pDepthTexture` | `pDepth` |
| `pExposureScaleTexture` | `nullptr` |
| `pResponsivePixelMaskTexture` | `pReactiveMask`（可为空） |
| `pOutputTexture` | `pColorOutput` |
| `jitterOffsetX/Y` | 原样 |
| `exposureScale` | `preExposure`，0 → 1.0 |
| `resetHistory` | `reset != 0` |
| `inputWidth` / `inputHeight` | 本帧渲染尺寸（Dynamic Input Resolution） |
| `pDescriptorHeap` | `nullptr`（v1 用 XeSS 内部堆） |

掩码是 **init 期开关**：这一帧掩码缺席 / 出现与 `maskExpected_` 不一致时置 `needsRecreate_`，
由下一次 `ensureContext` 重建一次上下文。

### 7.6 重建条件

`handle_ != nullptr && !needsRecreate_ && displayW/H == builtDisplayW/H &&
inputMin_ ≤ (renderW, renderH) ≤ inputMax_` → 直接复用。否则先等 fence
（`waitForFenceValue(ctx, ctx->fenceValue, 2'000'000'000ULL)`）再销毁重建。

显示尺寸变化、输入越界、掩码开关变化都会触发重建——这是「只在必要时重建」的全部条件，
不要在别处再读 `renderW_` 做判断。

### 7.7 caps

`minRender = 320x180`、`maxRender = 7680x4320`、`jitterPhaseCount = 8`、
`takesMasks = RESPONSIVE`、`externalHeap = 1`、`nativeAA = 1`、`coupledDenoise = 0`、
`needsExposure = 0`。

## 8. 三条后端的差异

| 项 | DLSS | FSR | XeSS |
| --- | --- | --- | --- |
| 需要的运行库 | `nvngx_dlss.dll`（导入库在编译期） | `amd_fidelityfx_upscaler_dx12.dll` | `libxess.dll` |
| 入口风格 | NGX feature + Streamline | 4 个 C 入口，`ffx_api` 描述符链 | 6 个必需 C 入口 |
| 上下文何时建 | 首帧 `evaluate`（NGX 要录命令） | `initialize`（`SrCreate` 时） | `initialize`（`SrCreate` 时） |
| 谁转输入输出屏障 | Streamline | 我们自己 | 我们自己 |
| 最小渲染尺寸 | 320x180 | 320x180 | 320x180 |
| caps 报告上界 | 7680x4320 | 7680x4320 | 7680x4320 |
| 硬拒绝上限 | — | 每边 8192 | 每边 8192 |
| jitter 相位 | 8 | `8 * (dw/rw)^2`，夹到 [1, 256] | 8 |
| jitter 来源 | `D3D12Temporal` 的 Halton(2,3)（本层不提供） | 同左 | 同左（SDK 内部按 `xessGetJitterScale` 缩放） |
| 掩码 | REACTIVE | REACTIVE | RESPONSIVE |
| 外部堆 | 1（`slAllocateResources`） | 0（caps 值，见 §10） | 1（`pTempBufferHeap` / `pTempTextureHeap`） |
| 自带降噪 | 0 | 0 | 0 |
| 曝光纹理 | 不接（标量 `preExposure`） | 不接 | 不接 |
| 性能档 | 由 `mcdr_dx12SetDlssQuality` / render preset 单独控制 | 无档位概念，按渲染尺寸走 | 7 个 `XESS_QUALITY_SETTING_*`，按区间挑 |
| 渲染比例来源 | JVM 滑块（33 / 50 / 58 / 67 / 100% 等） | 同左 | 同左 |
| 重建时机 | 尺寸变化时由 NGX 侧重建 | 尺寸变大超出已建上界 | 显示尺寸变化 / 输入越界 / 掩码开关变化 |

DLSS 的 caps 由 `fillDlssCaps()`（`src\advanced\d3d12_sr.cpp`）填：`available` 来自
`queryDlss()`，其余字段与 FSR / XeSS 同值（320x180 / 7680x4320 / 8 / REACTIVE / `externalHeap=1`）。

## 9. 失败与降级路径

| 失败点 | 表现 | 恢复方式 |
| --- | --- | --- |
| 运行库不在 / 入口缺失 | `caps.available=0`，`SrCreate` 返回 0，`initialize` 失败 | 补 DLL；`note` 里有 Win32 错误码或缺失入口名 |
| 上下文创建失败 | 一行 WARN（含 `rc` 与尺寸），`evaluate` 返回 0 | 修尺寸 / 驱动；同一失败不会每帧重试 |
| dispatch / execute 失败 | 一行 WARN（含 `rc`），输出已交回 `RENDER_TARGET` | 调用方退回无超分路径 |
| 渲染尺寸 < 320x180 | 跳过本帧，只记一次 `dbgLogError` | 抬高渲染比例 |
| 渲染尺寸 > 8192 | 拒绝，`problem_ = "size beyond 8k"` | 降低分辨率 |
| 缺输入 / 输出纹理 | 返回 `false`，原因写 `problem_` | 修 JVM 侧资源生命周期 |
| `SrCreate` 时命令列表没开 | 返回 -1 或 0（取决于后端） | 帧内调用顺序错误，改调用点 |

降级一律是「这一帧没有超分」，不是「这一帧失败」：`mcdr_dx12SrEvaluate` 返回 0，画面仍是低
分辨率放大，`SrQuery` 里能看到原因。

## 10. 已知缺陷

> **缺陷**：FSR 的 `caps.externalHeap` 报 0，与实现不符。`d3d12_fsr.cpp` 已经通过
> `ffxCreateBackendDX12AllocationCallbacksDesc` 接管了 DEFAULT 堆的分配，`fillCapsFields()` 里
> 「v1 用 FFX 自己的分配器」那句已经过期；上层若按 `externalHeap` 决定要不要准备堆，会误判。

> **缺陷**：`pColorOutput` 没有 `ALLOW_UNORDERED_ACCESS` 时，屏障在 `Close()` 时被拒为
> `E_INVALIDARG (0x80070057)`，日志只看到「关命令列表失败」，根因在 JVM 侧选了哪张纹理。
> 输出必须是 `USAGE_STORAGE` 纹理。

> **缺陷**：`mcdr_dx12SrJitter` 恒返回 0，亚像素 jitter 只有一个源（`D3D12Temporal` 的
> Halton(2,3)）。「后端偏好的 jitter 序列」在 ABI 上存在、实际不可用；给某个后端单独开一个
> jitter 源会让画面抖。

> **缺陷**：FSR 的 jitter 相位用公式 `8 * (dw/rw)^2` 近似，没有问运行库
> （`ffxQueryDescUpscaleGetJitterPhaseCount`）。运行库内部分辨率与公式有偏差时，相位长度会
> 与实际序列长度不一致。

> **缺陷**：`DlssSrProvider::shutdown()` 是空操作（刻意的）：`mcdr_dlss_shutdown` 会把实现单元的
> 全局状态整块清掉，连带关掉还没切到 SR 路径的老 DLSS 调用，导致 hand-off 丢输出纹理。
> 也就是说 `SrDestroy` 对 DLSS 不释放任何东西。

> **缺陷**：XeSS 的质量档是「按输入区间挑」而不是「按用户选的渲染比例」，caps 里也没有暴露
> 实际选中的档。要确认当前档位，只能看 `XeSS ready: … quality=<n> inputRange …` 这一行。

> **缺陷**：`gSrPicked` 只在 JVM 调 `mcdr_dx12SrPick` 时才可信；只调 `SrCreate` 的路径上它是
> NONE。任何「用户选了哪家」的判断在 native 侧必须用 `gSrActive`，用错会得到「永远 NONE」。

> **缺陷**：低于 320x180 时是**直接跳过**，没有自动抬高渲染比例的兜底；`eval=` 计数会持续增长
> 而画面无超分。

## 文件导航

| 路径（相对 `MCDR\`） | 负责什么 |
| --- | --- |
| `include\mcdr_abi.h` | SR 段公共契约：`mcdr_sr_caps`、`mcdr_sr_dispatch`、`MCDR_SR_*` 与九个入口的声明 |
| `src\c_abi\abi_sr.cpp` | C ABI 适配：参数校验、结构体填充、转发给设备模块；不含任何厂商名 |
| `src\advanced\d3d12_sr.cpp` | 注册表（`gSrPicked` / `gSrActive` / `gSrProvider`）、`DlssSrProvider`、`srStatusText`、jitter 相位 |
| `src\advanced\d3d12_fsr.cpp` | FSR 后端：运行库加载、堆中介、上下文创建与重建、dispatch |
| `src\advanced\d3d12_xess.cpp` | XeSS 后端：运行库加载、质量档挑选、外部临时堆、init / execute |
| `src\module\core\mcdr_device.cppm` | `SrBackendId`、`IUpscaleProvider` 接口、`transitionTextureTo` / `restoreDrawHeap` 等模块内声明 |
| `docs\advanced\dlss.md` | DLSS / NGX 那条实现路径（本文只做对比） |
| `docs\abi\README.md` | C ABI 总体约定（字符串写出、错误状态、布局自检） |
| `docs\device\README.md` | 设备模块、资源状态跟踪、延迟删除 |

## 文档维护

- 改 `mcdr_sr_caps` / `mcdr_sr_dispatch` 的**任何字段**，同步改 §3 的偏移表、`MCDR_Layout.kt` 的
  `SR_CAPS` / `SR_DISPATCH`，以及 `_port\abi_check.py` 的期望尺寸（80 / 136）。
- 新增一条后端：实现 §2 的虚函数、在 `makeProvider()` 加分支、在 `srFillCaps()` 加 case、在
  `mcdr_abi.h` 加 `MCDR_SR_BACKEND_*`，然后回来补齐 §8 的表与新后端的 caps 段落。
- 改任何「谁把哪张纹理转到哪个状态」的行为，同步 §5.1 与 §4.4（`restoreDrawHeap` 不能省）。
- 改 FSR / XeSS 的尺寸上下限或质量档策略，同步 §5.2、§7.2、§8，并确认 caps 报的值与实际
  拒绝条件一致。
- 修掉 §10 的某条缺陷时删掉对应警告块，不要留「已修复」的历史叙述。
