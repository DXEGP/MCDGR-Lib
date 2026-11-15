# C ABI 契约层

`MCDR\include\mcdr_abi.h` 是 JVM 与 native 之间**唯一**的契约：JVM 侧用 `java.lang.foreign`（FFM）
直接 downcall 这份头文件里的导出函数，没有 JNI、没有 `JNI_OnLoad`、没有 `RegisterNatives`、没有
中间包装对象。头文件同时是接口定义、类型映射规则和布局自检的数据源。

| 事实 | 值 | 出处 |
| --- | --- | --- |
| 契约文件 | `include\mcdr_abi.h` | — |
| 导出函数 | 268 个 `MCDR_API ... MCDR_CALL` 声明 | 头文件全文 |
| 跨语言 aggregate | 43 个（另有 9 个句柄 typedef，共 52 个 `typedef struct mcdr_*`） | 头文件全文 |
| 调用约定 | `__cdecl` + C 链接（`MCDR_CALL` / `extern "C"`） | `include\mcdr_abi.h:23`、`:44-46` |
| ABI 版本 | `MCDR_ABI_VERSION 47u` | `include\mcdr_abi.h` |
| 布局自检记录 | 41 个结构体 / 459 条字段 | `src\c_abi\abi_meta.cpp` 的 `layoutTable()` |
| 错误通道 | `int32_t` 返回值 + `mcdrLastError()` | `src\c_abi\abi_common.cpp`（错误槽）+ `src\c_abi\abi_meta.cpp`（导出） |
| 唯一的 upcall 类型 | `mcdr_fence_callback_fn` | `include\mcdr_abi.h:1102` |
| JVM 对端 | `MCDR_ABI.kt`（绑定）、`MCDR_Layout.kt`（布局镜像） | `Illusion\src\main\kotlin\io\github\dxegp\d3d12\extern\` |

ABI 47 的 RT 生产入口为 `mcdr_dx12RtFrameTransactionLighting`，以 248 B 描述和独立的有界内容事件载荷
连续录制同帧空间、追踪、降噪/SR 与最终合光，成功返回 288 B 整组输出。
事件载荷为 64 B 头部加每来源 64 B，最多 256 来源；完整 schema、预检与取消/提交游标规则见
[内容事件](../advanced/rt-frame.md#24-内容事件与当前查询影响)。既有 `mcdr_dx12RtFrameTransaction` 保持无事件调用与原布局。
新生产调用必须配套含该导出的 DLL；缺失时明确拒绝，不静默丢弃事件。
`mcdr_dx12CreateRtWorkGraphFrame(const mcdr_rt_frame_program*)` 新增实验 WG 帧创建，
与 DXR 创建共用 144 B 程序描述和 `uint64_t` 上下文编号；`rayDxil` 槽改为 WG `lib_6_9` 节点库，末尾 environmentDxil/environmentBytes 为共享环境 compute。
八对 DXIL 指针/长度、已有布局表和全部帧调用布局不变；两种创建共享 `MCDR_Layout.RT_FRAME_CREATE` descriptor。
WG 无 SBT，backing、初始化、取消与提交寿命见 [RT 帧创建](../advanced/rt-frame.md#1-创建与释放)。
RT 消费方还必须支持程序化 AABB 失效体积、`RtInvalidIntersection` 导出与无界 unknown 标记。
空间 setter 接受 flags bit 1 表达调用方尚未闭合的来源义务，native 与 scene 的无界要求按位或；其它未知位拒绝。
scene 描述携带完整当前输出支持清单，允许待编译整段要求携带经过认证的有限来源支持界；
它不提供实际几何或空空间资格。JVM、DLL 与 shader 必须配套，禁止混用旧契约或跳过程序化候选。

AS 分配预算追加只读可选导出 `mcdr_dx12GetRtAccelerationBudget(uint64_t*, int32_t)`，
数量固定为 4，返回在用与预留字节、512 MiB 上限、拒绝累计次数和含预留的峰值。
它不改变既有布局；内存所有权与错误行为见 [RT 帧链路](../advanced/rt-frame.md#13-场景候选发布与局部失效)。

可选导出 `mcdr_dx12SetRtManagedMemoryBudget(uint64_t)` 设置 RT 受管分配的共享上限，
`mcdr_dx12GetRtManagedMemoryBudget(uint64_t*, int32_t)` 要求 24 个 uint64，
返回 total、geometry GPU、geometry UPLOAD、AS、frame GPU、frame host 六组当前/上限/拒绝/峰值。
默认总上限 2048 MiB；几何两域与 AS 各限 512 MiB，帧域的上限 0 表示只受共享总上限约束。
两个入口不改既有布局，查询不回收资源、不等待 GPU；覆盖范围见 [RT 受管分配总账](../advanced/rt-frame.md#17-native-rt-受管分配总账)。

场景工作预算由可选导出 `mcdr_dx12SetRtSceneWorkBudget` 设置，CPU/GPU 参数为微秒，三角形单独计数。
`mcdr_dx12GetRtSceneWorkBudget` 只读 24 个 uint64，不消费 GPU 反馈或等待完成。
JVM 生产调用要求设置成功后才准备场景；计费、取消与软预算例外见 [场景增量工作准入](../advanced/rt-frame.md#14-场景增量工作准入)。

可选导出 `mcdr_dx12GetRtSceneCaptureDemands` 非阻塞收集已完成的有限 unknown 来源需求，并消费有界提示队列。
输出为 `4+2×capacity` 个 uint64，capacity 为 1–64；包含 world/live/scene 帧、数量及 node/需求帧对。
JVM 只为当前缺失捕获提高优先级；该接口不提供几何或空间证明，详见 [有限缺口的捕获需求](../advanced/rt-frame.md#16-有限缺口的捕获需求)。

## 1. 这层是什么、谁在调用

DLL 导出一组扁平的 C 函数，每个函数只做三件事：把入参翻译成 D3D12 模块的调用、把结果写回调用方
给的内存、把失败原因写进本线程的错误槽。业务逻辑全在 C++23 模块里（`mcdr.d3d12.device` 等），
适配层（`src\c_abi\`）不含渲染决策。

ABI 4 之后追加了四个入口，只在需要时调用：`mcdr_dx12QueryRtCaps(mcdr_rt_caps*)` 填能力面、
`mcdr_dx12QueryRtTrackStats(mcdr_rt_track_stats*)` 取状态跟踪统计、
`mcdr_dx12Barrier(ctx, const mcdr_barrier_desc* descs, int32_t count)` 下发显式屏障、
`mcdr_dx12SetSwapchainOptions(int32_t allowTearing, int32_t forcePresentChild)` 定下次建链选项
（`allowTearing` 0 = 不建带 `ALLOW_TEARING` 的链、非 0 = 允许；`forcePresentChild` 非 0 = 建在自建
呈现子窗口上、0 = 顶层窗口）。结构与字段偏移见 §6。

调用方只有一处：Kotlin `object MCDR_ABI`。它在类初始化时做三件事——加载 DLL、按名字绑定全部导出、
自检 ABI 版本与布局；任一步失败就把异常存进 `LOAD_FAILURE`，之后每个 wrapper 直接抛
`IllegalStateException("MCDR native library is unavailable: ...")`，后端整体不可用。

- 加载：`SymbolLookup.libraryLookup(dllPath, ARENA)` 找符号，`Linker.nativeLinker().downcallHandle(addr, desc)` 造 `MethodHandle`。
- 硬导出用 `skip()`：符号缺失立刻抛 `MCDR export not found: <name>`，整个库算加载失败。
- 可选导出用 `skipOptional()`：缺失返回 `null`，只让单个特性降级（例如旧 DLL 没有某个新开关）。
- 调用一律 `invokeExact`：实参的**静态**类型必须与 `FunctionDescriptor` 逐位一致，否则抛
  `WrongMethodTypeException`，不会自动宽化。

**必须**新增导出时同时改四处：`include\mcdr_abi.h` 的声明、`src\c_abi\` 里对应的转发、`MCDR_ABI.kt`
的绑定、以及需要的布局镜像（见 §4）。**不要**为任何功能新增 JNI 入口（`System.loadLibrary`、
`external fun`、`RegisterNatives` 都禁止）。

### 1.1 一次调用长什么样

```
JVM 线程（Kotlin）                                     MCDR.dll（C ABI 适配层）
────────────────────────────────────────────────────────────────────────────────
MCDR_ABI.D3D12CreateTexture(desc)
 ├ 取线程私有 Scratch 槽（4 个 Arena.ofConfined 轮换）
 ├ 把参数写进 MemorySegment（偏移来自 MCDRLayout 的 @JvmField 常量）
 ├ MH_createtexture.invokeExact(descSeg, outSeg)
 │                                     ┌──────────────────────────────────────┐
 ├────────────────────────────────────►│ mcdr_dx12CreateTexture(desc, out)    │
 │                                     │  ├ 校验入参                          │
 │                                     │  │   失败 → failWith("mcdr_...")     │
 │                                     │  │   写 TLS 错误串 + stderr，return 0│
 │                                     │  ├ 调 import 的 mcdr.d3d12.device 实现│
 │                                     │  └ 成功 → *out = 对象地址，return 1  │
 │                                     └──────────────────────────────────────┘
 ├ ret == 1 → MemorySegment.ofAddress(读出的地址) 作为句柄保存
 └ ret != 1 → D3D12LastError()
              ├ MH_lasterror(MemorySegment.NULL, 0) → need（含 NUL 的字节数）
              ├ arena.allocate(need)
              └ MH_lasterror(buf, need) → buf.getString(0)
```

`MCDR_ABI.kt` 的 `SCRATCH` 是每线程 4 个 `Arena.ofConfined`（`SLOT_COUNT = 4`），出参结构体与
字符串缓冲都从这里取，避免每次调用都新建 arena；要求更大时才重建该槽位的段。

### 1.2 native → JVM 只有一个方向

C++ 侧**不持有**任何 JVM 对象引用。唯一的回调用整数句柄定位目标：

```
native 渲染线程                                    JVM
──────────────────────────────────────────────────────────────
fence 到达 → 遍历注册表（锁外）
  cb(userHandle: int64_t, value: uint64_t, completed: int32_t)
      ──── mcdr_fence_callback_fn（唯一 upcall stub）────►
                                    按 userHandle 找回自己的对象
```

回调在注册表的锁**外**执行，所以回调里可以重新注册或注销（`include\mcdr_abi.h`）。
新增回调时**必须**沿用这个形状：只传整数句柄与标量，不要试图把指针转成 JVM 对象。

## 2. 类型映射

Java 侧不用 `Layout` 猜大小，镜像里每个字段都是显式类型。映射规则只有一张表：

| C 类型（头文件） | FFM `ValueLayout` | Kotlin 常量 | 宽度 | 说明 |
| --- | --- | --- | --- | --- |
| `int32_t` | `JAVA_INT` | `MCDRLayout.I32` | 4 | 枚举、计数、布尔、错误码全走这个 |
| `uint32_t` | `JAVA_INT` | `MCDRLayout.I32` | 4 | 无符号语义由调用方解释（如 `mcdrABIVersion`） |
| `int64_t` | `JAVA_LONG` | `MCDRLayout.I64` | 8 | 偏移、长度、fence 值 |
| `uint64_t` | `JAVA_LONG` | `MCDRLayout.I64` | 8 | 布局自检的 `offset` / `size` |
| `size_t` | `JAVA_LONG` | `MCDRLayout.I64` | 8 | x64 上 8 字节 |
| `float` | `JAVA_FLOAT` | `MCDRLayout.F32` | 4 | 矩阵、相机、锐度 |
| `double` | `JAVA_DOUBLE` | `MCDRLayout.F64` | 8 | 少量标量（清屏值一类） |
| `void*` / `T*` / `const T*` | `JAVA_ADDRESS` | `MCDRLayout.PTR` | 8 | 句柄与结构体指针 |
| `const char*` | `JAVA_ADDRESS` | `MCDRLayout.PTR` | 8 | UTF-8，以 NUL 结尾 |
| `void`（返回） | `JAVA_VOID` | —（`fdv()`） | — | 开关类入口 |

**MSVC 的 `long` 是 32 位**，不是 LP64 的 8 字节。`MCDR_ABI.kt:414` 把这条写成注释：C 的 `long`
必须映射成 `JAVA_INT` / `I32`，**绝不**映射成 `I64`。当前头文件里没有裸 `long`，只用
`int32_t` / `int64_t`；加字段时也**不要**引入 `long`。

### 2.1 返回值约定：三态 + 例外

绝大多数开关类入口返回三态，含义固定：

| 返回 | 含义 | 例子 |
| --- | --- | --- |
| `1` | 成功 / 已生效 / 已记录 | `mcdr_dx12SetDlssConstants`（已缓存）、`mcdr_dx12SetDebugLayer`（已记录，设备创建时才生效） |
| `0` | 失败，或被跳过 / 未启用 / 不可用 | `mcdr_dx12CreateDlssFeature`（NGX 不可用） |
| `-1` | 参数非法，或"请求了但加载失败" | `mcdr_dx12SetDlssQuality`（档位越界）、`mcdrSetAntiAliasing`（未知 mode）、`mcdrSetPixCapture` |

失败时错误串由 `mcdrLastError()` 给出（§7、§8）。**不要**用 `!= 0` 一刀切判断成功，下面这些
入口的 `0` 是**正常**结果：

| 入口 | `0` 的含义 |
| --- | --- |
| `mcdrSetLogFile` | 镜像已关闭（传入 `NULL` / 空串） |
| `mcdr_dx12SetDlssOutput` | 输出纹理已撤销（传 `NULL`） |
| `mcdr_dx12SetDlssEnabled` | 调用后处于禁用状态（返回的是归一化状态，不是成败） |
| `mcdrSetAntiAliasing` | 接受了但未实现（`DLAA` 降级为 OFF） |
| `mcdrSetShadingRate` | 设备无法兑现该 rate（tier 0，或缺 additional rates） |
| `mcdr_dx12NrdDenoiseTemporal` | 纹理/矩阵/偏移参数无效；非法偏移在接管前拒绝 |
| `mcdrSetPixCapture` | 没有请求捕获（请求了才会返回 `1` / `-1`） |

另外四种返回值形态同样属于契约：

| 形态 | 含义 | 例子 |
| --- | --- | --- |
| `-2` | 没有打开的命令列表 | `mcdr_dx12DispatchCompute`、`mcdr_dx12SetShadingRateNow` |
| 负 HRESULT | PIX 捕获的具体失败码 | `mcdrStartPixTimingCapture`（`E_FAIL` / `E_INVALIDARG` / `HRESULT_FROM_WIN32(ERROR_TIMEOUT)` / `E_ACCESSDENIED`） |
| `NULL` 指针 | 创建失败，句柄不可用 | `mcdr_dx12CreateGraphicsPipeline`、`mcdr_dx12CreateSurface` |
| 正整数 | 所需字节数（含 NUL） | `mcdrLastError`、各类字符串出口（§3.3） |

## 3. 句柄、字符串、数组

### 3.1 不透明句柄

句柄是 native 对象的地址，Java 侧只当 `MemorySegment` 存，**绝不**自己造一个。C++ 分支里
typedef 直接指真实类（`d3d12_mc::Dx12Object` 等），所以 native 侧 `reinterpret_cast` 零成本。

| typedef | 真实类 | 产生 | 释放 |
| --- | --- | --- | --- |
| `mcdr_object*` | `d3d12_mc::Dx12Object` | `mcdr_dx12CreateTexture` / `CreateBuffer` / `CreateSampler` / `CreateTextureView` / `CreateComputePipeline` / `CreateComputePipelineEx` | `mcdr_dx12DestroyResource` |
| `mcdr_command_ctx*` | `d3d12_mc::CommandContext` | `MCDR_D3D12_create_command_encoder` | `mcdr_dx12DestroyCommandEncoder` |
| `mcdr_pipeline*` | `d3d12_mc::Dx12Pipeline` | `mcdr_dx12CreateGraphicsPipeline` / `CreateMeshPipeline` | `mcdr_dx12DestroyPipeline` |
| `mcdr_surface*` | `d3d12_mc::Dx12Surface` | `mcdr_dx12CreateSurface` | `mcdr_dx12DestroySurface` |
| `mcdr_query_pool*` | `d3d12_mc::QueryPool` | `mcdr_dx12CreateQueryPool` | `mcdr_dx12DestroyQueryPool` |
| `mcdr_desc_alloc*` | `d3d12_mc::PartitionedDescriptorAllocator` | `mcdr_dx12AsyncDescriptorCreate` | `mcdr_dx12AsyncDescriptorDestroy` |
| `mcdr_bundle_pool*` | `d3d12_mc::BundleRecorderPool` | `mcdr_dx12AsyncBundlePoolCreate` | `mcdr_dx12AsyncBundlePoolDestroy` |
| `mcdr_executor*` | `d3d12_mc::MainCommandExecutor` | `mcdr_dx12AsyncExecutorCreate` | `mcdr_dx12AsyncExecutorDestroy` |
| `mcdr_fence_mgr*` | `d3d12_mc::AsyncFenceManager` | `mcdr_dx12AsyncFenceCreate` | `mcdr_dx12AsyncFenceDestroy` |

`0` / `NULL` 表示"缺失"：出参里为 0 就是没创建出来；入参里传 0 的语义按各函数头注释（例如
`mcdr_dx12SetDlssOutput(NULL)` 是撤销输出）。**不要**用 0 表示"成功"，也不要缓存已释放的句柄。

#### 时间戳快照

ABI 38 兼容追加两个 query 入口，结构布局不变。实时统计使用以下非阻塞协议；
`mcdr_dx12ReadQueryValues` 保留阻塞语义，不作为实时统计的缺功能回退。

| 入口 | 返回与约束 |
| --- | --- |
| `mcdr_dx12QueueQueryValues(ctx, pool, start, count)` | `1` 已录制、`0` 拒绝；要求打开的列表且位于 render pass 外，只录制 `ResolveQueryData`，不提交、不等待 |
| `mcdr_dx12TryReadQueryValues(pool, start, count, out)` | `1` 已复制并消费、`0` GPU 未完成、`-1` 参数/状态/设备错误；只有 `1` 改写输出 |

每池最多一份未消费快照，重复入队即使 GPU 已完成也拒绝。轮询范围必须与入队范围完全相同；
消费后再次读取报错，`0` 不消费；请求不匹配或设备错误保留 pending，录制已取消则返回 `-1` 并释放 pending。
JVM 包装同样仅在 `1` 时复制输出数组。
每个被 resolve 的槽必须写过；同 encoder 的写入与 resolve 由列表顺序保证，
其它 encoder 的生产者 fence 必须已经完成，否则拒绝入队，由调用方稍后重试。
其它 encoder 的未完成 resolve 覆盖某槽时，向该槽写 timestamp 也拒绝，
通过 `mcdr_dx12WriteTimestamp` 的现有 `mcdrLastError` 通道报告；不能以逆提交顺序覆盖待读数据。
同 encoder 顺序写入及 resolve 已完成后的覆写允许，消费时仍读取独立快照。

快照持有独立 READBACK 资源和 resolve 所属提交的 fence/value。后续同列表覆盖 query 槽不能改变已录制快照；
未提交且未取消的列表保持待定。producer 与快照同时绑定录制世代；未提交列表被重新 Begin 或销毁时取消该世代，
后续无关提交达到相同 fence 数值也不能使旧数据变成 ready；已取消 producer 必须重新写入。
命令列表槽强持 query heap 和快照到退休，
因此提前销毁池只取消调用方的查询资格，不释放 GPU 仍会访问的资源；销毁后不得再传旧池句柄。
这些接口与池的写入、释放由调用方串行使用。

### 3.2 用整数传的原生对象

有些对象不走 `mcdr_*` 句柄，直接用 `uint64_t` 传地址或计数，用途写在函数头注释里：

| 入口 | 返回值含义 |
| --- | --- |
| `mcdr_dx12CreateHiddenWindow` | `HWND` |
| `mcdr_dx12GetDeviceHandle` / `mcdr_dx12GetQueueHandle` | `ID3D12Device*` / `ID3D12CommandQueue*` 地址 |
| `mcdr_dx12GetBackBufferHandle` | 当前 back buffer 的 `ID3D12Resource*`（不是 `mcdr_object*`） |
| `mcdr_dx12AsyncBundleEnd` | 录制完成的 `ID3D12GraphicsCommandList*` |
| `mcdr_dx12AsyncDescriptorGpuHandle` | GPU 描述符句柄 |
| `mcdr_dx12AsyncFenceSignal` / `mcdr_dx12AsyncExecutorEndFrame` | fence 值 |
| `mcdr_dx12CreateMeshRootSignature` | `ID3D12RootSignature*`，须用 `mcdr_dx12ReleaseRootSignature` 释放 |

### 3.3 字符串：两阶段、caller 分配

约定固定：缓冲区由调用方分配，`(char* buf, int32_t cap)` 两个参数，返回**所需字节数（含 NUL）**。
`buf == NULL` 或 `cap <= 0` 只做容量查询。

```
第 1 次：mcdrLastError(NULL, 0)      → need = 12        （"bad fmt: x" 加 NUL）
          Java: arena.allocate(need)
第 2 次：mcdrLastError(buf, 12)      → 写 11 字节 + NUL，返回 12
        容量不足时（cap < need）：最多写 cap-1 字节并补 NUL，返回值仍是完整 need
```

写出函数 `writeString()`（`src\c_abi\abi_common.cpp:68`）的三条硬规则：

- 返回值恒为 `s.size() + 1`，与 `cap` 无关；
- `cap > 0` 时一定写 NUL，因此最多写 `cap - 1` 个字节；
- 编码是 UTF-8，解码用 `getString(0)`（读到 NUL 为止）。

路径类参数（`mcdrSetLogFile`、`mcdrSetPixDirectory`、`mcdr_dx12SetDlssVendorDir`）都是 UTF-8，
相对路径按**进程工作目录**（游戏目录）解析。

枚举类出口（适配器列表、状态行）也是同一协议，但**返回口径有两种**：

| 入口 | 查询时返回 | 写入后返回 |
| --- | --- | --- |
| `mcdrLastError` 及多数状态出口 | 需要字节数（含 NUL） | 需要字节数（含 NUL） |
| `mcdr_dx12EnumerateAdapters` / `mcdr_dx12GetAdapterInfo` | 需要字节数（含 NUL） | 已写字节数，**不含** NUL |
| `mcdr_dx12CompileShaderDxc` | 所需字节码字节数 | 字节码字节数（失败为 `0`） |

> **缺陷**：上面两种口径并存，且第二种只在"真的写入了"时才切换语义。按 `writeString` 的口径去
> 处理枚举出口，会把已写长度当成 `cap` 再算一次，边界上差 1 字节。处理这些出口时**逐函数**看头
> 注释，不要套统一规则。

### 3.4 数组参数

数组一律 `(指针, 数量)` 成对出现，长度用 `int32_t`：`mcdr_pipeline_desc::colorTargets` +
`colorCount`、`inputElements` + `inputElementCount`、`bindings` + `bindingCount`；
`mcdr_dx12SetDlssConstants(const mcdr_dlss_constants*)` 则是整块结构体一次传入。
指针只在调用期间有效：native 侧**不**保留指向 JVM 内存的指针，需要留存的数据必须自己拷贝。

## 4. 版本握手

两个出口，同一个值：

- 编译期宏 `MCDR_ABI_VERSION`（`include\mcdr_abi.h`，当前 `45u`）；
- 运行期 `uint32_t mcdrABIVersion(void)`，返回同一常量。

JVM 侧常量 `MCDR_ABI_VERSION = 45`（`MCDR_ABI.kt`）。加载期比对，不等就抛
`IllegalStateException("MCDR ABI version mismatch: java=45, native=X")`，装进 `LOAD_FAILURE`，
后端彻底不可用——在首次绘制之前失败，而不是画到一半才发现结构体错位。

`mcdr_dx12DlssdEval` 固定八张纹理：带噪线性颜色、输出、正 viewZ、像素 MV、原始 normal/roughness、
diffuse albedo、specular albedo、世界单位 specular hit distance。第八张为 `R32_FLOAT`，
反射打空写 `1e9`，无效距离写 0，标签为 `kBufferTypeSpecularHitDistance`。
禁止传入 NRD 的归一化距离或八面体编码法线。相机载荷为 104 floats：
四个行主序重投影矩阵、near/far/FOV/aspect、当前/上一帧屏幕内容偏移 xy、worldToView 与 viewToWorld。
偏移位于 68–71，单位为 render 像素；当前 68/69 写入 `sl::Constants.jitterOffset`。
四分量须有限且位于 `[-0.5,0.5]`，RR/RT SR 均在接管资源前拒绝非法值。所有 SDK 矩阵不含 jitter。
调用方必须提供完整长度；此入口没有独立的相机数组长度参数，旧载荷与新 DLL 不兼容。

`mcdr_dx12RtSrEval` 固定四张纹理：render 尺寸线性 HDR 颜色、display 尺寸 RGBA16F 输出、
render 尺寸 R32_FLOAT 设备深度、render 尺寸像素 MV。深度使用 `kBufferTypeDepth`，
禁止直接传入 RR 的正 viewZ。相机复用上述 104-float 载荷，矩阵与帧号的语义相同。
返回值大于 0 表示已接受求值，0 表示不可用或输入不完整，负数表示参数/命令列表/SDK 失败；
`outText` 接收容量受限的 NUL 结尾诊断。指针仅在调用期间有效。

`mcdr_dx12DlssdEvalExposure` / `mcdr_dx12RtSrEvalExposure` 在 `constants` 与 `outText` 之间
增加独立非空 `const float* exposure`，依次为 `preExposure / exposureScale` 两个 float。
相机载荷仍为 104 floats，不得在旧数组尾部读取曝光参数。两个分量须有限且在闭区间
`[2^-16,2^16]`。JVM 使用 `MCDRLayout.RT_EXPOSURE_FLOAT_COUNT=2`，检查相机段至少 416 B。
RR 接收八或九张纹理，RT SR 接收四或五张；额外最后一张必须是 1×1 `R32_FLOAT` 曝光倍率。
所有纹理须为单 mip、单层、单采样 2D 资源，尺寸符合各自标签；输出须可作 UAV，
输入须可作 SRV，禁止别名或已由 SDK 接管的资源。非法输入在资源接管前返回 0。
旧入口保持八/四纹理签名，曝光参数默认 1；两种入口共用同一 feature 生命周期。

曝光资源的有无改变时，先等待该 viewport 上次求值的 completion fence，再调用 `slFreeResources`
释放 feature 并失效选项缓存；下一次成功求值消耗 pending reset。同一未提交命令列表内切换模式
返回 0，诊断为 `exposure: submit before switching modes`，不接管纹理；其他未完成提交最多等待 5 秒。
跨 encoder 也必须等待旧 encoder 自己的 fence；另一 encoder 的完成值即使更大，也不能提前满足它。
旧列表尚未提交或 GPU queue 被阻塞时，超时返回 0，诊断 `exposure: previous frame still in flight`；
该拒绝保持资源状态/归属/计数不变。提交旧列表或解除 GPU 阻塞后，可以在原新列表上重试。
曝光纹理按 NPSR 接管，SDK 返回后与其余纹理一起归还 `COMMON`。
资源必须存活至提交 fence 完成；函数返回仅表示录制结束。native 不读取纹理内倍率，
调用方须保证有限正值。玩家 RR/RT DLSS-SR 调用显式入口，两个标量均为 1，
曝光纹理由引擎手动/自动测光生成，最终合光读取相同倍率。

`mcdr_dx12NrdDenoise` 接收七张必需纹理：像素 MV、packed normal/roughness、正 viewZ、
diffuse-in、diffuse-out、specular-in、specular-out。两路辐亮度均为 RGBA16F YCoCg，
alpha 为各自归一化命中距离；同一 SDK instance 注册 `REBLUR_DIFFUSE` / `REBLUR_SPECULAR`。
允许末尾增加第八张独立镜面 viewZ；七纹理调用仍共用索引 2 的深度。
十纹理调用在八纹理后增加 diffuse/specular history confidence，对应索引 8、9，必须成对提供。
confidence 为同当前深度尺寸的单层、单 mip、单采样 `R8_UNORM` / `R16_FLOAT` / `R32_FLOAT` SRV，
数值范围 `[0,1]`，按当前时刻对上一帧图像的可信度生成，不能当作当前缺样本掩码或严格局部 reset。
七/八/九纹理关闭该可选 confidence 输入。
九纹理在八纹理后追加整数历史撤销 mask；十一纹理在十纹理后追加该 mask。
mask 为同当前深度尺寸的单层、单 mip、单采样 `R8_UINT` / `R32_UINT` SRV，按上一帧历史图像坐标生成；
bit 0 撤销 diffuse，bit 1 撤销 specular，其它位忽略。它清除指定历史 texel 的旧辐射与资格，
不改变当前输入资格；调用者仍须覆盖真实累积依赖与空间传播，不能只传当前编辑矩形。
仅接受 7–11 张纹理；空指针、尺寸/格式不符和资源别名在接管前拒绝。
全部资源保持到实际录制完成，SDK 返回后归还 `COMMON`，详见 [历史 confidence](../advanced/nrd.md#历史-confidence)。
严格 texel 撤销与空间反馈边界见 [逐 texel 历史撤销](../advanced/nrd.md#逐-texel-历史撤销)。
生产 RT 帧使用八纹理，索引 2 为 diffuse 当前深度，第八张为 specular 当前深度；原始主表面深度另行保留。
携带内容事件与路径支持的统一事务经 native 内部参数生成累积 NRD 历史 mask，不增加公开纹理或 FFM 字段。
生产依赖传播与适用范围见 [NRD 累积内容依赖](../advanced/nrd.md#累积内容依赖)。
缺样本对应深度写范围外值，消费端拒读该区输出；RR/SR 的当前全色降级见 [RT 帧输入](../advanced/rt-frame.md#5-运动与降噪-guide)。
这些内部资源不增加公开输出字段，既有函数签名与 FFM descriptor 保持。
八纹理调用的两张深度必须是同尺寸的单层、单 mip、单采样 `R32_FLOAT` SRV，
格式/尺寸不合法时在接管前返回 0，全部资源仍禁止别名。
零基索引 4/6 转 UAV，其余转 NPSR，执行完成后回 COMMON。
四个列主序矩阵依次为当前/上一帧投影与当前/上一帧 worldToView，共 64 floats。
拒绝五纹理输入；此入口的函数形参类型与 FFM descriptor 固定，当前/上一帧 jitter 均为零。

`mcdr_dx12NrdDenoiseTemporal` 使用同样的 7–11 张纹理与 64-float 无 jitter 矩阵，
另以非空 `jitter` 指针传入四个 float，顺序为当前 x/y、上一帧 x/y。
数值直接写入 NRD `cameraJitter` / `cameraJitterPrev`，单位为渲染像素，
对应 SDK 的 `sampleUv = pixelUv + cameraJitter / rectSize`；不在 native 转符号或缩放。
每个分量必须有限且在闭区间 `[-0.5,0.5]`，空指针或非法值返回 0，拒绝发生在资源接管前。
原始 C 指针不携带长度，调用方须保证矩阵至少 64 floats、偏移至少 4 floats，生命周期覆盖整个调用。
JVM 的 `D3D12NrdDenoiseTemporal` 包装检查 256 B 矩阵与 16 B 偏移长度，
偏移个数登记为 `MCDRLayout.NRD_JITTER_FLOAT_COUNT=4`。实际 RT 帧使用显式入口，
JVM 将共享 Halton `(Jx,Jy)` 转为 NRD 采样偏移 `(-Jx,+Jy)`；RR/SR 使用 `(Jx,-Jy)`。
成功、SDK 失败与无打开命令列表的返回值沿用原入口，资源收尾见 [NRD 后端](../advanced/nrd.md)。

**什么情况下必须 +1**（满足任一条）：

| 变化 | 为什么 |
| --- | --- |
| aggregate 增删字段、改字段类型、重排字段 | Java 的 `MemoryLayout` 偏移全变 |
| 导出函数改签名（参数/返回类型/顺序） | `FunctionDescriptor` 不匹配，`invokeExact` 直接抛 |
| 删除导出 | 老绑定找不到符号，加载即失败 |
| 已有枚举编号的含义变化 | 跨语言只传整数，语义错位在边界上发现不了 |
| **纯追加**新结构体 / 新导出 | 也要 +1：Java 靠版本号判断 DLL 是否具备新出口 |

最后一条容易被跳过：`4` 之前的两次升级都是"只增不改"的追加，仍然逐次 +1。

### 4.1 同一次改动里必须改齐的五处

1. `include\mcdr_abi.h`：宏 +1、新增声明与结构体、更新注释里的 `sizeof`；
2. `src\c_abi\abi_meta.cpp` 的 `layoutTable()`：新结构体的每个命名字段一条 `MCDR_FIELD`；
3. `src\c_abi\`：对应的转发实现；
4. `MCDR_ABI.kt`：`MCDR_ABI_VERSION` 常量、`FunctionDescriptor`、wrapper；
5. `MCDR_Layout.kt`：`MemoryLayout.structLayout` 镜像 + `EXPECTED_C_SIZES` 一行。

漏第 2 或第 5 步不会编译失败，只会在设备创建时 abort；漏第 4 步是运行时 `WrongMethodTypeException`。

第 2 步和版本号可以离线自查，不必启动游戏：

```powershell
MSBuild MCDR.vcxproj /p:Configuration=Release /p:Platform=x64
python _port\abi_layout_check.py
```

脚本把 DLL 的 `layoutTable()` 与 `MCDR_Layout.kt` 的 `STRUCTS` 逐字段对名（偏移与尺寸由运行期
`MCDRLayout.verify()` 负责，因为那部分要靠 Java 的布局引擎算）。漏一条 `MCDR_FIELD` 时它直接列名。

## 5. 布局自检

### 5.1 `mcdrABILayout`

```c
void MCDR_CALL mcdrABILayout(const mcdr_abi_field** fields, int32_t* count);
```

- 出参：`*fields` 指向一张表，`*count` 是记录数；两个指针都可以为 `NULL`（只取其中一个）。
- 表是函数局部 `static const std::vector`，首次调用时构建，返回 `data()`，**进程生命期有效**；
  调用方不要释放、不要改。
- 记录类型 `mcdr_abi_field`：`{ const char* name; uint64_t offset; uint64_t size; }`，
  `sizeof = 24` / `align = 8`。
- `name` 形如 `mcdr_binding_slot.buffer`（`#T "." #F`），`offset` 来自 `offsetof`，
  `size` 来自 `sizeof(((T*)nullptr)->F)`；数组字段取整段字节数（`mcdr_fg_constants.view` 是 64）。
- **顺序不是契约**：Java 侧按名字建 map，不按位置比对。

### 5.2 发布范围

| 已发布（38 个结构体 / 425 条记录） | 记录数 |
| --- | --- |
| `mcdr_binding_slot` | 6 |
| `mcdr_partition_draw` | 10 |
| `mcdr_color_target_desc` | 9 |
| `mcdr_depth_state_desc` | 3 |
| `mcdr_input_element_desc` | 8 |
| `mcdr_pipeline_binding_desc` | 2 |
| `mcdr_pipeline_desc` | 18 |
| `mcdr_mesh_pipeline_desc` | 18 |
| `mcdr_dlss_input` | 24 |
| `mcdr_sr_caps` | 14 |
| `mcdr_sr_dispatch` | 28 |
| `mcdr_fg_caps` | 14 |
| `mcdr_fg_dispatch` | 15 |
| `mcdr_fg_constants` | 8 |
| `mcdr_rt_caps` | 15 |
| `mcdr_rt_track_stats` | 8 |
| `mcdr_rt_hit_group` | 4 |
| `mcdr_rt_frame_program` | 18 |
| `mcdr_rt_frame_desc` | 23 |
| `mcdr_rt_geometry_desc` | 15 |
| `mcdr_rt_geometry_output` | 7 |
| `mcdr_rt_geometry_content` | 9 |
| `mcdr_rt_geometry_delta_desc` | 15 |
| `mcdr_rt_geometry_delta_output` | 9 |
| `mcdr_rt_geometry_memory` | 2 |
| `mcdr_rt_acceleration_output` | 12 |
| `mcdr_rt_section_version` | 3 |
| `mcdr_rt_output_support` | 11 |
| `mcdr_rt_scene_desc` | 10 |
| `mcdr_rt_scene_output` | 11 |
| `mcdr_rt_frame_output` | 16 |
| `mcdr_rt_gbuffer_output` | 12 |
| `mcdr_rt_gbuffer_program` | 9 |
| `mcdr_rt_gbuffer_batch` | 14 |
| `mcdr_rt_gbuffer_replay` | 9 |
| `mcdr_rt_denoise_desc` | 10 |
| `mcdr_rt_denoise_output` | 7 |
| `mcdr_rt_transaction_desc` | 11 |
| `mcdr_rt_transaction_output` | 8 |
| `mcdr_rt_geometry` | 9 |
| `mcdr_barrier_desc` | 7 |

`mcdr_dlss_constants` 与记录类型 `mcdr_abi_field` 自身没有逐字段发布，JVM 的 `EXPECTED_C_SIZES`
只检查这两个镜像的大小。其余 41 个结构体逐字段校验偏移与大小；新增字段必须登记进 `layoutTable()`。

### 5.3 Java 侧的比对算法

```
MCDR_ABI.<init>
 ├ SymbolLookup.libraryLookup(dllPath, ARENA)
 ├ 绑定全部导出（skip / skipOptional）
 └ verifyABI()
    ├ v = mcdrABIVersion()                    v != 45 → 抛版本不匹配
    ├ mcdrABILayout(&fields, &count)          count <= 0 或 fields == NULL → 抛
    ├ fields.reinterpret(24 * count)
    ├ 逐条读 name / offset / size → nativeFields[name] = [offset, size]
    └ MCDRLayout.verify(nativeFields)
       ├ 对每个 Java 字段：native 没有 → "missing from the native layout report"
       │                    offset 不等 → "offset java=<a> native=<b>"
       │                    size 不等   → "size java=<a> native=<b>"
       ├ 对每个 native 名字：Java 没有 → "reported by native but absent from the Java mirror"
       └ 有任何问题 → 一次性列出全部（不是第一个）
          IllegalStateException("MCDR ABI layout mismatch (N field(s)); ...")
```

另有 `MCDRLayout` 的 `init` 块做**总大小**校验：`EXPECTED_C_SIZES` 里 32 行
（结构体名 / Java 布局 / C 侧 `sizeof`），`byteSize()` 不等就抛
`MCDR ABI struct size mismatch for <name>: java=<a> c=<b>`。头文件里**没有** `static_assert`，
C 侧大小只写在结构体注释里（例如 `include\mcdr_abi.h` 的 `sizeof = 112`），由这张表兜住。

> **缺陷**：`EXPECTED_C_SIZES` 里的 C 大小是 Kotlin 侧的**手写常量**，不是向 DLL 查询来的；
> `mcdrABILayout()` 只发布字段偏移，不发布结构体 `sizeof`。因此"镜像与手写常量一起写错"、
> "总大小对但内部字段位移了"这两类问题只能靠 §5.3 的逐字段比对发现——而 `mcdr_dlss_constants`
> 两边都没登记，它那 312 字节内部如何分布，两侧都无法校验。

> **已修（2026-10-29）**：`MCDR_Layout.kt` 的 `STRUCTS` 表曾把 `mcdr_fg_constants` 登记两次
> （同一布局重复 `put`），`describeSizes()` 会因此出现重复项；重复行已删除，现在每个结构体只有一条。

### 5.4 `_padN` 必须命名并登记

`MemoryLayout.structLayout` **不会**自动插入填充字节：C 侧编译器插的 4 字节空洞，Java 侧必须
显式写 `MemoryLayout.paddingLayout(4)`，而且要给它名字（`_pad0`…）。没名字的隐式尾部填充在
`describe()` 里被跳过，所以：

- 每个 `_padN` 都必须起名、必须进 `layoutTable()`；少一条就是 `missing from the native layout report`；
- 尾部补齐（`mcdr_partition_draw` 末尾 4 字节、`mcdr_depth_state_desc` 末尾 2 字节）在 Java 侧用
  `paddingLayout` 表达，它让 `byteSize()` 等于 C 的 `sizeof`；
- **不要**为了"省字段"删掉 `_padN`，也别指望重排字段后偏移还能对上。

## 6. 结构体镜像清单

下表列出 ABI aggregate。`sizeof` / `align` 与 `MCDR_Layout.kt` 的 `EXPECTED_C_SIZES` 以及头文件
注释一致（MSVC x64：`int32`/`float` = 4，`int64`/指针 = 8，`uint8` = 1）。

| 结构体 | sizeof | align | 用途 | 关键字段（字节偏移） |
| --- | --- | --- | --- | --- |
| `mcdr_binding_slot` | 40 | 8 | 一个管线绑定槽（CBV / SRV），push descriptors 与 bundle 录制共用 | `type`0 `texelFormat`4 `buffer`8 `view`16 `offset`24 `length`32 |
| `mcdr_partition_draw` | 56 | 8 | bundle 录制里一次绘制的索引/顶点几何 | `indexBuffer`0 `vertexBuffer`8 `vertexOffset`16 `indexType`24 `indexCount`28 `firstIndex`32 `baseVertex`36 `vertexSlot`40 `vertexStride`44 `_pad0`48 |
| `mcdr_color_target_desc` | 12 | 4 | 一个颜色目标的状态 | `format`0 `writeMask`4 `blendEnabled`5 `srcColor`6 `dstColor`7 `colorOp`8 `srcAlpha`9 `dstAlpha`10 `alphaOp`11 |
| `mcdr_depth_state_desc` | 8 | 4 | 深度目标状态 | `depthFormat`0 `depthWrite`4 `depthCompareOp`5（尾部 2 字节隐式填充） |
| `mcdr_input_element_desc` | 40 | 8 | HLSL 输入装配元素 | `location`0 `binding`4 `format`8 `offset`12 `stride`16 `stepRate`20 `_pad0`24 `semanticName`32 |
| `mcdr_pipeline_binding_desc` | 2 | 1 | 根签名绑定项 | `type`0 `reg`1 |
| `mcdr_pipeline_desc` | 96 | 8 | 图形管线（DXBC 字节码） | `vsData`0 `vsLen`8 `psData`16 `psLen`24 `colorCount`28 `colorTargets`32 `hasDepth`40 `depth`48 `topology`56 `inputElementCount`68 `inputElements`72 `bindingCount`80 `bindings`88 |
| `mcdr_mesh_pipeline_desc` | 96 | 8 | mesh 管线（DXIL 的 `as`/`ms`/`ps`） | `asData`0 `asLen`8 `msData`16 `msLen`24 `psData`32 `psLen`40 `colorCount`44 `colorTargets`48 `hasDepth`56 `depth`64 `bindingCount`80 `bindings`88 |
| `mcdr_dlss_input` | 112 | 8 | DLSS 每帧参数（含四张输入/输出纹理） | `renderWidth`0 `targetWidth`8 `jitterOffsetX`16 `preExposure`32 `nearZ`48 `depthInverted`56 `reset`60 `depthFormatOrdinal`64 `pInColor`72 `pInDepth`80 `pInMotionVectors`88 `pInOutput`96 `_pad1`104 `_pad2`108 |
| `mcdr_dlss_constants` | 312 | 4 | Streamline `sl::Constants` 的镜像（相机矩阵/向量/近远平面） | `viewProjection`0（`float[16]`）`prevViewProjection`64 `jitterOffsetX`128 `cameraPosX`144 `depthInverted`224 `projection`248（`float[16]`） |
| `mcdr_sr_caps` | 80 | 4 | 超分后端能力面 | `backend`0 `available`4 `coupledDenoise`8 `needsExposure`12 `jitterPhaseCount`24 `maxRenderWidth`36 `nativeAA`44 `reserved`48 `note`52（`char[28]`） |
| `mcdr_sr_dispatch` | 136 | 8 | 与厂商无关的超分每帧载荷 | `renderWidth`0 `displayWidth`8 `jitterOffsetX`16 `preExposure`32 `sharpness`52 `reset`56 `hdrInput`60 `backend`64 `flags`68 `pColorInput`72 `pColorOutput`80 `pDepth`88 `pMotionVectors`96 `pReactiveMask`104 `pExposure`112 `depthFormatOrdinal`120 `_pad2`132 |
| `mcdr_fg_caps` | 80 | 4 | 帧生成后端能力面 | `backend`0 `available`4 `runtimeLoaded`8 `deviceSupported`12 `chainCompatible`16 `needsReflex`20 `maxGeneratedFrames`36 `chainOwner`40 `reserved`44 `note`48（`char[28]`）`latencyProvider`76 |
| `mcdr_fg_dispatch` | 96 | 8 | 帧生成的"设期望值"载荷 | `displayWidth`0 `backend`8 `mode`12 `numFrames`16 `reset`20 `appFrameId`24 `backendFrameId`32 `presentId`40 `pBackbuffer`48 `pHudlessColor`56 `pDepth`64 `pMotionVectors`72 `pUiColor`80 `_pad2`88 |
| `mcdr_fg_constants` | 152 | 4 | 帧生成自己的帧常量 | `view`0（`float[16]`）`projection`64（`float[16]`）`jitterX`128 `mvScaleX`136 `resetHistory`144 `frameRenderTime`148 |
| `mcdr_abi_field` | 24 | 8 | 布局自检的一条记录（§5.1） | `name`0 `offset`8 `size`16 |
| `mcdr_rt_frame_program` | 144 | 8 | native RT 程序 | `rayDxil`0 `rayBytes`8 `guideDxil`16 `guideBytes`24 `exposureDxil`32 `exposureBytes`40 `motionDxil`48 `motionBytes`56 `packDxil`64 `packBytes`72 `signalDxil`80 `signalBytes`88 `remodulateDxil`96 `remodulateBytes`104 `compositeDxil`112 `compositeBytes`120 `environmentDxil`128 `environmentBytes`136 |
| `mcdr_rt_frame_desc` | 128 | 8 | native RT 帧输入 | `frameId`0 `worldEpoch`8 `sceneRevision`16 `materialRevision`24 `snapshotId`32 `width`40 `height`44 `accelerationId`48 `gbufferRevision`56 `constants`64 `constantBytes`72 `depthScale`76 `frameNanos`80 `exposureMode`88 `exposureEv`92 `exposureSpeed`96 `resetExposure`100 `signalEncoding`104 `jitterDeltaX`108 `jitterDeltaY`112 `denoisingRange`116 `contentJitterX`120 `contentJitterY`124 |
| `mcdr_rt_acceleration_output` | 80 | 8 | native 加速结构输出 | `snapshotId`0 `acceleration`8 `instances`16 `byteCount`24 `instanceCount`32 `blasCount`36 `buildCount`40 `ready`44 `blasMicros`48 `peakBuildMicros`56 `tlasMicros`64 `scratchBytes`72 |
| `mcdr_rt_section_version` | 16 | 8 | 当前 section 版本 | `node`0 `layer`8 `version`12 |
| `mcdr_rt_output_support` | 48 | 8 | 当前输出支持 | `node`0 `layer`8 `version`12 `flags`16 `reserved`20 `minimumX`24 `minimumY`28 `minimumZ`32 `maximumX`36 `maximumY`40 `maximumZ`44 |
| `mcdr_rt_scene_desc` | 64 | 8 | native 场景更新输入 | `worldEpoch`0 `snapshotId`8 `liveRevision`16 `sections`24 `sectionCount`32 `reserved`36 `frameId`40 `outputs`48 `outputCount`56 `outputMode`60 |
| `mcdr_rt_scene_output` | 232 | 8 | 场景发布与构建输出 | `snapshotId`0 `accelerationId`8 `pendingSnapshotId`16 `pendingAccelerationId`24 `sceneRevision`32 `geometryRevision`40 `materialRevision`48 `byteCount`56 `active`64 `candidate`144 `coverageFlags`224 |
| `mcdr_rt_geometry_desc` | 120 | 8 | native 几何注册输入 | `worldEpoch`0 `geometryRevision`8 `materialRevision`16 `reuseGeometry`24 `reuseMaterials`32 `records`40 `recordBytes`48 `vertices`56 `vertexBytes`64 `indices`72 `indexBytes`80 `materials`88 `materialBytes`96 `quads`104 `quadBytes`112 |
| `mcdr_rt_geometry_output` | 56 | 8 | native 几何快照与借用缓冲 | `snapshotId`0 `records`8 `vertices`16 `indices`24 `materials`32 `quads`40 `byteCount`48 |
| `mcdr_rt_gbuffer_output` | 88 | 8 | native G-Buffer 借用附件 | `revision`0 `resourceRevision`8 `frameNanos`16 `width`24 `height`28 `albedo`32 `specular`40 `normalRoughness`48 `emissive`56 `linearDepth`64 `depth`72 `byteCount`80 |
| `mcdr_rt_gbuffer_batch` | 96 | 8 | G-Buffer 批次 | `pipeline`0 `vertices`8 `vertexOffset`16 `sections`24 `sectionsOffset`32 `sectionsLength`40 `meshlets`48 `meshletsOffset`56 `meshletsLength`64 `meshletCount`72 `bindingCount`76 `bindings`80 `projectionBinding`88 `_pad0`92 |
| `mcdr_rt_gbuffer_replay` | 48 | 8 | G-Buffer 整组提交 | `revision`0 `batches`8 `batchCount`16 `projectionFloatCount`20 `projection`24 `cameraOffsetX`32 `cameraOffsetY`36 `cameraOffsetZ`40 `_pad0`44 |
| `mcdr_rt_frame_output` | 128 | 8 | native RT 借用输出 | `frameId`0 `snapshotId`8 `direct`16 `indirect`24 `aux`32 `specular`40 `specularGrazing`48 `constants`56 `byteCount`64 `exposure`72 `motion`80 `packedNormalRoughness`88 `viewZ`96 `diffuseSignal`104 `specularSignal`112 `specularHitDistance`120 |
| `mcdr_rt_denoise_desc` | 64 | 8 | native 降噪请求 | `frameId`0 `historyGeneration`8 `historyFrame`16 `planRevision`24 `outputWidth`32 `outputHeight`36 `nrdMatrices`40 `nrdFloatCount`48 `slFloatCount`52 `slConstants`56 |
| `mcdr_rt_denoise_output` | 48 | 8 | native 降噪借用输出 | `frameId`0 `snapshotId`8 `diffuse`16 `specular`24 `byteCount`32 `encoding`40 `dispatchCount`44 |
| `mcdr_rt_transaction_desc` | 248 | 8 | 同帧统一录制输入 | `trace`0 `denoise`128 `liveRevision`192 `spatial`200 `spatialBytes`208 `source`216 `upscaler`224 `debugMode`228 `timestamps`232 `firstTimestamp`240 `reserved`244 |
| `mcdr_rt_transaction_output` | 288 | 8 | 整组借用输出 | `trace`0 `denoise`128 `upscale`176 `composite`224 `fallbackFlags`272 `recorded`276 `timestampMask`280 `reserved`284 |
| `mcdr_rt_caps` | 80 | 4 | 光追 / mesh shader / work graphs / 着色率 / 采样器反馈的能力面（字段是否有效由 `options21Queried` / `options22Queried` 位决定） | `raytracingTier`0 `meshShaderTier`4 `workGraphsTier`8 `shadingRateTier`12 `shaderModel`16 `samplerFeedbackTier`20 `options21Queried`24 `options22Queried`28 `serActuallyReorders`32 `enhancedBarriers`36 `dxcAvailable`40 `max1DDispatchSize`44 `max1DDispatchMeshSize`48 `reserved`52 `note`56（`char[24]`） |
| `mcdr_rt_track_stats` | 92 | 4 | 光追状态跟踪的累计统计快照 | `transitions`0 `mismatches`4 `promotions`8 `widened`12 `firstUse`16 `uavBarriers`20 `tracked`24 `note`28（`char[64]`） |
| `mcdr_barrier_desc` | 40 | 8 | 一次显式屏障，`mcdr_dx12Barrier` 的数组元素 | `resource`0 `resourceAfter`8 `kind`16 `flags`20 `before`24 `after`28 `subresource`32 |

### 6.1 改结构体时必须守的规则

- **只能追加**：新字段加在末尾，或占用已保留的 `_padN`；重排/删除字段一律是破坏性变更（§4）。
- `_padN` 是**布局的一部分**，不是垃圾：它保证后续字段的偏移与 C 侧一致。
- 自己数对齐：`int32` 之后跟指针时必须留 4 字节填充（`mcdr_input_element_desc._pad0`、
  `mcdr_dlss_input._pad0` 都是这个用途）。
- 结构体指针参数一律 `const T*`（入参）或 `T*`（出参）；不要按值传大结构体。

### 6.2 字段取值速查

| 字段 / 入口 | 取值 |
| --- | --- |
| `mcdr_binding_slot.type` | `0` = CBV，`1` = SRV（texture view），`2` = SRV（texel buffer，类型化视图），`3` = SRV（RAW 缓冲视图 `R32_TYPELESS` + RAW 标志，供 `ByteAddressBuffer`） |
| `mcdr_pipeline_binding_desc.type` | `0` = CBV，`1` = SRV + static sampler，`2` = SRV |
| `mcdr_partition_draw.indexType` | `0` = `SHORT`（R16_UINT），`1` = `INT`（R32_UINT） |
| `mcdr_color_target_desc.format` | `GpuFormat` 序号；`-1` = 该槽位不用 |
| `mcdr_sr_dispatch.flags` | `0` = MV 含 jitter；`MCDR_SR_FLAG_UNJITTERED_MV`（1）= 已移除 jitter，限通用 FSR/XeSS；未知位与通用 DLSS 的非零 flags 返回 -1。布局仍为 136 B |
| `mcdr_input_element_desc.stepRate` | `0` = per-vertex，`1` = per-instance |
| `mcdr_input_element_desc.semanticName` | `NULL` 或 `""` 时按 `"TEXCOORD"` 处理 |
| `mcdrSetAntiAliasing(mode)` | `0` = OFF，`1` = SMAA，`2` = TAA，`3` = DLAA（当前降级为 OFF） |
| `mcdrSetShadingRate(rate)` | 原始 `D3D12_SHADING_RATE`：1X1=`0x0`、1X2=`0x1`、2X1=`0x4`、2X2=`0x5`、2X4=`0x6`、4X2=`0x9`、4X4=`0xA`；`-1` = 关闭 |
| `mcdr_dx12SetDlssgMode(mode, n)` | `0` = off，`1` = 固定倍率，`2` = dynamic |

## 7. 日志与诊断

| 导出 | 签名 | 语义 |
| --- | --- | --- |
| `mcdrSetLogLevel` | `void(int32_t level)` | 设置 native 日志门槛；入参被钳到 `0..3`（负数→0，>3→3） |
| `mcdrSetLogVerbose` | `void(int32_t enabled)` | 旧开关：非 0 → 级别 `3`，`0` → 级别 `1`（**不是** 0） |
| `mcdrSetLogFile` | `int32_t(const char* utf8Path)` | 设置/关闭逐行镜像文件；`1` = 已配置，`0` = 已关闭 |
| `mcdrSetShutdownTraceAlways` | `void(int32_t always)` | 关停/初始化进度标记是否无视级别（默认 `1`） |
| `mcdrLastError` | `int32_t(char* buf, int32_t cap)` | 读本线程最后一条错误串（§3.3 协议） |

### 7.1 级别 0..3

| 级别 | 名称 | 该级别的输出函数 | 门槛判断（`gLogLevel`） |
| --- | --- | --- | --- |
| `0` | `ERROR` | `dbgLogError` | 恒输出（判断是 `gLogLevel < 0`，钳制后永不成立） |
| `1` | `WARN` | `dbgLog` / `dbgLogWarn` | `>= 1` |
| `2` | `INFO` | `dbgLogInfo` | `>= 2` |
| `3` | `DEBUG` | `dbgLogDebug` | `>= 3` |

- 默认 `gLogLevel = 0`（只出 ERROR），只覆盖"JVM 把配置推进来之前"那段窗口；启动后由 Java 侧
  按配置调用 `mcdrSetLogLevel`。
- 进度标记 `dbgLogProgress` 标成 `INFO`，但只要 `gShutdownTraceAlways` 为真就无视级别输出
  （退出卡死时这是唯一线索）。
- `DEBUG` 会额外打开重活：D3D12 InfoQueue 全量 dump、像素落盘（都要求 `gLogLevel >= 3`）。
- `mcdrSetLogVerbose(0)` 会把级别压到 `1`，INFO 行会一起消失；只想关调试输出就显式调
  `mcdrSetLogLevel(2)`。

### 7.2 行格式与镜像文件

每行由 `formatLogPrefix()` 生成，别处**不要**再拼标签：

```
[ddMM月yyyy HH:mm:ss.mmm] [thread-<tid>/<LEVEL>] [dxr_mc/]: <正文>
[1510月2026 15:29:44.942] [thread-12345/ERROR] [dxr_mc/]: <正文>
```

- 时间块逐字对齐 JVM 侧 log4j 的 `ddMM月yyyy HH:mm:ss.SSS`，两路输出才能按时间戳对齐。
- 输出走 `stderr` 并 `fflush`；同时（若配置了路径）追加写入镜像文件，逐行 `fflush`。
- 镜像路径优先取 `mcdrSetLogFile` 推来的值，其次才是已弃用的环境变量 `DX12_NATIVE_LOG_FILE`；
  `fopen` 发生在**下一次写日志**时，所以 `mcdrSetLogFile` 返回 `1` 只代表路径已记录。
- 前缀都放不下时整行丢弃，不写半行。
- 错误通道走 `failVoid()`（`src\c_abi\abi_common.cpp:52`），行首 logger 名是 `mcdr`：
  `[thread-<tid>/ERROR] [mcdr/]: mcdr_dx12CreateTexture: invalid desc`。

> **缺陷**：镜像文件打开失败时没有任何提示——`mcdrSetLogFile` 已经返回了 `1`，而
> `mirrorLogToFile()` 直接 `return`。用户配了路径却拿不到文件时，日志里查不到原因。

> **缺陷**：`failVoid()` 的 ERROR 行只写 `stderr`，**不**经过镜像文件。所以"用户端拿不到 native
> 行"这件事，对最需要的那句失败原因仍然成立。

## 8. 失败处理总则

失败**从不**以异常形式跨越边界：没有 C++ 异常、没有 `longjmp`，只有返回值 + 错误串。

`mcdr_dx12NrdDenoiseTemporal` 返回正数表示已记录的 NRD pass 数，0 表示输入被拒绝，
`-1` 表示实例未就绪、派发槽位占满或 SDK 失败，`-2` 表示没有打开的命令列表。
空偏移指针与非法偏移返回 0，`outText` 在有效容量下清空；该早期拒绝不写 TLS 错误槽。
诊断为空时保留返回码，不把旧的 `mcdrLastError()` 当成本次失败原因。
装配规则固定为：

1. 校验失败 → `failWith(what, msg)`：写本线程错误槽，`stderr` 打一行 ERROR，返回 `0`；
   纯 `void` 入口用 `failVoid`。
2. 越界/未知枚举 → 返回 `-1`，同样写错误槽。
3. 需要打开的命令列表却没有 → 返回 `-2`。
4. 有现成 HRESULT 的捕获类入口 → 原样返回该 HRESULT（必为负）。
5. 创建类入口 → 返回 `NULL`，原因在错误槽。

### 8.1 错误串何时有效

| 问题 | 答案 |
| --- | --- |
| 作用域 | **每个线程一个槽**（`thread_local std::string`）：异步渲染线程、描述符 worker、upcall 各自独立 |
| 跨线程能否读 | 不能。在另一个线程读 `mcdrLastError` 只会得到那个线程自己的错误 |
| 何时被写 | 只在失败路径（`failWith` / `failVoid` / `setLastError`）；成功路径**不清** |
| 读会清空吗 | 不会，`mcdrLastError` 只拷贝 |
| 长度上限 | 1023 字节（`std::vsnprintf` 写进 `char scratch[1024]`，超长截断） |
| 消息形状 | `"<context>: <message>"`；`context` 为空时只有 `message` |

由此得到一条使用规则：**只在返回值明确表示失败之后才读 `mcdrLastError()`**。成功调用不清空槽位，
成功后读到的是上一次的旧原因。

### 8.2 不能靠重试补救的情况

- `mcdrSetPixCapture` 加载的捕获层对进程生命期**不可逆**（PIX 从内部 hook D3D12）：请求过就是
  请求过了，失败后在同一进程里重试没有意义。
- 返回 `0` 的"未启用/不支持"是**结论**，不是瞬时失败：例如设备不支持某个 shading rate、
  后端运行时 DLL 没加载。重试同样的参数只会得到同样的 `0`。
- 捕获入口返回的负 HRESULT 要**原样**显示给用户（`E_ACCESSDENIED` 表示进程未提权），不要统一
  改写成 `-1`。

### 8.3 代码中未明确

- 成功路径是否应该清空错误槽：`src\c_abi\abi_common.cpp` 里没有任何清空调用点，也没写这条约定。
- 错误槽是否在线程/设备销毁时重置：没有相关代码。

### 8.4 SDK 归属诊断

`mcdr_dx12QuerySdkResourceState(ctx, texture, values, count)` 读取指定 encoder 的纹理状态与 SDK 归属。
传入有效纹理和 8 个 `uint64_t` 的缓冲区，`count` 固定为 8。成功返回 1，参数无效返回 0。
字段顺序、owner 数值及适用范围见 [设备层 §5.7](../device/README.md#57-sdk-资源接管与归还)。
此入口只读取诊断信息，不等待 GPU，也不发屏障。JVM 包装为 `MCDR_ABI.D3D12QuerySdkResourceState`。

## 文件导航

| 路径（相对 `MCDR\`） | 负责什么 |
| --- | --- |
| `include\mcdr_abi.h` | 唯一契约：类型映射、句柄、aggregate、导出与 `MCDR_ABI_VERSION` |
| `src\module\c_abi\mcdr_abi_common.cppm` | `mcdr.abi.common` 接口单元：声明 `lastErrorText` / `setLastError` / `setLastErrorf` / `failWith` / `failVoid` / `writeString` |
| `src\c_abi\abi_common.cpp` | 上述 6 个函数的唯一实现：线程局部错误槽、两阶段字符串写出、ERROR 行格式 |
| `src\c_abi\abi_meta.cpp` | `mcdrABIVersion` / `mcdrABILayout` / `layoutTable()`（459 条记录）、`mcdrLastError`、日志级别与镜像文件入口、PIX/AA/DLSS 开关转发 |
| `src\core\d3d12_device.cpp` | 日志实现端：`gLogLevel`、`formatLogPrefix()`、`emitLogLine()`、`mirrorLogToFile()`、`setLogFile()`、`setLogLevel()` |
| `docs\overview.md` | 文档地图；本文是其中的 ABI 契约入口 |

对端（JVM 侧，不在 `MCDR\` 内）：

| 路径 | 负责什么 |
| --- | --- |
| `Illusion\src\main\kotlin\io\github\dxegp\d3d12\extern\MCDR_ABI.kt` | 全部导出的 FFM 绑定、`verifyABI()`、错误串读取、`SCRATCH` 槽位 |
| `Illusion\src\main\kotlin\io\github\dxegp\d3d12\extern\MCDR_Layout.kt` | `EXPECTED_C_SIZES`（36 行大小校验）、34 个结构体的字段镜像校验（`mcdr_dlss_constants` 与 `mcdr_abi_field` 只查大小）、`verify()` |

## 文档维护

1. 改 `include\mcdr_abi.h` 的结构体或签名 → 同一次改动里改齐 §4.1 的五处，并把 `MCDR_ABI_VERSION`
   +1；纯追加也要 +1。
2. §6 的 `sizeof` / `align` 必须与 `MCDR_Layout.kt` 的 `EXPECTED_C_SIZES`（29 行）和头文件注释
   三处一致：先改结构，再改镜像，最后改本文。
3. 新增 aggregate 时，每个命名字段（含 `_padN`）都要登记进 `layoutTable()`；漏登记只会以
   `missing from the native layout report` 的形式在设备创建时炸掉。
4. 新增导出 → 更新开头的计数（`MCDR_API ... MCDR_CALL` 声明数，当前 257）、§2.1 / §8 的返回值
   形态表，以及需要的 §3 句柄表。
5. 本文只写当下：不写工单号、会话号，不引用仓库外的文件；指路只指 `docs\` 下真实存在的文档，
   并与 `docs\overview.md` 的文档地图保持一致。
