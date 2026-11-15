# native RT 帧资源与重建

生产入口 `mcdr_dx12RtFrameTransactionLighting` 在调用方已打开的主命令列表上，一次录制空间证明、内容事件、DXR 或实验 WG 追踪、材质 guide、曝光、motion、信号编码、NRD/RR、可选 SR 与最终合光。
上下文拥有追踪状态对象、DXR SBT 或 WG backing、八个 compute PSO、6×1 RGBA16F 环境缓存、五张 raw 缓冲、五张降噪 guide、四张信号纹理、当前帧常量、空间证明与追踪记录表、曝光纹理/适应状态。
native 在一次帧锁内完成各阶段；成功后才返回整组借用输出。历史仍须等待所属录制的提交确认。
`mcdr_dx12RtFrame`、`mcdr_dx12RtFrameDenoise`、`mcdr_dx12RtFrameUpscale` 与 `mcdr_dx12RtFrameComposite` 保留为细粒度接口，工程探针使用它们验证拒绝与恢复，JVM 生产链不再逐阶段调用。
原版成功绘制的地形批次立即交给 `mcdr_dx12RtCaptureGBuffer` 捕获；G-Buffer 由 `mcdr_dx12RtFrameBeginGBuffer` 分配并清屏，
`mcdr_dx12RtFrameReplayCapturedGBuffer` 消费捕获令牌，最后由 `mcdr_dx12RtFrameEndGBuffer` 确认结果。
JVM 通过 `mcdr_dx12CreateRtGeometryDelta` 提交完整记录清单与变化的 CPU 内容；native 复用 arena 范围并保有不可变 GPU 快照。
native AS 上下文引用几何快照，拥有 BLAS/TLAS、scratch、TLAS view 与不可变实例表。
追踪提交几何与 AS 令牌、版本、G-Buffer 重放编号、416 B `WgaFrame`、时间戳和曝光设置，成功后借用 native 输出。

`mcdr_dx12PrepareRtSceneTargets` 管理候选续建、按 section 发布和局部失效；调用方提交选中几何快照、当前层版本表和独立所需 node 清单。
这是部分帧所有权：CPU 几何池与范围选择、G-Buffer 着色器生成与捕获通知、调试绘制与上屏 blit 仍由 JVM 组织。
帧录制不提交列表、不 Present、不回读；SDK 上下文切换可能等待此前提交的使用退休。
调用方负责列表提交、呈现和同队列消费顺序。

## 1. 创建与释放

ABI 版本为 47。`mcdr_dx12CreateRtFrame(const mcdr_rt_frame_program*)` 创建 DXR 上下文，
`mcdr_dx12CreateRtWorkGraphFrame(const mcdr_rt_frame_program*)` 创建实验 WG 上下文。
两者返回 `uint64_t` 上下文编号，0 表示失败；该编号不是 `mcdr_object*`，不得传给通用资源销毁入口。

`mcdr_rt_frame_program` 为 144 B：九个 DXIL 指针及各自的 `int64_t` 字节数，仅在创建期间读取；末尾为 environmentDxil/environmentBytes。
DXR 射线库须提供 `RtRayGen`、`RtMiss`、`RtClosestHit`、`RtInvalidHit`、`RtInvalidIntersection`；WG 的 `rayDxil` 槽提供 `lib_6_9` 节点库。
两者的 compute 入口为 `RtEnvironment`、`RtMaterialGuide`、`RtExposure`、`RtMotion`、`RtNrdPack`、`RtNrdRadiance`、`RtNrdRemodulate` 与 `RtResolve`。
追踪根签名固定 7 SRV / 5 UAV，t6 为共享环境；环境为 0/1，材质 guide 为 2/5，曝光为 8/1，motion 为 1/1，NRD pack 为 4/4，信号编码为 12/4，SR 重调制为 13/2，合光为 14/1；共用 b0。
追踪与 guide 的 b1 分别传入 0 和 depthScale 位表示，曝光 b1 传入模式、EV、适应权重、reset 与 source 可用标记。

积分器对当前主材质的零 diffuse 响应省略直射 diffuse 阴影和 diffuse bounce 查询。
分别检查全部 albedo 通道是否为零以及饱和 metalness 是否为 1，不用乘积或能量阈值判断。
主命中、guides、specular、自发光和曝光归属继续使用当前帧；零 diffuse 可作为合法输入交给 NRD/RR 准备阶段。
未查询的局部 relief 可见性不证明外部无遮挡。真实非零 lobe 的 unknown 保留原查询归属与缺口位，
bit 16/17/18 分别记录 direct、diffuse、specular 的显示近似，查询低位保持原值。
次级缺口不清可靠主命中或其它当前信号；次级阴影未知保留已知 emission 与环境贡献。
仅 primary unknown 清除主表面输出。显示估计不授予物理样本、训练或 reservoir 资格，SDK 输入与合成见 §5–§8。

DXR 命中组按 `HITOPAQUE`、`HITCUTOUT`、`HITINVALID` 排序，对应 TLAS 贡献下标 0、1、2。
SBT 记录步长 64 B，raygen / miss / hit 起点为 0 / 64 / 128，合计 320 B。
前两个组使用 `RtClosestHit`，失效代理使用 `RtInvalidHit` 与 intersection 导出 `RtInvalidIntersection`；不得以 closest-hit 名替代命中组标识符。
失效组为程序化命中组；消费方必须处理程序化候选，不能启用 `SKIP_PROCEDURAL_PRIMITIVES`。

WG 不分配 SBT。native 查询 `OPTIONS21.WorkGraphsTier`，创建名为 `RtGraph` 的 `EXECUTABLE` 状态对象，
验证非零 program identifier、非空节点、唯一入口 `RtWgPrimary` 及 12 B 入口大小。
`RtWgPrimary` 为 8×8 broadcasting，生成 8 B 像素任务；`RtWgTrace` 准备主表面和直射，
写 guide、初始 raw 与主表面/直射路径支持，将独立次级任务交给 thread 节点 `RtWgRecursive`。
递归记录为 108 B，最大深度 3；每次执行一个 diffuse/specular 查询，保存对应 raw 和路径支持，
累计有效性后继续剩余任务。两个 lobe 保持独立，不把记录延续当作新增物理弹跳。
零响应、拒绝的方向和主查询 unknown 跳过相应任务，未完成任务保留 unknown。
`RtWgResolve` 以 64 线程 coalescing 消费 12 B 像素/有效性记录，写最终逐信号 mask。
thread 节点输出声明总量为 120 B，不得超过 128 B；节点间 UAV 访问使用 device-scope barrier。
SDK query/training 图记录、更深输运和材质分流尚未实现，辐射缓存为 `none`。

backing 按 `GetWorkGraphMemoryRequirements().MaxSizeInBytes` 向 `SizeGranularityInBytes` 对齐后分配为 DEFAULT UAV，
归入 frame GPU 受管总账和实际 fence 退休，输出 `byteCount` 包含其逻辑字节数。
录制通过 `ID3D12GraphicsCommandList10.SetProgram` / `DispatchGraph(NODE_CPU_INPUT)` 完成，
CPU 记录为 `ceil(width/8), ceil(height/8), 1`，每轴最大 2048 组。
首次使用带 `INITIALIZE`；其录制被取消或对当前 encoder 不可消费时重新初始化，不能把已取消初始化当成 GPU 状态。
backing 和输入/输出在录制后归还 `COMMON`，并恢复 draw heap；帧事务、内容事件、历史与 NRD/SR 的提交确认不另建分支。
WG 工程检查不构成世界画面、性能或完整 S5 验收。用户侧选择、失败回退和默认策略由 JVM 负责。

`mcdr_dx12ReleaseRtFrame` 返回 1 表示释放成功，0 表示未知编号或 G-Buffer 重放仍打开；释放 0 是空操作。
释放立即撤销编号，资源通过设备的延迟销毁队列退休；已经录制的命令可执行至所属 fence 完成。
设备销毁时清空注册表。换包时释放上下文，再以新包 DXIL 创建。

### 1.1 不可变几何快照

生产入口 `mcdr_dx12CreateRtGeometryDelta(encoder, desc, output)` 接受 112 B 描述，返回 72 B 输出。
描述中的 `records` 是完整载荷记录表，每条 64 B；`contentCount` 与记录数相等，最多 65535，`reserved` 必须为 0。
每条记录对应一份 72 B `mcdr_rt_geometry_content`，分别指定 vertex/index/quad 的非零内容 identity、长度与可选数据指针。
内容 identity 由 JVM 不可变 allocation 分配，不使用可回收 CPU offset 代替身份。
复用 base snapshot 中未变内容时只交 identity 与长度；首次内容必须给出字节数据。
native 校验 28 B 顶点步长、u16/u32 索引和逐 quad u16 长度，并把记录偏移改写为 GPU arena 地址。
顶点地址按 28 B 对齐，索引地址与每段复制长度按 4 B 对齐；quad 偏移仍以 u16 元素为单位。

`baseSnapshot` 必须仍注册、同 worldEpoch 且生产者可消费；首次有载荷的请求为 0，并按描述的三份 capacity 建立新 arena 代。
生产容量由 JVM 固定为顶点 256 MiB、索引 64 MiB、quad 8 MiB；有载荷的 base 复用已有容量，不就地扩容或搬迁。
快照独立持有 record table、内容范围和材质代，五个 SRV 的顺序不变。
每次只上传新增内容、完整小表，以及没有复用的完整材质 blob；一次请求的上传合并为一块 staging。
`reuseMaterials` 仍须匹配 worldEpoch/materialRevision，且复用时材质指针为空、长度为 0。
材质条目追加会移动 PBR maps 起点，禁止只上传尾部。

零载荷请求将 `contentCount`、`recordBytes` 设为 0，`records`、`contents` 指针均为空；其它组合拒绝。
三份 capacity 仍须通过正常布局校验，材质仍须完整提供或合法复用。
返回非零 snapshotId、空 CPU 记录表和五个合法借用缓冲，不创建顶点、索引或三角形记录。
没有 base 时，record/vertex/index/quad 各只请求 16 B 的 raw 缓冲容量；实际 committed 分配继续由驱动查询计费。
有 base 时可保留已有 arena；后续载荷从最小空 arena 恢复时建立请求容量的新 arena 代，旧消费者继续持有旧资源。
空表不产生复制；材质也复用时不分配 staging，`uploadedBytes`、`uploadCapacity` 均为 0。
零记录快照的取消、跨 encoder 消费、注册与 fence 退休规则与非空快照相同。
使用该快照查询 scene 时必须提供至少一个已观测所需目标；无目标时以 snapshotId=0 撤销场景，不能据空记录表证明空空间。

成功返回 1，输出前 56 B 与 `mcdr_rt_geometry_output` 相同，另有 `uploadedBytes` 和 `uploadCapacity`。
前者是本次实际新增内容、小表与材质的有效字节之和，后者是 staging 的逻辑容量，包含逐段 4 B padding。
`byteCount` 仍为本快照五份借用缓冲的逻辑容量之和，不能跨快照相加当物理内存。
`mcdr_dx12GetRtGeometryMemory` 返回去重的 `gpuBytes` 与 `uploadBytes`，按 `GetResourceAllocationInfo` 计入 committed 对齐。
计费租约绑定实际 `Dx12Object`，只有底层资源释放后才退账；撤销注册、取消列表、换世界和 fence 完成均不直接退费。
没有快照强引用但仍在通用延迟删除队列中的资源继续计费，失败调用已创建的分配也不例外。
Snapshot 与 delta 入口共享上述 DEFAULT 与 UPLOAD 两类物理分配，各设 512 MiB 上限。
分配前检查新增容量，每份资源在调用驱动前取得租约；驱动创建失败只退还未物化的预留。
几何与 AS 共用同一种租约机制，除各自额度外还受 §1.7 的共享总上限约束；纯引用与 arena 复用不重复领取容量，换 worldEpoch 或 encoder 不重置共享账。
几何 buffer 使用独立精确容量分配，不借用或进入通用 buffer pool；延迟删除实际处理后额度才可再次使用。
通用 buffer 调用仍可复用更大的池内容量。AS、SDK 历史、CPU、描述符及驱动额外开销分别核算，以上不是整个 RT scene 或设备的总预算。

运行 `_port/rt_geometry_snapshot_check.py --physical-memory-only` 检查实际驱动分配、失败前缀、
共享去重、未注册 delta arena、阻塞 fence 下的延迟删除背压与物理回收后恢复准入。
检查期间按正常延迟删除时间片推进退休，不把弱引用消失或列表 fence 完成当作物理资源已经释放。

容量或预算不足返回 0、清零输出并报告 `arena capacity exhausted`、`memory budget exhausted` 或 `upload budget exhausted`。
录制复制命令之前完成全部校验、分配与暂存；失败范围可在下次请求回收，不发布半份清单。
内容范围随快照、AS 和实际 command-list slot 保活；撤销注册或结束 CPU 帧不允许提前复用在途范围。
取消的 producer 不能被无数据引用；提供新字节时建立另一个不可变范围，旧消费者仍保有旧范围。
未知、已删除、新内容待上传和 AS 待构建必须由目标清单与 live 版本协议区分，不能把背压伪装成删除。
当前所需层校验的覆盖边界见 §1.3，完整空间目标清单尚未完成。

`mcdr_dx12CreateRtGeometrySnapshot(encoder, desc, output)` 接受 120 B `mcdr_rt_geometry_desc`，返回 56 B `mcdr_rt_geometry_output`。
描述包含 worldEpoch、geometryRevision、materialRevision、reuseGeometry、reuseMaterials，以及 records/vertices/indices/materials/quads 五组 CPU 指针与字节数。
成功返回 1，输出为独立 snapshotId、五个借用缓冲句柄和 byteCount；失败返回 0 并清零输出。
必须在 pass 外的开放主列表上调用；上传直接录入该列表，不创建临时 encoder、不提交或等待。
源指针仅在调用期间读取；上传缓冲完整覆盖 payload，只将容量尾部补零，返回后允许调用方覆写或释放源数据。

每组新数据须非空，长度为 1 B–512 MiB。records 每条 64 B、最多 65535 条，vertices 每个 28 B，indices 与 quads 长度按 2 B 对齐。
materials 至少 40 B，长度按 4 B 对齐。每条记录要求顶点数为正且整除 4、索引数为正且整除 3，索引宽度仅 2/4 B；
顶点/索引偏移按对应步长对齐，记录引用的顶点、索引和逐 quad 材质编号范围不得越界，quad 数等于顶点数除以 4。
native 在调用期间复制 CPU 数据；返回后可修改或释放源内存。目标缓冲最少 16 B，向上补齐至 4 B，补齐区写零。
byteCount 为该快照引用的五个缓冲容量之和，不去重其他快照共享的资源，也不包含 staging 或驱动对齐开销。

reuseGeometry 复用 records/vertices/indices/quads 四个缓冲，reuseMaterials 独立复用 materials。
复用组的 CPU 指针必须全空、长度全零；令牌必须仍注册、worldEpoch 相同，且对应 geometryRevision/materialRevision 相同。
同 encoder 的后续录制可立即消费；跨 encoder 仅在生产者上传 fence 完成后接受，未提交或仍在途均拒绝。
每个缓冲绑定其实际上传的共享 `CommandRecording`；复用缓冲沿用原生产者，不绑定仅消费它的新列表。
列表重录、encoder 销毁或未提交取消后，即使 fence 数值相同也拒绝复用取消的生产者。
调用方收到 `recording cancelled` 后必须丢弃镜像/候选并重新上传，不能把普通 pending 或跨 encoder 未完成误判为取消。
即使完全复用，也创建独立令牌；各组底层资源共享所有权，释放原令牌不破坏其他有效快照。

`mcdr_dx12ReleaseRtGeometrySnapshot(snapshotId)` 立即撤销注册；未知、零或重复释放返回 0。
创建及每次追踪都将快照保留到实际 command-list slot 的 fence 完成，staging 同样由上传槽持有。
撤销令牌后不得再发起追踪或复用；已录制 GPU 命令仍有效。借用缓冲不得写入或单独销毁。
AS 上下文强引用几何快照；撤销几何令牌后，已有 AS 仍能完成构建，但不能使用已撤销的几何令牌发起新追踪。
释放 RT 帧上下文不撤销几何或 AS 注册，设备销毁统一清理。

### 1.2 native 加速结构

`mcdr_dx12CreateRtAcceleration(snapshotId, reuseAccelerationId)` 返回独立 `uint64_t` 令牌，失败返回 0。
几何令牌须仍注册；可选复用 AS 须仍注册且 worldEpoch 相同。每条几何以完整 `(uint64 node, uint32 layer)` 为键，重复键拒绝创建。
仅共享版本、顶点数、索引数和完整 flags 相同的已建 BLAS；记录重排与删除不重建未修改结果。
各上下文保留独立条目，BLAS 结果共享所有权；新建、修改或取消候选不得覆写仍被已发布场景引用的结果。

`mcdr_dx12PrepareRtAcceleration(encoder, accelerationId, output)` 只在 pass 外的开放主列表录制，不提交或等待。
每个 AS 上下文在同一 encoder 的同一列表上最多构建 16 条新 BLAS；同列表重复调用不补充预算，复用条目不计入预算。
返回 1 表示调用成功，仍须检查 output.ready；部分构建不发布 TLAS。全部 BLAS 就绪后创建 TLAS，后续静态调用复用 TLAS 与实例表。
几何、共享 BLAS 及已有 AS 的生产者未提交或仍在途时，只允许同 encoder 顺序消费；跨 encoder 须等对应 fence 完成。
取消的 producer ticket 会使 BLAS/TLAS、scene candidate、published/resident 结果失效并退回可重建状态；同队列已提交工作仍按实际完成 fence 保活。
新 AS 上下文拥有自己的预算，取消候选后创建新上下文不会继承旧上下文的预算。

`mcdr_dx12FilterRtAcceleration(encoder, accelerationId, invalidSections, count, output)` 接受完整且可消费的 AS。
失效集合最多 65535 项，非零 count 要求非空指针，重复 section 拒绝；native 在调用内复制集合。
保留未失效记录的原 InstanceID，移除失效 section 的旧实例，为每个失效 section 加入 16 m 立方体代理。
代理 ID 从原记录数起排，命中组下标为 2；所有实例 mask=1，关闭三角形背面剔除。
集合不变时复用 TLAS，与输入集合排列无关；变化时创建新 TLAS 与不可变 UPLOAD 实例表，录制成功才替换可用输出。
独立 AS 入口用于低层调用与自检。生产场景由 §1.3 推导失效集合及发布状态；scene 自有 AS 拒绝外部 Prepare/Filter/Release。

两个录制入口均返回 80 B `mcdr_rt_acceleration_output`，失败返回 0 并清零输出：

| 字段 | 含义 |
| --- | --- |
| `snapshotId` | 该 AS 强引用的几何快照编号 |
| `acceleration`、`instances` | 借用 TLAS view 与不可变实例表，不得写入或单独销毁 |
| `byteCount`、`scratchBytes` | 本上下文 AS/scratch/实例表的去重 committed 分配字节、BLAS scratch 的逻辑容量；不含几何和代理构建暂存 |
| `instanceCount`、`blasCount` | 当前 TLAS 实例数与已建几何 BLAS 数；后者不含失效代理 |
| `buildCount`、`ready` | 本次新建 BLAS 数与是否可用于追踪；复用或过滤时 buildCount=0 |
| `blasMicros`、`peakBuildMicros`、`tlasMicros` | CPU 录制时长，单位微秒；不是 GPU 执行时长 |

`mcdr_dx12RtAccelerationMemory(firstId, secondId)` 按底层对象去重计算两份上下文的 committed 分配字节；零令牌跳过，未知非零令牌使调用失败。
计费读取实际 `GetDesc()` 对应的 `GetResourceAllocationInfo().SizeInBytes`，首次查询后缓存在资源对象中，池内复用不改变该物理分配。
逻辑长度不能替代 committed 分配字节；小缓冲也须计入驱动要求的分配粒度。
独立 AS 与生产 scene 共用本节后述的 512 MiB 分配准入；16 条构建额度不提供额外内存额度。
`mcdr_dx12ReleaseRtAcceleration(accelerationId)` 立即撤销注册；未知、零或重复释放返回 0。
构建、过滤和追踪保留所用的具体 GPU 资源到实际 command-list slot fence 完成；替换实例表不会改变已录制命令读到的内容。
AS 结果、scratch、实例表、代理构建缓冲与 TLAS view 使用独立的完成资源释放出口。
每次录制都必须将实际使用的对象加入 `rtAccelerationRetained`；仅在全部 AS 持有者与录制槽引用消失后销毁对象。
未提交录制的取消由槽重录或 encoder 退休解除引用；已提交录制仍等待实际 fence。
最后一个引用释放时直接归还物理资源、描述符与分配额度，不等待其它 encoder 的开放列表。
令牌撤销后拒绝新的构建、过滤、复用与追踪；已录制 GPU 命令继续有效。生产路径不依靠 JVM 帧数延迟回收。

### 1.3 场景候选、发布与局部失效

`mcdr_dx12CreateRtScene()` 返回独立 scene 令牌；`mcdr_dx12ReleaseRtScene(sceneId)` 撤销 scene 及其候选/已发布 AS。
未知、零或重复释放返回 0；已录制命令的具体资源继续由列表槽保活。几何快照注册仍由调用方管理。

生产入口 `mcdr_dx12PrepareRtSceneTargets(encoder, sceneId, desc, targets, targetCount, output)` 接受 64 B `mcdr_rt_scene_desc`：

| 字段 | 约束 |
| --- | --- |
| `worldEpoch` | 当前 CPU 几何池的世界代际；变化时撤销两份旧 AS 和常驻 BLAS，不共享旧世界结果 |
| `snapshotId` | 当前选中候选几何；须仍注册、worldEpoch 匹配且上传可消费；0 表示无可用范围，清空场景 |
| `liveRevision` | 当前世界 section 版本表代际；同世界不得回退，相同代际必须提交相同内容与顺序 |
| `sections`、`sectionCount` | 最多 1048576 条 16 B 记录；非零 count 要求非空指针，重复 node/layer 键拒绝 |
| `reserved` | 必须为 0 |
| `frameId` | 偏移 40 的非零真实渲染帧编号；同世界不得回退，相同编号共享构建预算 |
| `outputs`、`outputCount` | 偏移 48、56 的当前输出支持清单；每条 48 B，与版本表同锁冻结并在调用内复制 |
| `outputMode` | 偏移 60；生产使用 1，表示完整当前输出清单；0 仅供低层与诊断，完整模式的 scene 不得降级 |

`mcdr_rt_section_version` 按 node:uint64、layer:uint32、version:uint32 排列。数组仅在调用内借用；native 按世界与 liveRevision 缓存不可变副本和查找表。
调用方在同一 CPU 锁内捕获 epoch/revision/记录，不得把新世界版本表与旧候选拼在一起。
`mcdr_rt_output_support` 包含 node:uint64、layer/version/flags/reserved:uint32，以及局部 minimum/maximum XYZ 六个 float。
flags=1 表示可信有限支持界，须包含来源 `[0,16]³`；flags=0 表示没有可信有限界，六个坐标须全零。
普通层使用当前输出或当前已完成编译的候选界；layer=`UINT32_MAX` 表示等待整段当前编译，version 必须非零。
整段要求可用 flags=1 携带 JVM 对当前冻结来源认证的候选界，否则使用 flags=0；它不写入实际层版本表或代表空间状态 1/2。
JVM 缺层与整段要求优先使用匹配当前版本的自身支持界，没有自身界时使用同次冻结的认证来源全集界。
两者都没有时才导出 flags=0；全集撤销推进 revision，不能复用先前有限清单。该后备不提供缺失内容或实际层版本。
完整清单须覆盖全部当前 layer/version 与无层目标；拒绝重复、缺层、外来版本、非法 flags、非有限界或不包含来源段盒的有限界。
同世界、同 liveRevision 的版本表和输出清单必须逐字节相同；错误清单在修改合法 scene 前拒绝。
版本表表示当前所需层，不是可用载荷列表。JVM 在非空层 CPU 导出失败、复制异常或缺少字节时仍提交当前所需版本，
并撤销该层的旧载荷；成功导出后提交实际几何身份，明确移除层或退出世界才删除要求。
同 node 的所需层不全时，剩余层不得认证为完整主表面；可认证的不透明遮挡仍遵守本节的阴影专用实例规则。
`targets` 是独立的 8 B node 数组，最多 65535 项，非零 count 要求非空指针，重复 node 拒绝。
允许目标没有当前非空层要求，用于已观测 dirty、等待当前完整编译的段；这类目标必须提供当前整段要求。
有限整段要求与普通有限界共用包含性检查、不可变清单校验和变换代理；无界要求继续设置全局 unknown 标记。
两者都不提供实际几何、待建原查询补查或遮挡证明，也不能解除内容缺口。
旧快照中该 node 的全部层均失去当前资格，不授予最近表面、待建原查询补查或阴影证明。
调用内复制目标；内容和顺序不变时复用缓存，不重新分配查找表。数组范围变化不要求改写 liveRevision，当前版本表仍遵守同代际同内容约束。
生产 JVM 对当前已捕获输出界与镜像锚点 120 m 闭球作相交判定；同段任一层相交就保留完整层组。
缺载荷、dirty 目标始终保留；有限界仅限定 unknown 代理，不按来源段盒或载荷选择预算裁剪当前要求。
失效集合检查快照节点与目标清单的并集；目标被预算整组省略时用当前有限输出界生成 unknown 代理。
当前目标、记录节点与认证支持界按 native 的 `uint64_t` 顺序合并扫描；
记录失效集合与目标失效集合去重合并，有限代理沿同一支持界顺序生成，不对每个目标重复二分查找。
输入仍允许任意排列；重复键、缺少支持与失效内容的拒绝规则保持，不能用输入顺序证明当前资格。
普通层请求与整段来源请求使用连续有序表；更新时合并当前层、目标和整段捕获版本。
完整版本序列相同时保留原请求身份、最早帧、需求及已交付状态，变化时分配新身份并剔除旧需求。
输出支持清单排序一次，逐项复核重复、实际层版本与有限界，再按来源聚合；不能仅凭 revision 接受内容。
局部发布复用可消费前代的 TLAS scratch，同队列通过 UAV barrier 排序；取消或跨队列未完成的前代不复用。
TLAS result 与实例表仍独立创建并保活到实际列表槽退休，不覆盖在途输入或结果。
内部 `recordTlas` 的实例表只写一次，保留到 GPU 消费结束；调用 `buildTlas(..., immutableInstances=true)`
直接使用该表，避免从 UPLOAD 堆读回复制。公开 C ABI 入口仍快照可变实例表，不允许调用方省略输入冻结。
缺载荷且没有可信有限界，或无法可靠表示有限界的目标显式标为 unbounded，不以来源段盒代替其最大影响域。
目标没有完整当前内容时不授予待建补查资格；当前有效不透明层的阴影证明继续独立认证。
未观测空间、已知空/卸载的独立状态和范围边界尚未纳入完整空间清单。
全无载荷但仍有已观测所需目标时，JVM 提交零记录 delta，scene 发布带有限代理或无界标记的可查询 TLAS。
只有无界要求且无可见实例时放置 mask=0 的占位实例，它不参与查询或提供空间证明。
该场景没有几何 BLAS，也不授予待建原查询补查或不透明阴影证明资格。恢复真实载荷后按当前版本重新认证并发布。
没有所需目标的零记录快照返回 `empty target scope`，不改变先前合法场景；snapshotId=0 仍清空 scene。
不得把清单中没有 node 当作空空间证明；快照自身的过期或缺层仍按当前版本检查，不因目标范围变小而授予旧几何资格。
独立低层与自检保留 `mcdr_dx12PrepareRtScene(encoder, sceneId, desc, output)`；scene 一旦使用显式目标，后续旧入口返回 `targets required`。
两入口共用候选、真实帧预算、局部失效与退休状态；错误目标在修改有效场景前拒绝，不能回退旧入口绕过目标校验。
必须使用 pass 外开放主列表，不在入口内提交或等待。返回 1 表示录制成功，是否有可用场景须另查 output.active.ready。

单次 `prepareScene` 在帧锁内核对候选与已发布选择的快照、失效 node、待建查询、阴影证明和代理覆盖。
部分场景已通过这组核对或刚完成发布时，直接保活选中资源；录制 TLAS 消费同次计算的有限/无界代理覆盖。
该复用仅限本次调用；完整候选与后续调用仍重新验证当前资格，选中 view 与 producer 可消费性继续单独检查。
不得跨版本、录制或调用保存资格结论，也不得改变当前支持界的保守浮点外扩、unknown 或发布额度。

每个 scene 在同一真实 frameId 内最多构建 16 条几何 BLAS。候选替换、空场景请求、更换命令列表或 encoder 均不补充同帧预算；
仅 frameId 前进或世界变化重置数量额度。仅 `cancelled && !submitted` 的录制退还自身构建额度，且只退还一次。
已执行工作即使任一 Signal 失败、ticket 被标记 cancelled，仍计入本帧预算；实际 GPU 完成也不补充同帧额度。
JVM 传入 `D3D12Temporal.frameIdentity + 1L`，从 1 开始；
该身份仅在真实呈现后推进，不得用 RT 派发次数、命令列表编号或 fence 值代替。
独立 AS 入口仍使用 §1.2 的列表预算，与生产 scene 的真实帧预算分开。

生产 scene 只为当前 live 表中 node/layer/version 匹配的记录新建 BLAS；缺席或过期记录不消耗构建额度，
仍由失效代理表示，等待当前版本的几何载荷。候选不会因此被标记为完整。
请求年龄按当前 live 表的 node/layer/version 与实际 targets 登记，不等待几何载荷进入快照；
没有显式 targets 的兼容入口使用当前 live 范围。待建工作按首次请求的真实 frameId 升序排列，同帧请求保留当前记录顺序；
有效 GPU 需求与最老请求交替准入，身份、期限和失败规则见 §1.5。
暂时没有载荷、预算省略、候选替换、记录重排与执行前取消重试均保留仍在目标中的当前版本年龄。
版本变化或退出目标时删除旧年龄；显式撤销场景、世界变化与 scene 释放清空年龄表。
年龄表只保留当前作用域内的 live 键，随版本/目标变化裁剪；它不是完整的捕获/上传队列或全场景预算。
这项调度只约束工作顺序和 16 条数量额度，不提供总录制耗时或显存预算。

`mcdr_dx12GetRtSceneStats(sceneId, values, valueCount)` 读取 CPU 场景状态，`valueCount` 必须为 14，
输出是调用方分配的 14 个 `uint64_t`（112 B）。成功返回 1；未知或已释放 scene 返回 0 并清零合法大小的输出。
空指针或错误数量返回 0，不写输出。不要求打开 encoder，不录制、提交或等待 GPU，也不改变构建额度或任务年龄。

| 下标 | 字段 | 含义 |
| --- | --- | --- |
| 0 / 1 / 2 | worldEpoch / frameId / targetSnapshotId | 当前世界、最后一次准备的真实帧、pending 优先的目标快照 |
| 3 / 4 / 5 | recordCount / recordedCount / pendingBuildCount | 目标记录、当前版本已录制 BLAS、当前版本仍待建的记录数 |
| 6 / 7 | obsoleteCount / absentCount | 目标中版本过期、缺席当前 live 表的记录数；均不计入待建 |
| 8 / 9 | proxySectionCount / buildsThisFrame | 已发布对象的代理 section 数、最后一次准备后的本帧构建账本 |
| 10 | oldestRequestFrame | 当前目标快照内待建记录的最早目标请求帧；没有此类记录时为 0 |
| 11 / 12 / 13 | sceneRevision / pendingSnapshotId / liveRevision | 发布版本、仍待完成的目标快照、live 表版本 |

取消录制的 BLAS 计入待建，读取本身不执行退款；下一次 Prepare 才按录制状态刷新账本。
recordedCount 只证明有效版本的 BLAS 已录制，不证明 GPU 完成或整个 section 已发布。
即使 pendingBuildCount 为 0，过期载荷或缺席层仍可能使目标不完整；proxySectionCount 只描述已发布对象。
尚未进入目标快照的当前层保留请求年龄，但不计入 recordCount、pendingBuildCount 或 oldestRequestFrame。
从工作区根运行 `python _port/rt_scene_request_age_check.py`，核查缺载荷、预算省略、版本/目标撤销、
换世界、取消重试、拒绝输入与多层情况下的实际 BLAS 构建顺序和发布实例。
这些数值不证明目标清单以外的空间覆盖，也不代替逐 lobe unknown 或显存/时间预算。

请求的 snapshotId 与 pending 相同时继续分批构建；新候选先共享已发布场景和旧候选的未修改 BLAS，再查询常驻缓存，最后替换旧候选。
已发布对象与 pending 可以引用同一 snapshotId，但持有不同 AS 令牌；重复请求该快照不得取消仍未完成的 pending，也不补充同帧预算。
仅重新请求已全部构建完成的发布对象时取消其他候选，已经完成且仍兼容的候选 BLAS 可保留在缓存。
缓存键为完整 `(node, layer, version, vertices, indices, flags)`，scene 的 worldEpoch 隔离世界。
缓存最多 1024 条且结果缓冲 committed 分配总和不超过 256 MiB，先触及任一上限即逐出最久未用条目；单项超过字节上限不入缓存。
新 BLAS 在录制成功时登记缓存，候选首次组合时登记继承条目；静态候选不逐帧重扫全部条目。
缓存逐出不撤销仍由候选、已发布 AS 或在途列表持有的 BLAS，也不将这些引用逐帧重新插入缓存。
section 暂时缺席 live 表时仅保留 dormant BLAS，不允许从缓存加入候选；重新进入 live 且版本及完整键匹配后才能复用。
live 显式发布同 node/layer 的新 version 时移除旧版本缓存。空场景、世界变化和 scene 释放清空缓存。
调用方只有在重新收到完整几何、严格验证 RT 所用内容相同后才能恢复旧 version；内容变化必须分配新 version。
常驻缓存只保存 BLAS，记录顺序、InstanceID、材质和逐 quad 映射仍来自新候选快照，禁止复用旧映射替代当前数据。

候选尚未全部构建完成时，native 为当前请求快照创建独立的可查询发布对象，共享候选中已建的 BLAS，并录制不可变 TLAS 与实例表。
候选与已发布 AS 身份、候选条目代次、完整当前清单和目标保持不变时，复用已经核对的失效集合、补查组、阴影资格与代理覆盖。
每次调用仍检查几何、BLAS 和 TLAS 的 producer 可消费性，并清除取消录制；新增或取消 BLAS 推进条目代次。
版本或输出支持清单变化、目标增删、候选替换、发布取消及跨 encoder 不可消费均禁止沿用该缓存。
相同 liveRevision 的逐字节载荷核对、同帧构建/发布预算和实际 fence 保活继续执行。
运行 `python _port/rt_scene_cpu_check.py` 检查大清单静态复用、版本撤销、目标撤销、支持界变更和取消发布；耗时仅代表离线 native 准备。
主可见性要求整个 section 的当前内容完整：其全部 layer 都须存在于请求快照、与 live 表版本一致，并由可消费 BLAS 或下述原查询补查覆盖。
未取得完整认证时，从最近表面查询中移除该 section 的全部真实实例；有可信有限界时加入程序化 AABB `HITINVALID` 代理，否则显式保持无界 unknown。
其中已有可消费 BLAS、node/layer/version 与 live 表匹配且 `(flags & 0xFF00) == 0x100` 的不透明记录，保留为 mask=`0x80` 的阴影专用实例。
完整 section、已认证补查组中补查范围外的真实实例及未知代理使用 mask=`0x01`；最近表面查询使用 `0x01`，阴影查询使用 `0x81`。
阴影实例只能证明实际射线区间内存在遮挡，不能提供不完整 section 的最近表面或证明无遮挡。
缺席、过期、未建、不可消费、cutout 与 translucent 层不授予此资格；独立 `FilterRtAcceleration` 没有 live 认证，仍移除全部真实实例。
共享 BLAS 包含一份 24 B `D3D12_RAYTRACING_AABB`，局部边界为 `[0,16]³`。
完整输出模式的实例按当前输出界平移、缩放；合并来源原点使用 Double，平移下界、上界与缩放保守舍入并验证包含性。
不能表示有限、正且可可靠求逆的变换时保留无界 unknown，不制造巨大代理或缩短原射线。
低层诊断模式仍以来源段盒平移；该模式不提供生产空间证明。
inline 查询对程序化候选沿原 `TMin/TMax` 做 slab 区间求交，使用最早重叠距离判断 unknown；
起止均在体积内时仍为 unknown。平行轴单独判定，不能延长原区间或把 AABB broad phase 候选直接当作相交。
`TraceRay` 路径由 `RtInvalidIntersection` 执行同一求交并 `ReportHit`，`RtInvalidHit` 返回 `hit=2`。
已完成且版本有效的其他 section 立即保留真实 BLAS 和当前快照的 InstanceID，无须等待未完成 section；热缓存返回同样遵守此规则。
同一 snapshot 的候选仍未完成，且失效集合、原查询补齐资格、阴影证明记录集合和代理覆盖均不变时，复用已发布 TLAS 与实例表，不推进 sceneRevision；
候选继续构建，不修改已录制的发布资源。
新候选只从当前 encoder 可消费的候选或发布前代借用 BLAS/TLAS scratch，容量不足时仍独立扩容。
每次构建保留 UAV 屏障和实际列表槽引用；TLAS result、view 与一次写入的实例表独立分配，不能覆写在途结果。
当前 live、输出支持、目标、失效集合和代理覆盖使用连续有序存储；变化载荷批量排序、去重后再认证，不逐项分配树节点。
每次准备仍完整核对 section/output 载荷、world epoch、producer 与取消状态。
同一世界的总 liveRevision 推进而两份完整载荷均相同时，复用已验证的版本表和请求身份；
载荷改变但 liveRevision 未推进仍拒绝。实际变化重新验证重复键、版本和保守输出界，不延迟失效撤销。
有限支持界认证后预先计算保守实例变换，逐次发布按当前失效集合选取；不能用旧界或旧世界的变换。
同帧 16 条构建额度已用尽时不重复排序待建记录，仍核对 producer、取消、当前版本与发布资格。
代理覆盖包括有限实例变换和无界标记；node、mask 与 snapshot 相同但输出界改变时仍须重建 TLAS。
就绪 section 或阴影证明集合改变时创建新的发布对象；候选全部构建完成后发布候选并清除 pending，live 版本失效的 section 仍须保留代理。
完成全部层后改用普通实例，撤销对应阴影专用实例，不能重复加入同一记录。证明层编辑、移除或生产者取消立即撤销旧资格。
native 在 TLAS 录制成功后才交换发布状态；失败不发布输出，已录制命令不能回滚。
局部失效只撤销相关 section 的样本，不要求全屏历史 reset。独立 AS 入口仍须全部 BLAS 构建完成才返回 ready，见 §1.2。

当前几何已经上传、全部层与 live 表匹配，但 BLAS 未全部建好时，生产 scene 允许补齐原射线查询。
先认证同一 section 的全部 layer/version，再找出第一个到最后一个尚无当前可消费 BLAS 的 record。
快照遵守几何协议的 65535 条 record 上限；补查资格逐 section 认证，不受整场景 384 条记录限制。
这个连续范围必须只包含同一 section、数量为 1–127，且各 record 仍与 live 版本匹配。
内容认证不受三角形总数限制；shader 按原查询类型与剩余工作额度决定能否取得可靠结果。
补查范围外的记录必须已有当前可消费 BLAS，以普通实例参加最近表面与阴影查询；已建层的三角形数不占补查额度。
section 的全部 records 可以不连续；两个待建 record 之间夹有其他 section 时不能认证。
范围内部已建记录仍按补查策略计费，不因已有 BLAS 跳过对应三角形。
实例 `InstanceID` 为 `0x800000 | (recordCount << 16) | firstRecordIndex`；其余代理仍从完整 record count 开始编号。
bit 23 是认证标志，bits 16–22 是连续 record 数，bits 0–15 是首 record 索引；只在认证标志存在时解码。
`g_misc.w` 始终是完整快照的 record count。shader 直接读取认证范围并校验 sectionNode，
使用原 origin/direction/TMin/TMax 与当前顶点、索引求交；
每条原查询最多补查 256 个三角形、8 个 section，不分配新缓冲、不提交或等待额外命令。
硬件候选遍历完成后才执行软件补查；八项局部队列按代理入口距离保留最近的不同待建组，
同组重复候选合并最早入口，相同距离按 identity 排序。后到的近组可替换远组；
队列溢出记录最早丢弃入口，只有其在最终命中之后才能忽略，否则仍保留 unknown。
最近表面由近到远补查，并用当前实际最近交点作为内部上界；交点之后的组不占三角形额度。
这项剪枝来自真实命中证明，不缩短原采样的 TMax。阴影已有硬件遮挡时不执行软件补查，
否则按同一队列寻找可靠遮挡。溢出需求至多提示最早丢弃组；其它未完成队列项分别提示。
被丢弃的 identity 后来重新入队，旧溢出入口仍可保守地维持 unknown，不因此授予完整性。
最近表面查询要求整个认证范围能在余额内完成，超额时不测试、不扣费并保留 unknown。
阴影查询在范围可完整补查时保留原预扣费与三角形类别行为；范围超额时只测试 `(flags & 0xFF00) == 0x100` 的不透明 record，
每实际测试一个三角形扣一份共享余额。原区间内找到真实遮挡即可成功；未找到、余额用尽或内容不合法时保留 unknown，不能证明无遮挡。
相交前仍检查整个认证范围的 sectionNode，不能靠早命中绕过错误组；超额 cutout 不授予该遮挡证明。
最近表面在可靠硬件命中与补查命中中取最近者，不能越过更近的未解决代理；任意可靠遮挡物都足以证明阴影。
补查得到实际 record、primitive、重心坐标与距离，后续材质、guides、SDK 输入和合光使用同一结果。
本认证只补齐当前几何查询，不签发 §2.2 的空间证明，也不把状态 0、3、4、5 改成已知。
即使全部 BLAS 已完成，未证明的查询空间仍为 unknown；生产 JVM 仅在 §2.2 的来源、内容和目标闭合后签发 1、2，
不能据受控完整空间 fixture 宣称世界覆盖已恢复。
缺层、版本变化或移除即撤销资格；即使失效 section 集合未变，也重建不可变实例表并推进 sceneRevision。
超额且没有取得可靠不透明遮挡证明时仍为 unknown，不解释为黑色、天空或无遮挡；一般缺样本恢复仍未完成。
`proxySectionCount` 包含有补查资格的代理，不能将该数值直接当作 unknown section 数。

警告：生产查询使用 `TMax=1e9`。有限区间代理不证明目标清单以外的空间；当前帧必须另交 §2.2 的空间证明，真实缺口仍需独立恢复。
256 次三角形测试与 8 个 section 仅为工作数量上限，不是 GPU 时间预算。

输出为 232 B `mcdr_rt_scene_output`：前 64 B 包含已发布 snapshotId/accelerationId、pendingSnapshotId/pendingAccelerationId、
sceneRevision、geometryRevision、materialRevision、scene AS 资源去重 byteCount；后面依次为 80 B active 与 candidate 输出。
偏移 224 的 uint64 `coverageFlags` bit 0 表示当前发布对象包含不能由有限代理表示的 unknown 生产者。
active.ready 表示存在可查询且可消费的 TLAS，其中可以包含未就绪 section 的代理；不能据此判断所有几何 BLAS 已完成。
candidate 描述本次构建进度与时长，candidate.ready 表示该候选的全部几何 BLAS 已构建完成；无候选的静态调用中 candidate 为空。
零记录候选的几何构建数为 0，ready 仍可同时存在 unknown 代理，不能据此推断整个查询支持域已知。
全部构建完成并发布后，pending 两个令牌清零，candidate 仍报告本次构建统计；此前允许 snapshotId 与 pendingSnapshotId 相同。
输出 AS 令牌为 scene 所有，禁止直接释放、准备或过滤。
sceneRevision 由 native 全局单调序列分配；发布、失效集合改变、清空及世界变化推进，静态调用保持。
同一世界清空或重新创建 scene 后仍不会回退，避免已存在的 RT 帧上下文拒绝恢复帧。

scene 的 byteCount 按底层资源去重并使用 committed 分配字节，覆盖已发布、候选、常驻 BLAS，以及实际列表槽仍保活的旧 BLAS/TLAS、scratch、实例表和失效代理的上传/源 AABB 缓冲。
登记仅持有弱引用，不延长资源寿命；容量查询时清掉已退休项。旧世界或清空场景的在途资源仍计入，直至所属 fence 完成且保活槽释放。
此值不含几何快照及其 staging、SDK 资源和描述符；不再有 RT 强引用而进入通用延迟删除队列或 buffer pool 的分配也不在其中。
因此它是 RT 当前引用资源的 committed 字节，不能当作设备实际驻留总量；场景之间共享或池内复用的分配不能直接相加。
16 条数量额度和 256 MiB 缓存上限不构成总时间/总内存预算；场景增量工作还须配置下述复杂度与时间准入。

AS 专项分配共用 512 MiB 上限，覆盖全部 scene、独立 AS、世界及 encoder。
准入按实际 buffer 描述调用 `GetResourceAllocationInfo`，包含 BLAS/TLAS 结果、两类 scratch、实例 UPLOAD 表、
失效代理的 UPLOAD 与 DEFAULT 源缓冲；相同共享对象只取得一次租约。
scratch 扩容时新旧分配同时计费；候选替换、撤销令牌、换世界、取消与 fence 完成均不直接退还额度。
租约随 `Dx12Object` 保留至其实际销毁并释放底层资源；AS 使用 §1.2 的录制引用退休，不加入通用开放列表等待。
AS 的表和暂存使用独立精确容量缓冲，不借用或进入通用 buffer pool。

在调用驱动创建资源前预留 committed 字节；超额返回 `rt accel: memory budget exhausted`，失败调用不发布输出。
已录制的合法前缀继续保活，尚未完成的候选可在安全退休后重试；不得借用旧输出绕过失败。
驱动创建失败退还尚未物化的预留；已录入命令的资源继续由相应列表槽保活，不因构建失败提前退费。
预算拒绝不提交列表、不等待 GPU，也不通过扩大上限或改换 scene 重置计费。

`mcdr_dx12GetRtAccelerationBudget(values, valueCount)` 读取四个 `uint64_t`，`valueCount` 必须为 4：

| 下标 | 内容 |
| --- | --- |
| 0 | 当前 AS 物理分配与未完成准入预留的 committed 字节 |
| 1 | 共享上限，536870912 B |
| 2 | 内存准入拒绝累计次数；驱动创建错误不增加此值 |
| 3 | 含短期准入预留的字节峰值 |

查询不要求打开 encoder，不提交、等待或回收资源；错误数量或空指针返回 0，保留原输出。
JVM 可选绑定为 `MCDR_ABI.D3D12GetRtAccelerationBudget()`，返回具名字段，缺少导出时返回 null。
该只读追加接口不改变 ABI 45 的已有结构与帧协议。

512 MiB 为 AS 专项工程保护值，不包含几何、CPU 镜像、SDK/历史、描述符和驱动内部开销，
也不代表整个 scene 的冻结内存/时间预算或设备实际驻留；长期回访与尾延迟仍须分别验证。

JVM 的 CPU 镜像缓存可查询 pendingSnapshotId，合并候选构建期间到达的更新；范围选择仍由 CPU 几何池负责。
追踪使用输出中配对的几何/AS 令牌和 sceneRevision；失败帧不得沿用旧输出，下一真实帧继续未完成候选。

### 1.4 场景增量工作准入

在准备生产 scene 前调用 `mcdr_dx12SetRtSceneWorkBudget(scene, cpuMicros, gpuMicros, triangles)`。
CPU/GPU 参数分别接受 100–1000000 微秒，三角形参数接受 1–1000000000；错误配置保留原值。
未配置的低层 scene 保留每帧 16 条计数契约；JVM 生产入口每次准备前读取同一份配置快照并设置预算，缺少导出时拒绝该次准备。
配置热更新不清空当前帧工作账，不重新获得额度。

当前世界和真实 `frameId` 内，候选替换、重复调用、列表和 encoder 共用 CPU 耗时、预测 GPU 耗时、已建三角形及发布额度。
只对当前 live 版本的缺失 BLAS 申请工作；普通请求与需求组内均保持首次请求年龄排序，按 §1.5 交替选择。
CPU 记录从合法准备调用开始到时间戳 resolve 录制完成，包含代理、TLAS、实例表和反馈分配的录制成本。
达到 CPU 预算后停止下一条 BLAS；已经付出的 CPU 耗时不因执行前取消而退还。
未提交的取消可以归还该 ticket 的三角形、预测 GPU 和发布额度；已经执行但 Signal 失败的工作继续占用当前帧额度。

冷启动 BLAS 预测为 `32000 + 8 × triangles` 纳秒；TLAS 连同代理和实例表的预测为
`200000 + 1000 × recordCount` 纳秒，再乘以场景校准系数。系数以 1024 为单位，初始 1024。
预留基础发布成本后判断下一条 BLAS 是否可准入；每帧最多录制一次 TLAS/代理发布。
同帧改变候选且必须重新发布时，超额调用返回 `rt scene: publication budget exhausted` 并清空输出，下一帧重试。
不得把旧场景输出当作更新成功。

本次选中的首条不可拆分 BLAS 即使超过三角形或 GPU 预测额度，也允许单独推进；记录 oversized 次数。
一次已开始的驱动分配、BLAS 或必要发布不能被上述软时间片抢占；实际 CPU/GPU 超限分别记录。
未建部分保留 unknown 代理，不恢复已删除几何或修改射线距离。
该机制不承诺单次调用必定低于配置毫秒数；单项过重仍需细化粒度和重新测量。

每次实际录制 AS 工作，使用两枚 D3D12 timestamp 和当前列表的独立 READBACK 快照测量整段 GPU 时间。
只有实际完成且提交已确认的 producer 才能校准；提交未完成不读回，取消和 Signal 失败的样本不进入成本估计。
已执行的取消样本保持到真实 fence 完成后退休；未提交取消可直接退出反馈队列。
校准使用实际 GPU 时间与预测值之比；高估计立即上调，低估计按 7/8 旧值与 1/8 新值收敛，系数范围为 256–1048576。
每个 scene 最多 16 份未退休测量，满额返回 `rt scene: timing feedback pending`，不提交列表或等待 GPU。
query heap 安全复用；底层 query/readback 仍由原 encoder 槽保活，释放 scene 不提前销毁在途资源。

`mcdr_dx12GetRtSceneWorkBudget(scene, values, valueCount)` 只读 24 个 `uint64_t`，数量必须为 24：

| 下标 | 内容 |
| --- | --- |
| 0 / 1 | 当前 worldEpoch / 真实 frameId |
| 2 / 3 / 4 | CPU 纳秒预算 / GPU 纳秒预测预算 / 三角形额度 |
| 5 / 6 / 7 / 8 | 当前帧 CPU 纳秒 / 预测 GPU 纳秒 / 三角形 / 发布次数 |
| 9 / 10 / 11 | 累计延后 / 单项 oversized / 已完成有效 GPU 样本数 |
| 12 / 13 | 最近有效 GPU 纳秒 / 累计有效 GPU 纳秒 |
| 14 / 15 | CPU 超限帧累计 / GPU 超限样本累计 |
| 16 / 17 / 18 | 未退休测量 / 取消测量累计 / 测量错误累计 |
| 19 / 20 / 21 | 最大有效 GPU 纳秒 / 最近 GPU 样本真实帧 / 样本世界 |
| 22 / 23 | 最近合法准备调用 CPU 纳秒 / 以 1024 为单位的成本系数 |

查询不轮询或消费反馈，不提交、等待、回收或改变输出资源；错误指针、数量或 scene 保留调用方原缓冲。
它不改变 ABI 45 的已有布局。内存专项与时间准入分别记账；该机制不覆盖几何上传、SDK 内部工作、CPU 导出和通用资源退休的完整预算。

### 1.5 待建组需求反馈

B-DXR 在原查询不能完成已认证待建组时，将该组首 record 的一位写到 indirect 的尾部位图。
位图位于 `width × height × 16` 之后，占 8192 B，覆盖最多 65535 records；不属于 diffuse 辐亮度。
最近表面查询在完整补查超额、阴影查询未取得当前遮挡证明或 section 访问额度不足时可登记需求。
已经获得可靠遮挡证明的阴影不登记该次需求；其他候选上的未完成遍历仍可提供调度提示。
尺寸乘法和位图范围先校验，短缓冲自检入口不写反馈。

native 全局最多三份未退休反馈，各有逻辑 8192 B READBACK 和不可变 CPU 身份清单。
单份清单最多 65535 条当前待建记录，仅冻结 node/layer/version、请求身份与组首索引，不保留整份几何载荷。
反馈复用一份逻辑 8192 B 的只读零上传缓冲；复制清零、trace 和回读都录入调用方现有主列表。
槽位满额或反馈资源分配失败时省略本次提示，继续保留查询的真实结果，不提交或等待 GPU。

读取必须同时满足实际提交、两次 Signal 确认、该帧最终合光成功及对应 fence 完成。
执行前取消、执行后 Signal 失败、未完成合光、提交确认前释放上下文及过期反馈不更新需求。
已提交反馈即使取消也须等真实 fence 退休后才复用 READBACK；设备移除保留到设备清理。
资源同时由实际列表槽保活，释放 scene/frame 或替换几何不会提前释放其在途缓冲。

消费时验证 scene、worldEpoch 和当前请求身份；版本变化、live/target 撤销及同版本重新加入均产生新身份。
记录重排通过冻结的索引映射回原 node/layer/version，不解释为后来快照的相同下标。
提示最长保留八个真实 scene 帧的年龄，年龄为八时仍可用，超过八即忽略；读取不刷新原请求帧。
保存 RT 帧号用于匹配最终提交，另存 `scene.frameId` 用于需求年龄与八帧期限；两种编号不能互相代用。
选择器从普通最老请求开始，在每次成功录制 BLAS 后交替选择最老普通请求和最老有效需求。
没有有效需求时继续普通年龄顺序；预算不足不增加构建数，也不切换下一次选择。
已有 16 条、CPU/GPU/三角形及发布额度继续共用；该交替不覆盖尚未上传的内容或目录生产调度。

这项反馈只提高当前待建数据的构建优先级，不保存跨帧射线、PDF 或 lobe 估计。
它不恢复缺失 CPU 内容，不签发空间证明，也不改变 unknown 或原采样归属；SDK 消费规则见 §5–§8。
逐信号恢复仍须独立闭合 trace、guides、曝光、SDK 与最终合光的处理。

### 1.6 有限缺口的捕获需求

显式 targets 场景中的普通有限 unknown 代理也可写入同一 8192 B 位图。
`InstanceID` 的高八位为 `0x40`，低 16 位为反馈槽；槽号避开所有已认证待建组的首 record，最多 65535 个槽。
它只登记调度需求，不授予几何、空空间或辐亮度资格。无界来源和独立空间目录缺页继续由普通恢复处理。
短缓冲、原始射线区间、补查额度、真实遮挡证明与 unknown 语义保持原契约。

每份在途反馈另存 node、当前 source 请求身份和槽号，不保留 CPU 几何。
source 身份由实际目标与全部当前层/版本维护，整段恢复使用输出清单中的 `layer=UINT32_MAX` 及其版本。
任一层版本变化、目标撤销/重加或换世界撤销旧身份；其它来源的版本变化不撤销仍当前的需求。
已确认提交、成功合光和真实 fence 的要求与 §1.5 相同。

可选导出 `mcdr_dx12GetRtSceneCaptureDemands(scene, values, valueCount)` 返回小端 uint64 数组：

| 索引 | 内容 |
| --- | --- |
| 0 / 1 / 2 | worldEpoch / 当前 liveRevision / 当前 scene.frameId |
| 3 | 本次有效输出数量 |
| 4 + 2 × i / 5 + 2 × i | 来源 node / 原需求的 scene 帧号 |

`valueCount=4+2×capacity`，capacity 为 1–64；JVM 使用 132 个 uint64。
每次最多弹出 capacity 个 FIFO 项，过期项也占本次检查次数；不每帧重新扫描和排序全部来源。
每个当前 source 最多一个队列项，新需求更新原帧号但不移到队尾，已导出的同帧提示不重复输出。
生产队列随最多 65535 个当前 targets 裁剪，版本变化删除旧身份项，换世界和释放清空。
导出先非阻塞收集已完成反馈，再消费队列；不提交、等待 GPU 或改变 scene 几何。
合法调用清零输出并写入头部，非法容量、空指针或未知 scene 不修改调用方缓冲。ABI 45 的既有布局不变。

JVM 在准备当前 scene 版本/目标后导出需求；worldEpoch/liveRevision 与当前池完全匹配时，
才为该来源的实际缺失层/整段目标登记提示，不提前消费旧元数据的导出。
提示最多 256 个 CaptureTarget，最多保留 250 ms；epoch、层版本与首次缺口身份须仍匹配。
选择从普通轮转开始，普通与需求交替，取出即消费提示；普通轮转、100 ms 忙碌退避、失败退避、
每次最多两个 region、四个在途任务及 1 ms 软准备时间片继续生效。
已捕获但被驻留预算省略的来源交给 JVM 独立有界驻留队列，不重复编译；
完整组 FIFO、250 ms 期限、实际几何提交确认与 100 ms 重选间隔见
[上传镜像](../../../Illusion/docs/rt/README.md#92-上传镜像)。
该消费不改变 native ABI、原位图或查询语义，也不提供完整缺页调度或通用逐信号恢复。

### 1.7 Native RT 受管分配总账

几何 DEFAULT、几何 UPLOAD、AS、帧 GPU 与帧 host 分配共用一份账本。
每次新分配在调用驱动前，按实际 resource description 的 `GetResourceAllocationInfo` 结果，
同时检查共享总上限与对应域上限并取得租约；共享引用不重复计费。
租约绑定 `Dx12Object::allocationCharge`，资源物理释放后同时退还两层额度。
未物化的驱动失败预留立即退还；已创建的失败前缀、取消、在途旧代与延迟删除继续计费。
撤销注册、换世界和 fence 完成都不直接退账。

`mcdr_dx12SetRtManagedMemoryBudget(bytes)` 设置共享上限，默认 2048 MiB，
接受 1 B–16384 MiB；0 或超范围值返回失败并保留原上限。
调低到当前用量以下时保留现有资源，新增分配继续背压，待正常退休或调高上限后恢复。
设置和查询均不提交命令、不等待 GPU、不主动回收资源。
JVM 通过 `[rt] managedMemoryMiB` 暴露 64–16384 MiB 的热配置，每次生产 `prepare()` 时提交。
首次帧上下文因内存背压创建失败时允许后续重试，不将其记为永久初始化失败。

`mcdr_dx12GetRtManagedMemoryBudget(values, valueCount)` 要求非空指针与 `valueCount=24`。
失败不写输出；成功按下表返回六组 `uint64_t[4]`：

| 元素索引 | 账户 | 四项顺序 | 域上限 |
| --- | --- | --- | --- |
| 0–3 | total | 当前字节、上限字节、拒绝次数、峰值字节 | 默认 2048 MiB，可热调整 |
| 4–7 | geometry_gpu | 同上 | 512 MiB |
| 8–11 | geometry_upload | 同上 | 512 MiB |
| 12–15 | acceleration | 同上 | 512 MiB |
| 16–19 | frame_gpu | 同上 | 0，表示只受共享总上限约束 |
| 20–23 | frame_host | 同上 | 0，表示只受共享总上限约束 |

当前 total 字节等于五个域当前字节之和；各域峰值发生时间可能不同，不要相加解释为总峰值。
准入失败同时累计 total 与对应域的拒绝次数，物理退休不清除峰值或累计次数。
既有 `GetRtGeometryMemory` 与 `GetRtAccelerationBudget` 保留原布局，从同一账本读取。

帧受管范围包括曝光、G-Buffer、raw/guide/信号、常量与记录表上传、SBT、捕获快照、
NRD/RR 的外部输入输出、独立 SR/合光目标，以及需求反馈的 READBACK/零值 UPLOAD。
NRD 永久/瞬态纹理池与常量 UPLOAD 环也计入 frame GPU/host；退休上下文保留租约至实际完成，
新帧创建和 G-Buffer 开始前收集可安全释放的旧池，账本查询不触发回收。
NRD 初始化的预算拒绝不会锁死相同 plan/尺寸的后续帧；接口与范围见 [NRD 受管分配](nrd.md#受管分配)。
`createTexture` / `createBuffer` 的内部 `rtManaged=true` 显式接入帧域；受管 buffer 不借用或进入通用池。
几何和 AS 的专用分配路径使用各自域，不再重复登记为帧分配。
CPU 导出/复制、NRD 的 CPU 对象、其它 SDK 内部分配与历史、通用渲染资源、描述符、PSO/状态对象、query pool 和驱动私有开销未计入，
不能将该数值解释为完整 scene、设备或 WDDM 显存用量。

`_port/rt_native_frame_check.py --managed-memory-only` 验证真实分配、跨域背压、
失败前缀、阻塞 fence 下的保活、物理退休与恢复准入。
`_port/rt_managed_memory_check.py` 验证真实 FFM、NeoForge 热配置、24 个字段的解码与诊断 JSON；
其 `prepare()` 用已有上下文 fixture，不代表世界运行或首次创建恢复的实机验证。

## 2. 输入与版本

`mcdr_rt_frame_desc` 为 128 B；`mcdr_rt_frame_output` 为 128 B。布局由 `abi_meta.cpp` 与 JVM 加载期逐字段校验。

| 输入 | 约束 |
| --- | --- |
| `frameId`、`snapshotId` | 均非零；同一上下文的 frameId 严格递增 |
| `worldEpoch`、`sceneRevision` | 同一 worldEpoch 不接受 sceneRevision 回退；换世界可重置 sceneRevision |
| `materialRevision`、`snapshotId` | 必须对应仍注册且上传可消费的 native 快照，worldEpoch/materialRevision 与注册值相同 |
| `width`、`height` | 1–16384；与常量中的 `g_screen.xy` 相同 |
| `constants`、`constantBytes` | 调用期间有效的 416 B 数据，所有浮点有限；`g_misc.w` 必须等于快照记录数 |
| `accelerationId` | 仍注册、具备可查询 TLAS 且可消费的 native AS 令牌；其 snapshotId 必须与本帧几何令牌相同，scene 的部分发布见 §1.3 |
| `gbufferRevision` | 当前上下文最近成功结束的重放编号；时间戳、尺寸、encoder 和未提交 fence 必须与重放一致，且未被 trace 消费 |
| `depthScale` | 有限正数；当前 JVM 传入 64 |
| `frameNanos` | 同一时钟域的有符号纳秒时间戳；重复或回退会复位曝光历史 |
| `exposureMode` | 0 为 MANUAL，1 为 AUTO；其他值拒绝 |
| `exposureEv`、`exposureSpeed` | 有限浮点，范围分别为 `[-8,8]` 与 `[0.1,10]`；速率单位为每秒 |
| `resetExposure` | 0 或 1；相机、后端等外部历史失效由调用方置 1 |
| `signalEncoding` | 0 为 RAW，跳过降噪输入；1 为 REBLUR，2 为线性 RR；其他值拒绝 |
| `contentJitterX/Y` | 当前屏幕内容偏移，均有限且在 `[-0.5,0.5]`；与同帧 SL 68/69 相同，RAW 也必须提交 |
| `jitterDeltaX/Y` | 有限、绝对值不超过 1；当前减上一帧的屏幕内容偏移，单位为像素，Y 向下 |
| `denoisingRange` | 有限正数，乘 2 不溢出；当前为 500000，须与 NRD 设置一致 |

native 材质 guide 顺序为 specular / normalRoughness / emissive / albedo / depth；格式分别为
RGBA8_UNORM / RGBA16_FLOAT / RGBA16_FLOAT / RGBA8_UNORM / RGBA16_FLOAT。
它们由光栅重放初始化；guide 在主命中与光栅深度匹配时写入采样材质，不匹配时保留输入。

调用方必须在同一主列表上先完成 native G-Buffer 重放和场景准备，冻结空间证明，再录制本入口，然后记录所有后续消费者。
G-Buffer、trace、reblur、denoise、SR 与 composite 通过共享 `CommandRecording` 核对同一未提交录制身份。
`End` 不提交再 `Begin` 会取消旧录制；相同 encoder 地址和 fence 数值不能让旧附件或输出恢复可用。
已提交的帧输出也不能进入下一次录制，须重新生成当前帧 G-Buffer 与信号。
参数校验拒绝时输出清零，版本、时间戳和曝光历史状态均不前进；录制中失败也不发布输出，但已写入列表的命令不能回滚。
开始录制后发生失败会保留曝光 dirty 标记，下一次成功录制必须重设曝光。
失败帧必须直通原版，不得继续使用上次输出。重试相同 frameId 仅在该编号尚未成功录制时合法。

### 2.1 G-Buffer 资源与重放提交

`mcdr_dx12CreateRtGBufferProgram` 接收 56 B `mcdr_rt_gbuffer_program`，返回非零程序令牌；0 表示创建失败。
配方包含 MS/PS DXIL 指针与 64 位长度、2 B binding 数组及数量、cullEnabled、polygonMode 和为零的填充字段。
两份 DXIL 均须非空且各不超过 16 MiB，bindings 为 1–64 项，类型为 0–3，剔除与多边形模式均为 0/1。
至少提供一个 CBV；同一寄存器空间内不可重号，保留 b8 与 t14/t15/t16，根签名总大小不得超过 64 DWORD。
native 复制完整配方，创建带 mesh 尾参的根签名、无 AS 的 mesh PSO；固定五张颜色格式、D32_FLOAT、深度写入和 GREATER_EQUAL。
按 DXIL 内容、完整 binding 顺序、剔除与多边形模式复用同一 PSO，每次注册返回独立令牌。
缓存只保留弱引用，程序的注册者与在途列表均释放后即可销毁，不保留无限增长的强引用缓存。

`mcdr_dx12ReleaseRtGBufferProgram` 返回 1 表示撤销成功，0 表示未知或已释放令牌；释放 0 是空操作。
撤销后禁止新捕获或直接回放使用该令牌，已经捕获的批次和已经录制的直接回放继续持有程序至列表槽 fence 完成。
JVM 在地形管线替换/关闭时释放自己的注册令牌，禁止将令牌传入 `DestroyPipeline`。
程序不依赖某个 RT 帧上下文；释放/重建帧上下文不影响仍有效的地形程序，设备销毁清空程序注册表。

`mcdr_dx12RtFrameBeginGBuffer(encoder, context, frameNanos, width, height, output)` 要求打开的主列表且不在 pass 内。
native 持有五张颜色附件与独立 D32_FLOAT 深度，颜色顺序为 albedo、specular、normalRoughness、emissive、linearDepth。
前两张为 RGBA8_UNORM，其余为 RGBA16_FLOAT；颜色 usage=44，深度 usage=8，均为单层/单 mip/单采样。
每次 Begin 清空全部附件：specular 为 `(0,0,0,1)`，其余颜色全零，反向设备深度为 0。
viewport/scissor 固定从 `(0,0)` 覆盖整个追踪尺寸。尺寸变化先创建完整替代组，成功后替换；旧附件按实际使用 fence 退休。

开始分配、清屏或撤销输出资格前，native 检查此上下文上一份 G-Buffer 的 producer ticket。
同一未提交录制可顺序重放；同一 encoder 已提交的工作可依靠队列顺序复用附件。
另一 encoder 的未提交或尚未完成 producer 必须拒绝，只有实际 fence 完成后才可重试。
拒绝不改变附件、revision/resourceRevision、时间戳及原 trace/NRD/SR/composite 资格，返回结构清零。
执行前明确取消或销毁的 producer 经取消清理后允许新录制；`EndGBuffer` 只结束 pass，不表示 GPU 已完成。
执行后 Signal 失败的 ticket 也会标记 cancelled，但必须保留到其实际 fence 完成才能清理与重新写入。
fence 缺失或完成值为设备移除标志时继续拒绝复用，不能将输出撤销当作 GPU 工作已经停止。

88 B `mcdr_rt_gbuffer_output` 返回 revision、resourceRevision、frameNanos、尺寸、六个借用句柄和 byteCount。
revision 每次成功 Begin 增加，resourceRevision 仅在资源替换时增加；字节数为 `width × height × 36`，不含分配对齐。
Begin 撤销上帧合光与降噪输出的消费资格，但正常重放不重置曝光适应。
JVM 仅保留捕获令牌与计数，禁止自行销毁附件，禁止在重放与 trace 之间提交列表。

`mcdr_dx12RtCaptureGBuffer(encoder, capture, frameNanos, batch)` 在原版源 pass 内接收一份 96 B 批次。
capture=0 创建捕获组；成功返回非零令牌，后续同 pass 的批次携带该令牌追加。失败返回 0，不追加无效批次。
每组最多 65536 批；令牌绑定 encoder、未提交 fence、源 pass 序号与帧时间戳。不得在 native G-Buffer pass 中捕获。
native 核对程序令牌，保留程序所有权并复制绑定描述及精确 SRV 描述符；texture view 的 mip/cube 语义保持原样。
同一捕获组内，相同源 CPU descriptor 的不可变 view 快照只创建一次，批次借用组内快照。
以 view descriptor 为身份，不按底层纹理合并不同 mip/cube 视图；不同 token、录制或源 pass 分别捕获。
源 view 在打开列表内延迟退休，描述符槽不得在同一捕获组中回收改写；快照及其纹理引用由列表槽保活至实际 fence 完成。
UPLOAD 顶点、常量、RAW/texel 与 section/meshlet 范围复制到所属列表槽的 64 KiB 上传页切片，
切片按 256 B 对齐，大于页容量的请求独占足够大的页；同帧每次捕获分配不同范围。
页仅在该槽的实际 fence 完成后重置游标，批次借用页内缓冲与偏移，不逐批创建和延迟销毁 committed buffer。
DEFAULT 常量、RAW/texel 与表范围录制 GPU 拷贝，
并恢复源缓冲状态。DEFAULT 顶点及纹理保留资源引用，调用方须维持其同列表内容版本，不得覆写。
原始 JVM 对象、程序令牌及 view 句柄可以在捕获后关闭，捕获组持有的资源保持至所属列表槽 fence 完成。

`mcdr_dx12RtFrameReplayCapturedGBuffer(encoder, context, capture, replay)` 接收同一 48 B 描述，
其中 batches 必须为空、batchCount 必须为 0；批次来自 native 捕获组。要求 G-Buffer pass 紧接源 pass，且帧时间戳一致。
成功回放只允许一次。投影或容量预检失败仍可重试；取消、提交后、换 pass 或跨 encoder 的令牌均不能回放。
`mcdr_dx12RtReleaseGBufferCapture(encoder, capture)` 撤销令牌；资源继续由列表槽保活，不立即释放在途 PSO。
JVM 在 submitRenderPass 的 finally 中调用释放，覆盖禁用计划、缺少相机及投影等提前退出。列表结束自动撤销未释放令牌。

独立诊断入口 `mcdr_dx12RtFrameReplayGBuffer(encoder, context, replay)` 接收 48 B 描述：revision、96 B 批次数组、
批次数、16-float 投影与相机偏移。批次数为 1–65536，每批包含程序令牌、顶点/section/meshlet 缓冲范围、
meshlet 数、40 B binding 数组及 Projection 绑定下标。描述与数组只在调用内借用。
native 拒绝未知或已释放程序，核对当前打开回放、PSO 的五张格式/反向深度写入/无 AS、根签名绑定类型、缓冲范围、投影与相机有限值。
每批 meshlet 数为 1–65535，section/meshlet 范围按 32 B 记录对齐；常量偏移须 256 B 对齐。
描述符容量按整组最坏需求预检，不能借用上次的描述符表掩盖容量不足。
复用外部根签名的 PSO 从根签名取得 binding 类型，不能依赖可能为空的 PSO 描述 bindings。

整组预检通过后，native 从所属列表槽上传页分配 256 B 对齐的投影和逐批 b8 常量，不逐批创建 committed buffer。b8 的 meshlet 数来自批次，
剔除/VRS 位清零，相机偏移位于 32–40；Projection 绑定改用 RT 投影。native 逐批绑定 PSO、推描述符、派发 mesh。
CPU 投影和 UPLOAD CBV 在返回后可修改；非投影常量沿底层录制快照进入列表槽，临时资源按 fence 退休。
顶点、section、meshlet、RAW/texel 输入和纹理仍为调用方借用资源，须保持内容与在途寿命。
每个回放编号只能成功提交整组一次。预检拒绝不写任何批次，可修正后重试；录制开始后失败只能取消本次回放。

`mcdr_dx12RtFrameEndGBuffer(encoder, context, revision, success)` 核对当前 pass 的五张颜色与独立深度、录制 ticket 和 revision。
success 只能为 0/1。两种值都结束 pass；只有 1 发布可消费的重放，0 撤销输入并令下次曝光 reset。
已开始录制但失败的批次拒绝 success=1，调用方必须以 0 结束；空附件/独立诊断绘制仍可通过 Begin/End 发布。
trace 的描述不接收外部 guide 指针；通过预检后，开始录制 GPU 工作时消费该重放编号，后续 trace 必须重新 Begin/End。
拒绝阶段不消费合法输入；有效录制的重放未结束时不能释放上下文。
执行前已取消的重放允许下一次 Begin 或释放；新重放不接受旧 revision 结束。

JVM 的 `Targets`/`Image` 仅保存借用元数据。每次重放生成新的 Image 身份，拒绝旧图像进入调试 blit；
同尺寸复用资源时保持独立 resourceIdentity，使正常重放不会每帧触发降噪历史 reset。

### 2.2 当前查询空间证明

`mcdr_dx12RtFrameSetSpatial(encoder, context, frameId, worldEpoch, liveRevision, data, bytes)`
复制调用方的空间证明。成功返回 1，失败返回 0；要求主列表打开、不在 pass 内，且当前 G-Buffer 重放已成功结束。
`frameId` 必须大于该上下文最近录制的帧号。证明绑定 G-Buffer revision 与同一个尚未提交的 `CommandRecording`。
同一重放内只允许逐字节相同且 frame/world/liveRevision 相同的重复调用；拒绝另一份冻结输入，不修改原证明。
旧录制取消、提交或新重放开始后，不允许继续使用旧证明。`mcdr_dx12RtFrame` 缺少当前证明时拒绝并清零输出。
scene 所属 AS 另要求 `liveRevision` 等于当前 scene 的 liveRevision；独立诊断 AS 没有 scene 版本可供比较。

载荷采用小端编码；schema 1 与 schema 2 共用 64 B 表头，分别校验：

| 字节偏移 | 内容 | 约束 |
| --- | --- | --- |
| 0、4 | magic、schema | `0x53505452`、1 或 2 |
| 8、12、16 | min section X/Y/Z | 有符号 32 位，范围与 SectionPos 的 X/Z 22 位、Y 20 位相同 |
| 20、24、28 | size X/Y/Z | schema 1 各为 1–256；schema 2 不大于 X/Z 的 4194304、Y 的 1048576；min + size 不超出 SectionPos 正向边界 |
| 32、36、40 | 物理几何界 min X/Y/Z | 有符号 32 位；仅在 flags bit 0 为 1 时使用 |
| 44、48、52 | 物理几何界 max X/Y/Z | 启用时各轴 min 严格小于 max |
| 56、60 | flags、页数 | 仅允许 flags bit 0、1；schema 1 页数字段为 0，schema 2 为 0–512 |
| 64 起，schema 1 | 空间状态数组 | X/Y/Z 顺序，格数最多 65536；载荷长度严格等于 `64 + 格数 × 4` |
| 64 起，schema 2 | 每页 16 B 目录 | 有符号 page X/Y/Z、无符号数据偏移；按有符号 Z/Y/X 严格递增，不允许重复 |
| schema 2 目录后 | 每页 2048 B 状态 | 每页 8³ 个 section，X 最快；第 i 页偏移严格为 `64 + 页数 × 16 + i × 2048` |

schema 2 页坐标是全局 section 坐标 floorDiv(8)，各页必须与声明域相交并位于 SectionPos 范围内。
页内超出声明域的状态必须为 0。载荷长度严格等于 `64 + 页数 × 2064`，最大 1056832 B；
零页目录保留声明域而不提供已知空间。native 在帧冻结前验证全部目录和状态。
产品 shader 对 schema 2 二分查页，缺页保持 unknown；DDA 最多执行 `min(sizeX + sizeY + sizeZ + 3, 页数 × 24 + 3)` 步，
耗尽时保留 unknown，不输出完整 miss。schema 1 的密集读取与既有保守边界判定继续使用。

状态 0=未观测、1=已认证空、2=当前完整查询空间、3=卸载、4=dirty、5=已加载但查询空间未认证、
6=`PROXY_COVERED`。native 拒绝大于 6 的状态。
1、2 表示完整空间；6 仅表示潜在来源均已登记，不完整来源均进入实际 targets，并具有有限 unknown 代理义务。
6 本身不提供已知或空域资格。native 在当前 acceleration 由显式 targets 和输出支持协议管理，
且没有无界 unknown 时，为本次独立追踪记录副本增加内部 flags bit 2（值 4）；
shader 仅在该位存在时让 6 通过整格检查，实际射线是否穿过未知部分仍由 TLAS 代理或原查询决定。
“空”针对当前几何表达所覆盖的查询空间，必须包含跨格模型、邻段几何伸出和当前内容版本的依赖。
不能仅凭 `hasOnlyAir()`、没有 terrain draw 或方块放置高度范围签发证明。
状态 2 也不能代替被选择预算省略的几何载荷、缺失层或未完成的 AS。

flags bit 0 表示已证明全部物理几何位于给出的 AABB 内，才允许把界外当作空。
没有这项证据时设 0；世界 minY/maxY 本身不能证明自定义模型没有伸出。
网格之外且不在已认证物理界外的区间保持 unknown，不把有限近域清单用于证明无限太阳或 bounce 射线无遮挡。

native 将不可变 CPU 几何记录与这份证明合为一张当帧 UPLOAD 表，作为追踪 SRV 0。
flags bit 1（值 2）表示存在尚未闭合的来源义务，适用于未观测或无可信有限影响域的来源。
调用方可冻结此位，scene 发布对象存在无界 unknown 时 native 也置此位；二者按位或，不能清除调用方的标记。
setter 接受 flags 0–3，拒绝其它位。同一重放内不能通过重调 setter 改写这个标记。
调用方不能冻结内部 bit 2，也不能以独立 acceleration 为状态 6 获得资格。
未观测来源、遗漏的相关 target、无界来源与缺页不能通过状态 6 获得完整 miss。
即使格子状态为 1/2 或同时启用物理界，未闭合来源仍使非空原查询从 TMin 保持 unknown。
shader 对非空原查询在原 TMin 返回 unknown，即使格状态或物理界另有已知证明；不将无界要求解释为 miss、天空或零贡献。
原几何快照的五份缓冲与版本身份不变；追踪仍使用 6 SRV / 5 UAV 和 416 B 常量。
表头在 `recordCount × 64` 处，缺失或无效表头按 unknown 处理。
最近表面查询先裁到最近候选命中，再遍历整个必要区间；更早的 unknown 阻止选取后方表面。
太阳查询已有可靠遮挡物时允许结束；没有遮挡物时须证明全部必要区间。
格遍历包含面、边、角相接的格子，按射线经过边界的时间校正起始格；数值不确定性过大或步数耗尽时返回 unknown。
可选物理界裁剪使用独立分支，禁止依赖 HLSL 逻辑表达式跳过带 `inout` 副作用的调用。

JVM 的 `coverageNear` 在同一池锁内冻结版本、node 和空间网格，`D3D12RtNativeFrame.record` 在追踪前调用本入口。
加载、卸载、dirty 与当前编译安装均更新权威生命周期；未证明时输出 0、3、4、5，物理界 bit 0 关闭。
JVM 的逐格保守要求同锁冻结全部已观测来源；卸载来源只取消恢复工作，仍保留空间义务。
JVM 在来源全集没有有限输出范围时置 bit 1；受认证静态资源全集与空附加监听器列表可将未观测补集收敛到逐格来源范围，
调用方清除 bit 1 不表示相应内容或查询空间已完整，存在未观测来源的格子仍按 unknown 消费。
有限全集覆盖的潜在来源须全部已登记；相关来源还须有当前完整编译、实际安装层清单及所有非空层的当前载荷。
当前相关来源全空时 JVM 签发 1；存在非空来源且全部相关非空 node 均进入实际 scene targets 时签发 2。
实际层集绑定整段发布作用域，候选 quad 不虚构实际 draw；缺层、dirty、卸载、未知支持或目标遗漏不签发 1/2。
全部相关来源已登记且不完整来源均进入 targets 时，JVM 可签发 6，由有限代理精确限制未知区间；
已捕获但 producer 证明不完整的来源额外保留 `layer=-1` 标记，不能因已有 BLAS 而遗漏未知代理。
没有自身支持界的来源可使用受认证全集保守限定影响域，全集无界时仍阻止任意非空原查询。
同一认证全集也进入缺层/整段输出清单，实际 scene 用它生成有限 unknown 代理；仅在空间网格内使用有限界仍会被 native 无界标记否决。
已加载清单、EMPTY、当前 terrain mesh 或有限全局来源界均不能单独签发 1/2；网格外仍为 unknown。
JVM 空间资格不代替 native 对选择预算省略、实际内容版本、缺失层与 AS 发布状态的检查。
原版绘制表示 reset 后，JVM 可保留当前完整内容及其原始依赖证明；数量最多 1024 段、载荷与记录最多 128 MiB。
这类内容仍由普通 live/outputs/targets 发布，native 不接收“旧帧可用”特权；实际编辑、卸载或淘汰照常撤销资格。
保留来源不扩大 JVM 镜像选择与载荷预算，也不消除域外或真实缺失来源的 unknown。
实际域选择最多 65535 条 records；显式近域兼容入口仍限 384 条。
警告：已接通的生命周期与 FFM 不提供完整查询覆盖；逐信号恢复与世界路线验收仍待完成。
要求本协议的 DLL、shader 与 JVM 必须一起使用，不得单独阶段化给旧桥接客户端。

部分 AS 构建进展允许合并发布：已有 TLAS 必须仍有效，其 mask 包含当前需要撤销的全部 node，
原查询映射和 shadow proof 必须仍与当前 live 一致，旧代理必须逐来源完整包含当前要求的未知体积。
仅在继续构建且剩余可建项非零时延后可用性发布，积累 16 次 entry 变化或距上次发布达到 100 ms 后，
在下一次准备调用发布；可建项耗尽时立即发布。编辑导致旧真实几何失效、支持界扩大、取消或上述核对失败时立即重建。
这项合并不延后必要撤销，不增加 CPU/GPU/三角形或内存额度，也不将待建来源当空气。

### 2.3 统一帧事务

`mcdr_dx12RtFrameTransaction(encoder, frame, desc, output)` 保留无内容事件的统一事务；返回 1 表示整组录制成功，0 表示拒绝或失败。
生产使用 §2.4 的附带内容事件入口，两者共用以下描述、输出与阶段规则。
它要求已结束的同帧 G-Buffer 重放、配对几何/AS、打开且不在 render pass 内的主列表，以及当前世界颜色源。
空间证明、trace 和最终合光使用同一 `CommandRecording`；入口不提交列表。

`mcdr_rt_transaction_desc` 为 248 B：

| 偏移 | 字段 | 大小与约束 |
| --- | --- | --- |
| 0 | `trace` | 128 B `mcdr_rt_frame_desc` |
| 128 | `denoise` | 64 B `mcdr_rt_denoise_desc`；非 RAW 时 frameId 配对，显示尺寸等于 source |
| 192 | `liveRevision` | 8 B，与空间证明配对 |
| 200 | `spatial` | 8 B 指针，CPU 内容在调用内复制 |
| 208 | `spatialBytes` | 8 B，64 至 1056832；实际长度按空间 schema 严格验证 |
| 216 | `source` | 8 B，合法的世界颜色纹理 |
| 224 | `upscaler` | 4 B，NONE=0 / DLSS=1 / FSR=2 / XeSS=3；RAW 只接受 NONE |
| 228 | `debugMode` | 4 B，0–10 |
| 232 | `timestamps` | 8 B 可选 query pool，连续保留四个槽 |
| 240 | `firstTimestamp` | 4 B，第一个 GPU 时间戳槽 |
| 244 | `reserved` | 4 B，必须为 0 |

入口复制描述、416 B 追踪常量、64-float NRD 矩阵、104-float Streamline 常量和空间字节后，
持有一次帧锁，连续设置空间证明、追踪、降噪、可选 SR 与合光。不得并发修改调用中的输入内存。
RAW 不读取降噪矩阵。JVM FFM 层另外使用调用作用域 Arena 复制这些载荷，保留原缓冲的 position/limit。

`mcdr_rt_transaction_output` 为 288 B：

| 偏移 | 字段 | 语义 |
| --- | --- | --- |
| 0 | `trace` | 128 B 当前帧输出 |
| 128 | `denoise` | 48 B 成功 NRD/RR 输出；RAW 或降噪失败为零 |
| 176 | `upscale` | 48 B 成功独立 SR 输出；未执行或失败为零 |
| 224 | `composite` | 48 B 最终合光输出 |
| 272 | `fallbackFlags` | 4 B，按位报告本帧回退 |
| 276 | `recorded` | 4 B，整组成功为 1 |
| 280 | `timestampMask` | 4 B，低四位表示 trace、降噪、SR、合光之后实际写入的 GPU 时间戳 |
| 284 | `reserved` | 4 B，为 0 |

`MCDR_RT_FALLBACK_RAW=1` 表示降噪失败后合光当前 raw；`MCDR_RT_FALLBACK_NRD=2` 表示 RR 失败后使用当前 NRD；
`MCDR_RT_FALLBACK_NO_SR=4` 表示独立 SR 失败后保留当前 NRD。回退不是事务整体失败，仍须最终合光成功。
时间戳按实际阶段写入；未执行的可选阶段仍可写阶段边界，未提供 pool 则 mask 为 0。

预检失败清零出参，允许更正后重试尚未消费的同一 replay，不撤销此前成功历史。
消费 replay 后的部分录制失败清零整组、撤销对应 pending 历史、曝光与输出资格；不能将已写入或已执行的整个列表标成执行前取消。
已经录制的 trace/SDK 资源仍归所属列表槽和真实 fence 管理，后续录制按失败范围恢复。
最终合光成功只授予同一未提交录制内的顺序消费资格；历史与曝光确认点见 §6。

### 2.4 内容事件与当前查询影响

`mcdr_dx12RtFrameTransactionLighting(encoder, frame, desc, lighting, lightingBytes, output)`
沿用 248 B 描述、288 B 输出及 ABI 45。`lighting` 必须非空，载荷为 little-endian，大小为
`64 + count × 64` B，`count` 为 0–256；即使没有事件，也传完整的 64 B 游标头部。
既有统一事务与细粒度入口不要求该载荷。

| 头部偏移 | 字段 | 规则 |
| --- | --- | --- |
| 0 / 4 | magic / schema | 两个 u32，分别为 `0x434C5452` / `1` |
| 8 / 16 | worldEpoch / liveRevision | 两个 u64，等于本次 trace 与空间版本 |
| 24 / 32 | afterRevision / revision | 两个 u64，消费者已确认游标与本批末游标，前者不得大于后者 |
| 40 / 44 | fullDomainReasons / count | 两个 u32，理由只允许低七位；游标相等时两者均为 0 |
| 48 / 56 | reserved | 两个 u64，均为 0 |

每条 64 B 记录为 `sectionNode`、`firstRevision`、`revision` 三个 u64，
`reasons`、`bounded` 两个 u32，六个 float min/max 与一个 reserved u64。
node 使用完整 `SectionPos.asLong` 编码，不允许重复；事件范围满足 `afterRevision < firstRevision ≤ revision ≤ 批末游标`。
reasons 非零且只允许低七位，依次为 INPUT=1、SOURCE=2、RESOURCES=4、COMPILE_NOTIFICATION=8、
CAPTURE_CHANGED=16、WORLD=32、OVERFLOW=64；bounded 仅为 0/1，reserved 为 0。
全部 bounds 分量必须有限；bounded=1 时每轴 min 不大于 max，坐标相对来源 section。
bounds 表达内容支持，不能直接当作照明影响域。

入口在调用期间复制并预检载荷；非法输入清零输出且不消费当前 G-Buffer replay。
同一 world 已确认游标后，拒绝 `afterRevision` 跳过已确认游标或批末游标倒退；较老起点允许保守重放。
成功追踪后登记 pending，只有该录制完成最终合光且实际提交确认才推进内容游标；取消或部分失败不确认。
JVM 同样只在实际提交确认推进游标，新导出缺失时明确拒绝，不静默改走旧入口。

载荷追加在当前几何 records 与空间 schema 1/2 目录后，复用受管 frame GPU/host 分配和实际列表槽寿命。
HLSL 从空间目录长度定位事件，逐实际查询与支持界做保守相交；无界事件和全域理由标记所有实际发出的查询。
`indirect.w` 的 bit 8/9/10/11 分别为 PRIMARY_CHANGED / DIRECT_CHANGED / DIFFUSE_CHANGED / SPECULAR_CHANGED。
最近表面查询止于当前最近交点或原 TMax；阴影查询使用原 TMax，允许保守包含遮挡后的支持界。
新增位不改变当前 unknown、材质、辐射或 NRD 当前输入资格，不引入额外 SDK reset。
当前查询标记本身不表达旧路径；最后已提交帧的独立路径支持见 §2.5。
完整旧积累与 SDK 空间/时域依赖仍未保存，不能直接当作严格历史撤销或上一帧 confidence。

### 2.5 已提交 raw 帧的路径支持

携带内容批次时，在 `aux` 的 `width × height × 80` B 之后追加路径尾部。
令 `columns=ceil(width/64)`、`rows=ceil(height/64)`、`tiles=columns×rows`，每块 `blockBytes=64+tiles×128` B。
current 块起于尾部偏移 0，previous 块起于 `blockBytes`，每 tile 的 u32 影响值起于 `2×blockBytes`。
总尾部为 `128+tiles×260` B，整个 raw 地址范围不得超过 u32。
旧统一事务及细粒度入口不分配尾部；同尺寸入口切换按实际 `aux` 大小更换 raw targets。

| 块内偏移 | 字段 | 规则 |
| --- | --- | --- |
| 0 / 4 | magic / schema | u32，`0x50445452` / `1` |
| 8 / 16 / 24 | worldEpoch / frameId / lightingRevision | 三个 u64，标识支持生产帧 |
| 32 / 36 | width / height | 两个 u32，生产帧图像尺寸 |
| 40 / 44 | tileSize / lobes | 两个 u32，64 / 4 |
| 48 / 52 | columns / rows | 两个 u32，完整图像 tile 网格 |
| 56 / 60 | flags / reserved | u32，bit 0 为 previousReady，其余及 reserved 为 0 |
| 64 | tile records | 每 tile 128 B，依次为 primary/direct/diffuse/specular 四类 |

每类 32 B：sortable u32 minXYZ、state、sortable u32 maxXYZ、reserved。
float 转序码时，负数按位取反，非负数翻转符号位；反变换按序码最高位判断。
空支持 state=0，min 为正最大有限值、max 为负最大有限值；state bit 0 表示实际查询，bit 1 表示不可信支持。
每像素保存实际查询段的保守 AABB，再按 tile 合并；未发出查询不登记该类。
primary 最近查询截到当前最近交点或原 TMax，shadow 覆盖原 TMax；长射线的 AABB 可能很宽。

追踪后复制 current 到独立 GPU 快照，仅在同录制最终合光且实际提交确认后替换已确认快照。
取消、失败或未合光不替换。快照、seed UPLOAD 与复制来源按列表槽/fence 保活；物理释放后才退受管内存账。
previous 只接受相同 world/尺寸及可消费 producer fence，缺失时影响值为 `0x80000000`。
有效 previous 的影响值低四位按四类支持与本批事件相交；空支持保持 0，无界/全域事件保守命中已有支持。

影响值使用 previous 帧图像 tile 坐标，没有转换为当前像素坐标或 SDK confidence。
记录仅涵盖最后一个已提交 raw 帧，不包含更早积累路径、滤波邻域、运动重投影及跨帧依赖传播。
该 raw 快照本身不构成累积历史撤销。携带事件的生产事务还把当前路径支持交给内部 NRD，
由逐 pass 累积依赖生成历史 mask；接线、额外资源与边界见 [NRD 累积内容依赖](nrd.md#累积内容依赖)。
RR 与独立 SR 内部历史分别适配，不能借 NRD 的结果声明它们也已支持。

## 3. 输出与资源寿命

每个像素为 16 B。direct、indirect、specular、specularGrazing 各一平面，aux 五平面，共九平面。
基础资源为 `width × height × 16 × 9 + 512 + 320 + 4 + 8192` 字节，indirect 尾部为 §1.5 的需求位图。
另计当帧合并追踪表的几何 `recordCount × 64`、空间目录与可选事件载荷。
空间 schema 1 为 `64 + 格数 × 4` B，schema 2 为 `64 + 页数 × 2064` B；事件至多 16448 B。
已分配降噪输入时另加 `width × height × 56`。
启用路径支持时另计 §2.5 尾部、当帧 pending 快照及上下文仍持有的 committed 快照，各快照为一个 `blockBytes`。
`byteCount` 统计 raw 缓冲、常量、SBT、曝光纹理、五张降噪 guide、四张信号纹理与当前合并追踪表，
以及上述路径支持；不含 seed UPLOAD、其它在途旧代、全局反馈 READBACK/零上传、描述符、PSO、几何、G-Buffer 或驱动分配对齐开销。

每次接受录制都复制 CPU 常量到新的 512 B UPLOAD 缓冲，b0 偏移为 0；底层 dispatch 再将其复制到
列表槽拥有的上传页。CPU 原内存可在返回后立即修改或释放。空间证明在 setter 返回前已复制，录制时另建不可变追踪表。
旧常量、旧追踪表和尺寸替换的 raw 缓冲按 fence 退休；不能把当前 `byteCount` 当作包括所有在途旧代的物理总量。
尺寸不变时按命令顺序复用五张 raw 缓冲，不提供跨帧独立的 raw 历史。

输出句柄仅为借用，调用方不得销毁或覆写 native 常量。请在下一次成功录制或释放上下文前记录完消费命令；
需要跨帧保留数据时自行拷贝。调试通道修改常量时使用独立副本，不修改 native 当前常量。

## 4. 曝光历史

上下文创建时分配一张 1×1 R32_FLOAT UAV/SRV 纹理。输出 `exposure` 是借用句柄，RR、独立 SR 与最终合光消费同一资源。
本次所需录制全部成功后才发布输出并提交曝光时间戳、模式与 EV；JVM 不保存适应状态，也不包装为独立所有权纹理。

MANUAL 写 `2^EV`；AUTO 通过包内 `RtExposure` 对 64×64 网格的完整混合显示图求亮度几何平均，
目标为 `clamp(0.18 * 2^EV / geometricMean, 1/256, 256)`。正常帧在 log2 域以 `1-exp(-speed*dt)` 适应，
dt 夹在 0–0.5 秒。时间差以浮点计算，避免有符号纳秒相减溢出。

首次录制、录制失败、尺寸/世界/模式/EV 变化、时间戳回退或重复，以及 `resetExposure=1` 均触发 reset。
仅几何/材质 revision 或适应速率变化不触发 reset。JVM 过滤纯 GEOMETRY/MATERIAL 失效，其他历史失效仍传入。
次级缺口按带来源标记的当前估计参与测光；primary 回退按上一曝光反解当前 source，reset 时用补偿倍率。
细粒度入口没有 source 时跳过无主表面像素，没有有效样本则沿用上一倍率；reset 时写 `2^EV`。最终显示变换只执行一次。

## 5. 运动与降噪 guide

`signalEncoding` 非零时，曝光后依次执行 `RtMotion`、`RtNrdPack` 和 `RtNrdRadiance`，同帧返回六张借用纹理：

| 输出 | 格式 | 语义 |
| --- | --- | --- |
| `motion` | RGBA16_FLOAT | 像素位移 `previous-current`，Y 向下，抵消矩阵中已有的 jitter；无深度写零 |
| `packedNormalRoughness` | RGBA16_FLOAT | NRD 101010 编码的归一化值，粗糙度下限 `1.5/512` |
| `viewZ` | R32_FLOAT | 视轴上的正投影深度；无效深度/法线写 `2*denoisingRange` |
| `diffuseSignal` | RGBA16_FLOAT | 编码 1 为解调 diffuse 的 YCoCg，编码 2 为含材质和自发光的线性总辐亮度；alpha 为归一化 diffuse 命中距离 |
| `specularSignal` | RGBA16_FLOAT | 解调 specular 的 YCoCg 与归一化反射命中距离；供 NRD 使用 |
| `specularHitDistance` | R32_FLOAT | 世界单位的首次反射距离；未知或 RR 全色光栅降级时为 0，禁止用 specularSignal 的 alpha 替代 |

上下文另持有独立 diffuse/specular `R32_FLOAT` viewZ 与 `RGBA16_FLOAT` 当前光栅降级色，不增加公开输出字段。
原始 `viewZ` 保留可靠主表面深度；NRD diffuse 消费完整 direct + indirect 显示估计，specular 独立判断。
查询可靠性与显示资格分开；带来源的近似可以进行显示降噪，unknown 事实与物理样本资格保持。
显示估计不可用时，仅该 lobe 的深度写 `2*max(denoisingRange,500000)`，不让占位零值进入 NRD 历史。
显示历史属于有偏混合估计；内容变化使用累计路径依赖撤销，不将每次随机方向的来源变化当作内容更新。

统一事务在信号编码时读取本次 `source`，按 `(pixel+0.5-contentJitter)/traceSize` 映射取样，
经 sRGB 解码、当前 ACES 拟合的反解和当前曝光除法，得到与 RT 出口匹配的辐射域颜色，范围夹到 65504。
这是有偏的同帧全色降级，不能恢复显示源中已裁剪的 HDR 信息，不代表未知 Monte Carlo 样本已补齐。
不复用旧 source、旧几何或旧光照；资源由实际录制保活，输入随当前编辑和绘制更新。
该纹理保存完整混合 HDR；仅 primary/材质无效时保存本次 source 的整像素回退。RR 消费混合输入，镜面未知的距离写 0。

motion 的 b1 为 depthScale、jitterDeltaX/Y；pack 的 b1 为 depthScale、denoisingRange，浮点均按位传入。
信号编码 b1 为 encodeReblur、hasRasterSource 与两个 contentJitter float，共四个 dword。
纹理在首次需要或尺寸改变时成组分配成功后替换；依赖底层 compute 的资源过渡，派发后归还 COMMON。
RAW 时六项输出句柄为 0，同尺寸可保留已分配资源供恢复；RAW 尺寸变化时退役旧输入纹理。
JVM 只持有当前帧句柄，RR/NRD/SR 不得取用全局缓存的上帧 guide；任一缺失即跳过重建并复位对应历史。

独立诊断入口 `mcdr_dx12RtFrameReblur(ctx, frame, frameId)` 在相同信号纹理上重新编码本帧 REBLUR 输入；
生产回退由 `mcdr_dx12RtFrameDenoise` 内部执行。
此入口只接受最新成功线性帧、同一个 encoder、同一次未提交的主列表录制；RAW、已编码 REBLUR、过期 frameId、
未知上下文、录制失败或已经提交的列表均拒绝。重复调用不再次编码。返回 1 才能派发 NRD，返回 0 必须跳过重建。
从首次录制到回退编码结束，调用方必须保持四张材质 guide 的句柄和内容有效，禁止覆写或释放。
回退只修改信号纹理和编码状态，不推进帧版本、曝光历史或重新追踪；原始 raw 和常量仍来自同一次录制。

## 6. 降噪与历史

`mcdr_rt_denoise_desc` 为 64 B，含 frameId、historyGeneration、historyFrame、planRevision、
显示宽高、64-float NRD 矩阵与 104-float Streamline 相机常量及各自数量；CPU 指针只在调用期间读取。
相机常量必须有限，68–71 的当前/上一帧 jitter 在 [-0.5,0.5]。NRD 使用它们的相反数。
显示尺寸不得小于输入尺寸，不得大于 16384。historyFrame 与 planRevision 必须非零。

只接受当前帧、同一 encoder、相同未提交录制、非 RAW 且尚未求值的信号。
预检失败清零输出并允许修正参数；通过预检后每帧只尝试一次，失败必须等下一帧。
native 保持四张材质 guide 有效至本帧降噪、独立 SR 与合光录制结束。禁止在这些阶段之间重新 Begin G-Buffer、修改 guide 或提交列表。

编码 2 在已准备当前光栅降级色时先调用 RR；成功仅发布显示尺寸 RGBA16F 输出，跳过 NRD。
细粒度追踪接口没有 source，未准备完整降级输入时直接转 NRD；统一事务已在追踪阶段提供 source。
RR 拒绝时在当前 Plan 内停止重试，将同帧信号编码为 REBLUR 并执行 NRD。
Plan 代际、轨道、输入/输出尺寸改变或上下文重建允许重试。RR 帧 token 在上下文重建时仍递增。
NRD 输出为输入尺寸的两张 RGBA16F 纹理；初始化失败在相同配置下不重复初始化，
Plan 或尺寸改变后可重试。就绪 SDK 上下文由 native 初始化缓存复用。

NRD 后端用同一 SDK instance 的 `REBLUR_DIFFUSE` / `REBLUR_SPECULAR` 独立 identifier 派发。
生产帧传八纹理：索引 2 使用 diffuse 当前 viewZ，末尾使用 specular 当前 viewZ；
原始主表面 viewZ 留给 RR 与 SR 深度转换。公开七纹理入口仍兼容共用深度，
具体限制见 [NRD 当前输入排除](nrd.md#当前输入排除)。范围外输出可能未写，合光和 SR 重调制均按当前深度拒读。

RR/NRD 各自保存最后成功的 historyGeneration/historyFrame。首次、代际变化、帧间断、
Plan 改变和 SDK 上下文归属切换均 reset；失败尝试不确认历史。
历史消费者同时持有该次求值的 producer ticket；执行前取消后，下次求值须 reset。
NRD 内部池的取消恢复与在途退休规则见 [NRD 帧输入](nrd.md#帧输入)。
执行后提交确认失败在内部映射为 `SubmissionFailed`，旧 NRD 池保留至实际 fence 完成；
后续新帧可在旧工作仍执行时创建独立池恢复，不能通过提前释放旧池完成恢复。
JVM 继续提供世界/几何/相机等失效事件，native 独立 SR 保存自己的历史消费者。

录制阶段只写 pending 状态。`ExecuteCommandLists` 成功后，所属 queue 的两个 `Signal` 都成功，
且最后一次降噪/SR 结果已经成功合光，native 才提交曝光时间戳、RR/NRD/SR history 和当前输出资格；任一 Signal、异步提交或录制取消失败，
pending 状态全部丢弃，已提交资源仍按实际 fence 保活。JVM 上一帧 view projection、view matrix 与 jitter
也只在同一提交确认点更新，且必须匹配所属 `recordingIdentity`；同步提交返回 0 不得确认，其他编码器不得代为确认或取消。
省略合光的已执行录制不确认历史；细粒度接口在合光之后追加降噪或 SR 时，必须重新合光才能确认新结果。
双 Signal 成功是提交确认，不是 GPU 完成；资源退休仍须实际 fence 完成。
下一帧开始前结算上一帧异步提交：观察超时保留所有权并阻止重录、重提和销毁，终态失败才取消对应 pending previous state。
这条提交边界修复了“录制成功但列表未确认”语义，不能替代旧区块回访噪点、卡顿或 SDK 内部重建的世界内验收。

`mcdr_rt_denoise_output` 为 48 B：frameId、snapshotId、diffuse/specular 借用句柄、
已分配输出字节数、encoding、dispatchCount。encoding=1 为 REBLUR 双输出，2 为线性单输出。
dispatchCount 包括成功 NRD/RR 派发及本帧回退编码，不包含追踪入口的六阶段。
byteCount 只统计缓存的降噪输出，和帧入口 byteCount 分开累计；不含 SDK 内部池。
仅成功才发布。替换的输出和释放上下文的资源按实际 fence 退休。
降噪失败时 native 使用本帧 raw 合光，禁止复用上帧输出。

## 7. 独立 SR

`mcdr_dx12RtFrameUpscale(ctx, frame, frameId, backend, output)` 接受 DLSS=1、FSR=2、XeSS=3。
它只接受当前帧已成功 NRD 求值、相同 encoder 和未提交列表；RR 成功、RAW、过期帧、重复调用均拒绝。
预检失败允许更正参数，进入资源分配后每帧只尝试一次。near 必须为正，far 大于 near，FOV 在 `(0,π)`，aspect 为正。
相机数据在降噪入口复制，SR 不再借用 JVM 的 CPU 指针；同帧 jitter 与历史事件保持一致。
必须已准备当前 source 的全色降级输入；缺失时返回 `rt sr: current raster source required`，允许提供输入后重试。
细粒度调用方先用同帧 source 执行一次 Composite，再执行 Upscale 和最终 Composite；统一事务自动满足此条件。

native 拥有 render 尺寸 RGBA16F 颜色、R32F 设备深度、display 尺寸 RGBA16F 输出。
输出使用 `storage | texture binding | render attachment`（44），允许 XeSS/FSR 返回 RENDER_TARGET。
三张纹理成组分配、成功后替换；旧资源与失败尝试中的临时资源均按实际录制 fence 退休。
`RtNrdRemodulate` 以完整混合 HDR 为基底，分别用过滤值替换原始 direct + indirect 与镜面贡献。
t9/t10/t11 为原始 indirect/specular/grazing，t12 为 direct；只有对应显示深度有效才读取该路 SDK 输出。
primary/材质无效时保留同帧 source 的整像素回退。四邻域重采样同样拒读排除区；材质、原始 viewZ 和 MV 均来自同一次追踪。
XeSS/FSR 的设备深度按需转 reverse-Z，MV 使用像素单位和 `MCDR_SR_FLAG_UNJITTERED_MV`；DLSS 使用 viewport 2。
帧间时间来自帧录制的 `frameNanos`，首帧或回退为 16.7 ms，其余限制在 1–1000 ms。
本次时间戳先暂存，只有 SR 历史在最终合光及提交确认后成功提交，才推进已确认的 SR 时间戳。

SR 历史独立保存代际/序号；Plan、后端、尺寸、代际、帧间断或 SDK 归属变化均 reset。
失败不确认 SR 历史，在当前 Plan 内停止重试；Plan/尺寸/后端改变允许重试。
SR 失败不清除已成功 NRD 的历史和本帧输出，native 继续合光当前 NRD 结果。
成功返回 48 B `mcdr_rt_denoise_output`，encoding=2、specular=0，byteCount 仅统计三张 SR 纹理，
dispatchCount 包含一次重调制和成功 SDK 求值；它们分别与降噪计数累加。
输出为借用句柄，不得由 JVM 销毁；native provider 的内部资源仍由共享 SDK 管理层负责退休。

XeSS/FSR provider 使用 `SrContextCompletion` 保存全部实际录制，外来 Pending 在资源接管前拒绝。
未执行取消后的同模式恢复也重建上下文，更晚取消不得覆盖更早提交。
执行后 Signal 失败只请求一次 reset，使用仍保活至原 fence 完成；SDK execute 错误不消费 reset。
重建、更换后端与关闭共用实际使用的退休检查，Pending 立即拒绝，实际提交的总等待上限为五秒；
超时保留实例和资源，禁止超时后继续释放。详见 [超分 §4.3](upscalers.md#43-实际录制与上下文退休)。

RR 与独立 DLSS-SR 的 Streamline feature 使用由底层桥接保存实际 `CommandRecording` 集合。
RR/SR 的物理 SDK frame token 在每次求值时递增；失败不复用已预留编号，也不因此推进逻辑已提交历史。
同模式也拒绝外来未提交录制；取消仅退休尚未执行的使用，执行后提交失败仍按 fence 保活。
更晚的录制取消不覆盖更早的在途提交。曝光资源模式切换须等全部使用完成，共享五秒等待上限，之后才能释放 feature。
取消含 SDK 使用的未提交录制后，恢复同模式也必须等待全部实际提交完成并重建 feature，
避免沿用 SDK 内部已前进、GPU 却未执行的资源状态；等待共享相同五秒上限。
这些门控发生在资源接管之前；SDK tag/evaluate 阶段失败仍保留录制，下一次请求 reset。
每份 SubmissionFailed 使用只在首次观察时请求一次 reset，后续轮询仅保留 GPU 生命周期。
成功恢复可在同一录制中继续正常积累；后续录制取消或 SDK 错误仍分别请求 reset。
录制取消后的恢复、内部 reset 常量与失败提交边界应分别验证，不能将帧输出资格检查当作 SDK 内部历史已经提交。
`rt_sdk_history_check.py` 转发并记录真实 RR/SR SDK 常量和 feature 释放入口，验证同模式取消及旧提交保活、
两处 Signal 故障，以及实际 SDK 成功后注入 constants/tag/evaluate 返回错误时的资源归还与恢复。
该独立检查复用已有 JVM 输入，不包含世界画质或其他 SR 上下文的事务验收。

光栅 DLSS 的 viewport 0 / NGX 由独立实际录制集合管理，不能借用本节 RT viewport 的通过结果。
绑定和录制预检先于 render pass 结束，常量拒绝不得改写原 GUI 输出；取消恢复也须等待旧提交并重建 feature。
设备清理前检查光栅 feature 的真实使用与 SDK 释放结果，失败保留整个设备。
独立命令与证据边界见 [DLSS §4.9](dlss.md#49-光栅录制与-feature-退休)。

## 8. 合光与借用输出

`mcdr_dx12RtFrameComposite(ctx, frame, frameId, source, debugMode, output)` 读取 native 当前帧资源，
不接受 JVM 指定的降噪纹理。成功 NRD 发布 REBLUR 双输出，RR/SR 成功发布线性单输出；
新帧开始录制时清空该选择，降噪失败使用 raw，SR 失败保留本帧 NRD。

source 必须是可采样的二维单层、单 mip、单采样颜色纹理，不得是深度或当前合光输出。
输出尺寸取 source，且不小于 trace。frameId 与未提交录制 ticket 必须与当前帧相同。
细粒度合光可在同帧重复录制调试图；debugMode 为 0–10，写入 b1 的第 4 个 dword。
后两个 dword 为追踪入口保存的 contentJitter，RAW/降噪失败也使用当前偏移。
RGB 曝光与显示变换只执行一次，原版直通颜色按输出像素读取。
缺 primary 或 G-Buffer 覆盖时，最终整像素交给本次 source；次级缺口保留有效 RT 并只替换缺失项。
raw 邻点按距离、材质与各项来源筛选并逐项归一化。NRD 输出按各自当前 viewZ 门控，
没有合法 SDK 深度时保留该项 raw 估计，其它项独立消费；不读取排除区内容，不重复叠加完整 source。
NRD 双输出按稳定 `outputUv * denoisedSize - 0.5` 位置进行深度过滤的双线性采样；
不得叠加 contentJitter 或用最近邻放大。原始 guide/raw 的取样位置仍包含当前内容偏移。
独立 SR 的输入重调制只对 NRD 照明应用一次 `pixel-contentJitter`，与当前材质/深度同相；
RR/SR 已完成重建的颜色按稳定输出坐标读取。

native 持有 RGBA16F 合光输出和 PSO，输出 usage=44；尺寸改变先创建替代纹理，完成录制才发布。
没有降噪时 t6/t10 绑定当前 emissive 作为未使用的有效 SRV，不需要额外占位纹理。
t12/t13 绑定当前 diffuse/specular viewZ；RAW 未分配 guide 时绑定不会取样的有效曝光纹理。
返回 48 B 结构的 diffuse 为借用输出，specular=0，encoding 描述本次使用的输入，byteCount 仅统计合光纹理。
JVM 使用带 frameId/尺寸的 `CompositeImage`，核对它仍是最新发布对象后才绘制 BLAS 线框或 blit；
不得包装成会销毁 native 资源的 `D3D12GpuTexture`。合光失败、帧失效、尺寸不匹配时保持原版目标。
普通法线/粗糙度调试纹理仍由 JVM 管理，与生产合光输出分开。

## 9. 校验与定位

统一事务检查使用独立临时 runtime 与指定 target DLL，不部署到游戏目录：

```powershell
python _port/rt_native_frame_check.py --transaction-only --no-runtime-deploy --native-dll MCDR/target/x64_Release/MCDR.dll
python _port/rt_native_frame_check.py --transaction-rr-only --no-runtime-deploy --native-dll MCDR/target/x64_Release/MCDR.dll
python _port/rt_world_geometry_check.py --frame-transaction-only --skip-product-compile --no-runtime-deploy --native-dll MCDR/target/x64_Release/MCDR.dll
```

第一组实际执行 NRD、XeSS、回退与四个 GPU 时间戳，覆盖预检重试、取消、SDK 后合光分配失败、双 Signal 故障、
旧合光不能确认新重建历史，以及阻塞队列中释放后最终像素读回。
RR 组转发真实 Streamline 常量/求值，验证连续历史、取消、SDK 后最终合光失败和两处 Signal 故障后的 reset；不接受回退冒充 RR 成功。
JVM 组执行实际 FFM downcall/upcall，验证 248/288 B 打包/解析、独立输入副本、拒绝结果与历史发布；它不执行 GPU 或世界渲染。
这些检查不替代原存档、完整 Mixin 客户端、事件矩阵与性能验收。

工作区 `python _port/rt_native_frame_check.py` 使用实际包内 DXR、材质/运动/打包/曝光/信号程序、真实 BLAS/TLAS，检查
5 次寿命录制、28 次曝光录制、40 次 guide 录制、12 次信号读回、48 类帧拒绝和 25 次回退拒绝。它阻塞 GPU 队列，覆写调用方常量，
并在 GPU 完成前与主列表关闭前分别释放上下文，随后回读命中距离、辐亮度、材质、常量与曝光。
曝光参考由回读的 raw/guide 重建亮度，独立检查时间适应、设置/版本切换、尺寸变化、时间戳边界、空覆盖及拒绝后的重试。
guide 检查覆盖 32 相 Halton 抵消与屏幕运动、RAW 开关和尺寸切换、无效深度/法线及实际 viewZ/packed-normal 读回。
信号参考由实际 raw、材质、viewZ 独立计算，检查线性 RGB、YCoCg、两路归一化距离及 RR 世界单位反射距离；
覆盖同帧 RR 回退、RAW/NRD/RR 切换、空覆盖和尺寸变化，拒绝过期/重复/已提交/错误 encoder 的回退请求。
调试层检查 error/corruption；该探针不替代游戏画面、性能或降噪验收。

`python _port/rt_acceleration_check.py` 检查 36 类 AS 拒绝、62 次实例表读取、完整 64-bit section 键、
BLAS 共享、不可变实例表、失效代理和集合重排；分别阻塞几何与 AS 生产者，验证跨 encoder 的未提交/在途拒绝与完成后重试。
撤销几何令牌后已有 AS 仍可构建，撤销 AS 后在途命令正常完成；调试层不得出现 error/corruption。
世界几何 GPU 探针另验证实际 RayQuery、分批预算、共享候选取消、世界切换和产品 DXR 失效代理路径。
scene 检查覆盖 11 项生命周期条件与 20 类拒绝：版本表自动失效、完整替换、空场景、跨世界不共享、
scene 自有 AS 的外部修改拒绝、重建后的版本单调性、版本表不变性及撤销后的阻塞退休。
56 项驻留检查覆盖热返回、同内容 live 重入、新版本、候选替换、真实帧预算、缓存容量及在途资源计量，
以及热 section 与冷条目混合、同 section 多 layer 原子发布、遗漏 live layer 的代理和同 snapshot 续建。

`python _port/rt_acceleration_check.py --submission-budget-only` 单独执行 31 项 scene 提交预算检查：
两种 Signal 失败时旧 GPU 工作保持阻塞，另一 encoder 的同帧恢复不能重新领取已执行构建额度；
未知几何仍发布代理，恢复提交不等待旧工作完成。实际完成后同帧额度仍耗尽，下一真实帧重新取得 16 条额度。
执行前取消另验退款、同帧重试和重复 Prepare 不得重复退款；两次实例表读取和调试层检查随真实提交执行。
完整 acceleration 探针也执行此组检查。

G-Buffer 另检查 17 次生命周期拒绝、18 次清屏附件读回和 6 次退休附件读回，包含取消、重复消费、
时间戳/尺寸/encoder/fence 不匹配、同尺寸覆盖清屏与改尺寸后在阻塞队列上释放。
JVM 探针调用真实 Begin/Replay/End 和外部根签名 mesh PSO，三帧九次绘制分别回读 459 个颜色与 guide 像素，
验证反向深度、交替 PSO、CBV/纹理/RAW/texel 绑定、投影/相机常量与 CPU 覆写后的快照。
错误批次整组预检后回读 153 个清零像素；还检查资源身份复用、旧借用拒绝、取消不发布和真实 blit。
降噪/SR 的 fixture 覆盖范围另行记录。

SR 检查执行六帧真实 XeSS 和两帧不可用 DLSS，覆盖复制后的相机数据、后端/尺寸/帧间断、
NRD 历史保持、输出有限非零读回，以及阻塞 GPU 时开列表释放；另检查 18 次错误请求。
真实 DLSS/FSR 求值、逐事件 SR 复位效果和世界内表现须单独验证；不得将 JVM fixture 视为 SDK 成功。

合光部分检查 25 次 GPU 输出与 19 次拒绝，覆盖 NRD、SR、SR 失败回退、NRD→RAW、RAW 非零 jitter、
十一种 debugMode、NRD 初始化失败/恢复、尺寸替换及阻塞队列上释放。参考为同一 HLSL 手动绑定同帧输入，逐像素比较只验证接线；
不是跨降噪方案一致性或最终画质验收。JVM 探针另验证借用句柄发布、旧帧/错误尺寸拒绝和真实 blit。

降噪部分执行 10 帧真实 NRD/无 Streamline 环境的 RR 回退，读回双输出，检查初始化、
连续积累、代际/帧间断/尺寸复位、重复求值及 19 次拒绝，并在阻塞队列和未关闭列表下释放上下文。
正常阶段不得出现调试层错误。额外四帧用 8×8 输入触发 SDK 下采样池的零尺寸分配失败，
严格检查两次预期错误、同配置抑制、Plan 变化重试与 64×64 恢复；它们单独记录，不当成正常阶段错误。
JVM 发布/SR 路由探针使用 native 结果 fixture，不能证明 native SDK 求值；实际 RR 求值由独立 RR 事务检查覆盖，世界表现仍需开发客户端验证。

| 文件 | 作用 |
| --- | --- |
| `include/mcdr_abi.h` | 几何/AS、G-Buffer、帧、降噪/SR 与合光契约 |
| `src/core/d3d12_rt_frame.cpp` | 几何/AS 与帧资源、版本检查、曝光/SDK 历史、录制、降噪与回退 |
| `src/c_abi/abi_device.cpp` | C ABI 转发与异常边界 |
| `src/c_abi/abi_meta.cpp` | 结构体布局登记 |
| `src/core/d3d12_device.cpp` | 底层 dispatch、上传快照、延迟销毁与设备清理 |
