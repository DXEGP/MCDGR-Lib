# 着色器资源绑定

当前原版 GLSL 由 JVM 的 naga 转换器生成 HLSL 与 manifest，native 使用 DXC 创建 DXIL 和 PSO。
`mcdr_pipeline_desc` 的资源寄存器与 JVM 最终绑定表下标一致；native 不重新解释 GLSL 资源名。

## Buffer SRV

`mcdr_binding_slot` 的 `type=2` 表示 buffer SRV，`texelFormat` 为原版 GpuFormat ordinal，或 `-1` 表示 RAW。
RAW 用于转换后的 R8_SINT 字节 texel buffer：HLSL 是 ByteAddressBuffer，`Load` 读取一个 DWORD 再提取并符号扩展目标字节。
必须使用 `DXGI_FORMAT_R32_TYPELESS`、`D3D12_BUFFER_SRV_FLAG_RAW`；不能提供 R8_SINT typed view。

`pushDescriptors` 和 `writeBufferSRVToDrawHeap` 共用 `bufferSrvDescription`：

- 验证 offset 非负、length 正数且切片不超过逻辑 buffer size。
- RAW offset 必须 4 字节对齐，NumElements 为 `ceil(length / 4)`；最后不足 4 字节的内容由 shader 按字节提取。
- typed view 的 offset/length 必须按元素宽度对齐。
- 验证 NumElements 不溢出，以及包含尾部填充的视图不超过物理 allocation。

`createBuffer` 按 256 字节对齐分配物理资源，逻辑 size 仍为调用方长度；RAW 尾部填充不改变逻辑可读字节数。
地形 mesh 专用 t14/t15/t16 的 RAW 描述符由独立 mesh 录制路径维护，其资源契约保持不变。

## Mesh 派发契约

`drawMeshlets` 的 `meshletCount` 参数表示本次 `DispatchMesh` 的组数：MS-only 为 meshlet 数，
AS/MS 为 `ceil(meshlet 数 / 32)`。有效范围为 `[1, 4194304]`（`2^22`）；超限返回错误。

不超过 65535 组时使用一维派发；更大时使用 `256 × ceil(count / 256)`，满足每轴与总组数限制。
JVM 生成的 AS 与 MS-only 必须同步使用 `gid.x + gid.y * 256` 还原线性组号，
尾部填充组不能读取超出 `gMeshletCount` 的描述符。MS 在统一控制流中仅调用一次 `SetMeshOutputCounts`。

`b8` 的 `MeshletGlobals` 固定为 32 B：meshlet 数在 offset 0，剔除开关在 offset 4，
offset 8/12 为填充，距离带 VRS 的 `float4` 从 offset 16 开始。CBV 分配尺寸按 256 B 对齐。

## 文件

| 文件 | 职责 |
| --- | --- |
| `include/mcdr_abi.h` | JVM/native 的 binding slot 契约 |
| `src/backend/device/d3d12_device.cpp` | buffer 范围校验、资源视图、根签名/PSO |
| `src/backend/shader/d3d12_shader.cpp` | DXC 与 DXIL 缓存 |
