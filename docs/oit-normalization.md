# OIT tile 原生归一化

四路 OIT execution stream 在每组 tile 的 lane 重放后，依次录制四次 normalization dispatch 和一次均值计算。完整输出由 execution stream 复制到 corrected transparency。归一化执行器为 lane 和 average 创建 compute PSO，共用一份 root signature。

## 接口和所有权

唯一结构定义在 `include/mcdr_oit_normalization.h`，由 `include/mcdr_abi.h` 公开。七个计算执行入口使用 V1；program 输入真实 lane/average DXIL、targetCount（1/2/4）和 programGeneration，DXIL 来自 Illusion 的 `tile_normalization.hlsl`，root 对应 b0、t0..5、u0..1。结构大小依次为 program=56、region=16、batch=344、lane=96、info=96 字节。这七个执行入口尚未接入 JVM FFM，对应结构尚无 JVM 布局镜像。

主 Composite 图形管线另通过 `CreateOitNormalizationPipelineV1` 创建，136 字节的认证描述由 JVM FFM 与双向布局检查覆盖。认证包含版本、targetCount、programGeneration 和三组 SHA-256。JVM 检查原源码及编译资源，native 检查实际 descriptor，并在同次 PSO 创建中保存不可变 owner。V2 normalization 事件必须逐项匹配该 PSO 的认证。

program 由调用者持有 registry token，绑定创建线程、context 指针、稳定的 context fence COM 身份和设备。batch 另绑定 recordingIdentity、frameSerial、帧 identity、historyGeneration 和 programGeneration。destroy 只移除调用者 token；录制阶段的 batch/program/资源由实际 command-context recording lease 持有，等对应 slot fence 成功后退休。调用者必须关闭所有 program 和 batch token，避免 registry 长期持有资源。

同一 program 的一次 recording 分配一份 `68×68×64` RGBA32F normalized array 和一份完整输出尺寸的 RGBA32F corrected texture。每组从帧的 64 KiB 常驻映射 upload 页取五份 512 字节常量空间。帧还按需创建每页 512 格的 shader-visible descriptor heap。创建 batch 时提前留出五张表的空间，录制时只为新视图写入一张 8 格的表。常量、描述符页和输入租约由 recording lease 保留到对应 slot fence 完成。两类页按页计入 OIT 共享预算，硬上限为 512 MiB。bounds、Q、coeff 和 depth 沿用已有 owner。

输出以私有 ComPtr 持有，调度声明使用 `object=null`、raw resource 和已知 COMMON 初态。它们不伪装成通用 Dx12Object，不对外暴露可被通用销毁函数误处理的句柄。输入 Q/coeff 则持真实对象租约。

## 录制顺序

每个 batch 包含 1..16 个不重叠区域，区域尺寸不超过 64×64，原点为偶数且位于输出范围。帧内后续 batch 也不得覆盖先前区域。一个 batch 必须完成 lane 0、1、2、3、average 才能创建下一个 batch；这样四 lane scratch 在 GPU 命令流中串行复用，corrected 中已写区域不会被清除。

lane 的 Q 与 targetCount 个 coeff 都是 68×68、单 mip、单采样的 RGBA16F 或 RGBA32F array，binding 的 slice 数等于 residentCount。每次 dispatch 使用独立常量。同一 recording 内，最近使用的表按 Q/coeff 的资源、格式、起始 slice 和 slice 数匹配，lane 与 average 各按自己的 SRV/UAV 布局匹配。已录制的描述符保持到 fence 完成，新表追加到空闲位置。Normalize dispatch 为 `(9,9,residentCount)`，输出 slice 为 `lane*residentCount+tileSlot`。Average dispatch 为 `(8,8,residentCount)`，从 guard+2 取四 lane 均值，只写已声明区域。

lane 将 normalized 全 array 声明为 UAV；average 将其转为 NON_PIXEL_SHADER_RESOURCE，并把 corrected 声明为 UAV。两类 dispatch 后都有真实 UAV barrier，录制结束恢复 draw heap 与图形绑定缓存。各 PSO 不使用的 normalized SRV/UAV 描述符填 null，避免无用的读写别名。

## 结果与失败

lane 和 average 成功返回 `RECORDED_UNVERIFIED=2`。query 只报告录制进度；availability 为 0，publicationToken 为 0，输出 bindings 为空，unavailableReason 为 STAGE_EXECUTION。它不会发布给 normalized composite。corrected 未写区域仍未初始化；完整 stage 执行器必须拥有完整事件完成事实、有效区域与最终发布，低层 dispatch 不能制造这些事实。

参数错误返回 INVALID_REQUEST；身份失效返回 STALE；batch 顺序占用返回 BUSY；预算或 E_OUTOFMEMORY 返回 BUDGET_EXCEEDED。资源创建保留操作名和 HRESULT 诊断，设备移除/重置返回 SUBMISSION_UNKNOWN 并阻断提交，明确能力缺失返回 UNSUPPORTED，其余后端错误返回 BACKEND_FAILURE。

开始 GPU 录制后的任何失败同时污染 batch、共享帧和 context 的 recordingFailed 状态，返回 RECORDING_FAILED；通用 submit 拒绝该列表。已知录制失败与提交结果未知分别保存，录制失败的 context 仍可结束、丢弃和释放，不能重试同一 batch。只有设备移除或实际提交结果未知才设置 submissionFailed。
