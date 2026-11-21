# OIT material RGB history

当前 native 实现只提供有界 fragment records、不可变 producer/primitive 语义、GPU compact、逐 surface 材质 RGB 历史与 Resolve V3。生产主循环的启用由 JVM 帧编排负责；本模块不会自行替换原透明合成，alpha/coverage AA 与 FG 分层尚未实现。

`include/mcdr_abi.h` 是 CPU ABI、producer kind、结果码与状态位的唯一来源。Resolve 使用 schema 3，terrain Begin/metadata/draw 使用 schema 2。`include/mcdr_oit_shader.h` 定义 shader ABI 5、144 B record、8 B pixel 存储与 shader contract hash。调用内复制 metadata，JVM Arena 指针只在调用期间有效。

`include/mcdr_oit_execution.h` 只服务 `d3d12_oit_records.cpp` 内部的 Frame/stream 值结构，不再由 `mcdr_abi.h` 公开，也没有对应的 JVM 布局镜像。

世界输出使用 `mcdr_world_output_input` schema 3、360 B，尾部追加 `sourceStage` 与零值 `reservedStage`。`COMPLETE_WORLD` 在 WorldReady 后运行整幅世界重建；`OPAQUE_BEFORE_COMPOSITE` 在 Recording 内对不透明层运行曝光、可选 provider mask、重建与场景转存，随后保持 OpaquePost，允许继续录制云、透明物体、粒子、天气及 OIT Resolve。它不要求透明捕获已 seal。完整世界合成完成后，调用 WorldReady 将 OpaquePost 转为 WorldPost，再进入 GUI；该调用检查不透明输出已完成，禁止第二次重建。首次无前帧时仍按原规则复制当前不透明层，透明合成的顺序保持一致。普通 draw、terrain WG 与 OIT 继续使用同一冻结资源版本和 declared-pass 调度。

Vanilla frame 保存实际 D32 深度纹理和 clear/write 计数。`mcdr_dx12VanillaRegisterWorldDepthV1` 在世界帧开始后绑定纹理、录制身份和 fence，登记完成后才进入世界 clear 和 draw。登记拒绝迟到、换目标和失效录制。未启用 opaque seed consumer 时只保留帧级租约和计数。

```mermaid
flowchart LR
    Programs[Prepare / CommitFramePrograms] --> Begin[BeginFrame]
    Begin --> Metadata[PublishMetadata / BindDraw]
    Metadata --> Draw[IA / mesh / WG 实际 draw]
    Draw --> Seal[UnbindDraw / SealFrame]
    Seal --> Resolve[Resolve V3: compact + RGB history]
    Resolve --> Pending[pending realFrameId]
    Pending --> Queue[queuePresented: final fence]
    Queue --> Present{Present S_OK}
    Present -->|接受| Previous[commitPresented: presented history]
    Present -->|取消 / 失败| Discard[discardResolved]
    Previous --> Resolve
```

capture PSO 使用独立工厂，根参数追加在既有 push constants 之后。space2 的 b0 为 208 B 常量；11 个 SRV 中 t0/t1/t2 保存 RAW source/primitive/previous 与原 section 索引，t9/t10 保存 current/previous terrain section；u0/u1/u2 是 RAW records、pixel 状态与 allocator。每次 BindDraw 只允许一次实际 native draw，WG 的一次 ExecuteIndirect 可覆盖整批 mesh。capture 前保存并解绑 SRI，强制 1×1 与 PASSTHROUGH VRS，退出恢复原绑定。native 反射要求 early depth、pixel frequency、仅颜色输出、匹配常量与三个 RAW UAV；PSO 固定单 sample 且禁止 depth write。设备必须支持 `SV_Barycentrics`。

记录追加对每个片元只做一次全帧分配器原子：`AppendV1` 用 `McdrOitAllocatorV1.InterlockedAdd(0u, 1u, index)` 取索引，禁止改回 compare-exchange 重试循环——它会把全帧所有透明片元串行化到同一条 cache line 上。索引只决定落点，顺序由逐像素 `InterlockedExchange` 的链表与 `previous` 链决定。

每帧拥有独立的 draw 常量页，每页是 64 KiB、持续映射的 UPLOAD buffer，预算按页计费。BindDraw 依次使用 256 B 对齐的独占 slice，写入 208 B capture 常量，通过页 GPU VA 加偏移绑定 CBV；每页容纳 256 次 draw，不覆盖已录制 slice。initialize/resolve 使用原独立 constants buffer，不与 draw 页混用。Frame 通过 `unique_ptr` 容器保持页地址稳定；DrawLease 只持有 Frame，由当前 command-list 槽的 recording lease 保留到对应 fence 完成，Frame 不反持有 DrawLease。presented/pending/history 引用可能延长 Frame 与页的寿命，全部受同一 512 MiB 预算约束。页分配预算不足返回 overBudget，不插入等待，也不改变 records 配额。

144 B 私有 records 保存 chunk 混合前 material RGB、当前 sampleAlpha、transmittance、depth/梯度、UV/梯度、geometry/material 两套 previous UV/depth、coverage/validity、fog factor、chunkVisibility、material previous UV 的屏幕梯度，以及稳定 RGB/history length。原 finalQ、materialAlpha、accumAlpha、response、fog RGB 的 finite 校验保留，只取消从未读取的 GPU 存储；scope serial 继续参与 CPU 绑定校验。CPU previous triangle 仍为 80 B。记录里的 previous 指本帧提交的前帧三角几何；HistoryState 的 presented 才是已显示的前帧 GPU records。

8 B pixel 状态保存完整 uint32 head 与打包 count/status：count 占低 8 位，实际每像素上限保持 32。Append 对逐像素计数做 CAS，失败后用 observed 重试，保留并发 OR 的状态位；compact 在 capture 结束后写回认证状态。allocator 的索引表保持原样。

metadata 行容量不足使用专用结果 `MCDR_OIT_RESULT_METADATA_CAPACITY_EXCEEDED`（-7），在完整输入校验之后、任何旁表变更之前返回；调用方可在 capture PSO 绑定前改用真实未记录覆盖。资源分配或共享预算不足继续返回 -3，中止当前帧，不能作为未记录 draw 继续执行。BeginV2 为有界 terrain section buffer 申请额度，draw 常量页继续按需计费，所有 reservation 共用 512 MiB 上限；更多在途 Frame、其他 context 或重叠 program 都消耗该额度。

terrain 用 80 B GPU section 表代替逐三角形 previous：保存 surface/material generation、逻辑 primitive 范围、真实 VBO 顶点范围和 previous 相对原点。metadata 同步复制 CPU section 行并保留真实 VBO 租约；实际 terrain draw 核对 VBO/偏移后消费绑定。MS 从同代静态顶点计算 previous clip，材质 previous 复用同一 UV；几何 generation 变化清 geometry/material previous，材质或 texture generation 变化只清 material previous。`identity.z` 最高位标识 terrain，其余 31 位为帧内逻辑 primitive，通过有序 prefix 二分定位 section，且同时核对 source kind。

Resolve V3 每像素遍历自己 records，按当前帧 alpha、transmittance、fog、chunkVisibility 与稳定材质 RGB 重组，输出原版 Q 量纲的 corrected transparency。原版透明合成仍由唯一 Composite 执行，records 不按 producer 分类做 source-over。

## 帧预算

默认记录配置为 records=262144、perPixel=32、source=4096、primitive=65536、previous=65536。64 KiB 对齐后的四个完整 Frame 加 4 MiB program 预算：

| 分辨率 | 单 Frame | 4 Frame + program | 剩余页额度 |
| --- | ---: | ---: | ---: |
| 854×480 | 60.9375 MiB | 247.75 MiB | 264.25 MiB |
| 1920×1080 | 73.625 MiB | 298.5 MiB | 213.5 MiB |
| 2560×1440 | 85.875 MiB | 347.5 MiB | 164.5 MiB |
| 3840×2160 | 121.0625 MiB | 488.25 MiB | 23.75 MiB |

4K 余量可容纳共 380 个 64 KiB draw 常量页。pending 长期未提交仍会达到固定预算并返回 overBudget，不能承诺无上限 pending。

## 已移除的 tile 执行设施

正常帧只走「per-draw records + Resolve V3」。以下设施已从实现中删除，不再有导出，也不再有 JVM 绑定或布局镜像：

- 执行流：`OitBegin/Append/Query/Seal/AbortExecutionStream*`（V1 与 V2）、`OitBegin/Finish/Cancel/QueryExecutionOperation*`、`OitAppendExecutionOperationEventV1`、`OitCaptureExecutionTextureV1`。
- tile 重放：`OitPrepareTerrainTilesV2/V3`、`OitExecuteTileStreamV2`、lane depth/cull 录制与 tile binning 相关实现。
- terrain stage 与读取范围：`OitBegin/Finish/AbortTerrainStageDrawV2`、`OitQueryTerrainStageReceiptV2`、`OitRegister/CancelTerrainReplayReadSetV2`、`OitQueryTerrainDrawReceiptV1`、`OitQueryTerrainDrawFrontierV1`。
- 归一化：整个 `d3d12_oit_normalization.cpp` 与 `include/mcdr_oit_normalization.h`、`OitCreate/Record/Query/DestroyNormalization*`、`CreateOitNormalizationPipelineV1`，以及 `PrepareFrameProgramsV2` 的 normalization 形参。
- `OitResolveRecordsV4`：校验后恒返回 unavailable，没有消费者。

仍编译但正常帧不可达的残留面：`mcdr_dx12CreateOitLaneOperationPipelineV1`、`CreateOpaqueSeedPipelineV1`、`CreateOitTileIAPipelineV1`、`AttachOitTileIARouteV1`、`BindTerrainOpaqueSeedV1`、`d3d12_device.cpp` 里 `desc.opaqueSeed.programGeneration` 的 depth-only PSO 分支、`VanillaArm/SealWorldDepthSeedV1` 与 graphics command observer 机制。它们与共享的图形管线创建器、声明式 pass 记录和 graphics capture 路径交织，删净需要重写实时地形绘制分支；当前没有调用方，运行时零成本。
