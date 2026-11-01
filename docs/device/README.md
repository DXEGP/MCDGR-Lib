# D3D12 设备与资源层

> 模块 `mcdr.d3d12.device`（导出命名空间 `d3d12_mc`）。
> 接口单元 `src\module\core\mcdr_device.cppm`（1688 行）；
> 实现单元 `src\core\d3d12_device.cpp`（10694 行）与 `src\core\d3d12_surface.cpp`（交换链，见
> [`surface/README.md`](../surface/README.md)）。

本文覆盖：设备创建、适配器选择、描述符堆与分配器、命令列表与 fence、屏障约定、
管线/根签名、瞬态内存与上传回读、resize 挂点、PIX 挂点，以及当下仍然成立的限制与缺陷。
异步录制的三件套（分区分配器 / bundle 池 / 执行器）只在本文点到为止，细节在
[`async/README.md`](../async/README.md)。

---

## 1 这一层是什么

`mcdr.d3d12.device` 是 MCDR 里**唯一**持有 `ID3D12Device` 的单元。
它把 D3D12 的"有状态、必须按顺序调用"的 API 收敛成一组**无状态、可任意顺序调用**的导出函数：
调用方不需要知道当前堆是什么、资源处在什么 barrier 状态、哪条命令列表是打开的 —— 这些状态全部由
`DeviceContext` / `CommandContext` 里的字段和跟踪表维护。

### 1.1 职责边界

| 归这一层管 | 不归这一层管 |
| --- | --- |
| `ID3D12Device` / `ID3D12CommandQueue` / `ID3D12Fence` 的创建与销毁 | 交换链与 Present（[`surface/README.md`](../surface/README.md)） |
| 七个描述符堆的容量、分配、回收 | 分区描述符分配器与 bundle 池（[`async/README.md`](../async/README.md)） |
| `Dx12Object`（texture / buffer / sampler / view / compute PSO）的生命周期 | 图形 PSO 的*判据*（哪条 `RenderPipeline` 对应哪个 PSO，在 JVM 侧） |
| 命令列表的录制—提交—等待模型 | 超分 / 帧生成后端的内部资源（各 provider 自己管，见 `advanced/`） |
| 资源状态跟踪与 barrier 插入 | 每帧要画什么、什么顺序画 |
| 设备能力和适配器信息的上报 | 配置项解析（`IllusionConfig` → `Snapshot` → ABI） |
| PIX 抓帧的运行库加载 | PIX 的使用流程（[`misc/debug-and-logging.md`](../misc/debug-and-logging.md)） |

### 1.2 调用链

```
Kotlin（Illusion\src\main\kotlin\dev\nts\expdxr\d3d12\**）
  D3D12Device / D3D12CommandEncoderBackend / D3D12GpuBuffer / D3D12TransientMemory
        │  MCDR_ABI.kt：Linker.nativeLinker().downcallHandle(...)（java.lang.foreign，无 JNI）
        ▼
include\mcdr_abi.h                     extern "C"，MCDR_ABI_VERSION = 4u
        │  src\c_abi\abi_device.cpp / abi_command.cpp / abi_draw.cpp / abi_pipeline.cpp
        ▼
module mcdr.d3d12.device                接口 mcdr_device.cppm → 实现 d3d12_device.cpp
        │
        ▼
ID3D12Device  ID3D12CommandQueue  ID3D12DescriptorHeap  ID3D12GraphicsCommandList
```

跨界只有两种类型：**不透明句柄**（`mcdr_object*`、`mcdr_command_ctx*`、`mcdr_pipeline*` …，
在 C 侧是 `typedef struct mcdr_object mcdr_object;`）和 **POD 结构体**。
绝不要把模块内部的 C++ 类型放进导出签名 —— 那会让定义侧与引用侧的修饰名对不上（`LNK2001`），
原因见 [`c_abi/README.md`](../c_abi/README.md)。

### 1.3 最小用法

```c
char info[1024];
mcdr_dx12CreateDevice(info, sizeof info);              // 建设备 + 自检，返回适配器与能力串
mcdr_command_ctx* ctx = MCDR_D3D12_create_command_encoder();

mcdr_dx12BeginRenderPass(ctx, colors, 1, mips, clears, clearColors,
                         depth, 0, 1, 1.0, 0, 0, w, h);
mcdr_dx12SetPipeline(ctx, pso, 1);
mcdr_dx12SetViewport(ctx, 0, 0, w, h);
mcdr_dx12SetScissor(ctx, 0, 0, w, h);
mcdr_dx12SetVertexBuffer(ctx, 0, vbuf, 0);
mcdr_dx12SetIndexBuffer(ctx, ibuf, 1);
mcdr_dx12PushDescriptors(ctx, bindings, count);
mcdr_dx12DrawIndexed(ctx, indexCount, 1, 0, 0, 0);
mcdr_dx12EndRenderPass(ctx);

uint64_t fence = mcdr_dx12Submit(ctx);                 // 非阻塞，返回本 encoder 的 fence 值
mcdr_dx12WaitForFence(ctx, fence, 5000000000LL);       // 需要时再等
```

**顺序不是建议，是要求**：`SetPipeline` 必须在任何 `Set*Root*` / `PushDescriptors` 之前
（D3D12 规定使用 root 参数前必须先设根签名）；`BeginRenderPass` / `EndRenderPass` 必须成对；
`Submit` 会自动替你 `EndCommandList`。

---

## 2 生命周期

### 2.1 状态图

```
       进程启动（无设备）
             │  mcdr_dx12CreateDevice() / 任何懒调用 → ensureDevice()
             ▼
    ┌──────────────────┐
    │   DEVICE READY   │◄──────────────────┐  ensureDevice() 幂等快路径（deviceReady()）
    └────────┬─────────┘                   │
             │  MCDR_D3D12_create_command_encoder()
             ▼                             │
    ┌──────────────────┐                   │
    │  ENCODER CREATED │  3 个 allocator + 3 条 list（全部 closed）
    └────────┬─────────┘                   │
             │  BeginCommandList()          │
             ▼                             │
    ┌──────────────────┐  EndCommandList() │
    │  LIST RECORDING  │──────────────┐    │
    └────────┬─────────┘              │    │
             │  BeginRenderPass()     │    │
             ▼                        │    │
    ┌──────────────────┐ EndRenderPass│    │
    │   IN RENDER PASS │──────────────┤    │
    └────────┬─────────┘              │    │
             │  Submit()              │    │
             ▼                        ▼    │
    ┌──────────────────┐  ◄────────────────┘
    │    SUBMITTED     │  再次 BeginCommandList()
    └────────┬─────────┘
             │  DestroyCommandEncoder() / DestroyDevice()
             ▼
    ┌──────────────────┐
    │    TORN DOWN     │
    └──────────────────┘
```

### 2.2 `ensureDevice()` 的固定顺序

`ensureDevice()`（`src\core\d3d12_device.cpp:2402`）是设备创建的**唯一**入口，
懒调用点有十几个，所以它自己做了双检 + `std::mutex gDeviceInitMutex` 串行化
（另有一个 `thread_local bool t_deviceInitInProgress` 检测重入，命中直接返回
`"recursive ensureDevice() during device initialization"`）。

这个顺序不能改，每一步都依赖前一步：

| # | 动作 | 为什么必须在这个位置 |
| --- | --- | --- |
| 1 | `streamlineInitEarly()` | 必须在**任何** DXGI / D3D12 调用之前（`slInit` 要在驱动被 hook 前装好） |
| 2 | `applyAgilitySdk()` | 决定后面 `D3D12CreateDevice` 用的是 system 还是 app-local 的 `D3D12Core.dll` |
| 3 | `ensurePixCapturerLoaded()` | PIX 的捕获层必须在**第一个** D3D12 设备存在之前注入 |
| 4 | `installCrashHandlerOnce()` | 后面任何一步都可能崩，过滤器要先生效 |
| 5 | `deviceReady()` 快速路径 | 已建好就直接返回（12 项与门） |
| 6 | 慢路径加锁 + 重入检测 | 十几个懒调用点会并发进来 |
| 7 | 读 `DX12_DEBUG_LAYER` 决定开不开调试层 | 创建之前唯一的决策点；**Agility 生效时不开**（见 §3.3） |
| 8 | `selectDeviceAdapter()` → `createD3D12Device(...)` | 见 §3.1；失败会依次降级 |
| 9 | `queryDeviceCapabilities()` + `applyStreamline()` | 需要真实 device 才能查 shader model / mesh tier / VRS tier |
| 10 | `EnumAdapterByLuid` 取回 adapter | RTX 探测要走 NGX 的 `GetFeatureRequirements`，要真实 adapter |
| 11 | `wireDlssImpl()` → `probeRtxSupport()` → `notifyDlssDeviceReady()` | 日志出口必须先接好，探测内部会调 `mcdr_dlss_load_runtime()` |
| 12 | `CreateCommandQueue(DIRECT)` + `GetTimestampFrequency` | 单队列，见 §5.1 |
| 13 | `CreateFence(0)` | 全局队列 fence |
| 14 | 建**七个**堆 | 顺序即失败点，任一失败报 `"CreateDescriptorHeap failed"` |
| 15 | 取四个 increment、`resetDescriptorAllocationState()` | 之后所有句柄偏移都靠它们 |
| 16 | 两个间接绘制签名 | `cmdSigIndexed`(`DRAW_INDEXED`, stride 20) / `cmdSigNonIndexed`(`DRAW`, stride 16) |

`deviceReady()`（`:2564`）的判据必须与慢路径**真正建出来的东西**逐项一致：

```cpp
device && queue && queueFence && srvHeap && srvCpuHeap && rtvHeap && frameRtvHeap
    && dsvHeap && samplerHeap && drawHeap && cmdSigIndexed && cmdSigNonIndexed
```

漏一项的后果不是"慢一点"，而是**每次调用都命中快速路径**，然后在解引用空指针时崩。

### 2.3 每帧

```
主线程                                      渲染线程（可选，initAsyncRenderer 之后）
──────────────────────────────────────────  ─────────────────────────────────────
ensureListOpen() → BeginCommandList()
   （Reset allocator + Reset list）
BeginRenderPass / Set* / Push / Draw ...
EndRenderPass
Submit()
   ├─ EndCommandList()   → Close()
   ├─ asyncRenderBeginFrame()  ───────────►  等 gEvtBeginFrame
   │                                        SetEvent(gEvtRecordingReady)
   ◄───────────────────────────────────────
   ◄─ 等 gEvtRecordingReady，开录
   ├─ asyncSendCommandsReady() ───────────►  等 gEvtCommandsReady
   │                                        ExecuteCommandLists + Signal
   │                                        SetEvent(gEvtSubmitDone)
   │                                        presentSurface()
   └─ 下一帧 settlePendingSubmit()
```

没有异步渲染器（初始化阶段 / 无 active surface）时走 SYNC：`Submit` 内部直接
`ExecuteCommandLists` + `Signal`，返回时命令已进队列但**没有**等 GPU。

### 2.4 `destroyDevice()`

`:2781`，五步，顺序不可换：

1. 关掉 `queueFenceEvent`；
2. 若 `queueFence->GetCompletedValue() < queueFenceValue` 则 `SetEventOnCompletion` + 等 1000ms；
3. `flushPendingDeletes(/*force=*/true)` —— 无视 fence 门槛，把延迟删除队列清空；
4. `mcdr_dlss_shutdown()` —— **必须在设备还活着时**，NGX 要拿 device 释放自己的资源；
5. `gCtx = DeviceContext{}`，再 `resetDescriptorAllocationState()`。

### 2.5 JVM 侧 `D3D12Device.close()` 的顺序

native 侧没有"析构顺序"这个概念（单例 + 手工 teardown），真正的顺序约束在 JVM 侧：

| # | 动作 | 约束 |
| --- | --- | --- |
| 1 | `D3D12TerrainSampler.release()` | 地形 mip-bias 采样器由模组持有，必须在设备销毁前释放 |
| 2 | 共享 encoder `close()` + `D3D12DestroyCommandEncoder()` | 共享 encoder 的 `close()` **不**调 destroy（`CubeMap.render()` 内部 close 会顺手销毁 ctx），所以这里要显式再调一次 |
| 3 | `clearPipelineCache()` | 释放全部 PSO / 根签名 |
| 4 | `glslCompiler.close()` | |
| 5 | `D3D12AsyncContext.shutdown()` + `D3D12DestroyAsyncRenderer()` | 先停渲染线程，否则线程会继续碰已销毁的 ctx |
| 6 | `MCDR_ABI.D3D12DestroyDevice()` | **必须趁 DXGI/D3D12 仍存活时显式调用**：留给 `DLL_PROCESS_DETACH` 会让 `ComPtr::Release()` 在 `dxgi.dll` 已 detach 后执行 → 原生异常，退出码变成 2170 |
| 7 | `D3D12AdapterInfo.current = null` | |

---

## 3 适配器选择与设备创建

### 3.1 枚举逻辑

`selectDeviceAdapter()`（`:854`）**不用** `D3D12CreateDevice(nullptr, ...)`：

```cpp
constexpr UINT kMaxAdapters = 32;
CreateDXGIFactory2(0, IID_PPV_ARGS(&factory6));           // 失败则退 CreateDXGIFactory1
factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, ...);
// 跳过 DXGI_ADAPTER_FLAG_SOFTWARE
// 对每块适配器 probeHighestFeatureLevel()，>= 12_2 立即返回
```

不用"系统默认适配器"的理由：混合显卡机器上默认适配器常常是核显，与窗口不在同一 GPU 时
`CreateSwapChainForHwnd` 在所有 tier 上返回 `E_ACCESSDENIED`（`0x80070005`）。

全部不达标时报错串是逐块候选的报表：

```
no adapter supports D3D_FEATURE_LEVEL_12_2; candidates:
<每块适配器的名字与最高 FL>
```

`probeHighestFeatureLevel(IDXGIAdapter*)` 按 `kProbeLevels[] = {12_2, 12_1, 12_0, 11_1, 11_0}`
逐个试 `D3D12CreateDevice`；返回 `(D3D_FEATURE_LEVEL)0` 表示这块适配器完全不支持 D3D12。

### 3.2 feature level 与 shader model

```cpp
constexpr D3D_FEATURE_LEVEL kRequiredFeatureLevel = D3D_FEATURE_LEVEL_12_2;
constexpr D3D_SHADER_MODEL kShaderModelCeiling = static_cast<D3D_SHADER_MODEL>(0x6a);  // 6.10
```

- **12_2 是硬要求**：它是 mesh shader（`OPTIONS7`）、DXR（`OPTIONS5`）、VRS（`OPTIONS6`）的共同下限。
  `ensureDevice()` 只接受 12_2，没有低于它的降级路径。
- **Shader model 上限是 6.10**：SDK 10.0.26100 的 `d3d12.h` 只定义到 `D3D_SHADER_MODEL_6_9`，
  6.10 用数值 `0x6a` 补齐。`effectiveShaderModel()` 取"驱动上报"与 `kShaderModelCeiling` 的较小值。
- **必须逐档探测，不能直接填 0x6a**：在只支持到 6.9 的 runtime 上
  `CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, ...)` 返回 `E_INVALIDARG`，能力会静默停在默认 5.1。
  探测序列是 `{0x6a, 6_9, 6_8, 6_7, 6_6, 6_5, 6_4, 6_3, 6_2, 6_1, 6_0, 5_1}`。
- **图形管线只走 SM 6.9**（`graphicsShaderModelProfile()`，`:5176`）：VS/PS 由 DXC 编成
  `vs_6_9` / `ps_6_9`，**没有** `D3DCompile` 或 SM 5.1 兜底。拿不到 profile 时失败串由
  `sm69RequirementText()` 给出。

### 3.3 debug layer

只有一条开关：环境变量 `DX12_DEBUG_LAYER`，首字符为 `'1'` 才开。
开了以后额外做两件事：

1. `QueryInterface(ID3D12InfoQueue)` + `ID3D12InfoQueue1::RegisterMessageCallback(...)`，
   把消息送进 `dx12DebugMessageCallback`；
2. `createD3D12Device` 失败时关掉调试层**重来一次**（调试层自己也会让创建失败）。

> **限制**：**Agility 生效时 `DX12_DEBUG_LAYER` 被忽略**，只打
> `agility: DX12_DEBUG_LAYER ignored - inbox D3D12SDKLayers would not match the 1.619 core`。
> inbox 的 `D3D12SDKLayers.dll` 与 1.619 的 core 版本不匹配，同时装载会让设备创建直接失败。

### 3.4 Agility SDK

`applyAgilitySdk()`（`:2287`）**不由环境变量门控**，永远尝试：

```
ownModuleDir() → 同目录必须存在 D3D12Core.dll
                 （否则 WARN "agility: %s not found next to MCDR.dll - skipped"）
  → LoadLibraryW("d3d12.dll") → GetProcAddress("D3D12GetInterface")
  → CLSID_D3D12SDKConfiguration → QI ID3D12SDKConfiguration1
  → CreateDeviceFactory(kAgilitySdkVersion = 619, <模块目录 utf8>, ...)
  → factory->ApplyToGlobalState()
```

为什么是 619：SM 6.9 只有 Agility 619 的 core 才会报，inbox `D3D12Core` 最高 6.8。

**判断"到底用了哪份 core"只能靠 `ID3D12DeviceConfiguration::GetDesc().SDKVersion >= 619`**
（`logAgilityRuntime()`，`:2351`）。不要用 `GetModuleHandleW("D3D12Core.dll")` 的路径判断：
`d3d12.dll` 在返回 `SDKConfiguration` 之前已经把 System32 那份载入，app-local 的同名模块是第二个模块。

### 3.5 Streamline interposer

开启 DLSS 时设备本身也必须由 Streamline 的 interposer 创建：

```cpp
typedef HRESULT(WINAPI* PFN_SL_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
// interposerCreateDeviceFn()：mcdr_dlss_streamline_requested() != 0 时
//   从 MCDR.dll 同目录 LoadLibraryW("sl.interposer.dll") 取 "D3D12CreateDevice"
```

漏掉这一步的后果不是"性能差一点"：SL 的 manual hooking 要求设备由 interposer 创建，
否则插件虽然加载了但永远 not ready —— `slIsFeatureLoaded` / `slGetFeatureFunction`
返回 `0x1F`（`eErrorFeatureNotSupported`）。

### 3.6 能力查询

`queryDeviceCapabilities(ID3D12Device*, Capabilities&)`（`:2082`）填这张表：

| 字段 | 来源 | 用途 |
| --- | --- | --- |
| `shaderModel` | `D3D12_FEATURE_SHADER_MODEL` 逐档探测 | DXC profile 选择 |
| `meshShaderTier` | `D3D12_FEATURE_DATA_D3D12_OPTIONS7` | `meshShadersSupported()` = tier ≥ TIER_1 |
| `samplerFeedbackTier` | `OPTIONS7` | 仓库内未消费 |
| `raytracingTier` | `OPTIONS5` | 光追能力上报 |
| `shadingRateTier` / `shadingRateImageTileSize` / `additionalShadingRates` | `OPTIONS6` | VRS 是否放行（§10） |
| `resourceBindingTier` | `OPTIONS` | 上报 |
| `enhancedBarriers` | `OPTIONS12` | 上报（**没有**使用 enhanced barrier API） |
| `dxcAvailable` / `meshShaderCompileOk` / `dxcPath` / `dxcProfile` / `dxcNote` | `probeDxc()` | 图形与 mesh 管线能否创建 |

`probeDxc()`（`:1900`）搜索 `dxcompiler.dll` 的顺序：`MCDR_DXC_DIR` 环境变量 →
`MCDR.dll` 同目录 → Windows SDK `bin\<版本最大>\x64`。找到后 `DxcCreateInstance`，
并用一段内联 mesh shader（`kMeshSmokeHlsl`，`ms_6_9`）做冒烟编译，成功才置
`meshShaderCompileOk = true`。

`mcdr_dx12CreateDevice(buf, cap)` 写出的就是这一串（`capabilitiesText()`，`:2153`）：

```
<适配器名> (FL=12_2 SM=6.9(0x69)/eff=6_9 mesh=1 ... dxc=yes(ms_6_9) meshSmoke=ok ...); SELF-TEST OK (...)
```

完整字段走 `mcdr_dx12GetAdapterInfo()` 的 JSON（`adapterInfoJson()`，`:7499`），
键是 `shader_model` / `mesh_shader_tier` / `shading_rate_tier` … 这样带 `caps_*` 语义的名字，
**刻意不含 `feature_level`**（适配器 JSON 里已经有这两个键，重名会让解析方拿到后者）。

### 3.7 创建失败的兜底

```
selectDeviceAdapter() → createD3D12Device(chosen, 12_2, ...)
   ├─ 失败且开了调试层 → 关调试层重来一次
   ├─ 再失败 → createD3D12Device(nullptr, ...) 系统默认适配器
   │            （成功时 dbgLogWarn "using the system default adapter after: <原因>"）
   └─ 全失败 → mcdr::err("ensureDevice", <selectDeviceAdapter 的错误>)
```

---

## 4 堆与描述符分配

### 4.1 七个堆

全部在 `ensureDevice()` 第 14 步创建（`createHeap()`，`:296`），容量是编译期常量：

| 堆字段 | `D3D12_DESCRIPTOR_HEAP_TYPE` | 容量常量 | 数值 | Visibility | 用途 |
| --- | --- | --- | --- | --- | --- |
| `srvHeap` | `CBV_SRV_UAV` | `kSrvHeapSize` | 262144 | `SHADER_VISIBLE` | 长期 SRV/UAV，`createTextureView()` 在这建 GPU 侧视图 |
| `srvCpuHeap` | `CBV_SRV_UAV` | `kSrvHeapSize` | 262144 | `NONE` | 同槽位的 CPU-only 镜像，作 `CopyDescriptorsSimple` 的源 |
| `rtvHeap` | `RTV` | `kRtvHeapSize` | 2048 | `NONE` | **持久** RTV：back buffer 的句柄在这里，由 `allocRtvHandle()` 单调分配 |
| `frameRtvHeap` | `RTV` | `kFrameRtvHeapSize` | 256 | `NONE` | **帧级瞬态** RTV，每帧从 0 复用 |
| `dsvHeap` | `DSV` | `kDsvHeapSize` | 256 | `NONE` | 帧级瞬态 DSV |
| `samplerHeap` | `SAMPLER` | `kSamplerHeapSize` | 2048 | `SHADER_VISIBLE` | `createSampler*()` 的落地处 |
| `drawHeap` | `CBV_SRV_UAV` | `kDrawHeapSize` | 622592 | `SHADER_VISIBLE` | 每帧瞬时 CBV/SRV 的 ring |

**`rtvHeap` 与 `frameRtvHeap` 绝不能混用**。back buffer 的 RTV 是持久的（每帧数量不变），
帧级 RTV 每帧被倒回重写；把颜色纹理的 RTV 也塞进 `rtvHeap` 会让 blit 把颜色纹理当成 RT 绑定 → 黑屏。

increment 全部来自 `GetDescriptorHandleIncrementSize()`，且 `drawInc = srvInc`
（同一个 `CBV_SRV_UAV` 类型，尺寸相同）。

### 4.2 `drawHeap` 的帧槽分区

`drawHeap` 是唯一按"帧槽 × worker"切分的堆。容量由几个常量推出来：

| 常量 | 定义位置 | 数值 | 含义 |
| --- | --- | --- | --- |
| `kAsyncFrameSlots` | `d3d12_async_descriptor.cppm` | 4 | 帧槽数 |
| `kMaxAsyncWorkers` | 同上 | 8 | 分区方案的上限 worker 数 |
| `kAsyncSyncRingReserve` | 同上 | 131072 | 每帧段里给**同步**录制预留的槽位数 |
| `kAsyncWorkerSlotsPerSection` | 同上 | 24576 | 每帧段里给**一个** worker 的槽位数 |
| `kDrawHeapPerFrame` | `d3d12_device.cpp:167` | 155648 | = 131072 + 24576，一个帧段 |
| `kDrawHeapSections` | `d3d12_device.cpp:168` | 4 | 帧段数（= `kAsyncFrameSlots`） |
| `kDrawHeapSize` | `d3d12_device.cpp:169` | 622592 | = 155648 × 4，总槽位数 |

```
drawHeap（622592 槽）
├── frameSlot 0（第 0 .. 155647 槽）
│   ├── 同步 ring 预留：131072 槽（0 .. 131071）
│   └── worker 区：24576 槽，再按 workerCount 均分
├── frameSlot 1（155648 .. 311295）
├── frameSlot 2（311296 .. 466943）
└── frameSlot 3（466944 .. 622591）
```

槽位地址由两段拼出来，两段都必须算对：

```cpp
ctx->drawHeapSlotBase = (fenceValue % kDrawHeapSections) * kDrawHeapPerFrame;  // beginCommandList
slot = ctx->drawHeapSlotBase + (ctx->nextDrawSlot++);                          // pushDescriptors
```

worker 区的起点用 `PartitionedDescriptorAllocator::regionBaseSlot()`：

```
sectionSize    = heapSize / kAsyncFrameSlots                         // 155648
slotsPerRegion = (sectionSize - reservedPerSection) / workerCount    // 24576 / workerCount
regionBaseSlot = frameSlot * sectionSize + reservedPerSection + worker * slotsPerRegion
```

所以 `kAsyncWorkerSlotsPerSection = 24576` 只覆盖"一个 worker"；实际多 worker 时每人分到的更小，
`allocate()` 在区域不足时返回 `UINT_MAX`，调用方回退串行录制（由同步 ring 承接）。

为什么同步 ring 要 131072：每次 `pushDescriptors()` 里每个 SRV 占 1 槽，
图集上传（`TextureAtlas.uploadInitialContents`）在同一条未提交列表里录制
`mipLevelCount × staticSprites.size()` 个 draw。622592 槽仍远低于 `CBV_SRV_UAV` 堆的
1,000,000 上限。

### 4.3 分配器策略与回收

| 分配器 | 游标 | 回收方式 | 耗尽行为 |
| --- | --- | --- | --- |
| SRV（`allocSrvSlot`，`:251`） | `gNextSrv` 单调 | `Dx12Object->descSlot` 在销毁时归还 `gFreeSrvSlots` free-list | 见下 |
| Sampler（`allocSamplerSlot`，`:269`） | `gNextSampler` 单调 | 同上 → `gFreeSamplerSlots` | 同构 |
| 持久 RTV（`allocRtvHandle`，`:2843`） | `gNextRtv` 单调 | **不回收** | `"RTV descriptor heap exhausted"` |
| 帧级 RTV（`frameRtvFor`，`:4522`） | `gNextFrameRtv` 每帧归零 | 每 `beginCommandList` 归零 + 缓存清空 | `"frameRtvFor: frame rtv heap exhausted"` |
| 帧级 DSV（`frameDsvFor`，`:4558`） | `gNextDsv` 每帧归零 | 同上 | `"frameDsvFor: dsv heap exhausted"` |
| draw heap | `ctx->nextDrawSlot` 每 list 归零 | 靠帧槽轮转 + fence 保证 GPU 已用完 | 复用上一张 SRV 表，否则报错（§10） |
| compute UAV（`computeUavHeap`） | `computeUavNext` | 64 槽 ring | typed UAV 必须走描述符表（root UAV 只支持 raw/structured） |

`allocSrvSlot()` 的完整策略：

```
1. 有 free-list 条目 → 直接弹一个（后进先出）
2. gNextSrv < kSrvHeapSize → 取 gNextSrv 并 ++
3. 堆满，且 gOpenListCount == 0 且 pendingDeletesCount() > 0
      → flushPendingDeletes()（此时没有打开的 list，延迟删除可立即结算）后再试一次
4. 仍满 → mcdr::err("allocSrvSlot",
      "srv heap exhausted (next=… free=… pending=… openLists=…)")
```

第 3 步的条件很严：**只要还有 list 打开就不 flush**。所以一条超长列表（图集上传那种）里
耗尽 SRV 堆是**不可恢复**的 —— 报错而不是等待。

### 4.4 帧槽与 fence 的关系

"第几帧"在下面五处各有一份表示，必须能对上：

| 表示 | 表达式 | 深度 | 谁在用 |
| --- | --- | --- | --- |
| 命令列表槽 | `ctx->fenceValue % 3` | 3 | `allocators[3]` / `commandLists[3]` |
| draw heap 帧段 | `ctx->fenceValue % kDrawHeapSections` | 4 | `drawHeapSlotBase` |
| 异步帧槽 | `currentDrawFrameSlot(ctx)` = `fenceValue % 4` | 4 | `PartitionedDescriptorAllocator` / `BundleRecorderPool` |
| 异步渲染帧 | `gSubmittedFrames` / `gPresentedFrames` | 无固定深度 | 渲染线程 |
| JVM 瞬态内存帧 | `FRAMES_IN_FLIGHT = 2` | 2 | `D3D12TransientMemory` 的 frame 队列 |

**GPU 是否用完某个槽位，只有一个判据：全局队列 fence 的已完成值。**
`beginCommandList` 在 Reset 之前只等该槽位上一次的 per-list fence（三路 ring 的全部意义），
而 `drawHeap` 的帧段重用依赖"上一轮的同一帧段已完成"，由 4 段 × 提交节奏共同保证。

### 4.5 缓冲池

`createBuffer()` 不总是新建 `ID3D12Resource`：非 UAV 缓冲先走 best-fit 池。

| 项 | 值 |
| --- | --- |
| 池上限 | `kMaxFreeBufferBytes = 96ull * 1024 * 1024`（96 MiB） |
| 池键 | `(D3D12_HEAP_TYPE, 容量)` |
| 命中条件 | 同堆类型、容量 ≥ 请求、且是该堆类型里容量最小的那块 |
| 容量口径 | `resource->GetDesc().Width`，或 `(size + 255) & ~255` |
| 回收时机 | `flushPendingDeletes()` 里，`kind == Buffer` 且**不是** `force` 模式 |
| 不进池 | READBACK 堆缓冲（一次性读回，进池只会把 96 MiB 占满）；带 `ALLOW_UNORDERED_ACCESS` 的 UAV 缓冲（池键里没有这个维度，复用会丢 flag） |

超出 96 MiB 时直接 `delete`，并把 `gLiveBuf` / `gLiveUploadBytes|Default|Readback` 的计数减回去。

---

## 5 命令列表管理

### 5.1 录制模型：一个 encoder、三条 list、**一个**队列

`createCommandEncoder()`（`:3674`）一次创建 **3 个 allocator + 3 条 list**，全部初始 `Close()`：

```
CommandContext
├── allocators[3]      ID3D12CommandAllocator（DIRECT）
├── commandLists[3]    ID3D12GraphicsCommandList（初始 closed）
├── fence              ID3D12Fence（per-ctx）
├── fenceValue         从 1 单调递增
├── slotFence[3]       每条 list 最后一次提交的 fence 值（三路 ring 的等待依据）
├── resourceState      unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES>
└── rtvCache/dsvCache  RTV/DSV 帧级缓存，键 = (资源指针, mip)
```

设备只有**一个** `ID3D12CommandQueue`（`D3D12_COMMAND_LIST_TYPE_DIRECT`）。
没有 copy queue、没有 compute queue、没有多队列并行。

`beginCommandList()`（`:3733`）挑槽位：`slot = fenceValue % 3`，然后只等这个槽位上**上一次**的
per-list fence（`ctx->slotFence[slot]`）。环境变量 `DX12_CMD_RING=0` 可以退回"等本 ctx 全部提交"的
保守行为，仅供排查用。

### 5.2 两套 fence

这是最容易搞错的地方：**`CommandContext::fence` 与 `DeviceContext::queueFence` 是两个不同的序列。**

| | per-ctx fence | 全局 queue fence |
| --- | --- | --- |
| 字段 | `ctx->fence` / `ctx->fenceValue` | `gCtx.queueFence` / `gCtx.queueFenceValue` |
| 何时 Signal | 每次 `ExecuteCommandLists` 之后，值 `++ctx->fenceValue` | 同一时刻，值 `++gCtx.queueFenceValue` |
| 语义 | "我这一个 encoder 的第 N 次提交完成了" | "整个设备到这一刻为止的所有提交都完成了" |
| 谁等它 | `waitForFenceValue` / `waitForContextFenceValue` / `beginCommandList` 的 ring 等待 | `flushPendingDeletes` / `deviceWaitIdle` / `waitForPendingPresents` / `surfaceFences` |
| ABI | `mcdr_dx12GetFenceValue(ctx)` / `mcdr_dx12WaitForFence(ctx, v, ns)` | 无直接导出（内部用） |

`submitCommandList()` 返回的是 **per-ctx 的** 值，不是 `queueFenceValue`。
JVM 侧 `createFence()` 的 token 必须是 `D3D12GetFenceValue(ctx) + 1`，
**不能**退回全局序列：一次性 encoder（`createBuffer(data)` 的上传）提交时也会推进全局序列，
而 vanilla 在帧中间取 token，会让 `StagedVertexBuffer` / `MappableRingBuffer` 的缓冲在帧提交前被提前回收。

### 5.3 提交语义

`submitCommandList(ctx)`（`:4142`）：

| 前置状态 | 行为 | 返回值 |
| --- | --- | --- |
| `listOpen == 0`（空帧） | **不执行** `ExecuteCommandLists`（重复执行同一 list 会重写资源） | 当前 `ctx->fenceValue`，**不递增** |
| `listOpen == 1` | `EndCommandList` → `ExecuteCommandLists(1, {list})` → `++fenceValue` → `slotFence[slot] = value` → `Signal(ctx->fence, value)` → `++queueFenceValue` → `Signal(queueFence, qv)` | 新的 `ctx->fenceValue` |

`EndCommandList`（`:4033`）失败时会先 `dumpInfoQueueMessages()`，再返回
`"endCommandList: Close <HRESULT> — <deviceStatusText>"`。
`listOpen = 0` 之后 `--gOpenListCount`，**但此处不 flush 延迟删除** —— 必须等渲染线程的 Present fence。

### 5.4 延迟删除

`destroyObject()`（`:3256`）从不立即 `delete`：

```cpp
required = (gOpenListCount > 0) ? kFenceBlockedByOpenList /* UINT64_MAX */
                                : (gCtx.queueFenceValue + 1);
gPendingDeletes.push_back({obj, required});
```

- 立即释放会在 `Close()` 时报 `"deleted prior to closing the command list"`（`E_INVALIDARG`）。
- `+1` 覆盖"并发中尚未 Signal 的那次提交"。
- `kFenceBlockedByOpenList` 是占位值，`flushPendingDeletes()` 在 `gOpenListCount == 0` 时
  才把它回填成当时的 `queueFenceValue`。
- `flushPendingDeletes()` 用 `InterlockedExchange(&gPdInProgress, 1)` 防重入；并发/重入调用
  直接跳过并打 `"flushPendingDeletes: concurrent/re-entrant call skipped (pending=…)"`。

### 5.5 异步渲染线程

`initAsyncRenderer()` 起一条渲染线程，它的**唯一**职责是
`Close + ExecuteCommandLists + Signal + Present`。

**`allocator->Reset()` 和 list 的 `begin` 只能由主线程做。**
渲染线程如果也 Reset，会把主线程刚录制的整帧命令丢掉：back buffer 永远不被写入，
Present 只能显示未初始化的内容 —— 表现是窗口闪烁各种颜色。

四阶段握手：

| 阶段 | 主线程 | 渲染线程 |
| --- | --- | --- |
| `BEGIN_FRAME` | `asyncRenderBeginFrame(ctx)`：存 `gAsyncRenderCtx`、Reset 三个事件、`SetEvent(gEvtBeginFrame)` | 等 `gEvtBeginFrame`，再 `SetEvent(gEvtRecordingReady)` |
| `RECORDING_READY` | `asyncRenderWaitRecordingReady(ctx, 5000)` 后开始录制 | 等 `gEvtCommandsReady` |
| `COMMANDS_READY` | `asyncSendCommandsReady(ctx)` → `SetEvent(gEvtCommandsReady)` | 等到后 `submitCommandList()` |
| `SUBMIT_DONE` | 下一帧 `settlePendingSubmit()` | `++gSubmittedFrames` → `SetEvent(gEvtSubmitDone)` → `presentSurface()` |

- `gEvtBeginFrame` 必须是 **auto-reset**：manual-reset 且无人 Reset 时，渲染线程完成一帧回到等待点
  会立刻通过 → 自旋进下一帧 → 可能解引用已释放的 ctx → `0xC0000005`。
  另外三个事件保持 manual-reset（广播语义）。
- 渲染线程等 `gEvtCommandsReady` 时每 500ms 检查一次 `gRenderRunning`，**10 次（5 秒）超时**
  就 `endCommandList` + 清 `gAsyncRenderCtx` + `SetEvent(gEvtSubmitDone)` 放弃这一帧。
- `gEvtSubmitDone` **先于** `presentSurface()` 触发，让主线程下一帧的录制与本帧 Present 重叠
  （Present 实测 0.13–1.7ms）。
- 每个 back buffer 的 `surfaceFences[bbIdx] = queueFenceValue` 必须在 `presentSurface()`
  **之前**捕获 `bbIdx`；`presentSurface()` 结尾会把 `currentImageIndex` 重置为 -1。

`waitForPendingPresents(timeoutMs = 100)`（`:10470`）超时**返回 false，绝不放行**：
Present 完成后 back buffer index 才轮转，提前 acquire 会拿到同一个 back buffer →
`DXGI_ERROR_ACCESS_DENIED`（`0x887A0005`）。

### 5.6 屏障：谁插入、哪些会被自动合并

**插入责任在设备层。** 调用方只声明"这个资源现在是 X 状态、要变成 Y"，
设备层用 `CommandContext::resourceState` 这张表决定要不要真的发 barrier：

```cpp
mcdr::Result<void> transitionTo(CommandContext* ctx, ID3D12Resource* res,
                                D3D12_RESOURCE_STATES fromInitial, D3D12_RESOURCE_STATES to);
// resourceState 里有记录就用记录值，没有才用 fromInitial；成功后 m[res] = to
```

`from` 与 `to` 相同的三种情形全部被吸收，**这是唯一的"自动合并"**：

| 情形 | 处理 |
| --- | --- |
| 跟踪表里当前状态 == 目标状态 | 完全不发 barrier（不是发一条 nop barrier） |
| `resourceBarrier()` 收到 `res == nullptr` | 打 `"resourceBarrier: null resource (<from> -> <to>) ignored"` 后返回（不放行的话 `Close()` 只剩一句 `0x80070057`，定位不到源头） |
| 连续两次过渡到同一状态 | 第二次被上一条吸收 |

本层**不做**的（也是不能依赖的）：

- **没有 barrier 批处理**：每次状态变化就是一次 `ResourceBarrier`，没有数组合并，
  也没有按资源去重（`Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES`，所以是整资源粒度）。
- **没有 split barrier**：不存在"Begin 现在发、End 后面发"的写法。
- **没有 enhanced barrier**：`OPTIONS12` 的 `enhancedBarriers` 只上报，不使用。

`endCommandList()` 会自动收尾 —— 每个被跟踪的资源按规则回切，回切后从表里删掉：

| 当前状态 | 回切到 |
| --- | --- |
| 含 `NON_PIXEL_SHADER_RESOURCE` 或 `PIXEL_SHADER_RESOURCE` | `COMMON` |
| `COPY_SOURCE` 且在 `stagingCopySourceResources` 里（UPLOAD 堆缓冲） | `GENERIC_READ`（UPLOAD 堆的合法状态；回 `COMMON` 会让下次 Map 读到未定义内容） |
| 其它 | `COMMON` |

**`endRenderPass()` 必须自己回切**，不能指望 `endCommandList` 兜底：
`RENDER_TARGET` / `DEPTH_WRITE` 是**非可提升状态**，不会隐式 decay。不回切会让下一条 list 的
barrier `Before` 与实际状态错配 → 每帧一条验证错误 → 最终 TDR。

`COMMON` 是唯一可提升的状态：从 `COMMON` 隐式 decay 到"任何只读状态"是合法的，
所以采样前的 `transitionTextureTo(PIXEL_SHADER_RESOURCE)` 在多数路径上会被吸收；
但**从 `COMMON` 采样（不发 barrier）是未定义行为**，NVIDIA 驱动会静默返回黑色。

---

## 6 管线与根签名缓存

### 6.1 两级，且只有一级是真的缓存

| 级别 | 位置 | 键 | 内容 |
| --- | --- | --- | --- |
| JVM 侧（唯一缓存） | `D3D12Device.kt` 的 `pipelineCache` / `pipelineCacheFlipY` | `(RenderPipeline 身份, flipY)`，前者用 `IdentityHashMap`，后者用 `HashMap<FlipYKey, ...>` | `D3D12CompiledRenderPipeline`（持有 native 的 `Dx12Pipeline*`） |
| native 侧 | 无 | — | 每次 `createGraphicsPipeline()` 都现建根签名与两条 PSO |

native 侧只有两个**每 ctx 的**小缓存，且都在 `beginCommandList` 时清空：

- `ctx->cachedPso`：`setPipeline()` 的快速路径。`pso == ctx->cachedPso` 时只更新
  `ctx->currentPipeline` 就返回，省 3 次驱动调用。比较的是 PSO 裸指针，管线重建会得到新指针，不会误命中。
- `ctx->rtvCache` / `ctx->dsvCache`：帧级 RTV/DSV 句柄，**键是 `(资源指针, mip)`**；
  另有代计数 `frameViewGeneration` —— 任何 ctx 的 `beginCommandList` 都会倒回设备级游标
  `gNextFrameRtv` / `gNextDsv`，所以游标一倒回，所有 ctx 的缓存必须按代整体失效
  （跨代复用旧句柄会把另一张资源的视图当成自己的）。

### 6.2 根签名布局

`createGraphicsPipeline()`（`:5688`）每次现建 `ID3D12RootSignature`：

| root 参数 | 类型 | 说明 |
| --- | --- | --- |
| 第 0 … `cbvCount-1` | `D3D12_ROOT_PARAMETER_TYPE_CBV`（root descriptor） | 按 `PipelineDesc::bindings` 里的 CBV 顺序**稠密编号**。每 draw 用 `SetGraphicsRootConstantBufferView` 直接把地址烘进命令列表，零描述符堆写入 |
| 第 `cbvCount`（即 `params.back()`） | `D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE` | 所有 SRV 汇入**单个**表；每个 SRV 一条 `D3D12_DESCRIPTOR_RANGE`（`NumDescriptors = 1`、`RegisterSpace = 0`、`OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND`、`BaseShaderRegister = b.reg`） |

- 静态采样器只给 `Binding::type == 1` 的 SRV 生成：`Filter = MIN_MAG_MIP_POINT`、
  `AddressU/V/W` 全 `WRAP`、`MaxAnisotropy = 1`、`ComparisonFunc = NEVER`、
  `BorderColor = OPAQUE_BLACK`、`MinLOD = 0`、`MaxLOD = D3D12_FLOAT32_MAX`、
  `ShaderVisibility = ALL`。
  **必须是 `WRAP`**：对齐 MC 的 REPEAT 默认采样器。用 CLAMP 会让天气粒子的
  `v = y*0.25 + offset`（可达 16~40）整根雨柱都取边缘行 → 贯穿屏幕的实心竖条。
- `Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT`。
- 必须 `D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, ...)` 再
  `CreateRootSignature(0, ...)`；错误串分别是
  `"createGraphicsPipeline: D3D12SerializeRootSignature hr=0x…"` /
  `"createGraphicsPipeline: CreateRootSignature hr=0x…"`。
- `std::vector<D3D12_DESCRIPTOR_RANGE> srvRanges` **必须先填满再取 `data()`**，
  否则 `params` 里的指针会因扩容悬空。
- 输入布局的 `SemanticName` 是 `const char*`，**必须指向持久化内存**
  （`std::vector<std::string> semanticStore`），不能指向局部 string 的 `c_str()`；
  语义名的尾随数字要拆进 `SemanticIndex`（D3D12 要求 `SemanticName` 不能以数字结尾）。

### 6.3 PSO 的两个变体

| 变体 | 何时创建 | DSV format |
| --- | --- | --- |
| `withDepth` | **总是** | `DXGI_FORMAT_D32_FLOAT` |
| `withoutDepth` | 仅 `depthState == nullptr` | `DXGI_FORMAT_UNKNOWN` |

`setPipeline(ctx, p, hasDepth)`（`:6235`）取 `hasDepth ? withDepth : withoutDepth`，
`withoutDepth` 为空时回退 `withDepth`。三条必须遵守的顺序 / 补充：

1. **必须先 `SetGraphicsRootSignature` 再 `SetPipelineState`**。D3D12 规定使用任何 root 参数前
   必须先设根签名；缺失时 NVIDIA UMD 首次真实 draw 会对 NULL root signature 解引用崩溃。
2. **非 mesh 管线必须显式 `IASetPrimitiveTopology()`**。命令列表的初始 topology 是 `UNDEFINED`，
   不设的话 GPU 丢弃全部图元 —— 屏幕上只剩 clear 色。
3. mesh 管线（AS/MS）没有 IA 阶段，跳过上一条。

`destroyPipeline(p)` 就是 `delete p`（`withDepth` / `withoutDepth` 由 `ComPtr` 释放）。

### 6.4 失效条件与 `close()` 顺序

| 触发 | 后果 |
| --- | --- |
| vanilla 资源重载（`clearPipelineCache()`） | 所有 `D3D12CompiledRenderPipeline` 被 close，native 的 PSO 全部释放 |
| 命中缓存但 `cached.isValid()` 为假 | 丢掉重编。**必须验活**：缓存仍握着同一个实例，不验活会导致 `setPipeline` 抛 "not valid" 且一条 COMPILE FAILED 都不打 |
| `D3D12Backend` 自检前后 | 各调一次 `clearPipelineCache()`。自检用内嵌 GLSL 填过 cache，不清的话真实 shader 永远命中、从不编译、屏幕是紫色 |

`clearPipelineCache()` 的顺序固定：**先 close 后 clear，两个 map 分别处理**（`pipelineCache` →
`pipelineCacheFlipY` → `shaderCache`，每个都是 `values.forEach { close() }` 再 `clear()`）。

DXC 侧另有一层**磁盘**缓存（不是内存缓存）：键 =
`fnv1a64(dxcompiler.dll 的尺寸+时间戳) → fnv1a64(profile) → fnv1a64(entry) → fnv1a64(源码)` 链式累加
（`MCDR_DXIL_CACHE=0` 关闭）。`CachedDxc` 单例是 `*new` 出来的，**永不 delete** ——
析构里 Release DXC 的 COM 对象会在 CRT 退出阶段触发 ucrtbase fail-fast（`0xc0000409`）。

---

## 7 瞬态内存与上传/回读环

这一层横跨两端：native 只提供"最小原语"，环与块在 JVM 侧。

### 7.1 native 侧的原语

| 原语 | 关键约束 |
| --- | --- |
| `createBuffer(usage, size)` | `alignedSize = (size + 255) & ~255`（256 对齐）；非 UAV 先走 best-fit 池；UPLOAD 初态 `GENERIC_READ`、READBACK 初态 `COPY_DEST`、其余 `COMMON` |
| `mapBuffer(buf, offset, length, read, write)` | 堆必须是 UPLOAD/READBACK；首次调用做一次 `Map(0)` 并把基址缓存进 `mappedPtr`，之后返回 `mappedPtr + offset`；`read` 需 `usage & 1`、`write` 需 `usage & 2` |
| `unmapBuffer(buf)` | `Unmap(0)` 并把 `mappedPtr` 置空；下次 `mapBuffer` 会重新 Map |
| `copyBufferToBuffer` / `copyBufferToTexture` / `copyTextureToBuffer` | 自动过渡到 `COPY_SOURCE` / `COPY_DEST`，不负责等待 |
| `writeCBVToDrawHeap` / `writeTextureSRVToDrawHeap` / `writeBufferSRVToDrawHeap` | 直接往 `drawHeap` 的槽位写；CBV 的 size 向上取整 256；**不做 transition** |

native 侧**没有** `TransientMemory` / `PipelineCache` 类。`grep` 整个 `src\` 与 `include\`
只命中一处注释，所以"瞬态内存"在 native 就是"256 对齐 + 缓冲池 + Map"。

### 7.2 JVM 侧的环与块

`D3D12TransientMemory.kt`（456 行）的常量就是这一层的参数表：

| 常量 | 值 | 作用 |
| --- | --- | --- |
| `FRAMES_IN_FLIGHT` | 2 | 帧队列深度：帧内 bump 分配的资源保持 2 帧存活 |
| `UBO_RING_COUNT` | 3 | `allocateGpuMapped` 的常驻 UPLOAD ring 路数 |
| `UBO_RING_BLOCK_SIZE` | 4096 | 每路 ring 的字节数 |
| `STAGING_BLOCK_SIZE` | 512 KiB | staging 上传的块基准大小（对齐官方 `TransientBlockAllocator`） |
| `MIN_BLOCK_ALIGNMENT` | 512 | 块内子分配的最小对齐 |
| `STAGING_POOL_COUNT` | 3 | staging 块池的路数（3 > `FRAMES_IN_FLIGHT=2`） |
| `MAX_POOL_BYTES_PER_POOL` | 4 MiB | 单路池常驻字节上限 |
| `MAX_POOLED_BLOCK_SIZE` | 8 MiB | 超过该大小的单次上传块不入池，走帧延迟释放 |
| `MAX_STAGING_MAP_RETRIES` | 1 | 映射失败最多再换 1 块 |

**上传 ring 的 block 对齐是 512，不是 256。** 纹理上传以
`PLACED_FOOTPRINT.Offset = srcOffset` 直接 `CopyTextureRegion`，偏移只有 256 对齐会让校验失败
或内容错乱 —— `MIN_BLOCK_ALIGNMENT` 必须等于 `D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`（512）。

两条上传路径的归属不同，不能混：

| 路径 | 分配方式 | 释放 |
| --- | --- | --- |
| `uploadStaging(payload, align, usage)` | 512 KiB 块分配器，一次上传只切一块，返回块内切片；调用方只做一次 `CopyBufferRegion` | 块随帧 `rotate()` 归还本路池（`P[N % 3]`），最早帧 N+3 才再被取用 |
| `allocateGpuMapped(size, align)` | 3 路常驻 UPLOAD ring 的当前路内 bump；`size > 剩余` 就切下一路 | **只由 `close()` 销毁**，绝不进 frame 队列 —— 归 `uboRing` 所有的缓冲如果也登记进 frame，第 3 次 `rotate()` 整体 close 之后会 map 到已销毁的对象 |

映射失败不抛异常：打一行 WARN、`discardStagingBlock`（不入池、不登记 frame）、换块重试；
连续失败 `MAX_STAGING_MAP_RETRIES` 次才抛。

### 7.3 回读的完成回调

回读**不是** fence 回调，是"下一次 submit 之后"的延迟列表：

```
copyTextureToBuffer(src, dst, offset, callback, ...)
   → native：过渡到 COPY_SOURCE / COPY_DEST，录 CopyTextureRegion
   → pendingCallbacks.add(callback)
   → 帧 encoder：返回，不 submit
   → 下一次 submit()：transientMemory.rotate() 之后依次 run()
```

**一次性 encoder 必须在这里立刻 `submit()` 并同步跑回调。**
`Screenshot.takeScreenshot` 用的那种 encoder `device == null`、从不 submit；
不立刻提交的话，每秒一次的自动截图都会留下一个 1.6 MB 的 READBACK 缓冲
（vanilla 回调里的 `buffer.close()` 永不执行）。

回调抛出的异常必须打出来（`"post-submit callback failed: <t>"`），
否则外层的截图缓冲不会 close。

### 7.4 为什么必须设备级全局队列

整个设备只有一条 `ID3D12CommandQueue` 和一个 `queueFence`，这不是省事，是四条约束共同要求的：

1. **回读的可见性**：`copyTextureToBuffer` 的读回要在"这条 copy 完成"之后才有意义。
   如果每个 encoder 各有一条队列，JVM 侧的 `submit()` 顺序就不等于 GPU 执行顺序，
   回调里的 `memcpy` 会读到旧内容。
2. **帧槽重用的判据只能有一个**：`drawHeap` 的 4 个帧段、`resetFrame(frameSlot)`、
   缓冲池的回收，全部以"全局 fence 已过某个值"为前提。多队列会让"某帧段是否安全"变成
   与队列数量相关的组合问题。
3. **Present 与提交的先后**：渲染线程在 `ExecuteCommandLists` 之后立刻 `presentSurface()`。
   多队列时 Present 的完成无法用一条 fence 表达（`deviceWaitIdle` 也只等命令队列，
   **不等** Display Controller 完成 flip），`surfacePresentFences` 的语义会失效。
4. **`flushPendingDeletes()` 的判据**：它只读一个 `queueFence->GetCompletedValue()`。
   多队列时"最慢的那条队列"才是安全线，代码里没有这个结构。

per-ctx fence 只用于 **allocator / list 槽位复用**（三路 ring），
它的值域与全局序列无关 —— 这也是 JVM 侧 `createFence()` 必须用 per-ctx token 的原因（§5.2）。

---

## 8 resize 与 back buffer 的换出流程

交换链本体在 `src\core\d3d12_surface.cpp`，完整流程见 [`surface/README.md`](../surface/README.md)。
设备层只提供三个挂点，**改动时必须三个一起改**：

| 挂点 | 位置 | 设备层的责任 |
| --- | --- | --- |
| `allocRtvHandle()` | `:2843` | 从**持久** `rtvHeap` 单调分配 RTV 句柄。`gNextRtv` **不回收**，`kRtvHeapSize = 2048` 是硬上限 —— resize 次数多了会用完 |
| `acquireTextureRtv(ctx, tex)` | `:4550` | 给 `d3d12_surface.cpp` 的纹理→纹理 blit 用；等价于 `frameRtvFor(ctx, tex, 0)`，走**帧级**堆 |
| `getBackBufferHandle()` / `surfaceFences[]` | `d3d12_surface.cpp` | 每帧把 `queueFenceValue` 记到当前 back buffer 上，`ResizeBuffers` 之前要按它等 GPU 释放 |

resize 的设备层不变量：

- **`ResizeBuffers` 只在"没有任何 list 打开、`rtvHeap` 上的旧句柄已无人引用"时调用。**
  旧 back buffer 的 `ComPtr` 必须先全部释放，否则 `ResizeBuffers` 返回 `DXGI_ERROR_INVALID_CALL`。
- **`swapChainFormat` 必须与 `ResizeBuffers` 传入的格式同值**（重建交换链时也一样），
  否则同样 `DXGI_ERROR_INVALID_CALL`。
- **`drawHeap` / `frameRtvHeap` / `dsvHeap` 与 back buffer 无关**，resize 时不需要动它们；
  只有 `rtvHeap` 上那 N 个持久 RTV 需要跟着新 back buffer 重新 `CreateRenderTargetView`。
- **`kSurfaceBufferCount = 3`**：back buffer 数量在创建时定死，resize 不改变它。
- resize 之后**必须重算**所有依赖尺寸的东西：SMAA 的 `smaaWidth/Height`、TAA 的
  `taaWidth/Height` 与两张 history、`computeUavHeap` 里的环、`drawHeap` 的帧段划分。
  设备层不替你重算 —— 这些在各子系统的 `ensure*Targets()` / `taaEnsurePipelines()` 里。

帧生成开启时交换链可能被 provider 包了一层（`fgWrapPresentChain` / `fgUnwrapPresentChain`），
此时 resize 必须先 `fgUnwrapPresentChain()` 再重建。
细节在 [`advanced/framegen.md`](../advanced/framegen.md)。

---

## 9 PIX 抓帧在设备层的挂点

两条互相独立的路径，共同的约束是**都必须在第一个 D3D12 设备存在之前**把 PIX 的 DLL 装进进程。

| 路径 | 入口 | 设备层何时调用 | 特点 |
| --- | --- | --- | --- |
| GPU capture layer | `mcdrSetPixCapture(enabled)` / `mcdrSetPixDirectory(path)` | `ensureDevice()` 第 3 步 `ensurePixCapturerLoaded()`；`enumerateAdaptersJson()` 里再兜底一次 | 载入 `WinPixGpuCapturer.dll`，之后由 PIX 的 UI 抓帧。**不可撤销**（PIX 从进程内部 hook D3D12） |
| Timing capture | `mcdrStartPixTimingCapture(path)` / `mcdrStopPixTimingCapture()` | 首次调用时懒加载（`ensurePixTimingRuntime()`），不需要设备已创建 | 由进程自己驱动 `PIXBeginCapture(PIX_CAPTURE_TIMING, {file_name})` … `PIXEndCapture(0)`，直接写 `.wpix` 到指定路径 |

`mcdrSetPixDirectory()` 必须先于 `mcdrSetPixCapture()`。它接受两种值：
一个 DLL 文件（取它的兄弟文件）或一个目录（拼文件名）。搜不到时顺序退到
`MCDR.dll` 同目录、再退到 `%ProgramFiles%\Microsoft PIX\<版本最大>\`。

Timing capture 的关键数值（`d3d12_device.cpp` 起）：

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `kPixCaptureTiming` | `1u` | `PIX_CAPTURE_TIMING` |
| `kPixCaptureStorageMemory` | `0u` | 存储到内存而非磁盘 |
| `kPixCpuSamplesPerSecond` | `4000u` | CPU 采样率 |
| `kPixMaximumToolingMemoryMb` | `4096u` | tooling 内存上限 |
| `kPixBeginWaitMs` | `10000` | `PIXBeginCapture` 的等待上限 |

`startPixTimingCapture()` 在自己的线程里调 `PIXBeginCapture`（PIX 的 runtime 会在 load 时装 COM，
不能在渲染线程上做），超时返回 `HRESULT_FROM_WIN32(ERROR_TIMEOUT)`；
`stopPixTimingCapture()` **不等**写盘就返回（`PIXEndCapture` 要写几百 MB，等它等于卡死这一帧）。

> 抓帧的操作流程、`.wpix` 的落点、ETW 会话需要提升权限这些事，见
> [`misc/debug-and-logging.md`](../misc/debug-and-logging.md)。

---

## 10 已知限制与缺陷

下面每一条都是**当下仍然成立**的行为，不是待办清单。

> **缺陷**：`drawHeap` 的同步 ring 耗尽时**静默复用上一张 SRV 表**。
> `pushDescriptors()` 在 `nextDrawSlot + srvCount > syncRingCapacity()` 时，会打最多 8 次
> `"descriptor ring exhausted (next=… need=… cap=… fence=…) - reusing previous SRV table"`
> 然后复用上一张同样大的表；复用不了才返回
> `"pushDescriptors: draw descriptor heap exhausted for this frame"`。
> 前一种情况意味着**这一 draw 的 SRV 绑定是错的**（用的是上一批纹理），画面上表现为贴图错乱，
> 而调用方拿到的仍是成功。

> **缺陷**：GPU 忙时 `flushPendingDeletes()` 会**并发跳过**。
> 它用 `InterlockedExchange(&gPdInProgress, 1)` 防重入，重入/并发调用直接 return，
> 于是同一次 `destroyObject()` 的槽位要等下一次 flush 才回收。
> 在"每帧销毁大量 view"的路径上，`srvHeap` 会比实际用量看起来更快耗尽。

> **缺陷**：`srvHeap` / `samplerHeap` 的耗尽**不可恢复**，只要还有 list 打开。
> `allocSrvSlot()` 只在 `gOpenListCount == 0 && pendingDeletesCount() > 0` 时才 flush 重试；
> 一条超长列表（图集上传）里堆满就是硬报错
> `"srv heap exhausted (next=… free=… pending=… openLists=…)"`。

> **缺陷**：`gNextRtv` 只增不减，`kRtvHeapSize = 2048` 是终生配额。
> `allocRtvHandle()` 没有回收路径，反复 `ResizeBuffers` / 反复重建交换链会把它耗到
> `"RTV descriptor heap exhausted"`。back buffer 数量固定为 3 时正常游戏内不会触发。

> **缺陷**：一条列表内最多 256 个不同的 `(资源, mip)` 帧级 RTV（`kFrameRtvHeapSize = 256`），
> DSV 同样 256 个（`kDsvHeapSize = 256`）。超了就是 `"frameRtvFor: frame rtv heap exhausted"` /
> `"frameDsvFor: dsv heap exhausted"`，不降级、不重试。

> **缺陷**：**附件同时被当 SRV 采样（自采样）只记账、不阻止**。
> `pushDescriptors()` 命中 `activeColorTargets` / `activeDepthTarget` 时不改行为，
> 只在调试层报一条 #538。零售驱动下读到的是未定义内容。

> **缺陷**：VRS 能力不足时 `setShadingRate()` 返回 0 但**不改变任何状态**，
> 而 `setShadingRateNow()` 在不支持的设备上仍然会把档位下发给 list。
> 下发设备不支持的速率会让整条 list 被 runtime 丢弃 —— 屏幕直接黑。
> 兜底逻辑（`shadingRateUnsupportedReason()`）只在 `issueShadingRate()` 里，且
> `additionalShadingRates` 为假时**仅拒绝** `2X4` / `4X2` / `4X4`。

> **缺陷**：`DX12_DEBUG_LAYER` 与 Agility 互斥。
> `gAgility.applied` 为真时调试层被忽略，只有一条 WARN。要调试就得把 `D3D12Core.dll`
> 从 `MCDR.dll` 同目录移走（同时也失去 SM 6.9，图形管线全部建不出来）。**实际上不可兼得。**

> **缺陷**：`createTexture()` 的 `usage` 位缺 `TEXTURE_BINDING`（`0x4`）时，
> `createTextureView()` 只打一行 WARN 就**照建 SRV**：
> `"createTextureView: texture usage=0x%x lacks TEXTURE_BINDING (fmt=%d) — creating SRV anyway"`。
> 之后绑到管线里属非法用法，只有调试层才会报。

> **缺陷**：设备移除（device removed）后没有 CPU 侧调用就不会崩，也**不会自己恢复**。
> `deviceRemoved()` / `logDeviceStatus()` 只读 `GetDeviceRemovedReason()` 并 dump InfoQueue 最近
> 24 条消息；要等下一次 `ExecuteCommandLists` 才把失败暴露出来。
> 恢复路径只有"重启进程"—— 没有 `destroyDevice()` 之后重建设备的流程。

> **限制**：只有一个 `ID3D12CommandQueue`（`DIRECT`），没有 copy / compute 队列，
> 也没有多队列并行。理由见 §7.4。
> **限制**：不使用 enhanced barrier（`OPTIONS12` 只上报）。
> **限制**：`shaderModel` 上限 6.10 是用数值 `0x6a` 补齐的；`shaderModelName()` 只认
> `6.8` / `6.10` 两档，其余档位走 `shaderModelProfileSuffix()`。
> **限制**：`runResourceSelfTest()` 是唯一的内建自检，只验证 texture / buffer / map 写读 /
> sampler / textureView 五步，不覆盖 PSO、barrier、交换链。

---

## 文件导航

| 路径（相对 `MCDR\`） | 内容 |
| --- | --- |
| `src\module\core\mcdr_device.cppm` | 导出接口：`DeviceContext` / `Dx12Object` / `Dx12Pipeline` / `CommandContext` / `Dx12Surface` / `Capabilities` / 全部导出函数声明 |
| `src\core\d3d12_device.cpp` | 实现：设备创建、堆、描述符、命令列表、fence、屏障、资源、管线、瞬态原语、PIX、异步渲染线程 |
| `src\core\d3d12_surface.cpp` | 交换链、HDR、Present、back buffer 句柄、像素回读（[`surface/README.md`](../surface/README.md)） |
| `src\module\core\mcdr_error.cppm` | `mcdr::Error` / `mcdr::Result<T> = std::expected` / `mcdr::err()` / `mcdr::describe()` |
| `src\module\core\d3d12_async_descriptor.cppm` | `kAsyncFrameSlots` / `kAsyncSyncRingReserve` / `kAsyncWorkerSlotsPerSection` / `PartitionedDescriptorAllocator` |
| `include\mcdr_abi.h` | C ABI（`MCDR_ABI_VERSION = 4u`）、`mcdr_object` 等不透明句柄的前向声明 |
| `src\c_abi\abi_device.cpp` | 设备与资源的 C 入口（`mcdr_dx12CreateDevice` / `Create*` / `MapBuffer` / `GetAdapterInfo` …） |
| `src\c_abi\abi_command.cpp` | 命令层 C 入口（encoder、拷贝、clear、render pass、query、异步渲染控制） |
| `src\c_abi\abi_draw.cpp` | 绘制录制 C 入口（`SetPipeline` / `PushDescriptors` / `Draw*`） |
| `src\c_abi\abi_meta.cpp` | PIX 与 ABI 元信息的 C 入口 |
| `docs\device\README.md` | 本文 |

JVM 侧对应位置（相对仓库根）：

| 路径 | 内容 |
| --- | --- |
| `Illusion\src\main\kotlin\dev\nts\expdxr\d3d12\extern\MCDR_ABI.kt` | FFM downcall 绑定与 `MCDR_Layout` |
| `Illusion\src\main\kotlin\dev\nts\expdxr\d3d12\render\adapter\D3D12Device.kt` | 设备封装、`pipelineCache`、`close()` 顺序 |
| `Illusion\src\main\kotlin\dev\nts\expdxr\d3d12\backend\D3D12CommandEncoderBackend.kt` | encoder 封装、`submit()` 的同步/异步分支、回读回调 |
| `Illusion\src\main\kotlin\dev\nts\expdxr\d3d12\render\core\D3D12TransientMemory.kt` | 上传 ring、staging 块池、帧队列 |

---

## 文档维护

1. 改了 §4 的任何一个容量常量（`kSrvHeapSize` / `kDrawHeapPerFrame` / `kFrameRtvHeapSize` …），
   同一次改动里更新本文的表格与 §4.2 的推导 —— 常量与表格对不上比没写更坏。
2. 改了 `ensureDevice()` 的步骤顺序、新增一个堆、或改动 `deviceReady()` 的判据，
   必须同步 §2.2 的表；新增堆还要同步 §4.1 与 §2.4 的 `destroyDevice()`。
3. 改了屏障的自动回切规则（`endCommandList` / `endRenderPass`），同步 §5.6 的两张表 ——
   这是整套代码里最容易"看起来对、跑起来 TDR"的地方。
4. 修掉了 §10 里的任何一条，就把它从这一节删掉；新增一条当下成立的限制，就按
   `> **缺陷**：…` 的格式补上，并写清触发条件与能观测到的日志串。
5. 本文与 [`overview.md`](../overview.md) 的「文档地图」互为入口；新增子章节时不要改变
   已有的 `§` 编号，其它文档按编号引用这里。
