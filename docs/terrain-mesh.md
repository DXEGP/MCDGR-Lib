# 地形 Work Graph 与 meshlet 驻留

不透明、镂空和 OIT 地形的生产路径是 `prepareTerrainWorkGraph` → `drawTerrainWorkGraph`。
JVM 从原版 MDI 批次生成 section 范围，GPU 五节点图执行 section/meshlet 剔除、展开与可见记录压缩，
compute 终结器生成间接参数，`ExecuteIndirect` 调用 visible MS。普通有序混合管线保持 indexed indirect 顺序。

## 资源布局

每个 encoder 的 JVM 地形 owner 维护 DEFAULT heap 静态 atlas。checked buffer-copy ABI 上传新观察到的
不可变几何，全部拷贝录制成功后才发布新 atlas 与偏移。每个静态描述符为 16 B：量化最小边界/quad 数、
量化最大边界/方向位、局部顶点偏移和保留 uint；不包含临时 section 下标。

每批候选为 40 B/section，GPU 输出为 32 B 表头 + 32 B/section + 8 B/可见 meshlet。
mesh root bindings 是 t14 顶点流、t15 GPU 可见表、t16 静态 atlas、b8 globals 和 b9 可见切片起点。source/tile route 还保存完整 graphics binding 的 type/register 顺序；两侧布局、push constants、附件格式和 contract generation 不一致时拒绝登记。
每个间接切片至多 `2^22` 个 meshlet，以最多 256 列派发；尾组在 MS 中检查 GPU 可见数。
零可见的间接参数 X=0，不产生几何；GPU 容量错误将本批间接计数置零。
native 在计算切片数量前拒绝会使 `uint32_t` 向上取整加法回绕的容量，避免生成零长度间接参数。

## 录制与寿命

prepare/draw 在 native 内通过 `beginDeclaredPass`/`finishDeclaredPass` 导入资源，继承实际附件的父 token。
prepare 声明 uniform/globals、候选范围、atlas 及 visible/arguments 输出；draw 同时导入几何读取和
当前 mesh pipeline 实际绑定的材质 CBV/SRV，纹理保留真实 subresource 范围。mesh descriptor 与
root signature 必须传递同一组常规 binding 类型与顺序；JVM Prepared.draw 只调用一次 native 绘制。

通用 graphics command 采集的 VBO 读取范围使用实际 vertex offset 到 buffer 末尾的可读区间；已有 terrain capture 时沿用经过 metadata 校验的精确 vertex 区间。候选 section 表长度只描述候选输入，不能代替顶点字节范围。范围声明用于输入保活与内容检查，缺少完整事件的 packet 仍不可重放。

mesh PSO 的颜色附件依次为原 pass 附件、可选 OIT additional 附件、可选 raster motion 附件。
motion 必须位于最后，格式为完整写入的 `RGBA32_FLOAT`；`setPipeline` 校验 PSO 与当前附件数量一致。
切换 additional 附件保留已选中的 motion；解除 motion 注册保留 additional 附件。
motion 仅在单颜色 pass 的颜色、深度和 mip 与注册的 world 附件完全匹配时激活，
OIT accumulate pass 不会因存在 additional 附件而自动取得 world motion。

mesh PSO 按 motion 前各颜色附件的格式与写掩码计算是否写世界颜色。
Work Graph draw 仅在该标记有效时记录原 pass 的颜色写入；只写 motion 的 PSO 不更新该标记。

wrapped 与私有 raw 资源共用 `PassResourceUse`，同一 resource/subresource 的兼容只读状态统一合并。
UPLOAD 保持 `GENERIC_READ`；私有输出创建于 `COMMON`，基准初态仍经 context tracker 查询。
列表结束回到 `COMMON`，batch 不保存跨列表状态。prepare 只接受有效 Recording 帧的冻结版本，
draw 还校验 batch 的资源版本、context fence 和录制序号；祖先附件与子输入重叠时终止录制。

`TerrainRecordingLease` 把 program、batch 和输入 COM 引用保留在当前 context slot，
声明入口保留 wrapper/raw resource；draw 另保留 vertex、PSO、root signature 和 command signature。
JVM 关闭 Prepared 仅释放外部 batch 句柄，frame reset 不撤销 context 中尚未完成的录制租约。

atlas 替换和上传失败的 buffer 先登记到当前 encoder 的提交退休队列，只有对应 fence 完成后才释放；
这层 JVM 退休与 native 的录制租约同时生效。`flushPendingDeletes` 必须同时满足 queue fence/open-list
条件和 `recordingReferences == 0`；强制清理也不回收有录制引用的对象。slot 租约只在对应 per-context
fence 等待成功并 Reset 后清空。

encoder 的 `retainUntilCommandsClosed` 负责最终 atlas owner 的关闭；它不是扩容旧 atlas 的唯一保护。
关闭 encoder 时先等待 fence、销毁 context，再关闭 owner；等待失败保留资源。

## 能力与容量

JVM 接受原版按设备限制拆分的连续 indirect command 子范围：同 GPU buffer owner、完整位于采集范围内且按 20 B 对齐，原 command、geometry 与 primitive 顺序保持一致。prepared facts 在申请 native batch 前构建。JVM 的 FFM 参数分配按实际目标布局和大小选择缓存或调用内内存；上传资源仍按真实 context fence 退休。

terrain draw receipt 与 tile binning/replay 录制失败使用 `recordingFailed`，不把尚未提交的失败标成 `submissionFailed`；Work Graph draw 通过入口参数检查后，执行准备和 GPU 录制阶段的失败同时完成 graphics observer 的 `Failed` 回执。旧 `BatchV2/V3` 无法表达完整原版 OIT 事件顺序，公开准备入口返回 `UnsupportedContent`；低层三阶段 binning/replay 不作为完整四 lane executor 的成功结果。

JVM 在地形开启时检查 mesh shader、Work Graph Tier 1.0 和 SM 6.9，缺失即拒绝设备初始化。
原版 terrain 路由选择 MDI：D3D12 地形开启时不能被临时的 MDI=false 绕过，原版间接绘制能力缺失明确报错。

`quadsPerMeshlet` 支持 16/32/64，仅影响新捕获几何。生产编译三个 visible MS PSO，
批次按不可变记录的实际最大 quadCount 选择容量；64 档用 128 线程循环写最多 256 顶点与 128 图元。
方向/空间聚类仍须同步重排顶点流或引入 quad 索引流，单独排序描述符不会改善组内布局。
