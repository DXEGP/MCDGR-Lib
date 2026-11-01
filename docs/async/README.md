# 异步渲染基础设施（descriptor 分区 / bundle 录制池 / fence 管理器 / 主列表执行器）

本层把「录制命令」与「提交命令」拆成两条独立的时间线：worker 线程在各自的描述符分区里把一段 draw 录进 `BUNDLE` 命令列表，主线程只负责屏障、按序回放（`ExecuteBundle`）、闭合并提交通用命令列表。回收由 fence 驱动，不靠「固定睡 N 帧」。

适用读者：改提交路径、调并行度、或排查「描述符耗尽 / bundle 录制失败 / fence 不回收」的人。

范围：

- `MCDR\src\module\core\d3d12_descriptor_allocator.cppm` + `MCDR\src\core\d3d12_descriptor_allocator.cpp`
- `MCDR\src\module\core\d3d12_async_descriptor.cppm` + `MCDR\src\core\d3d12_async_descriptor.cpp`
- `MCDR\src\module\core\d3d12_async_bundle.cppm`（实现并入接口单元）
- `MCDR\src\module\core\d3d12_async_fence.cppm` + `MCDR\src\core\d3d12_async_fence.cpp`
- `MCDR\src\module\core\d3d12_async_executor.cppm`（实现并入接口单元）
- `MCDR\src\c_abi\abi_async.cpp`（41 个 C ABI 导出）

## 1. 关键常量

| 常量 | 值 | 定义处 | 含义 |
| --- | --- | --- | --- |
| `kAsyncFrameSlots` | `4` | `d3d12_async_descriptor.cppm` | 描述符分区的帧槽数（与 drawHeap ring 同为 4） |
| `kMaxAsyncWorkers` | `8` | 同上 | 单帧槽内可划分的 worker 分区上限 |
| `kAsyncSyncRingReserve` | `131072` | 同上 | 每段起始保留给同步 ring 的槽位数 |
| `kAsyncWorkerSlotsPerSection` | `24576` | 同上 | 每段留给全部 worker 的槽位总数 |
| `kDrawHeapPerFrame` | `155648` | `d3d12_device.cpp:167` | `131072 + 24576`，每段槽位数 |
| `kDrawHeapSections` | `4` | `d3d12_device.cpp:168` | drawHeap 段数 |
| `kDrawHeapSize` | `622592` | `d3d12_device.cpp:169` | `155648 × 4`，CBV_SRV_UAV / SHADER_VISIBLE 堆槽数 |
| 每 worker 分区槽数（8 worker） | `3072` | 计算值 | `(155648 - 131072) / 8` |
| `kBundleFrameSlots` | `4` | `d3d12_async_bundle.cppm` | 每个 recorder 的帧槽数 |
| `kBundleBatchesPerFrame` | `16` | 同上 | 单帧槽内允许的并行批次数 |
| `kBundleSlotCount` | `64` | 同上 | `4 × 16`，每个 recorder 持有的 allocator + list 对数 |
| `MainCommandExecutor::kFramesInFlight` | `4` | `d3d12_async_executor.cppm` | 主列表在飞帧上限 |
| CBV 地址对齐 | `256` 字节 | `d3d12_async_descriptor.cpp`、`abi_async.cpp` | `BufferLocation` 必须 256 字节对齐 |

## 2. 这层解决什么问题

三个问题，三种手段：

| 问题 | 手段 | 代价 |
| --- | --- | --- |
| 单批 draw 数可达数千（区块 renderGroup），单线程录制成为提交瓶颈 | 按 worker 切段，各 worker 在独立描述符分区里并行录 `BUNDLE` | 需预先知道每段描述符需求量，且要按序回放 |
| 描述符堆是一段全局 ring，多线程同时写会互相覆盖 | `(frameSlot, worker)` 二维分区，物理上互不相交 | 每段要预留，分区容量固定（8 worker 时每区 3072 槽） |
| 命令 allocator 只能在对应 GPU 工作完成后 `Reset` | fence + 多槽轮转（描述符 4 帧槽、bundle 4×16 槽、主列表 4 槽） | 在飞帧数被硬性限制在 4 |

### 2.1 录制 → 提交 → fence 完成 → 回收

```
worker w=0..N-1                    主线程（main list）              GPU                   回收条件
──────────────────────────────────────────────────────────────────────────────────────────────────
fenceValue = F，frameSlot = F % 4
   │ AsyncDescriptorResetFrame(slot)          ← 帧首单线程调用，只清游标
   │
   ├─ Allocate(slot, w, slots) → base
   │     分区 = [regionBase, regionBase+3072)
   ├─ BundleBegin(w, slot, F)                 → allocator/list Reset + SetDescriptorHeaps
   ├─ SetPipelineState / RecordPartition / Draw*
   └─ BundleEnd(w) → ID3D12GraphicsCommandList*
                                    │ PrepareCBVBuffers / PrepareIndexBuffers
                                    │ PrepareTextureViews      ← barrier 只能在主列表
                                    │ ExecuteBundle(bundle[0]) … bundle[N-1]   ← 严格按 w 升序
                                    │ Close()
                                    │ ExecuteCommandLists(1, main) ──► 开始执行
                                    │ Signal(fence, F)        ──► fence 达到 F
                                    │                              │
                                    │                              ├─ 该段 4 帧后可 ResetFrame
                                    │                              ├─ bundle 槽 (slot,b) 可 Reset
                                    │                              └─ 主列表槽 (F % 4) 可复用
```

必须遵守的顺序（违反即错，且不报错）：

1. 每个 worker 只碰自己的分区和自己的 `BundleRecorder`；不得跨区分配，不得让两个线程共用同一个 `worker` 下标。
2. `Prepare*` 的 barrier 必须在 `ExecuteBundle` 之前、在主列表上完成。
3. `ExecuteBundle` 必须按 worker 升序（或至少与录制的批次顺序一致）回放；重叠/混合几何依赖这个顺序。
4. `Close()` 之后才能 `ExecuteCommandLists`；`ExecuteBundle` 只能在主列表处于 open 状态时调用。
5. `ResetFrame` 只能在同一帧槽的 GPU 工作完成之后调用；在飞帧数固定为 4，第 5 帧会覆盖仍在读的槽位。

## 3. 架构与数据流

```
JVM: D3D12AsyncContext（worker 线程池 + 两个 native 句柄）
     D3D12RenderPassBackend.tryParallelDrawMultipleIndexed（切段、录制、回放）
        │ FFM downcall（Linker.nativeLinker → mcdr_abi.h）
        ▼
src\c_abi\abi_async.cpp        句柄：mcdr_desc_alloc / mcdr_bundle_pool / mcdr_fence_mgr / mcdr_executor
        │
        ├── mcdr.d3d12.async_descriptor  → PartitionedDescriptorAllocator（分区、CAS 分配、写 CBV/SRV）
        ├── mcdr.d3d12.async_bundle      → BundleRecorderPool / BundleRecorder（64 槽 × worker）
        ├── mcdr.d3d12.async_fence       → AsyncFenceManager（fence + 监控线程 + 回调表）
        ├── mcdr.d3d12.async_executor    → MainCommandExecutor（4 槽主列表，未接入 JVM）
        └── mcdr.d3d12.device            → DeviceContext::drawHeap / drawInc / syncRingReserve / cmdSig*
```

组件职责：

| 组件 | 拥有资源 | 线程模型 | JVM 侧现状 |
| --- | --- | --- | --- |
| `PartitionedDescriptorAllocator` | 非拥有 `drawHeap` 引用 + 32 个 `std::atomic<UINT>` 游标 | 分配无锁 CAS；`resetFrame` 须帧边界单线程 | 已使用 |
| `BundleRecorder` | 64 个 `BUNDLE` allocator + 64 个 `BUNDLE` list | 每个 recorder 同一时刻只允许一个线程录制 | 已使用 |
| `BundleRecorderPool` | `workerCount` 个 `BundleRecorder` | `recorder(w)` 只读 | 已使用 |
| `AsyncFenceManager` | 1 个 fence + 1 个监控线程 + `std::unordered_map<UINT64, PendingCallback>` | `signal` / `waitFence` / `registerCallback` 线程安全 | 未使用（仅 ABI 声明） |
| `MainCommandExecutor` | 4 × (DIRECT allocator + list) + 1 个 fence | `tryBeginFrame` / `endFrame` 必须主线程 | 未使用（仅 ABI 声明） |

一次 `drawMultipleIndexed` 的实际调用序列（JVM 侧，`D3D12RenderPassBackend.kt`）：

1. `PARALLEL_MIN_DRAWS = 24`：draw 数不足直接走串行。
2. 收集本批 CBV buffer / VB / IB / texture view 去重集合，调用 `AsyncPrepareCBVBuffers` / `AsyncPrepareIndexBuffers` / `AsyncPrepareTextureViews`。
3. `frameSlot = AsyncCurrentFrameSlot(ctx)`，`frameValue = AsyncCurrentFrameValue(ctx)`。
4. 决定模式：`CommandBundleMode.SERIAL` → 单分区；`PARALLEL` → 并行；`AUTO` → `n <= SERIAL_BUNDLE_MAX_DRAWS(512)` 且 `slotsNeeded <= 2048` 时单分区。
5. 每个 worker：`ID3D12AsyncDescriptorAllocate(alloc, frameSlot, w, slotsNeeded)` → `D3D12AsyncBundleBegin` → `SetPipelineState` → `AsyncBundleRecordPartition[Shared]` → `D3D12AsyncBundleEnd`。
6. 主线程按 w 升序 `D3D12ExecuteBundle`；单分区路径失败时升级为并行，并行录制失败则丢弃 bundle 回退串行。

## 4. 分区描述符分配器

### 4.1 堆从哪来、怎么切

`mcdr_dx12AsyncDescriptorCreate(workerCount)` 直接取 `DeviceContext::drawHeap`（`CBV_SRV_UAV` / `SHADER_VISIBLE`，`622592` 槽）与它的 `GetDesc().NumDescriptors`，再用 `kAsyncSyncRingReserve = 131072` 作为 `reservedPerSection` 调 `PartitionedDescriptorAllocator::init`。

```
slot 0                                                                    622592
│
├─ 段 0（155648 槽）─┬─ sync ring 131072 ─┬─ w0 3072 ─┬─ w1 3072 ─ … ─┬─ w7 3072 ─┤
├─ 段 1（155648 槽）─┼────────────────────┼───────────┼───────────────┼───────────┤
├─ 段 2（155648 槽）─┼────────────────────┼───────────┼───────────────┼───────────┤
└─ 段 3（155648 槽）─┴────────────────────┴───────────┴───────────────┴───────────┘
```

- `sectionSize = heapSize / kAsyncFrameSlots = 622592 / 4 = 155648`。
- `slotsPerRegion = (sectionSize - reservedPerSection) / workerCount`；8 worker 时 `(155648 - 131072) / 8 = 3072`。
- `regionBaseSlot(f, w) = f * sectionSize + reservedPerSection + w * slotsPerRegion`；`frameSlot >= 4` 或 `worker >= workerCount` 返回 `UINT_MAX`。
- 两段不重叠是硬要求：同步路径（`pushDescriptors` / `blitBindSourceTexture` / `smaaBindSrv` / `meshletDispatch`）只允许用段首 `syncRingReserve` 个槽位。`dx12AsyncDescriptorCreate` 会把 `DeviceContext::syncRingReserve` 置为 `131072`，`syncRingCapacity()` 随之从 `155648` 降为 `131072`；`dx12AsyncDescriptorDestroy` 再置回 `0`。

### 4.2 接口语义

| 函数 | 成功返回 | 失败返回 | 备注 |
| --- | --- | --- | --- |
| `init(device, heap, heapSize, workerCount, reservedPerSection)` | `mcdr::Result<void>` 有值 | `mcdr::Error` | 参数校验见 §4.3 |
| `regionBaseSlot(frameSlot, worker)` | 绝对槽位 | `UINT_MAX` | 只读，不加锁 |
| `allocate(frameSlot, worker, count)` | 绝对起始槽位 | `UINT_MAX`（区域耗尽 / `count == 0`） | 整段 `count` 原子预留，非逐槽 |
| `resetFrame(frameSlot)` | — | — | 只清该帧槽全部 worker 的游标 |
| `gpuHandle(slot)` / `cpuHandle(slot)` | 句柄 | `ptr == 0` | `slot >= heapSize` 时为空 |
| `writeCBV(slot, buffer, offset, size)` | `true` | `false`（未对齐 / 越界） | `SizeInBytes` 上取整到 256 的倍数，最小 256，上限 `0xFFFFFFFF` |
| `writeDescriptor(slot, src)` | `true` | `false`（`src.ptr == 0`） | `CopyDescriptorsSimple(1, …)` |

ABI 层把 `UINT_MAX` 映射成 `-1`（`mcdr_dx12AsyncDescriptorAllocate` / `RegionBase` 返回 `int32_t`），JVM 侧以 `base < 0` 判定失败。

`allocate` 不回收、不碎片整理：游标只增不减，直到 `resetFrame` 归零。同一帧内对同一 `(frameSlot, worker)` 多次分配是累加（bump），不是复用。

### 4.3 构造校验与线程安全

`init` 拒绝以下输入（全部返回 `mcdr::Error`，ABI 侧把 `describe(error)` 拼进 `mcdrLastError`）：

- `device` / `heap` 为空，或 `heapSize == 0`
- `workerCount == 0 || workerCount > kMaxAsyncWorkers(8)`
- `sectionSize == 0`，或 `reservedPerSection >= sectionSize`
- `slotsPerRegion == 0`（堆太小，分不出每 worker 一区）

线程安全：

| 操作 | 同步手段 | 约束 |
| --- | --- | --- |
| `allocate` 快路径 | `std::atomic<UINT>` 上的 `compare_exchange_weak`（失败重试） | 不同区域零竞争；同一区域并发也只会拿到互不重叠的区间 |
| `regionBaseSlot` / `gpuHandle` / `cpuHandle` | 无 | 只读，`m_slotsPerRegion` / `m_increment` 初始化后不变 |
| `resetFrame` | `store(…, memory_order_release)` | 只能在「该帧槽 GPU 工作已完成且无并发分配」时调用；否则游标归零会让两个线程拿到同一区间 |
| `writeCBV` / `writeDescriptor` | 无 | 调用方须保证目标 `slot` 归自己所有 |

`m_cursors` 是 `std::array<std::atomic<UINT>, kAsyncFrameSlots * kMaxAsyncWorkers>` = 32 个游标，按 `frameSlot * workerCount + worker` 索引。

### 4.4 跨帧回收

回收的唯一动作是 `resetFrame(frameSlot)`，由 JVM 侧在帧槽变化时调用（`D3D12AsyncContext.frameSlot(ctx)`：`slot != lastResetFrameSlot` 才 reset）。语义依赖调用顺序：

- 帧号必须来自 `mcdr_dx12AsyncCurrentFrameSlot(ctx)`（即 `ctx->fenceValue % 4`），不要自己造 `% 4` 的计数器——它必须与 `ctx->drawHeapSlotBase = (fenceValue % 4) * kDrawHeapPerFrame` 同步，否则分区与段起点错位。
- 调用时机必须在命令列表 `begin` 之后（此时 `fenceValue` 已是本帧值）。
- 代码不检查 fence：分区复用的正确性完全由「在飞帧数 ≤ 4」保证。把在飞帧推到 5 帧，第 5 帧会把仍在被 GPU 采样的描述符内容改写（表现为贴图串、UBO 读到上一帧矩阵），且不会报错。

### 4.5 对齐与写入细节

- CBV 的 `BufferLocation = GetGPUVirtualAddress() + offset` 必须 256 字节对齐；`writeCBV` 与 ABI 的 `mcdr_dx12AsyncDescriptorWriteCBV` 都会拒绝未对齐的 offset（ABI 报 `CBV is not 256-byte aligned (buffer offset=…)`，native 层另有 `logAlignmentFail` 同文本）。
- `SizeInBytes` 由调用方给的 `size` 上取整到 256 的倍数，`0` 修正为 `256`；超过 `0xFFFFFFFF` 截断。
- `writeCBV` 写的是描述符堆里的 CBV；而 `RecordPartition` 走的是 root descriptor（地址直接烘进命令列表，不占堆槽）。两条路径不要混用同一批绑定的语义。

### 4.6 `SegmentedDescriptorAllocator`（当前未启用）

`d3d12_descriptor_allocator.cppm` 另有一个通用分段分配器：`sections × descriptorsPerSection` 的堆，`sectionFor(frame, worker) = (frame % inFlightFrames) * workerCount + worker`，快路径 `fetch_add`、慢路径 `freeSlot` + `std::mutex` 保护的 LIFO `freeList`，`resetSection` 整段复用。

它在 `src\` 下没有任何调用点（只有自身 `.cpp` 的定义）：异步路径用 `PartitionedDescriptorAllocator`，同步路径用 `DeviceContext` 的 drawHeap ring。改用它之前先确认帧槽布局与 `syncRingReserve` 的兼容性。

## 5. bundle 录制池

### 5.1 粒度

一个 `BundleRecorder` = 一个 worker = 一组 `64` 个 `D3D12_COMMAND_LIST_TYPE_BUNDLE` allocator + list（`kBundleSlotCount = 4 × 16`）。一次 `begin()` 得到其中一组，录一段连续的 draw（JVM 侧把整批按 `perWorker = ceil(n / workerCount)` 切段），`end()` 关闭并交出 `ID3D12GraphicsCommandList*` 句柄（`uint64_t`）。句柄在回放后才失效。

池 `BundleRecorderPool` 只做一件事：按 worker 下标持有 recorder，`recorder(w)` 越界返回 `nullptr`。8 worker 的池在 `init` 时创建 `8 × 64 × 2 = 1024` 个 D3D12 对象。

### 5.2 槽位轮转：`(frameSlot, batch)`

```
slot = (frameSlot % 4) * 16 + batch        batch ∈ [0, 16)
```

- `batch` 的归零条件是 `m_frameOfSlot[fs] != frameValue`（`frameValue` 是原始帧号，不是 `% 4` 的值）。用 `% 4` 的值判断会导致「空闲若干帧后帧槽回落」时误判为新帧。
- `batch >= kBundleBatchesPerFrame(16)` → `begin()` 返回 `false`，调用方回退串行。这是静默降级：native 只返回失败，JVM 侧只有 prof 计数与一条 `warn`。
- 同一帧内对同一 worker 连续调用 `begin()` 会依次占用 batch 0、1、2…，因此同一帧的多个批次不会 Reset 仍在被 GPU 读取的 allocator。

### 5.3 `begin()` / `end()` 顺序约束

`begin(frameSlot, frameValue)` 内部按此顺序执行，任何一步失败即返回 `false`：

1. 检查 `m_recording == false`（重入直接失败）。
2. 按 `frameValue` 决定是否把该帧槽的 batch 归零。
3. 取 `slot`，`allocator->Reset()`。
4. `list->Reset(allocator, nullptr)`。
5. `SetDescriptorHeaps(1, {drawHeap})`。
6. 清 `m_rootSig` / `m_pso` / `m_topology` / `m_correctedStrides`，`resetStats()`。

约束：

- **每个 bundle 必须自己绑定一次 descriptor heap**。bundle 不继承父列表的堆；不绑就 `SetGraphicsRootDescriptorTable` 时会在运行时解析 GPU 句柄处解引用空堆指针崩溃（命令列表成员 `+0x28`）。这个堆必须与父列表执行该 bundle 时绑定的堆是同一个（都是 `DeviceContext::drawHeap`）。
- **`SetDescriptorHeaps` 只能一次**，且必须在本 bundle 的任何描述符表命令之前。
- `end()` 只做 `Close()` 并返回句柄；不 Close 就回放会让 GPU 侧读到未封口的列表。
- `begin()` 之后、`end()` 之前不得在别的线程动这个 recorder；`m_recording` 不是原子的。
- `Reset` 正在被 GPU 读取的 allocator 是未定义行为——由 §5.2 的槽位轮转保证不重叠。

### 5.4 允许的录制命令

| 允许 | 禁止 |
| --- | --- |
| `SetGraphicsRootSignature`（去重） | `ClearRenderTargetView` / `ClearDepthStencilView` / `ClearUnorderedAccessView*` |
| `SetPipelineState`（去重） | `CopyResource` / `CopyBufferRegion` / `CopyTextureRegion` / `ResolveSubresource` |
| `SetGraphicsRootDescriptorTable` | `ResourceBarrier`（任何类型） |
| `SetGraphicsRootConstantBufferView` | `OMSetRenderTargets` / `RSSetViewports` / `RSSetScissorRects` |
| `IASetVertexBuffers` / `IASetIndexBuffer` / `IASetPrimitiveTopology` | 第二个 `SetDescriptorHeaps` |
| `DrawIndexedInstanced` / `DrawInstanced` / `ExecuteIndirect` | 嵌套 `ExecuteBundle` / render pass 相关命令 |
| `SetDescriptorHeaps`（仅一次） | — |

`drawIndexedInstanced` 忽略 `indexCount == 0`；`drawIndexedIndirect` 在 `cmdSigIndexed` 为空时直接返回 `false`，绝不向 `ExecuteIndirect` 传 `nullptr`。

拓扑映射（`bundlePrimitiveTopology`）：`0/1 → LINELIST`、`2 → LINESTRIP`、`3 → POINTLIST`、`5 → TRIANGLESTRIP`、`6 → TRIANGLEFAN`、`7 → TRIANGLELIST`（QUADS 不支持，回退）、其余默认 `TRIANGLELIST`。

### 5.5 回放：`ExecuteBundle` 的副作用

`mcdr_dx12ExecuteBundle(ctx, bundle)` 只是 `ctx->commandList->ExecuteBundle(b)`，但有两处状态必须一起处理：

- native：`ctx->cachedPso = nullptr`。bundle 里设置的 PSO / root signature 不会回滚到父列表，父列表的绑定状态从此不可知，必须让 `setPipeline` 的快速路径失效。
- JVM：同样把 `anyDescriptorDirty = true`，强制后续 core 重推描述符（否则继承 bundle 留下的根表参数）。

前置检查：`ctx`、`ctx->commandList`、`ctx->listOpen`、`bundle` 任一为空 → `failWith("mcdr_dx12ExecuteBundle", "invalid args")`。

### 5.6 `AsyncBundleRecordPartition` 的录制策略

入参是两组 struct 数组（`mcdr_binding_slot` / `mcdr_partition_draw`），零拷贝切片，无逐元素 marshalling：

| 参数 | 含义 |
| --- | --- |
| `baseSlot` | 本分区起始描述符槽（来自 `Allocate`） |
| `bindingCount` | 绑定个数 `bc` |
| `sharedBindings` | `0` = 逐 draw 绑定（`bindings[i*bc + j]`）；非 0 = 整批共用 `bindings[j]` |
| `draws` | 本分区的 draw 数组，`draws[0]` 即本段第一个 draw（没有 `startDraw` 参数） |

录制策略：

1. **SRV 表只写一次**：所有 `type != 0` 的绑定按顺序 `writeDescriptor(baseSlot + s)` 写入，然后 `setDescriptorTable(cbvCount, gpuHandle(baseSlot))`。前提是调用方已证明整批 SRV 集合一致（JVM 侧只在 `srvUniform` 时进 bundle 路径）。
2. **CBV 走 root descriptor**：不占描述符堆槽，地址烘进命令列表。所以 CBV 数量不消耗 `slotsNeeded`。
3. **常量 CBV 提升**：`cbvCount <= 64` 时先扫一遍，只有「至少两个 draw 之间 buffer 或 offset 不同」的 CBV 才逐 draw 重设；其余在循环外设一次。`cbvCount > 64` 时放弃优化，全部逐 draw 设。
4. **VB/IB 提升**：整批 VB/IB/stride/offset/slot 全相同（`vbIbUniform`）时只在 `i == 0` 设一次。
5. **stride 以管线为准**：`correctedStride(vertexSlot)` 非 0 时覆盖 Java 传入的 `vertexStride`（Java 的 `getVertexSize()` 对部分管线是错的）。
6. 任一引用为空 / 类型不对 / `offset < 0` / CBV 地址未对齐 → 返回 `0`（ABI 侧再补 `invalid buffer/view in partition` 或对齐错误文本），本批次整体失败，调用方回退串行。

## 6. fence 管理器

### 6.1 fence 值语义

单调计数器：内部 `ID3D12Fence` 初值 `0`，`signal(v)` = `queue->Signal(fence, v)`。完成判定是 `GetCompletedValue() >= v`，因此值只能递增、不可回退；跳值（直接 `Signal(7)`）会让所有 `<= 7` 的等待一起完成。`AsyncFenceManager` 的 fence 是独立于 `MainCommandExecutor` 与 `DeviceContext` 的另一个 fence 对象，三者的值不可互换。

### 6.2 等待与轮询

| API | 行为 | 超时 |
| --- | --- | --- |
| `waitFence(v, timeoutMs)` | 先查 `GetCompletedValue() >= v`，未完成则 `CreateEventW` + `SetEventOnCompletion` + `WaitForSingleObject` | `0` = 无限；`> 0xFFFFFFF0` 钳到 `0xFFFFFFF0` |
| `isFenceComplete(v)` | 直查完成值，无阻塞 | — |
| `registerCallback(v, cb[, timeoutMs])` | 已完成 → 立刻在调用线程回调；未运行 → 立刻回调 `success=false`；否则入表并唤醒监控线程 | 见 §6.3 |

没有 1ms 轮询：等待全部基于 `SetEventOnCompletion`。监控线程只在「有待完成回调」时工作，否则以 100ms 超时睡在 `m_wakeEvent` 上。

### 6.3 回调排队

- 表：`std::unordered_map<UINT64, PendingCallback>`，键是 fence 值，`PendingCallback{ fenceValue, callback, timeoutMs, startTimeMs }`。**一个 fence 值只保留一个回调。**
- 监控线程：`THREAD_PRIORITY_BELOW_NORMAL`；循环里取表中最小 fence 值 → `SetEventOnCompletion(minValue, ev)` → `WaitForMultipleObjects(2, {ev, m_wakeEvent}, FALSE, 100)`。`WAIT_OBJECT_0` 触发 `processCompletedFences()`；`WAIT_OBJECT_0 + 1` 重置唤醒事件；`WAIT_TIMEOUT` 直接下一轮（用于重算最早值和处理超时）。
- 派发：`processCompletedFences` 在锁内把「已完成」与「超时」的条目搬进局部 `vector` 并 `erase`，**在锁外**逐个调用回调。所以回调内部可以再次 `registerCallback` / `unregisterCallback`，不会自锁。监控线程是单线程串行的，单次回调过长会推迟后续 fence 的派发。
- 超时精度受 100ms 唤醒粒度限制：`timeoutMs` 到点后可能再晚最多约 100ms 才被判定。`timeoutMs == 0` 表示不超时。

### 6.4 为什么回调必须走这张设备级单表

`AsyncFenceManager` 是整个设备一份（`mcdr_dx12AsyncFenceCreate` 用 `DeviceContext` 的 device/queue 建），所有登记都汇进同一个 `m_pending`。不要把它改成「每个帧槽 / 每次提交各自持有一组回调」：

- 帧槽是 4 个一循环的。若回调挂在帧槽的局部容器上，槽在 4 帧后被回收时容器随槽一起清空，未触发的回调就永久丢失；它持有的 `std::function` 捕获（通常是待释放的 `ID3D12Resource` / descriptor 区块 / 上传缓冲）不会执行任何释放逻辑，等于每帧泄漏一次，且在 fence 迟迟不完成时（GPU 卡住、设备移除）泄漏量与帧数成正比。
- 单表 + `shutdown()` 才能兑现「退出时不留悬挂回调」：`shutdown` 停线程、`join`、把剩余 pending 全部以 `success=false` 回调出去、关 `m_wakeEvent`、`Reset` fence/queue。若回调散落在局部容器里，`shutdown` 无法枚举它们，析构时 `std::function` 直接销毁而不执行，回调里的清理逻辑永远不跑。
- 表中键必须唯一：同一 fence 值二次 `registerCallback` 会**覆盖**前一条（`m_pending[v] = std::move(p)`），前一个回调既不执行也不报错。

### 6.5 统计与关闭

`Stats{ totalCallbacks, pendingCallbacks, completedCallbacks, timedOutCallbacks, avgWaitTimeUs }`；`pendingCallbacks` 来自 `m_pending.size()`，`avgWaitTimeUs` 的更新是 `(avg + waitedUs) / 2`（递推平滑，不是算术平均）。`shutdown()` 可重复调用；析构函数自动调用它。

## 7. 主列表执行器

### 7.1 状态机

```
                   tryBeginFrame()                       endFrame()
      [Idle] ───────────────────────────► [Recording] ─────────────────► [Submitted]
        ▲   slot.submittedFenceValue 已完成                                      │
        └──────────────── m_slotIndex 前进；4 帧后该槽 fence 完成 ───────────────┘
```

- `tryBeginFrame()` 在 `!valid()`、已在帧中、或当前槽「上一帧未完成」时返回 `false`；绝不阻塞等 GPU。
- `endFrame()` 在未在帧中或 `!valid()` 时返回 `0`。

### 7.2 4 槽在飞

`m_slots[kFramesInFlight] = 4`，每槽 `FrameSlot{ allocator, commandList, submittedFenceValue }`，`init` 时创建 `DIRECT` allocator + list 并立刻 `Close()`。槽号按 `(m_slotIndex + 1) % 4` 前进，因此可用的在飞帧数是 4（第 5 次 `tryBeginFrame` 会在原槽未完成时返回 `false`）。

`tryBeginFrame` 的顺序：检查槽完成 → `allocator->Reset()` → `list->Reset(allocator, nullptr)` → `SetDescriptorHeaps(1, {m_heap})`（`init` 传入的堆，可为 `nullptr`）→ 清空 bundle/barrier 队列 → `m_frameInProgress = true`。

### 7.3 `endFrame()` 的提交顺序

1. 锁内 `swap` 出 barrier 数组与 bundle 数组（此后两条队列为空）。
2. 主列表 `ResourceBarrier(n, barriers)` —— 全部 barrier 集中在这里，bundle 内不允许。
3. 按加入顺序 `ExecuteBundle(b)` 逐个回放；顺序影响重叠/blend 的绘制结果。
4. `Close()`；失败 → `m_frameInProgress = false`、清队列、返回 `0`。
5. `ExecuteCommandLists(1, {mainList})` → `++m_fenceValue` → `queue->Signal(fence, value)` → 记录 `slot.submittedFenceValue`、`m_lastFenceValue`。
6. `m_slotIndex` 前进，返回 fence 值。

`m_frameInProgress` 直到第 6 步之后才置 `false`，所以 `endFrame` 执行期间的并发 `addBundle` 会入队但不会被本帧执行。约束：不要在 `endFrame` 期间并发 `addBundle`。

`waitForFrame(v, timeoutMs)`：`v == 0` 或已完成直接 `true`，否则 `SetEventOnCompletion` + `WaitForSingleObject`（`0` = 无限）。

### 7.4 与设备 submit 的对接（现状）

`mcdr_dx12AsyncExecutorCreate` 用 `DeviceContext` 的 `device` / `queue` / `drawHeap` 建执行器：提交走同一个 `ID3D12CommandQueue`，但主列表、allocator、fence 都是执行器私有的，不经过 `CommandContext`，也不推进 `ctx->fenceValue`。

因此：

- 执行器的 `m_fenceValue`（从 `0` 起，每次 `endFrame` 先 `++` 再 `Signal`）与分区分配器用的 `ctx->fenceValue` 是两个独立计数器。
- 在同一个渲染循环里混用两者，会把 `frameSlot` 与分区布局错位：`Allocate` / `ResetFrame` 的 `frameSlot` 必须来自 `AsyncCurrentFrameSlot(ctx)`。统一的帧号来源「代码中未明确」。
- JVM 侧当前不创建执行器，提交仍由同步 `CommandContext` + `mcdr_dx12ExecuteBundle` 完成（见 §10）。

## 8. 屏障预处理

bundle 内禁止 `ResourceBarrier`，所以每批 draw 用到的资源必须在主列表上先过渡。这层提供三个入口，行为一致：遍历数组，对每个有效对象调用一次 `transitionBufferTo` / `transitionTextureTo`（状态已相同的调用是 no-op）。

| 入口 | 目标状态 | 适用对象 |
| --- | --- | --- |
| `mcdr_dx12AsyncPrepareCBVBuffers(ctx, buffers, count)` | `D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER` | CBV 缓冲 + 顶点缓冲（两者目标状态相同） |
| `mcdr_dx12AsyncPrepareIndexBuffers(ctx, buffers, count)` | `D3D12_RESOURCE_STATE_INDEX_BUFFER` | 索引缓冲（`R16_UINT` / `R32_UINT`） |
| `mcdr_dx12AsyncPrepareTextureViews(ctx, views, count)` | `NON_PIXEL_SHADER_RESOURCE \| PIXEL_SHADER_RESOURCE` | 采样纹理视图（读 `view->sourceTexture`，无源纹理则跳过） |

调用方给什么：native 对象句柄数组 + `count`。`nullptr` 条目、`kind != Buffer` 的缓冲条目、没有 `sourceTexture` 的视图条目都被静默跳过（不报错），所以数组里混进无效句柄不会失败，只会少过渡一个资源。

前置条件：`ctx != nullptr`、`ctx->listOpen == true`、`objects != nullptr`，否则 `failWith(what, "invalid args")`；`count <= 0` 直接返回 `1`。

错误用法的后果：

- 漏调：bundle 里的 draw 读到未过渡的资源——debug layer 报前置状态不匹配，Release 下是脏数据/贴图错乱，严重时设备移除。
- 在 `ExecuteBundle` 之后才调：barrier 落在使用之后，同样不覆盖 bundle 内的访问，等于没做。
- 在 worker 线程调用：`ctx->commandList` 不是线程安全的，并发 `ResourceBarrier` 会破坏命令列表（本层不加锁）。

## 9. ABI 入口 `abi_async.cpp`

41 个 `MCDR_API` 导出，五族：

| 族 | 数量 | 函数 |
| --- | --- | --- |
| 分区分配器 | 8 | `AsyncDescriptorCreate` / `Destroy` / `RegionBase` / `Allocate` / `ResetFrame` / `GpuHandle` / `WriteCBV` / `WriteSRV` |
| bundle 池 | 12 | `AsyncBundlePoolCreate` / `PoolDestroy` / `Begin` / `End` / `SetPipelineState` / `SetDescriptorTable` / `SetVertexBuffer` / `SetIndexBuffer` / `DrawIndexed` / `Draw` / `DrawIndexedIndirect` / `RecordPartition` |
| 回放与屏障 | 6 | `ExecuteBundle`、`AsyncCurrentFrameSlot`、`AsyncCurrentFrameValue`、`PrepareCBVBuffers`、`PrepareIndexBuffers`、`PrepareTextureViews` |
| fence | 6 | `AsyncFenceCreate` / `Destroy` / `Signal` / `Wait` / `IsComplete` / `RegisterCallback` |
| 执行器 | 9 | `AsyncExecutorCreate` / `Destroy` / `TryBeginFrame` / `AddBundle` / `AddTransition` / `EndFrame` / `LastFenceValue` / `IsFrameInProgress` / `WaitFrame` |

线程约束：

| 函数族 | 允许的调用线程 | 备注 |
| --- | --- | --- |
| `AsyncDescriptorCreate/Destroy` | 主线程 | 会改 `DeviceContext::syncRingReserve`；`Destroy` 之后同步路径恢复整段容量 |
| `AsyncDescriptorAllocate` / `RegionBase` / `GpuHandle` / `WriteCBV` / `WriteSRV` | 任意 worker（同一 `(frameSlot, worker)` 内可并发） | 只碰自己的区域 |
| `AsyncDescriptorResetFrame` | 主线程，帧首 | 与任何 `Allocate` 互斥 |
| `AsyncBundlePoolCreate/Destroy` | 主线程 | `Destroy` 前必须停掉所有 worker 线程 |
| `AsyncBundleBegin` / `Set*` / `Draw*` / `End` / `RecordPartition` | 对应 worker 线程 | 同一 `worker` 下标同一时刻只能有一个线程；`SetPipelineState` 会从 `Dx12Pipeline` 抄 `vertexStrides` |
| `ExecuteBundle` / `Prepare*` / `AsyncCurrentFrame*` | 主线程，主列表 open 期间 | `ctx` 命令列表非线程安全 |
| `AsyncFence*` | 任意线程 | 内部互斥；回调在监控线程 |
| `AsyncExecutorTryBeginFrame` / `AddBundle` / `EndFrame` | 主线程 | `addBundle` 有锁，但帧状态机是单线程的 |

失败上报：

- 参数/状态错误 → `mcdr::abi::setLastError("mcdr_dx12Xxx", "…")`，JVM 侧读 `mcdrLastError`；`failWith` 同时写 last-error 并返回 `0` / `-1`。
- 结构性失败（`init` 失败、`CreateCommandAllocator` / `CreateCommandList` 失败）→ `mcdr::describe(error)` 拼进 last-error，并 `logFail(...)` 打一行 `… failed`，前缀走 `formatLogPrefix(..., "ERROR", "dx12")`。
- 读不到设备上下文时（`device` / `drawHeap` / `queue` 为空）→ 返回 `nullptr`，last-error 为 `device/drawHeap not ready` 或 `device/queue not ready`。

## 10. 已知限制与缺陷

> **缺陷**：`m_pending` 表一键一值。对同一个 fence 值二次 `registerCallback` 会覆盖前一条记录（`m_pending[v] = std::move(p)`），前一个 `std::function` 被就地销毁、不执行。若它负责释放资源，该资源永久泄漏且没有任何日志。

> **缺陷**：`AsyncFenceManager` 与 `MainCommandExecutor` 在 JVM 侧只有 `MCDR_ABI.kt` 的声明与 `Linker` 绑定，没有任何调用点（`D3D12AsyncContext` 只创建描述符分配器与 bundle 池）。实际提交仍是同步 `CommandContext`。这两套设施处于「可调用但未接入」状态。

> **缺陷**：`MainCommandExecutor` 自带的 `m_fenceValue` 与 `CommandContext::fenceValue` 不同源，且执行器不推进 `ctx->fenceValue`。若把 `EndFrame()` 的返回值当 `frameSlot` 传给分区分配器，会与 `drawHeapSlotBase` 的段号错位；正确来源是 `AsyncCurrentFrameSlot(ctx)`。统一方案「代码中未明确」。

> **缺陷**：分区容量是纯 bump，只有 `resetFrame` 归零，没有单槽 `free`。同一帧内对同一 `(frameSlot, worker)` 多次 `Allocate` 会累加：8 worker 时单区仅 3072 槽，一帧多批（3 批区块 ≈ 6100 槽）必然耗尽。JVM 侧的应对是「每批一次性整段分配」并在单分区失败时升级并行，而不是在 native 侧回收。

> **缺陷**：`SegmentedDescriptorAllocator` 没有任何调用点（`src\` 内只有自身实现）。它的 `inFlightFrames × workerCount` 布局与当前 drawHeap 的 4 段布局不一致，直接启用前需要重新核对 `sectionFor` 与 `syncRingReserve` 的关系。

> **缺陷**：`kBundleBatchesPerFrame = 16` 用尽后 `BundleBegin` 返回 `false`，只静默降级为串行录制；JVM 侧只有一条 `warn` 与一个 prof 计数，native 端没有可观测计数器。

> **缺陷**：在 fence 回调里调用 `shutdown()` 会自我 `join`（回调由监控线程执行，`shutdown` 里 `m_monitorThread.joinable()` 为真），造成死锁或 `std::system_error`。代码中未明确防护，回调里只允许 `register` / `unregister`。

> **缺陷**：`avgWaitTimeUs` 的更新是 `(avg + waitedUs) / 2`，不是算术平均；把它当精确统计会误判等待时间分布。

> **缺陷**：`AsyncFenceManager::waitFence` 与执行器的 `waitForFrame` 每次调用都 `CreateEventW` / `CloseHandle`。高频等待（每帧每批）会带来可观的事件对象创建开销，代码未缓存事件对象。

## 文件导航

| 路径（相对 `MCDR\`） | 内容 |
| --- | --- |
| `src\module\core\d3d12_descriptor_allocator.cppm` | `SegmentedDescriptorAllocator` 声明（当前无调用点） |
| `src\core\d3d12_descriptor_allocator.cpp` | 上者实现：`sectionFor` / `allocSlot` / `freeSlot` / `resetSection` |
| `src\module\core\d3d12_async_descriptor.cppm` | `kAsyncFrameSlots` / `kMaxAsyncWorkers` / `kAsyncSyncRingReserve` / `kAsyncWorkerSlotsPerSection`、`PartitionedDescriptorAllocator` |
| `src\core\d3d12_async_descriptor.cpp` | 分区布局、CAS 分配、`writeCBV` / `writeDescriptor` |
| `src\module\core\d3d12_async_bundle.cppm` | `kBundleFrameSlots` / `kBundleBatchesPerFrame` / `kBundleSlotCount`、`BundleRecorder`、`BundleRecorderPool`、`bundlePrimitiveTopology`、`executeBundle` |
| `src\module\core\d3d12_async_fence.cppm` | `AsyncFenceManager` 声明与 `Stats` |
| `src\core\d3d12_async_fence.cpp` | 监控线程、`processCompletedFences`、`waitFence`、`shutdown` |
| `src\module\core\d3d12_async_executor.cppm` | `MainCommandExecutor` / `kFramesInFlight` |
| `src\c_abi\abi_async.cpp` | 41 个 C ABI 导出、`prepareObjects`、`recordPartition`、`logFail` |
| `include\mcdr_abi.h` | `mcdr_binding_slot` / `mcdr_partition_draw` / `mcdr_fence_callback_fn` 与全部导出声明 |
| `src\core\d3d12_device.cpp` | `kDrawHeapPerFrame` / `kDrawHeapSections` / `kDrawHeapSize` / `syncRingCapacity` / `currentDrawFrameSlot` |
| `src\module\core\mcdr_device.cppm` | `CommandContext::drawHeapSlotBase` / `syncRingReserve` |

## 文档维护

1. 改动 `kAsyncSyncRingReserve` / `kAsyncWorkerSlotsPerSection` / `kAsyncFrameSlots` 时，同步更新 §1 表与 §4.1 的布局图，并核对 `d3d12_device.cpp` 的 `kDrawHeapPerFrame = kAsyncSyncRingReserve + kAsyncWorkerSlotsPerSection` 仍成立。
2. 改动 bundle 槽位轮转（`kBundleFrameSlots` / `kBundleBatchesPerFrame`）时，同步更新 §5.2 的公式与 §10 的「批次数用尽」缺陷条目。
3. 新增 `abi_async.cpp` 导出时，更新 §9 的分族表与函数计数（当前 41），并核对 `mcdr_abi.h` 的声明与 `MCDR_Layout.kt` 的签名登记。
4. 把 fence 管理器或执行器接进 JVM 后，删除 §10 对应的「未接入」条目，并在 §7.4 补上帧号统一的实际做法。
5. 修改 fence 回调语义（覆盖、超时、shutdown 顺序）时，同步更新 §6.3 / §6.4 与 §10 的泄漏条目。
