# native 集中渲染器契约

状态：确定的目标设计，当前源码尚未实现。本页定义新模块所有权与 C ABI 边界。

## 1. RenderPipeline

RenderPipeline 是唯一组装入口，创建业务模块、编译资源图并安排提交；不实现材质解码、遍历算法、SDK 参数换算或 LoD 编码。
模块声明读写、格式/尺寸、队列、history key 与外部 SDK 状态影响；集中编译依赖、barrier、资源别名和 fence。
外部 SDK 的 context、descriptor heap 恢复和输入寿命归对应 adapter，不散落在 device、surface 与 JVM 调用点。

| 模块 | 业务输出 | 独占状态与边界 |
| --- | --- | --- |
| SceneDatabase | SceneView、变更集和几何源数据 | 世界/对象/region/level 身份、内容版本、持久化 LoD 中间数据；不依赖 SDK 格式 |
| GeometryManager | GeometryView、SceneQuery、缺页请求 | 分层 DDA 驻留、BVH/MG、覆盖版本、OMM/RTXMU；不复制世界注册表 |
| MaterialSystem | MaterialView、MediumDefinitions | labPBR/光谱语义、NTC/RTXTF/RTXTS、动画、资产/模型版本；不决定曝光和光照缓存 |
| AtmosphereModule | EnvironmentView、有限区间 L/T | LUT、云/雾、天气与环境历史；不独立合成或 Present |
| TransportModule | LightingSignals 与各项 guides | BuiltinWG、RTXDI、NRC/FSR-RC/SHaRC、轨迹/介质/光谱输运；不调用重建或呈现 |
| ReconstructionModule | 显示尺寸 FrameOutput | NRD+SR / RR、分层恢复/合成、guide 转换和对应历史；不改场景 |
| PresentationModule | Present 与显示统计 | UI、FG/Reflex/XeLL、显示变换、交换链与资源租约；不重做世界光照 |

Work Graph 是内置光影生产调度；BVH 通过 inline RayQuery、简单实心块通过 DDA，共用最近命中规则。
精确域内不按距离把简单块转 BLAS；MG 表示精确域外的远景聚合，并保留屏外阴影/反射/GI 所需内容。
Cooperative Vector/LinAlg 为 RTXNS/NTC 和光谱网络提供真实推理；SDK 的 host 调用在图边界执行。

<a id="encapsulation"></a>
### 1.1 依赖和业务入口

`contracts` 定义版本化值/句柄/视图，不能依赖 Minecraft、D3D12、SDK 或业务实现。
模块只依赖公共契约、自己的私有实现与显式注入的最小资源/任务服务；业务间传值、只读视图和图资源，禁止取得对方可变 manager。
RenderPipeline 负责把这些入口接起来；模块不能反向取出整个 pipeline，也不使用全局 service locator 或装有所有业务的 RenderContext。
SDK adapter 接业务输入并返回声明输出，SDK 类型不出 adapter；通用 GPU/推理服务不决定训练目标、材质语义和缓存模式。
SceneView/MaterialView 分别由场景/材质发布；Geometry 消费场景与材质的只读几何/opacity 描述，Atmosphere 消费介质/帧/光源参数。
Transport 消费几何/材质/环境视图，Reconstruction 消费信号，Presentation 消费 FrameOutput/UI；下游不反向修改上游状态。

每个模块明确配置/能力、输入输出、增量更新、图声明、历史失效、退休、统计/错误入口。
SceneDatabase 和 MaterialSystem 分别是本域唯一写者；世界/材质更新经不可变变更集发布，帧使用冻结视图。
后台构建结果带 worldEpoch、内容/配置版本，过期结果不能发布；冻结视图通过版本和寿命实现，不每帧复制整份场景。
跨帧反馈经 previous/history 资源或下一批请求传递，不形成循环调用和循环编译依赖。

创建时验证配置与依赖；重配/重载先准备完整候选图，再在帧边界发布；未提交候选失败时只释放候选并报告错误。
旧图在显式替换前仍属于原配置，不能把新配置标为生效；关闭/切世界停止新提交、取消未发布任务，按实际 fence 退休资源。
需要停机重建的交换链/SDK 切换明确报告停止状态；资源销毁不得回调已销毁的业务模块。
业务错误保留模块、操作、输入版本和原因，统一向上报告；禁止吞错后偷偷更换 provider。

图运行时管理物理分配、预算、fence 和 HistoryController；业务模块管理逻辑资源与失效语义，SDK adapter 管内部 context。
共用 Streamline 会话由 pipeline 专属服务管理，重建和呈现只取得各自功能句柄；不分别创建相互争用的全局会话。

### 1.2 目标布局

公开模块接口放 `src/module/render/<domain>/`，实现放 `src/render/<domain>/`；domain 包含 contracts、pipeline、scene、
geometry、material、atmosphere、transport、reconstruction、presentation。SDK 实现归所属 domain 的 `adapters/<provider>/`。
RadianceCache 归 transport，TextureStreaming 归 material；WG node record/队列布局留在 transport 内部。
这些是目标目录，尚未创建对应源码。不要将新业务平铺到 device/surface、ABI 桥或无边界的 manager/helper/util 集合。
封装必须承担业务不变式、生命周期或 SDK 隔离，避免只有逐层转发的接口；热路径按图/模式特化，配置检查不进入逐光线循环。

## 2. 两条重建模式

`LightingSignals → NRD → 分项重调制/合成 → DLSS SR / FSR SR / XeSS SR → WorldColor`。
`LightingSignals + Guides → DLSS RR → WorldColor`。
RR 与 NRD 互斥，RR 不再叠另一轮 SR；世界 HDR 输出只有一个 owner。
查询 unknown 与显示估计分离，有效光照项独立保留；不把缺样本写成真实黑值，也不整像素读原版颜色掩盖缺口。
信号携带辐射域/单位、估计来源、置信度和 guide/history 引用；表面、透射、体积与 Kerr 贡献可使用不同重投影描述。
SDK adapter 只产生其公开 API 支持的输入；特殊层的独立恢复和合成位置写入支持组合表，不能假装首表面运动描述了所有贡献。
HistoryController 按 worldEpoch、view ID、内容/驻留/覆盖、材质/模型、光照、相机和尺寸变化通知真实消费者。
SDK 历史只经其公开 reset/confidence/disocclusion 接口失效；无局部协议时执行合法 reset，不修改 SDK 私有布局。

## 3. ABI 与资源

公开 ABI 采用 size/version、固定枚举/单位的 POD 描述和不透明句柄，保持 C linkage；C++ 模块私有类型不跨 ABI。
扩展记录声明类型/大小和是否必需，未知必需扩展报错；公共场景 schema 不包含 SDK 结构、GPU 地址、descriptor 或 cluster 索引。
JVM 调用聚合为创建/销毁、增量场景、图配置、帧执行和统计；低层 Blaze3D ABI 继续服务原版渲染抽象。
SceneDelta 在接收时取得完整所有权；异步任务不能持有已经关闭的 Java Arena。
geometry、AS、texture、frame、SDK history 共用实际分配预算，按最后使用 fence 回收；未提交与取消分别处理。
账本包含 AS scratch、上传、训练缓冲、重载/流送新旧副本和 SDK in-flight 资源；共享资源只计一次，另列每帧上传/构建/训练工作预算。
不会每帧重新扫描/排序所有来源、构造全量 FFM 描述或复制整份几何镜像。

### 3.1 可重建资产和查询边界

LoD 数据库保存几何中间表示、材质映射、误差界、来源和生成配方；MG cluster/AS 仅为派生缓存。
材质保留原图、颜色空间、通道/动画和授权；NTC/GPU 布局不能替代源资产。模型保存训练配方、归一化、谱基、支持域与版本。
缓存键包含源内容、schema、生成器和工具链；格式变化从中间数据重建，不要求玩家重新探索世界。

SceneQuery 返回稳定表面/材质 ID、距离、法线与覆盖/unknown 事实；内部 DDA 页号和 cluster 地址不得泄漏给着色消费者。
DDA 的页/跳空/压缩/任务组织在本模块优化，不能改变简单实心块的归属。
Transport 的 TrajectoryIntegrator 生成有限段和误差界，Geometry 查询段内交点；累计路径长度、局部段参数与屏幕 viewZ 分开。

## 4. 神经光谱和环境

路径状态包含 wavelength/PDF/throughput/medium；观察输出使用 380–780 nm，可见结果在声明的输出边界积成 XYZ/线性 RGB。
源发射/介质谱声明独立有效范围、单位、谱基和范围外规则，覆盖所选 Kerr 频移域，不把发射数据硬裁成观察波段。
光谱材质与 guiding 网络有训练配方、谱基、归一化、版本与 RTXNS 推理；RGB cache 不冒充波长辐亮度。
光谱来源标记 measured/authored/inferred；RGB 推断记录参考照明和模型域，不当成唯一真实光谱。
光谱域、RGB 缓存域和显示域的转换各有 owner、单位和版本；范围外访问需可诊断，不能静默延用错误模型。
大气/云使用共同有限区间 L/T，MG 与精确域共享世界单位；黑洞使用专用 Kerr 曲线路径节点和光谱频移。

## 5. 错误和构建

优先使用 SM6.9、WG/CoopVec；每个模块声明实际 shader profile、设备能力和资产要求，图编译聚合所选组合的硬门槛。
WG/CoopVec/MG 分别验证，锁定 Agility/DXC/Slang/SDK/驱动；SM6.9 编译成功不能代替设备验证。
支持组合表规定输运域、轨迹、缓存、重建和呈现的合法连接，不承诺任意开关组合可用。
只创建活跃图需要的 SDK context 和历史；缺能力、运行库或图编译失败时明确报错，不创建自动低 SM、无 WG 或换 provider 路径。
SDK include 留在模块全局片段或适当 TU；构建继续使用项目定义的 MSBuild/C++ 最新标准。
所有新实现同步更新 ABI 和对应模块文档；设计文档不代替构建、资源寿命和完整场景检查。

## 6. 变更与验证

公共语义、业务所有权、DDA/BVH/MG 分工和生产模式是不变式；内部页布局、节点拆分、SDK 编码和网络结构可在所属模块调整。
ABI/单位/寿命、持久格式、信号语义或支持设备集合改变时，更新契约、消费者、迁移和检查；不只增加版本号。
升级 NRD 不要求修改 Minecraft 采集，升级 MG 不要求重写材质源数据，替换光谱网络不要求修改 Present。

保留确定性采样、带版本/hash 的场景增量回放、分项光照/历史拒绝输出、关闭缓存的参考积分；不承诺跨 GPU 位级一致。
CPU/GPU marker 对应采集、上传、遍历、构建、训练、SDK、重建、呈现和等待，供 PIX/Nsight 检查。
检查真实 ABI/扩展、资源租约/退休、坐标/单位/PDF、历史失效及无环依赖/SDK 隔离，不能以源码字符串匹配证明行为正确。
模块交付同时包含这些诊断和契约检查，不按每个小步骤建立最小出图门槛；画面质量由实际场景和用户目视确认。
