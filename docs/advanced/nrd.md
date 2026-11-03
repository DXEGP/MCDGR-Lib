# NRD 双通道与 validation

NRD 后端使用 `REBLUR_DIFFUSE_SPECULAR`，在独立 D3D12 compute pass 中处理 diffuse 与 specular。
设备、纹理与命令列表由引擎提供；后端按 SDK 的 `InstanceDesc` 创建 PSO、永久池、瞬态池和描述符。
公开入口与七张帧纹理的顺序见 [ABI 契约](../abi/README.md)。

## 帧输入

`mcdr_nrd_frame` 是 native 内部结构，由 `src/advanced/mcdr_nrd_impl.h` 定义。
调用方提供已打开的命令列表、completion fence 和非零 completion value。
资源必须保持有效，直到对应 completion value 完成。

| 字段 | 用途 | 调用前状态 |
| --- | --- | --- |
| `motion` | 像素单位 MV | `NON_PIXEL_SHADER_RESOURCE` |
| `normalRoughness` | SDK 配置对应的 packed normal/roughness | `NON_PIXEL_SHADER_RESOURCE` |
| `viewZ` | 世界单位的正 viewZ | `NON_PIXEL_SHADER_RESOURCE` |
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
常态使用 `CONTINUE`，仅在 `resetHistory` 非零时使用 `CLEAR_AND_RESTART`。

`rt_jitter_check.py` 从 JVM 产品函数导出真实当前/上一帧偏移与无 jitter 矩阵，
执行 32 帧非零偏移 NRD 求值，并检查有限输出、接管/归还、恢复堆与 D3D12 debug layer。
这项 NRD 检查没有噪声收敛或偏移符号的画质预算；RR/SR 的符号负例不能替代 NRD 画质验收。

## SDK validation

独立验证探针将 `validationOutput` 指向与输入同尺寸的 UAV 纹理。
后端据此设置 `CommonSettings.enableValidation=true`，并将 `OUT_VALIDATION` 映射到该纹理。
传入空指针时关闭此 pass。引擎的七纹理公开入口将内部帧结构零初始化，因此默认不运行 validation。
该内部字段不改变公开 ABI 或 FFM 的纹理数量。

SDK 的 validation 输出是一张诊断图，alpha 表示透明度；它不是错误计数器。
保持这张纹理的内容，以便 SDK 的小视图跨帧积累。历史重置时 SDK 清零诊断图。
调用方负责外部纹理状态，后端逐 pass 跟踪永久池与瞬态池状态，并在每个派发后插入 UAV 屏障。
公开七纹理入口使用 [SDK 资源接管](../device/README.md#57-sdk-资源接管与归还)：
完整校验后批量转态并登记 owner；SDK 返回后重新声明入口状态，统一归还到 `COMMON`。
未初始化和派发槽位占满时也必须归还资源、恢复 draw heap。

验证同时启用 D3D12 debug layer，并执行以下检查：

1. 在相同输入与常态设置下交替开关 validation，启用时必须比相邻关闭帧多一个 pass。
   上下文首次使用包含额外资源清理，先完成预热再比较数量。
2. 保持 8 个尚未完成的提交占用槽位，第 9 次派发必须拒绝；改变尺寸或销毁上下文时保留在途资源。
3. 独立读回 diffuse、specular 与 validation，确认两路有色信号均符合预期。
4. 确认诊断图的所有分量有限，alpha 在 `[0,1]`，并且存在实际写出的非透明像素。
5. 收集 debug layer 消息，要求 corruption/error 为 0。

这些检查证明资源映射、validation 派发和诊断图写入有效；
噪点、残影及诊断图表达的可见现象仍需目视判断。

持续派发探针支持 `--size=1920x1080 --seconds=600`。尺寸范围为 16–4096，
持续时间范围为 0–3600 秒；不带参数时使用 64×32、256 个批次。
每批次录制八次派发并验证第九次被拒绝；持续模式至少完成 256 个批次，并保持实际派发直到指定时长。
诊断消息只存储 corruption/error，避免无关消息占满队列后丢弃后续错误。
离线持续派发仅验证该输入与尺寸下的后端资源、派发和读回，不代替世界内画质或窗口缩放验收。

## 深度、MV、法线空间与历史对照

在工作区根目录运行 `python _port/rt_nrd_temporal_check.py`。
增加 `--highlight` 同时验证实际 NRD 过滤后的镜面高光宽度。
增加 `--jitter-interface` 验证显式偏移入口；GPU 总门同时启用这两个参数。
探针经公开七纹理 ABI 调用实际 `REBLUR_DIFFUSE_SPECULAR`，不启动 Minecraft。
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
| `src/core/d3d12_device.cpp` | 七纹理公开入口、外部状态过渡与绘制描述符堆恢复 |
| `external/nrd/Include/NRDSettings.h` | SDK `CommonSettings` 与 `ReblurSettings` |
| `external/nrd/Include/NRDDescs.h` | SDK `OUT_VALIDATION` 资源契约 |
