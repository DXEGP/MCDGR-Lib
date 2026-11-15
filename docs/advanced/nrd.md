# NRD 双通道与 validation

NRD 后端在同一 SDK instance 内注册 identifier 0 的 `REBLUR_DIFFUSE` 与 identifier 1 的 `REBLUR_SPECULAR`，
分别处理 diffuse 与 specular，独立维护各自的历史和当前输入深度。
设备、纹理与命令列表由引擎提供；后端按 SDK 的 `InstanceDesc` 创建 PSO、永久池、瞬态池和描述符。
公开入口的七张必需纹理、独立镜面深度、历史 confidence 与撤销 mask 顺序见 [ABI 契约](../abi/README.md)。

## 历史 confidence

十/十一纹理调用在八纹理布局末尾增加 diffuse、specular 两张 confidence，分别映射
`IN_DIFF_CONFIDENCE` / `IN_SPEC_CONFIDENCE`；同时启用 `CommonSettings.isHistoryConfidenceAvailable`。
两张纹理必须成对提供，为同当前深度尺寸的 `R8_UNORM`、`R16_FLOAT` 或 `R32_FLOAT` SRV，
单层、单 mip、单采样，无资源别名。其它纹理数量、空指针、格式与尺寸错误在资源接管前拒绝。
调用方保持值在 `[0,1]`，资源寿命覆盖真实录制与 completion fence；SDK 返回后统一归还 `COMMON`。

confidence 表达当前时刻对上一帧历史的可信度，按上一帧图像坐标生成。
SDK 在 surface/virtual reprojection 的上一帧 UV 读取，不能直接上传当前屏幕矩形作为同坐标 mask。
1 保留常规累积，0 降低历史权重；当前 REBLUR 的零 confidence 仍可能消费旧历史，
不得将它声明为严格局部清空、当前 missing 排除或经编辑认证的照明依赖撤销。
七/八/九纹理调用关闭此可选输入，切换布局不隐式设置全屏 `resetHistory`。

生产统一 RT 帧保持八纹理布局；携带内容事件与当前路径支持时，内部另行生成累积历史撤销 mask，
见下节“累积内容依赖”。RR/独立 SR 的对应接入与世界体验仍待验证。
`python _port/rt_nrd_eligibility_check.py --history-confidence` 单独执行真实 NRD 检查：
分路及局部 confidence、恢复、七/八/十纹理切换、当前深度排除、三种标量格式与非法输入归还。
输出差异用于诊断实际通道响应，不要求成图一致，不替代用户对世界画质的判断。

## 逐 texel 历史撤销

九纹理调用在八纹理后追加整数 mask；十一纹理调用在十纹理 confidence 布局后追加 mask。
mask 按上一帧历史图像坐标生成，bit 0 撤销 diffuse，bit 1 撤销 specular；其它位忽略。
支持与当前深度同尺寸、单层、单 mip、单采样的 `R8_UINT` / `R32_UINT` SRV，禁止资源别名。
格式、尺寸或绑定错误在公开入口接管前拒绝；全部输入随实际录制与 completion fence 保活，返回后归还 `COMMON`。

两个 identifier 各在实际 `REBLUR_TemporalAccumulation` 前运行一次撤销派发。
从 dispatch 的 SRV/UAV 语义位置获取永久池资源，核对数量、类型与格式，不固定永久池下标。
对 mask 外扩两像素后的 texel，将历史 viewZ 写到 500000 范围外，清除累积长度及慢/快辐射历史；
镜面同时清除 tracking 的 ping/pong。外扩覆盖 Catmull-Rom 外沿，避免只检查 virtual 中心资格时仍混入旧值。
只撤销历史资格，不清当前深度或当前样本，不将 unknown 变成有效黑值；零 mask 保持原累积。

这个接口保证指定历史 texel 的旧辐射被移除，不能单独保证对应当前像素的最终输出只含当前样本。
REBLUR 的空间滤波仍可引入 mask 外的合法历史；调用者必须生成覆盖真实累积依赖的完整 mask，
包括 primary、过去随机路径、surface/virtual 重投影与空间传播。最后一个 raw 帧或当前编辑矩形不足以证明它。
生产统一事务可由累积内容依赖生成内部 mask；公开九/十一纹理入口仍由调用方提供 mask。
世界编辑与流送效果、RR/SR 历史适配分别验证。

撤销 PSO 与 root signature 由 NRD context 持有，首次需要时编译 `cs_6_9`，沿既有 context 退休规则保活。
每个 dispatch slot 额外保留 14 个描述符，不增加纹理池或常量上传环。
布局不受支持或管线创建失败时明确拒绝，归还外部资源；SDK CPU 状态已推进时将下一次求值标为重新开始，
不能静默忽略 mask 或将失败替换为零 confidence。有效局部 mask 和布局切换不增加全屏 reset。

`python -B _port/rt_nrd_eligibility_check.py --history-rejection` 执行实际 NRD 检查，
覆盖旧高亮污染、分 lobe 撤销、隔离表面的左右保留、surface motion、当前缺口恢复、
两种整数格式、两种公开入口、取消录制、非法绑定及 131×67 非整线程组尺寸。
同平面的空间反馈单列诊断，不将输入 mask 与最终像素范围混为一谈，也不要求不同 provider 成图一致。
`python -B _port/rt_nrd_retirement_check.py` 验证真正阻塞队列后的管线、root signature 和永久池保活，
以及执行后失败与执行前取消；独立探针使用 Agility 619 和产品撤销 shader。

## 累积内容依赖

携带内容事件的统一 RT 事务将当前 primary/direct/diffuse/specular 路径支持与事件表交给内部 NRD 后端，
保持公开八纹理布局与 ABI 45。diffuse 初始依赖合并 primary、direct、diffuse；specular 合并 primary、specular。
法线、深度、motion 等 guide 使用 primary 支持，确保主表面变化也能影响两个照明估计。

`scripts/build-nrd-dependencies.py` 为当前 NRD 4.18 的 REBLUR 生成 21 个跟踪 shader，
保留 SDK 的计算，并为实际纹理读取、采样和输出追加依赖传播。原始第三方源码保持在 `external/nrd`。
Load/索引读取按实际位置查询，双线性 Sample/Gather 覆盖四个邻点；同线程组读取的支持保守合并到实际写出的 tile。
支持 surface/virtual reprojection 和空间滤波时使用的真实输入坐标，不将当前屏幕矩形直接套到历史。

永久池、瞬态池及外部双输出分别登记依赖。每条记录为 32 B world-space AABB 与状态，
tile 尺度为 `max(1,64/downsampleFactor)`；只有完整覆盖该 tile 的写入才替换旧支持，部分写入继续并入旧依赖。
每个 pass 前冻结读取副本，写入另一份缓冲，避免跨线程组原地修改读取依赖。
依赖随实际保留的数据继续传播，当前帧不再查询某条路径时也不会立即丢弃它。

每帧 temporal 之前，把当前内容事件与永久池历史依赖相交，按 lobe 生成 previous 图像坐标的 `R32_UINT` mask，
调用既有永久池撤销派发，同时清理对应旧依赖。无事件的帧不制造撤销；局部事件不自动全屏 reset。
首次启用、关闭跟踪或 world epoch 改变时重新开始相应 SDK 历史，禁止复用没有配套依赖的累积结果。

首次使用按共享准入创建两份 DEFAULT 依赖缓冲、一张整数 mask 和 262144 B UPLOAD 常量环，
每槽比公开撤销布局多一个描述符；实际 committed 字节分别计入 frame GPU/host。
两份内部 buffer 在每次 dispatch 返回前归还 `COMMON`，避免提交后的 buffer decay 使下一列表的状态记录失真。
分配前缀失败全部退还，返回 `MCDR_NRD_MEMORY_PRESSURE` 的调用尚未推进 SDK 工作，可恢复预算后重试。
池、mask、常量及跟踪 PSO/root signature 共用 context 的录制/fence 退休规则。

MSBuild 的 `BuildNrdDependencies` 在 C++ 编译前生成 `build/nrd-dependencies.h`。
脚本按 SDK shader、跟踪源码、DXC 和 MathLib 输入指纹复用结果；首次构建下载固定 SHA256 的 MathLib v11。
临时预处理和 DXIL 在工作区顶层生成并自动删除；只保留生成头、指纹清单和可复用的 MathLib。
不支持的 shader 操作、SDK 布局或管线显式拒绝；不以零 confidence 冒充已处理内容事件。

`python -B _port/rt_nrd_retirement_check.py --dependencies` 检查实际多帧 NRD：
旧高亮路径停止采样两帧后仍被事件撤销、motion/空间传播、当前缺口恢复、独立 lobe、远 tile 保留及 131×67 边缘。
同一入口覆盖四个分配前缀的背压重试、阻塞队列上的执行后失败/关闭与执行前取消。

这是当前 NRD 设置的工程接入。保守 AABB、线程组与 tile 合并可能扩大撤销范围；
额外传播派发尚未完成世界性能验证。RR 与独立 SR 的内部历史没有接入此依赖，
普通实体、全部生产者和通用逐 lobe 缺口估计继续按各自范围实现；世界画质由用户判断。

## 当前输入排除

七纹理调用共用 `viewZ`，兼容已有 FFM 调用。八纹理调用在末尾增加独立 `specularViewZ`；
两张深度必须为同尺寸、单层、单 mip、单采样的 `R32_FLOAT` SRV，禁止与其它资源别名。
identifier 0 的全部 `IN_VIEWZ` 绑定 diffuse 深度，identifier 1 绑定镜面深度；
不能按两个 identifier 共用的 pipeline 下标选择深度。

`CommonSettings.denoisingRange=500000`。按 SDK 契约，当前缺样本可写范围外的 viewZ 排除对应 lobe，
另一 lobe 仍消费当前有效输入。不要将 history confidence 当作当前 missing 掩码。
范围外像素的 SDK 输出可能未写，消费者必须按同帧、同 lobe 的资格拒读；
不能因求值成功就把整张输出发布为所有像素有效，也不能将占位 RGB 当作可靠黑样本。

生产 RT 帧使用八纹理与独立 diffuse/specular 当前深度。次级缺口保留可靠主表面与其它信号，
缺 direct/diffuse 时排除 diffuse 当前输入，缺 specular 时独立排除 specular。
曝光只消费完整当前照明；最终缺任一信号的像素由当前 source 整像素所有。
RR 和独立 SR 使用转换到 RT 辐射域的同帧光栅全色降级，不将 unknown 占位黑值当作当前样本。
该降级有偏，不恢复未知 Monte Carlo 样本或已裁剪 HDR；数据就绪后重新使用当前 RT 估计。
合光与 SR 重调制均按当前 lobe 深度拒读排除区；完整策略见 [帧资源与重建](rt-frame.md#5-运动与降噪-guide)。
完整逐 lobe 照明恢复、动态历史依赖和世界体验仍须分别验证。
`python _port/rt_nrd_eligibility_check.py` 经实际 DLL 检查独立/同时/局部缺口、
排除区高亮占位变化、当前恢复、显式编辑 reset、七纹理兼容和资源归还。
`rt_pending_frame_check.py --consumer-eligibility` 经实际统一事务检查当前缺口、恢复与最终归属；
`--consumer-sr` 以 compute 污染被排除的 NRD 输出后执行实际 XeSS，`--consumer-rr` 单独执行真实 RR。
各 provider 独立检查输入与求值，不要求重建成图数值一致；世界画质由用户目视判断。

## 受管分配

永久池、瞬态池的 committed 纹理与常量 UPLOAD 环计入 RT 共享内存账本，
分别归入 frame GPU 和 frame host，受 `[rt] managedMemoryMiB` 约束。
内部初始化入口必须提供 `McdrNrdReserveMemory`；生产入口与公开 `mcdr_dx12NrdInit`
都使用设备模块的 `reserveRtNrdMemory`，不允许绕过准入。
按实际 `D3D12_RESOURCE_DESC` 查询分配字节，在调用驱动创建前取得租约。
纹理池和常量环全部准入后才创建 root signature 与 PSO，预算不足时不编译管线。

每份资源与租约由同一个 `NrdResource` 持有，先释放资源、再归还额度。
初始化失败时未执行的前缀可立即销毁；退休上下文继续持有全部池与租约，
遵守下节的实际录制/fence 状态，不因 shutdown、取消输出或换尺寸提前退账。
生产帧创建与 G-Buffer 开始前非阻塞收集已完成的退休池，避免共享背压挡住后续初始化；
查询账本本身不触发收集，不等待 GPU。

内部初始化以 `MCDR_NRD_MEMORY_PRESSURE=-2` 区分预算拒绝，其它初始化失败返回 -1。
公开 `mcdr_dx12NrdInit` 将预算拒绝映射为原有 -1，具体原因写入 `outText`，保持公开返回契约。
生产降噪在预算拒绝时清零输出，保留可重试资格；下一帧即使尺寸和 plan 不变也可重试。
同一帧的降噪尝试仍按原协议消费输入资格，不能重复提交该尝试。
无效池尺寸/格式在查询分配或调用驱动前拒绝，不将参数错误记为内存背压。
描述符、PSO、NRD 的 CPU 对象与其它 SDK 的内部资源尚未计入该账本；
24 值布局和总账范围见 [RT 受管分配](rt-frame.md#17-native-rt-受管分配总账)。

`python _port/rt_native_frame_check.py --nrd-memory-only` 用真实 DLL 检查逐分配准入、
驱动失败前缀、纹理/上传背压、同配置下一帧恢复、在途双池与物理退休。
`rt_nrd_retirement_check.py` 独立编译实际后端，检查执行后失败、取消和双通道 GPU 读回；
其分配回调为无预算 fixture，不单独证明生产账本正确。

## 帧输入

`mcdr_nrd_frame` 是 native 内部结构，由 `src/advanced/mcdr_nrd_impl.h` 定义。
调用方提供已打开的命令列表、completion fence 和非零 completion value。
资源必须保持有效，直到对应 completion value 完成。
生产桥额外传入共享录制 ticket 与状态查询，派发槽持有该 ticket。未提交录制不能因相同数值的 fence 完成而回收，
执行前取消的槽可释放；已执行的槽须等所属 fence 完成，device removal 的 `UINT64_MAX` 不算完成。
未携带 ticket 的独立内部探针仍由调用方保证提交和 fence 生命周期。

内部 `McdrNrdRecordingState` 将输出失效与 GPU 寿命分开：

| 状态 | 槽位与资源池 |
| --- | --- |
| `Pending` | 只属于原录制，即使 fence 数值已满足也不回收 |
| `Submitted` | 等所属 completion fence/value 完成 |
| `Cancelled` | 执行前取消，可直接回收，无需伪造 fence 完成 |
| `SubmissionFailed` | 工作已执行但提交确认失败；撤销输出，资源仍等实际 completion |

生产桥优先将 `submitted && cancelled` 映射为 `SubmissionFailed`；不能退化成执行前取消。
两种失败状态都会触发池退休与新池恢复；`SubmissionFailed` 不让旧池提前释放或复用。
缺少 fence 或完成值为 `UINT64_MAX` 时不能判定已执行槽完成。

初始化检查发现池内存在取消录制时，退休整个 NRD 上下文并创建新池；其它仍在途的槽继续保护旧池资源。
新池第一次成功派发强制 `CLEAR_AND_RESTART`，后续按显式 `resetHistory` 决定。
这同时恢复 SDK 的 CPU 历史和永久/瞬态池的资源状态；仅设置历史 reset 不能修正未执行的 barrier。
不同未提交 ticket 不允许共享同一池；同 ticket 的顺序派发仍可用。槽位忙和其它录制未提交的拒绝发生在 SDK 设置/派发计算之前。

NRD 的 history 与输出资格在录制期间只保留 pending ticket。`ExecuteCommandLists` 成功后，所属 queue 的两个
`Signal` 都成功才确认该 ticket；任一 Signal、异步提交或录制取消失败都会取消 pending，不能把同值 fence
或下一次录制误认为已提交。已执行但尚未确认的槽位继续保留至实际 fence 完成，JVM 的上一帧矩阵与 jitter
在匹配 `recordingIdentity` 的提交确认点更新。异步观察超时保留编码器所有权，终态失败取消相应 pending，
其他编码器的提交不得确认或取消该 history。该契约只约束帧所有权与历史边界，不是世界内噪点、卡顿或收敛通过证据。

| 字段 | 用途 | 调用前状态 |
| --- | --- | --- |
| `motion` | 像素单位 MV | `NON_PIXEL_SHADER_RESOURCE` |
| `normalRoughness` | SDK 配置对应的 packed normal/roughness | `NON_PIXEL_SHADER_RESOURCE` |
| `viewZ` | 世界单位的正 viewZ | `NON_PIXEL_SHADER_RESOURCE` |
| `specularViewZ` | 可选独立镜面 viewZ，空指针时共用 `viewZ` | `NON_PIXEL_SHADER_RESOURCE` |
| `radianceIn` / `specularIn` | 各路 YCoCg 与归一化命中距离 | `NON_PIXEL_SHADER_RESOURCE` |
| `radianceOut` / `specularOut` | 两路降噪结果 | `UNORDERED_ACCESS` |
| `validationOutput` | 可选 SDK validation 输出，RGBA8 或更高精度 | `UNORDERED_ACCESS` |

投影与视图的当前/上一帧矩阵均为列主序，且不含 jitter。内部帧的 `cameraJitter[2]` 与
`cameraJitterPrev[2]` 逐分量传给同名 SDK 字段；MV 缩放为 `{1/width,1/height,0}`，
`isMotionVectorInWorldSpace=false`。输入只有二维屏幕像素位移，z 缩放必须为零，
让 NRD 从当前世界位置与上一帧视图矩阵推导 viewZ 变化；z=1 会把未提供的深度位移当成零。
`mcdr_dx12NrdDenoise` 将内部偏移零初始化；`mcdr_dx12NrdDenoiseTemporal` 单独接收四个偏移，
不会读取原 64-float 矩阵数组之后的内存。偏移必须有限且处于 `[-0.5,0.5]` 渲染像素，
按 NRD 的采样点约定直接传递，不改变符号或单位。非法值在接管纹理前返回 0。
当前 RT 帧使用显式入口；JVM 将光栅/PT 共用 Halton `(Jx,Jy)` 转为 `(-Jx,+Jy)`，
上一帧同式。SDK 矩阵及 MV 不含 jitter，启停使用统一历史复位。
启用降噪的 RT Plan 消费共享偏移，包括没有独立 SR 的 `NRD_SR` 100% 路径；
禁用 RT 或降噪时不由 RT 请求偏移。不要用上一帧 SDK 是否求值成功控制当前帧采样。
常态使用 `CONTINUE`，显式 `resetHistory` 非零或新池首次派发时使用 `CLEAR_AND_RESTART`。

`rt_jitter_check.py` 从 JVM 产品函数导出真实当前/上一帧偏移与无 jitter 矩阵，
执行 32 帧非零偏移 NRD 求值，并检查有限输出、接管/归还、恢复堆与 D3D12 debug layer。
这项 NRD 检查没有噪声收敛或偏移符号的画质预算；RR/SR 的符号负例不能替代 NRD 画质验收。

## SDK validation

独立验证探针将 `validationOutput` 指向与输入同尺寸的 UAV 纹理。
后端据此设置 `CommonSettings.enableValidation=true`，并将 `OUT_VALIDATION` 映射到该纹理。
传入空指针时关闭此 pass。引擎的七纹理公开入口将内部帧结构零初始化，因此默认不运行 validation。
该内部字段不改变公开 ABI 或 FFM 的函数签名，七/八纹理入口均默认关闭 validation。

SDK 的 validation 输出是一张诊断图，alpha 表示透明度；它不是错误计数器。
保持这张纹理的内容，以便 SDK 的小视图跨帧积累。历史重置时 SDK 清零诊断图。
调用方负责外部纹理状态，后端逐 pass 跟踪永久池与瞬态池状态，并在每个派发后插入 UAV 屏障。
公开七纹理入口使用 [SDK 资源接管](../device/README.md#57-sdk-资源接管与归还)：
完整校验后批量转态并登记 owner；SDK 返回后重新声明入口状态，统一归还到 `COMMON`。
未初始化和派发槽位占满时也必须归还资源、恢复 draw heap。

验证同时启用 D3D12 debug layer，并执行以下检查：

1. 在相同输入与常态设置下交替开关 validation，两个 identifier 各自追加一个 pass，
   预热后比关闭时多两个 pass；
   它们顺序写入同一诊断纹理，最终以镜面诊断为准。
2. 保持 8 个尚未完成的提交占用槽位，第 9 次派发必须拒绝；改变尺寸或销毁上下文时保留在途资源。
3. 独立读回 diffuse、specular 与 validation，确认两路有色信号均符合预期。
4. 确认诊断图的所有分量有限，alpha 在 `[0,1]`，并且存在实际写出的非透明像素。
5. 收集 debug layer 消息，要求 corruption/error 为 0。

这些检查证明资源映射、validation 派发和诊断图写入有效；
噪点、残影及诊断图表达的可见现象仍需目视判断。

在工作区运行 `python _port/rt_nrd_retirement_check.py` 验证执行后失败的资源池寿命。
探针编译实际 NRD 后端与生产桥的状态回调，使用阻塞 GPU 队列检查旧池保活、新池恢复、
完成后退休及执行前取消，并读回失败帧与恢复帧的 diffuse/specular；它不加载游戏，也不判定世界画质。
`rt_native_frame_check.py --submission-only` 另通过公开 ABI 注入两次 Signal 失败，
在旧 GPU 工作未完成时创建新帧上下文并提交 NRD 恢复，随后检查原输出与恢复输出可读。

持续派发探针支持 `--size=1920x1080 --seconds=600`。尺寸范围为 16–4096，
持续时间范围为 0–3600 秒；不带参数时使用 64×32、256 个批次。
每批次录制八次派发并验证第九次被拒绝；持续模式至少完成 256 个批次，并保持实际派发直到指定时长。
诊断消息只存储 corruption/error，避免无关消息占满队列后丢弃后续错误。
离线持续派发仅验证该输入与尺寸下的后端资源、派发和读回，不代替世界内画质或窗口缩放验收。

## 深度、MV、法线空间与历史对照

在工作区根目录运行 `python _port/rt_nrd_temporal_check.py`。
增加 `--highlight` 同时验证实际 NRD 过滤后的镜面高光宽度。
增加 `--jitter-interface` 验证显式偏移入口；GPU 总门同时启用这两个参数。
探针经公开七纹理 ABI 调用实际 `REBLUR_DIFFUSE` / `REBLUR_SPECULAR`，不启动 Minecraft。
使用 128×64 的 RGBA16F 信号与 packed normal/roughness、R32_FLOAT viewZ，
每组 24 帧；每帧完成 fence 后读回双通道，并要求全部分量有限。
输出报告为工作区的 `_tmp_s4_nrd_temporal.json`，包含设备、DLL SHA-256、参数、阈值、提交数与逐项数值。
使用 `--runtime-dir <目录>` 指定独立运行库目录，使用 `--report <文件>` 单独保存报告。
独立目录内的 `MCDR.dll`、D3D12Core、匹配版本的 D3D12SDKLayers 与 DXC 运行库必须同目录。

| 实验 | 正确输入 | 负例与诊断 |
| --- | --- | --- |
| 深度 | 两个倾斜平面、轴向相机平移，使用世界单位 viewZ | 同场景分别误喂 deviceZ 与 reverse deviceZ，记录双通道平均边缘 MSE 与相对亮度对比的 RMSE |
| 法线空间 | 相机在 0 与 0.45 rad 间旋转，输入固定世界法线 | 误喂随相机旋转的视图法线，记录双通道平均 MSE |
| MV | 平面上的世界位置辐亮度渐变，使用解析 `prevUV-curUV` 像素位移 | 分别翻转方向、误用归一化 UV 位移，记录平均与单路 MSE |
| 历史 reset | 预热后改变信号，执行 CLEAR_AND_RESTART | 与新建上下文、相同 SDK 帧号及相同输入的首帧比较，并记录继续累积的差异 |

这些图像数值与预算只进入 `image_diagnostics`，不决定工程通过状态。
工程检查要求 SDK 求值成功、输出有限、资源正确归还且 D3D12 调试层无 error/corruption。
不得从平均值推断每一路负例都具有同等敏感度，也不要求不同降噪器输出一致。
对照组固定 SDK 帧号与输入噪声序列，避免 SDK 空间采样旋转造成比较偏差。
时域场景中相机 jitter 恒为零，roughness 为 0.5。
额外的 8,192 个 GPU 样本将产品 `EncodeNormalRoughness101010` 与官方 `NRD_FrontEnd_PackNormalAndRoughness`
逐分量比较，并用官方 unpack 验证法线和 0.1–0.7 的七档线性粗糙度。
SDK 编码必须由运行库 inspect 确认为 `normal=2 / roughness=1`，否则测试直接失败。
平方与开方粗糙度作为错误输入，经官方解码后计算 GGX NDF 高光半宽；正确半宽必须与解析解一致，
且比平方输入大于 1.5 倍、比开方输入小于 1/1.5。
该部分验证官方解码与高光形状的数学关系，不是 NRD 时域过滤后的高光宽度验收。

`--highlight` 额外执行 864 次 NRD 求值：roughness 为 0.2/0.35/0.5，
每档使用三个固定噪声 seed，各编码/seed 独立重建上下文、匹配 SDK 帧号并累积 32 帧。
仅改变 guide 中的粗糙度编码，比较线性 roughness、误用平方与误用开方。
取中央八行镜面首通道（Y，输入为灰度）的平均 profile，测量基底以上半峰值处的全宽 FWHM。
独立 CPU 解析 GGX profile 与三次无噪声原始 GPU profile 的最大差必须小于 0.005。
过滤后的宽度误差、随粗糙度的变化及错误编码的差异均记录为图像诊断，
不将固定误差预算用于阻塞工程验收。
报告保留完整 profile、参考、峰值、profile MSE、参数、阈值、探针/HLSL 哈希及子项通过状态。
启用两个参数且工程检查通过时输出
`PASS nrd temporal: engineering checks passed, filtered_highlights=measured, jitter_interface=passed`。
这个实验只验证过滤后的宽度；峰值和能量不在判据内，错误编码也不保证每档、每个指标均更差。
它不代替世界内高光、噪点或鬼影的目视验收。

`--jitter-interface` 在匹配 SDK 帧号、信号与 reset 的八帧序列中比较原入口与显式零偏移入口，
记录两路完整读回的差异；再用八组非零偏移与边界值执行真实 SDK 求值，要求输出分量全部有限。
对四个分量分别测试 NaN、正/负 Inf、正/负 0.5001 与正/负 1.0，另测试空指针，
共 29 次拒绝。每次拒绝的资源状态、owner 与接管/归还/恢复堆计数必须保持不变。
报告的 `jitter_interface` 记录比较误差、实际偏移、输出哈希、非法输入及通过状态。
这仅覆盖接口兼容、范围验证与求值收尾；非零测试的合成信号没有匹配的采样投影，
不能证明 jitter 符号、共享序列、重投影或世界内画质正确。

此测试不覆盖完整非零 jitter 链路或曝光对照。
这些数值只验证 SDK 输入语义及历史行为，不能判断玩家可见的噪点或鬼影。

同一探针经 `mcdr_dx12QuerySdkResourceState` 检查每次 SDK 返回后的状态与累计接管/归还/恢复堆次数。
空纹理、重复输出、非纹理对象和缺少 UAV 标志的输出必须在转态前拒绝，原有纹理状态保持不变。
另外检查未初始化时的 SDK 拒绝，以及同一列表中八次派发后第九次被拒绝的收尾。
启用 D3D12 debug layer，要求 corruption/error 消息为空；成功派发后继续 compute 读取双通道。
这些额外调用不参与时域输入语义的对照，完成 fence 后重建 NRD 上下文再开始对照组。

## 文件导航

| 文件 | 职责 |
| --- | --- |
| `src/advanced/d3d12_nrd.cpp` | 上下文、资源映射、设置、派发和 fence 回收 |
| `src/advanced/mcdr_nrd_impl.h` | native 内部帧结构与后端入口 |
| `src/core/d3d12_device.cpp` | 公开入口、可选历史输入、外部状态过渡与绘制描述符堆恢复 |
| `external/nrd/Include/NRDSettings.h` | SDK `CommonSettings` 与 `ReblurSettings` |
| `external/nrd/Include/NRDDescs.h` | SDK `OUT_VALIDATION` 资源契约 |
