# OIT tile 原生归一化

当前实现是可调用的计算执行器：创建 root signature 和两个真实 PSO，分配归一化资源，按四个 lane 录制 dispatch，再录制均值计算。它尚未接入正常帧循环。原来的 depth、cull、TRANSMITTANCE、ACCUMULATE 与 InitializeLane 执行器仍然缺失；execution stream 保持 `MissingExecutionAdapters`，旧 tile V2/V3 保持 unavailable，旧清屏规则保持原样。

## 接口和所有权

唯一结构定义在 `include/mcdr_oit_normalization.h`，由 `include/mcdr_abi.h` 公开。七个计算执行入口使用 V1；program 输入真实 lane/average DXIL、targetCount（1/2/4）和 programGeneration，DXIL 来自 Illusion 的 `tile_normalization.hlsl`，root 对应 b0、t0..5、u0..1。结构大小依次为 program=56、region=16、batch=344、lane=96、info=96 字节。这七个执行入口尚未接入 JVM FFM，对应结构尚无 JVM 布局镜像。

主 Composite 图形管线另通过 `CreateOitNormalizationPipelineV1` 创建，136 B 的认证描述已有 JVM FFM 与双向布局检查，配套 ABI 为 23。认证包含版本、targetCount、programGeneration 和三组 SHA-256；JVM 检查原源码及编译资源，native 检查实际 descriptor，并在同次 PSO 创建中保存不可变 owner。V2 normalization 事件必须逐项匹配该 PSO 的认证。普通管线没有认证 owner，不能消费该事件；device 不解释来源规则。此绑定不补齐四 lane stage executor，现有缺项继续保留。

program 由调用者持有 registry token，绑定创建线程、context 指针、稳定的 context fence COM 身份和设备。batch 另绑定 recordingIdentity、frameSerial、帧 identity、historyGeneration 和 programGeneration。destroy 只移除调用者 token；录制阶段的 batch/program/资源由实际 command-context recording lease 持有，等对应 slot fence 成功后退休。调用者必须关闭所有 program 和 batch token，避免 registry 长期持有资源。

同一 program 的一次 recording 只分配一份 `68×68×64` RGBA32F normalized array 和一份完整输出尺寸的 RGBA32F corrected texture。它们属于帧；batch 只新增不可变的五份 512 字节常量、40 个 descriptor 和输入租约。帧分配、每个 batch 的 upload/descriptor 分配和 program 使用已有 OIT 共享预算，预算硬上限仍是 512 MiB。bounds、Q、coeff、depth 沿用已有 owner；这里不会重复分配它们。

输出以私有 ComPtr 持有，调度声明使用 `object=null`、raw resource 和已知 COMMON 初态。它们不伪装成通用 Dx12Object，不对外暴露可被通用销毁函数误处理的句柄。输入 Q/coeff 则持真实对象租约。

## 录制顺序

每个 batch 包含 1..16 个不重叠区域，区域尺寸不超过 64×64，原点为偶数且位于输出范围。帧内后续 batch 也不得覆盖先前区域。一个 batch 必须完成 lane 0、1、2、3、average 才能创建下一个 batch；这样四 lane scratch 在 GPU 命令流中串行复用，corrected 中已写区域不会被清除。

lane 的 Q 与 targetCount 个 coeff 都是 68×68、单 mip、单采样的 RGBA16F 或 RGBA32F array，binding 的 slice 数等于 residentCount。每个 lane 使用独立常量和 descriptor 区，避免录制后改写旧 descriptor。Normalize dispatch 为 `(9,9,residentCount)`，输出 slice 为 `lane*residentCount+tileSlot`；average dispatch 为 `(8,8,residentCount)`，从 guard+2 取四 lane 均值，只写已声明区域。

lane 将 normalized 全 array 声明为 UAV；average 将其转为 NON_PIXEL_SHADER_RESOURCE，并把 corrected 声明为 UAV。两类 dispatch 后都有真实 UAV barrier，录制结束恢复 draw heap 与图形绑定缓存。各 PSO 不使用的 normalized SRV/UAV 描述符填 null，避免无用的读写别名。

## 结果与失败

lane 和 average 成功返回 `RECORDED_UNVERIFIED=2`。query 只报告录制进度；availability 为 0，publicationToken 为 0，输出 bindings 为空，unavailableReason 为 STAGE_EXECUTION。它不会发布给 normalized composite。corrected 未写区域仍未初始化；完整 stage 执行器必须拥有完整事件完成事实、有效区域与最终发布，低层 dispatch 不能制造这些事实。

参数错误返回 INVALID_REQUEST；身份失效返回 STALE；batch 顺序占用返回 BUSY；预算或 E_OUTOFMEMORY 返回 BUDGET_EXCEEDED。资源创建保留操作名和 HRESULT 诊断，设备移除/重置返回 SUBMISSION_UNKNOWN 并阻断提交，明确能力缺失返回 UNSUPPORTED，其余后端错误返回 BACKEND_FAILURE。

开始 GPU 录制后的任何失败同时污染 batch、共享帧和 context 的 recordingFailed 状态，返回 RECORDING_FAILED；通用 submit 拒绝该列表。已知录制失败与提交结果未知分别保存，录制失败的 context 仍可结束、丢弃和释放，不能重试同一 batch。只有设备移除或实际提交结果未知才设置 submissionFailed。
