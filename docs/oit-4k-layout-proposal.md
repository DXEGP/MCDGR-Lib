# 4K OIT 存储方案

状态：已在 ABI20 / shader ABI4 实施。保持当前材质 RGB 历史算法、全部 FP32 参与量、262144 records、每像素 32 条上限与 512 MiB 共享预算。通过删除 GPU 从不读取的冗余字段、压缩 pixel 状态解决正常四 Frame 存活预算，不降低 records 容量，不改变 pending/Present 语义，不插入等待。当前契约见 [OIT records](oit-records.md)，本页保留选择依据和预算计算。

## Record 192 B → 144 B

Resolve 当前不读取 finalQ（16 B）、fog RGB（12 B）、scope serial（8 B）、materialAlpha（4 B）、accumAlpha（4 B）与 response（4 B），合计 48 B。Append 仍应接受这些输入并保留原 finite 校验，但不必存入 GPU record。当前 accum 是 Resolve 的独立真实输入，修正公式只需要 material RGB、当前 transport、fogFactor、chunkVisibility、sampleAlpha；不会从历史复用被删除量。

其余值按 FP32 原样保存，不用 FP16，也不改变标识、几何/材质 footprint、深度梯度或历史权重。已实施布局：

| 偏移 | 内容 | 大小 |
| ---: | --- | ---: |
| 0 | previousIndex / pixel / primitiveIndex / sourceIndex | 16 |
| 16 | current material RGB / transmittance | 16 |
| 32 | depth / coverage / validity / geometry expected previous depth | 16 |
| 48 | material UV / material dUVdx | 16 |
| 64 | material dUVdy / geometry previous UV | 16 |
| 80 | material previous UV / material expected previous depth / fogFactor | 16 |
| 96 | chunkVisibility / sampleAlpha / depthDx / depthDy | 16 |
| 112 | stable material RGB / history length | 16 |
| 128 | material previous dUVdx / dUVdy | 16 |

## Pixel 16 B → 8 B

当前 pixel state 的 uint4 为 linked-list head、count、flags、compact offset。compact offset 仅写入，任何后续 shader 都不读取，可删除。head 保留完整 uint32。count 与 flags 打包到第二个 uint32：count 使用低 8 位（实际上限 32），flags 使用其余位。Append 的 count CAS 必须比较并更新完整 packed word，保留并发写入的 flags；flags InterlockedOr 写入移位后的掩码，任何 CAS 失败重新取 observed word。compact/resolve 统一解包为旧逻辑的 count/flags，不在并发历史查询中改写 compact flags。

新的 compact 可保持 allocator 的索引表以减少改动；后续如移除它，必须独立核对其他消费者。本次预算计算仍包含原 allocator，避免把未实施收益算进去。

## 预算

元数据容量、CPU 预留、allocator、descriptor 页、program 保持原预算。64 KiB 对齐后的四个完整 Frame 加 4 MiB program：

| Render 尺寸 | 单 Frame | 4 Frame + program | 剩余页额度 |
| --- | ---: | ---: | ---: |
| 854×480 | 60.9375 MiB | 247.75 MiB | 264.25 MiB |
| 1920×1080 | 73.625 MiB | 298.5 MiB | 213.5 MiB |
| 2560×1440 | 85.875 MiB | 347.5 MiB | 164.5 MiB |
| 3840×2160 | 121.0625 MiB | 488.25 MiB | 23.75 MiB |

4K 余量可容纳共 380 个 64 KiB draw 常量页。pending 票据无限延迟仍会达到固定预算，继续明确 overBudget；不能承诺无上限 pending。

## 实施范围与验证

已同步 native capture/resolve shader headers、record/pixel stride 与 static_assert、JVM `OitShaderPreludeV1.SUPPORTED_SHADER_ABI`、独立真实 shader GPU 检查中的记录构造与像素布局。shader ABI 为 4、contractHash 自动更新；同批顶层 ABI 为 20，使旧 JVM/DLL 明确拒绝混装。shader CPU 输入结构和业务 Append 参数不变。

独立 `src/probes/oit_history` 已通过原七项 RGB history 检查，并新增实际 Initialize/Append 并发写入检查：count 维持32、unknown与overflow标志共存、linked records完整、其他像素清零。日志为 JVM 工作区 `build/reports/workgraph/oit-history/20261115-104537/gpu.log`；没有生产自检或运行游戏采集。native Release/x64 已通过0警告0错误；配套打包及用户动态确认见 JVM 的 Work Graph 实施记录。
