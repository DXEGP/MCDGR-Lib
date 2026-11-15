# mesh shader（meshlet）管线

mesh 管线是 D3D12 里与图形管线并列的另一条几何通路：**amplification shader（AS）与 mesh shader（MS）取代
输入装配（IA）与顶点着色（VS），`DispatchMesh` 取代 `Draw*`**。MCDR 提供它的四个环节——能力门控、
HLSL → DXIL 编译、PSO 创建、每帧派发——收在 `mcdr.d3d12.mesh` 模块与 mesh 的 C ABI 适配 TU 里。
本项目的实际使用者是地形：整批 chunk 的 quad 由 CPU 切成 meshlet，一次 `DispatchMesh(meshletCount, 1, 1)`
画完整批；但这一层对"画什么"没有假设。

它的对外出口只有下面这些函数，全部经 C ABI 暴露（句柄一律不透明）：

| ABI 函数 | 作用 | 实现位置 |
| --- | --- | --- |
| `mcdr_dx12QueryMeshSupport` | 返回 1/0 + 一条 UTF-8 原因串 | `src\c_abi\abi_mesh.cpp:45` → `meshPipelineSupported` |
| `mcdr_dx12CompileShaderDxc` | 一个 entry point 的 HLSL → DXIL | `src\c_abi\abi_mesh.cpp:58` → `compileHlslToDxil` |
| `mcdr_dx12CreateMeshPipeline` | 按 bindings 内部建根签名 + PSO | `src\c_abi\abi_mesh.cpp:193` |
| `mcdr_dx12CreateMeshPipelineWithRootSignature` | 复用调用方根签名建 PSO | `src\c_abi\abi_mesh.cpp:224` |
| `mcdr_dx12CreateMeshRootSignature` / `mcdr_dx12ReleaseRootSignature` | 建/释放带 mesh 尾参的根签名 | `src\c_abi\abi_mesh.cpp:197` / `:250` |
| `mcdr_dx12DispatchMesh` | 通用 `DispatchMesh(x, y, z)` | `src\c_abi\abi_mesh.cpp:229` |
| `mcdr_dx12MeshDrawMeshlets` | meshlet 批量派发（补 b8 + t14/t15/t16 后一次 dispatch） | `src\c_abi\abi_mesh.cpp:252` |

创建/销毁沿用图形管线那套生命周期：PSO 走 `mcdr_dx12SetPipeline` / `mcdr_dx12DestroyPipeline`，
绑定走 `mcdr_dx12PushDescriptors`（契约未变，见 [`docs/abi/README.md`](../abi/README.md) 与
[`docs/device/README.md`](../device/README.md)）。

## 1. 管线形状

### 1.1 与图形管线的阶段对比

```
图形管线（mcdr.d3d12.device 建的图形 PSO）        mesh 管线（mcdr.d3d12.mesh 建的 mesh PSO）
┌──────────────────────────────────────┐      ┌────────────────────────────────────────┐
│ IA   输入装配：绑顶点/索引缓冲、输入布局 │      │ （无 IA 阶段：不绑索引、PSO 里没有输入布局）│
│ VS   顶点着色                          │      │ AS   amplification shader（可选）        │
│ HS/TE/DS/GS（可选细分与几何）           │      │ MS   mesh shader（必需）                 │
│ RS   光栅化                            │      │ RS   光栅化                              │
│ PS   像素着色                          │      │ PS   像素着色（可选）                    │
│ OM   输出合并（RTV/DSV）                │      │ OM   输出合并（RTV/DSV）                 │
└──────────────────────────────────────┘      └────────────────────────────────────────┘
   Draw / DrawIndexed / DrawInstanced           DispatchMesh(x, y, z)
```

| 关注点 | 图形管线 | mesh 管线 |
| --- | --- | --- |
| 顶点来源 | IA 按输入布局从顶点缓冲取 | MS 自己从 `ByteAddressBuffer` 读（`Load` / `Load4`） |
| 图元来源 | 索引缓冲 + 拓扑 | MS 输出 `indices uint3 tris[]`，拓扑由 `[outputtopology("triangle")]` 指定 |
| 提交 API | `ID3D12GraphicsCommandList::Draw*` | `ID3D12GraphicsCommandList6::DispatchMesh`（拿不到 List6 就不可用） |
| PSO 创建 | `D3D12_GRAPHICS_PIPELINE_STATE_DESC` | `D3D12_PIPELINE_STATE_STREAM_DESC` + `ID3D12Device2::CreatePipelineState` |
| 着色器字节码 | DXC 编 `vs_*` / `ps_*` | DXC 编 `as_*` / `ms_*`（`D3DCompile` 到不了，SM 5.1 上限） |
| 输入布局 | `inputElements` 写进 PSO | 不存在；传了也不生效 |
| 拓扑状态 | `setPipeline` 里 `IASetPrimitiveTopology` | **跳过**（`pipeline->isMesh` 为真时） |
| 根签名标志 | 可带 `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` | **不能带**，带上 `CreatePipelineState` 直接失败 |
| 深度 | 两个 PSO（`withDepth` / `withoutDepth`） | 同样两个，DSV 固定 `D3D12_FORMAT_D32_FLOAT` |

### 1.2 native 提供的四层

| 层 | 入口 | 说明 |
| --- | --- | --- |
| 能力门控 | `meshPipelineSupported(std::string* why)` | 五项条件全过才为真，见 §4.5 |
| 编译 | `compileHlslToDxil(source, entry, profile, out)` | 进程级 DXC 单例 + 磁盘缓存，见 §4 |
| PSO | `createMeshPipeline` / `createMeshPipeline(desc, rootSig)` | 建根签名（或复用）、组子对象流、建 1–2 个 PSO |
| 派发 | `dispatchMesh(ctx, x, y, z)` / `drawMeshlets(...)` | 前者只做校验 + `DispatchMesh`，后者额外补四个尾参绑定 |

## 2. 从 JVM 一次调用到每帧 dispatch

### 2.1 一次性建立（每个地形管线变体走一遍）

```
JVM (Kotlin / FFM)
  │
  ├─ mcdr_dx12QueryMeshSupport(reason, cap)                      abi_mesh.cpp:45
  │     └─ d3d12_mc::meshPipelineSupported(&why)                 d3d12_mesh_base.cpp:111
  │          ├─ !ctx.device                    → "device not initialized"
  │          ├─ featureLevel < 12_2            → "feature level below 12_2 ..."
  │          ├─ caps.meshShaderTier < TIER_1   → "D3D12 mesh shader tier is 0 ..."
  │          ├─ !caps.dxcAvailable             → "dxcompiler.dll not available"
  │          ├─ !caps.meshShaderCompileOk      → caps.dxcNote
  │          └─ 取 ID3D12Device2 失败           → "ID3D12Device2 is not available ..."
  │
  ├─ mcdr_dx12CompileShaderDxc(hlsl, len, "as_main", "as_6_9", ...)   abi_mesh.cpp:58
  ├─ mcdr_dx12CompileShaderDxc(hlsl, len, "ms_main", "ms_6_9", ...)
  │     └─ compileHlslToDxil(...)                                d3d12_device.cpp:1826
  │          ├─ DXC 单例未初始化 → findDxcDirectory + LoadLibraryW + DxcCreateInstance
  │          ├─ 磁盘缓存命中 → 直接返回字节码
  │          └─ compileWithDxc(-T <profile> -E <entry> -HV 2018 -O3)   d3d12_device.cpp:1556
  │
  ├─ mcdr_dx12CreateMeshRootSignature(bindings, count, 1)        abi_mesh.cpp:197
  │     └─ d3d12_mc::createMeshRootSignature(list, meshExtras)   d3d12_mesh_base.cpp:488
  │          ├─ CBV 按 binding 顺序稠密编号（b0..bN）
  │          ├─ SRV 汇入一张描述符表（地形纹理）
  │          ├─ meshExtras：追加 root CBV b8 + 一张 3 range 的表（t14 / t15 / t16）
  │          └─ recordMeshRootSig(handle, segmentParam, geometryParam)
  │
  └─ mcdr_dx12CreateMeshPipelineWithRootSignature(&desc, rs)     abi_mesh.cpp:224
        └─ createMeshPipelineImpl(desc, rs)                      d3d12_mesh_base.cpp:165
             ├─ ensureDevice()（幂等；首次会按 GPU 偏好建设备）
             ├─ meshPipelineSupported(&why) 再判一次
             ├─ desc.msDxil 必须非空
             ├─ 外部根签名：AddRef 后挂进 ComPtr，从注册表抄两个参数索引
             ├─ 组 D3D12_PIPELINE_STATE_STREAM_DESC 子对象流
             ├─ buildPso(withAs, withPs, withDepth)              → 失败时读 InfoQueue 前 8 条
             └─ 无深度时再 buildPso(..., withoutDepth)（DSV=UNKNOWN）
  ▼
mcdr_pipeline*（不透明句柄，JVM 侧存成 long）
```

### 2.2 每帧派发

```
JVM (Kotlin / FFM)
  │  mcdr_dx12BeginRenderPass(ctx, ...)
  │  mcdr_dx12SetPipeline(ctx, meshPso, useDepth)               d3d12_device.cpp:5751
  │     ├─ 与 ctx->cachedPso 相同 → 直接返回（跳过 3 个驱动调用）
  │     ├─ SetGraphicsRootSignature(pipeline->rootSignature)
  │     ├─ SetPipelineState(pso)
  │     └─ !pipeline->isMesh ? IASetPrimitiveTopology(...) : 跳过
  │  mcdr_dx12PushDescriptors(ctx, bindings, len)               地形 CBV / 纹理表
  │  mcdr_dx12MeshDrawMeshlets(ctx, vb, vbOff, sec, secOff, secLen,
  │                            meshlets, mOff, mLen, globals, gOff, count)   abi_mesh.cpp:252
  │     └─ d3d12_mc::drawMeshlets(...)                          d3d12_device.cpp:6232
  │          ├─ 校验：list 打开 / render pass 打开 / 当前是带尾参的 mesh PSO
  │          ├─ 校验：meshletCount > 0；四个缓冲都非空且 kind == Buffer
  │          ├─ 校验：t14 剩 ≥4 B、段表 ≥32 B、meshlet 表 ≥32 B、范围不越界
  │          ├─ 预留 3 个 draw 描述符槽位（syncRingCapacity 内）
  │          ├─ UPLOAD 堆缓冲跳过过渡，其余切 NON_PIXEL|PIXEL_SHADER_RESOURCE
  │          ├─ 写 3 个 RAW SRV（R32_TYPELESS + BUFFER_SRV_FLAG_RAW，FirstElement = off/4）
  │          ├─ SetGraphicsRootDescriptorTable(geometryParam, gpu)
  │          ├─ SetGraphicsRootConstantBufferView(segmentParam, globals + gOff)（须 256 对齐）
  │          ├─ ID3D12GraphicsCommandList6::DispatchMesh(count, 1, 1)
  │          └─ markMeshColorTargets（所有 RTV 标记已写入）
  │  mcdr_dx12EndRenderPass(ctx)
  ▼
```

### 2.3 两条派发入口的分工

| | `mcdr_dx12DispatchMesh` | `mcdr_dx12MeshDrawMeshlets` |
| --- | --- | --- |
| 绑定责任 | 调用方自己推好全部绑定 | 只补 b8 与 t14/t15/t16，地形绑定仍由 `pushDescriptors` 推 |
| 组数 | 调用方给定 x / y / z | 固定 `(meshletCount, 1, 1)`；AS 变体传 `ceil(count / 32)` |
| 前提 | 已 `setPipeline` 一个 mesh PSO | 同一个 PSO **且** `geometryParam != 0`（即用 `meshExtras=1` 的签名建） |
| 典型用途 | 自定义 AS/MS 几何（全屏、粒子、实验管线） | 地形 |

## 3. 顶点 / meshlet 数据布局

native 只把 t14 / t15 / t16 当 **RAW SRV**（`ByteAddressBuffer`）绑定，步长与字段含义全部由 HLSL 手写解析——
SRV 上没有 `StructureByteStride`。因此下面每个数值都是跨 ABI 的契约：改一端必须同时改另一端。

### 3.1 顶点池 t14

| 项 | 值 | 常量 / 位置 |
| --- | --- | --- |
| 每顶点字节数 | **28** | `VERTEX_STRIDE = 28`（meshlet 生成器） |
| 顶点格式 | `DefaultVertexFormat.BLOCK`（位置 + UV + 光照/颜色，无法线） | JVM 侧 |
| 寻址 | 4 字节为单位（`Load` 取 dword、`Load4` 取 16 B） | `writeRawBufferSRV` |
| native 校验 | 剩余长度 **≥ 4 B** 即可（`vbBytes < 4` 报错） | `d3d12_device.cpp:6257` |
| 顶点号 | `s.vertexBase + m.localVertexOffset + vid` | HLSL `ms_main` |

> **缺陷**：native 不校验 28 B 的步长。传错 stride（例如 32 B 的实体顶点）不会报错，MS 会按错位的地址
> 读顶点，表现为地形顶点乱飞或塌成一个点。

### 3.2 段表 t15（`SectionDesc`，每段 32 B）

| 偏移 | 字段 | 类型 | 含义 |
| --- | --- | --- | --- |
| +0 | `vertexBase` | `uint` | 该段在全局顶点池里的起始顶点号 |
| +4 | `quadBase` | `uint` | 保留：全局 quad 起始（调试 / 分桶） |
| +8 | `meshletBase` | `uint` | 该段在 meshlet 表里的起始下标 |
| +12 | `meshletCount` | `uint` | 该段有多少个 meshlet |
| +16 | `chunkX` | `int` | 方块坐标（MS 里减 `CameraBlockPos`） |
| +20 | `chunkY` | `int` | 同上 |
| +24 | `chunkZ` | `int` | 同上 |
| +28 | `visibility` | `float` | 逐段可见度（0..1） |

### 3.3 meshlet 表 t16（`MeshletDesc`，每条 32 B，8 个 `uint`）

| 偏移 | 字段 | 打包方式 |
| --- | --- | --- |
| +0 | `packedMin` | `minX(R8)` \| `minY<<8` \| `minZ<<16` \| `quadCount<<24` |
| +4 | `packedMax` | `maxX(R8)` \| `maxY<<8` \| `maxZ<<16` \| `directionMask<<24` |
| +8 | `sectionId` | 段号（用来查 t15） |
| +12 | `localVertexOffset` | 段内顶点号 = `firstQuad * 4`（`VERTICES_PER_QUAD = 4`） |
| +16 | `firstQuad` | 段内 quad 号（调试用） |
| +20 / +24 / +28 | `pad0..pad2` | 恒 0，凑满 32 B |

- AABB 量化：`[0,16] × 15.9375 → [0,255]`（`QUANTIZE = 15.9375f`，16 映射到 255 而非 256，正好装进 uint8）。
- `directionMask` 位：`1<<0..1<<5` = +X / −X / +Y / −Y / +Z / −Z，`1<<6` = 交叉面 / 双面材质（不做背面剔除）。
  这张掩码只有 AS 读，MS 不读。
- `quadCount` 上限 16（见 §3.5）。

### 3.4 小常量 b8（root CBV）

| 偏移 | 字段 | 说明 |
| --- | --- | --- |
| +0 | `gMeshletCount` | 本批 meshlet 总数（AS 按它循环） |
| +4 | `gCullEnabled` | 0 = AS 全放行，1 = 开剔除；只改常量，不换 PSO |
| +8 / +12 | pad | 恒 0 |
| +16 | `gVrsBand`（`float4`） | 逐图元着色率的三档距离带；`w = 0` 时不写语义 |

- 整块 32 B，但绑定点是 **root CBV**，`SetGraphicsRootConstantBufferView` 要求 **256 B 对齐**：
  `drawMeshlets` 会检查 `globalsVA % 256 != 0` 并报错，调用方必须把上传偏移按 256 对齐。
- 它同时承担"meshlet 总数"与"段表基址"两种候选用途，只能选一个——当前实现用前者
  （段号写在 `MeshletDesc.sectionId` 里）。

### 3.5 索引格式与线程组

| 项 | 值 | 说明 |
| --- | --- | --- |
| 索引缓冲 | **不存在** | 索引由 MS 算术展开，native 不绑 IB |
| 1 个线程组 = | 1 个 meshlet | `[numthreads(64, 1, 1)]`，`THREADS = 64` |
| 每组顶点上限 | 64 | `QUADS_PER_MESHLET = 16` × `VERTICES_PER_QUAD = 4` |
| 每组图元上限 | 32 | `MAX_PRIMITIVES = 32`（16 quad × 2 三角形） |
| 一个 quad 的三角形 | 2 个 | 前 `quadCount` 个线程各写 `tris[vid*2]` / `tris[vid*2+1]` |
| AS 组大小 | 32 meshlet / 组 | 派发组数 = `(meshletCount + 31) / 32` |

`SetMeshOutputCounts(vertexCount, primCount)` 必须在统一控制流里调用（所有线程同值），否则 MS 的输出
计数未定义。

### 3.6 对齐与长度校验一览

| 对象 | 要求 | 违反后的错误串 |
| --- | --- | --- |
| b8 globals 偏移 | 256 B 对齐 | `globals root CBV is not 256-byte aligned (offset=N)` |
| t14 顶点池 | 至少剩余 4 B | `empty vertex/section/meshlet range` |
| t15 段表 | 至少 32 B，且 `sectionsLength` 不超出缓冲 | 同上 / `sections range overflow` |
| t16 meshlet 表 | 至少 32 B，且 `meshletsLength` 不超出缓冲 | 同上 / `meshlet range overflow` |
| 三个 SRV 描述符 | 必须连续（native 自己写入 3 个相邻槽位） | `draw descriptor heap exhausted for this frame` |
| SRV 首元素 | `offset / 4`（RAW 视图按 4 B 计） | 不校验；offset 非 4 对齐会读到错位 dword |

`sectionsLength` / `meshletsLength` 传 0 时按"到缓冲末尾"取（`remainingToEnd`）。

## 4. HLSL → DXIL

### 4.1 谁编译

只有 DXC。`D3DCompile` / fxc 的着色器模型止于 5.1，**产不出 AS/MS**，本项目的图形管线也没有 5.1 兜底。
`compileHlslToDxil` 在第一次调用时把 `dxcompiler.dll` 加载成进程级单例（`IDxcUtils` + `IDxcCompiler3`），
之后每次编译复用。

> **缺陷**：这个单例故意 `new` 出来永不析构（等价于进程级泄漏）。让它析构会在 CRT 退出阶段对 DXC 的
> COM 对象 `Release`，触发 `ucrtbase` 的 fail-fast（事件日志 0xc0000409），表现为"渲染完全正常、进程仍以
> -1073740791 退出"。不要"顺手修掉"这处泄漏。

### 4.2 DXC 的搜索顺序

| 顺序 | 位置 | 备注 |
| --- | --- | --- |
| 1 | 环境变量 `MCDR_DXC_DIR` | 只是**追加候选**，不是权威覆盖 |
| 2 | `MCDR.dll` 同目录 | 发行包里 `dxcompiler.dll` + `dxil.dll` 解到 `dxrmc\` |
| 3 | Windows SDK 最新的 `bin\<ver>\x64` | 按版本号字符串取最大且有 `dxcompiler.dll` 的一份 |

`MCDR_DXC_DISABLE=1`（完全等于 `"1"`）会把三站全部跳过，只用于验证"没有 DXC"的失败路径。
全部找不到时 `caps.dxcNote` 为 `dxcompiler.dll not found (MCDR_DXC_DIR / MCDR.dll dir / Windows SDK)`。

### 4.3 入口约定与失败上报

| 项 | 值 |
| --- | --- |
| source 上限 | `kMaxSourceBytes = 4 * 1024 * 1024`（4 MiB） |
| `entry` 为空 | 当作 `"main"` |
| `profile` 为空 | 报错 `profile is empty (expected e.g. "ms_6_6")` |
| 返回值 | 成功 = DXIL 字节数；`outDxil == NULL` 或 `outCapacity < size` 时**只返回大小不写缓冲**（两趟调用） |
| 失败 | 返回 0，原因在 `mcdrLastError()` |
| 实际使用的 profile | MS `ms_6_9`，AS `as_6_9`（JVM 侧硬编码；native 把 profile 原样透传给 DXC） |

`compileWithDxc` 的固定参数：`-T <profile> -E <entry> -HV 2018 -O3`。失败时从 `IDxcResult` 的 error
buffer 取完整诊断文本写进 `mcdr::Error`（**不是**只取第一行；只有能力探测的 `dxcNote` 才截首行、超 220
字符加 `...`）。PSO 创建失败时另有一条旁路：调试层开着时读 `ID3D12InfoQueue` 前 8 条消息逐行 WARN，
用来定位被拒的具体子对象字段。

### 4.4 DXIL 磁盘缓存

| 项 | 值 |
| --- | --- |
| 键 | `FNV-1a64(dxcompiler.dll 版本标签 + profile + entry + 源码)` |
| 版本标签 | `dxcompiler.dll` 的文件大小 + 最后写入时间 |
| 目录 | `MCDR.dll` 同目录下的 `MCDR_dxil_cache`，或 `MCDR_DXIL_CACHE_DIR` |
| 关闭 | `MCDR_DXIL_CACHE=0` |
| 单文件上限 | 64 MiB（超出按未命中处理） |

命中与未命中各打一行 INFO；未命中带耗时（实测 0.4–3 s，且全部落在主线程）。

### 4.5 能力门控的五个条件

`meshPipelineSupported` 按顺序检查，第一个不满足的原因写回 `why`：

1. 设备已建（`ctx.device` 非空）——`mcdr_dx12QueryMeshSupport` **不会**替你建设备，设备未就绪时原因是
   `device not initialized`。
2. `caps.featureLevel >= D3D_FEATURE_LEVEL_12_2`。
3. `caps.meshShaderTier >= D3D12_MESH_SHADER_TIER_1`（`meshShadersSupported()`）。
4. `caps.dxcAvailable`。
5. `caps.meshShaderCompileOk`（设备初始化时用固定 `ms_6_9` 冒烟编译验证过 DXIL 链路）。

`createMeshPipeline` 自己还会先 `ensureDevice()`，并在建 PSO 前把同一组条件再判一次。

## 5. 资源与 PSO 的创建/销毁顺序

```
mcdr_dx12CreateMeshRootSignature(..., meshExtras = 1)
   └─ 句柄 rs（uint64_t，就是 ID3D12RootSignature* 的地址值）
      ├─ 注册表里留一条画像：segmentParam  = cbvCount + (有地形 SRV 表 ? 1 : 0)
      │                        geometryParam = segmentParam + 1
      │
      └─ mcdr_dx12CreateMeshPipelineWithRootSignature(desc, rs)
            └─ 内部 AddRef(rs) 挂进 ComPtr，并把两个索引抄进 Dx12Pipeline
               （所以之后 release 掉 rs 也不会让已建的 PSO 失效）

每帧：SetPipeline(ps) → PushDescriptors → MeshDrawMeshlets
                              └─ 只用 rs 画像里那两个索引，不再碰根签名对象

mcdr_dx12DestroyPipeline(ps)          → delete Dx12Pipeline（ComPtr 释放 rs 那一份引用）
mcdr_dx12ReleaseRootSignature(rs)     → 删画像 + Release 调用方那一份引用（传 0 安全）
```

| 句柄 | C 类型 | 实际是什么 | 谁释放 |
| --- | --- | --- | --- |
| PSO | `mcdr_pipeline*`（`void*`） | `Dx12Pipeline*` | `mcdr_dx12DestroyPipeline` |
| 根签名 | `uint64_t` | `ID3D12RootSignature*` 的地址值 | `mcdr_dx12ReleaseRootSignature` |
| 命令上下文 | `mcdr_command_ctx*` | `CommandContext*` | 编码器生命周期（`abi_command.cpp`） |
| 缓冲 | `mcdr_object*` | `Dx12Object*` | `mcdr_dx12DestroyObject` |

JVM 侧全程只搬运这些值，**永不解引用**。这也是模块边界的要求：`Dx12Pipeline` 属于 `mcdr.d3d12.device`
模块，写进导出签名会得到模块私有修饰名（`Dx12Pipeline@$$_Amcdr.d3d12.device`），普通 TU 消费方按公共名
`@1` 引用 → `LNK2001`。规则细节见 [`docs/c_abi/README.md`](../c_abi/README.md)。

## 6. 约束清单

| # | 约束（祈使句） | 违反后的现象 |
| --- | --- | --- |
| 1 | 先确认 `mcdr_dx12QueryMeshSupport` 返回 1，再建 mesh PSO | 返回 NULL，`mcdrLastError` 为 tier / FL / DXC 之一 |
| 2 | 先建根签名，再用 `mcdr_dx12CreateMeshPipelineWithRootSignature` 建 PSO | PSO 没有 b8 与 t14/t15/t16 槽位，`drawMeshlets` 报 `current pipeline is not a mesh PSO with mesh extras` |
| 3 | 必须提供 MS 的 DXIL（`msLen > 0`） | `msDxil is empty (mesh shader bytecode is required)` |
| 4 | 只传 DXC 产出的 DXIL，不要传 HLSL 源码或 DXBC | `CreatePipelineState` 返回 `E_INVALIDARG`（0x80070057），只有 InfoQueue 消息能定位 |
| 5 | 建 mesh 根签名时不要带 `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` | 建 PSO 失败；`meshExtras=1` 路径已由 native 保证不带 |
| 6 | 必须在同一个 command list 内先 `mcdr_dx12SetPipeline(meshPso)` 再派发 | `mesh pipeline is not bound`（`ctx->cachedPso` 为空） |
| 7 | 必须在 `mcdr_dx12BeginRenderPass` 之后派发 | `render pass is not open` |
| 8 | 必须把 b8 常量缓冲的偏移按 256 B 对齐 | `globals root CBV is not 256-byte aligned (offset=N)` |
| 9 | 必须保证段表 / meshlet 表各 ≥ 32 B 且范围不越界 | `empty vertex/section/meshlet range` / `sections range overflow` / `meshlet range overflow` |
| 10 | 必须让 `meshletCount > 0` | `meshletCount must be positive` |
| 11 | 每个轴 1..65535，且 `x*y*z <= 4194304`（`1<<22`） | `invalid group counts (axis <=65535, product <=4194304)`；单轴超限时 ABI 层先说 `thread group counts exceed 65535` |
| 12 | 不要把一帧的 draw 槽位用尽：每次 `drawMeshlets` 占 3 个槽 | `draw descriptor heap exhausted for this frame`，该批地形本帧不画 |
| 13 | 用 `mcdr_dx12DestroyPipeline` / `mcdr_dx12ReleaseRootSignature` 释放，各只释放一次 | 重复释放 → 访问已释放内存；不释放 → PSO / 签名常驻 |
| 14 | 不要在 mesh PSO 上依赖 `IASetPrimitiveTopology` 与顶点 stride 修正 | `setPipeline` 对 `isMesh` 跳过拓扑设置；MS 不读 IA 状态，`vertexStrides` 也不生效 |
| 15 | 让 `hasDepth` 与 render pass 的深度附件一致 | `setPipeline` 挑错 PSO；没有 `withoutDepth` 时回退到 `withDepth`，调试层报 DSV 不匹配 |
| 16 | 不要对 UPLOAD 堆缓冲做资源过渡 | 调试层直接报错；`drawMeshlets` 已对 UPLOAD 堆跳过过渡，自己写路径时别补 |

## 7. 已知限制与缺陷

> **缺陷**：`src\module\advanced\d3d12_mesh.cppm` 的模块头注释写"混合状态（blend）留待后续"，但
> `d3d12_mesh_base.cpp:271` 已按 `colorTargets` 组出完整 `D3D12_BLEND_DESC`（8 个槽位先填合法默认值，
> `IndependentBlendEnable = TRUE`）。以代码为准：混合可用，注释过时。

> **缺陷**：native 不校验 AS payload。每线程组 32 个 slot、payload 大小与组内压缩格式全部由 HLSL 决定，
> `dispatchMesh` 只按调用方给的组数发；payload 越界写不会在这里被拦住。native 侧没有 payload 上限常量
> （代码中未明确）。

> **缺陷**：`gMeshRootSigs` 画像表以根签名地址值为键。`mcdr_dx12ReleaseRootSignature` 之后若还有 PSO
> 存活，派发照旧工作（索引已抄进 PSO），但用同一句柄再建 PSO 会查不到画像、拿到 `segmentParam = 0`，
> 建出来的 PSO 一律被 `drawMeshlets` 拒（`geometryParam == 0`）。根签名句柄一旦释放就不要再用。

> **缺陷**：`StencilEnable = FALSE` 固定写入深度模板状态，mesh PSO 不支持模板测试。需要模板的地形效果
> 只能走图形管线路径。

> **缺陷**：采样数固定 `DXGI_SAMPLE_DESC{1, 0}` 与 `SampleMask = UINT_MAX`，mesh 管线不支持 MSAA。

> **缺陷**：`depthFormat` 只参与"有没有深度"的判断，DSV 恒为 `D3D12_FORMAT_D32_FLOAT`
> （`depthStencilFormat()`）。传 D24S8 等其它 ordinal 不会生效，也不会报错。

> **缺陷**：`NODE_MASK` 子对象被刻意排除在子对象流之外（值 0 在部分驱动上被判 `E_INVALIDARG`），
> 因此无法用 mesh PSO 指定多 GPU 节点；当前恒为 node 0。

| 限制 | 说明 |
| --- | --- |
| 无索引缓冲 | MS 算术展开 quad 索引；需要真实索引的几何得自己在 MS 里查表 |
| 顶点步长 | 28 B 是调用方契约，native 只按 ≥ 4 B 校验（见 §3.1） |
| 段 / meshlet 步长 | 32 B 硬编码在 HLSL 解析函数里；改步长必须同时改 HLSL 与 `MESHLET_DESC_BYTES` |
| 批次拆分 | `drawMeshlets` 不拆批；`meshletCount` 变体由 AS 把派发组数折算成 `ceil(count / 32)` |
| 环境变量逃生口 | DXIL 缓存只能靠 `MCDR_DXIL_CACHE` / `MCDR_DXIL_CACHE_DIR` 控制，没有对应的配置项 |

## 文件导航

路径相对 `MCDR\`。

| 文件 | 内容 |
| --- | --- |
| `include\mcdr_abi.h` | `mcdr_mesh_pipeline_desc`（`:243`）、`mcdr_color_target_desc`（`:170`）、`mcdr_depth_state_desc`（`:182`）、`mcdr_pipeline_binding_desc`（`:202`）、7 个 mesh 入口的声明（`:1275-1321`） |
| `src\module\advanced\d3d12_mesh.cppm` | `mcdr.d3d12.mesh` 接口单元：`MeshPipelineDesc`、`createMeshPipeline` 两个重载、`createMeshRootSignature` / `releaseMeshRootSignature`、`meshPipelineSupported`、`dispatchMesh` |
| `src\advanced\d3d12_mesh_base.cpp` | 实现（普通 TU）：`meshPipelineSupported:143`、`createMeshPipelineImpl:204`、`dispatchMesh:503`、`createMeshRootSignature:544`、`releaseMeshRootSignature:660`，以及子对象流封送与根签名画像表 |
| `src\c_abi\abi_mesh.cpp` | C ABI 适配：参数边界（`kMaxColorTargets=8`、`kMaxPipelineBindings=64`、`kMaxSourceBytes=4 MiB`）、描述符 → `MeshPipelineDesc` 的搬运、7 个 `extern "C"` 入口 |
| `src\core\d3d12_device.cpp` | `drawMeshlets:6753`（尾参绑定与 RAW SRV）、`compileHlslToDxil:1970`、`compileWithDxc:1672`、`probeDxc:1900`、`setPipeline:6235`、`destroyPipeline:6225` |
| `src\module\core\mcdr_device.cppm` | `Dx12Pipeline`（`:627`：`withDepth` / `withoutDepth` / `isMesh` / `segmentParam` / `geometryParam`）、`Capabilities`（`:277`：`meshShaderTier`、`dxcAvailable`、`meshShaderCompileOk`） |
| `docs\advanced\mesh.md` | 本文 |
| `docs\abi\README.md` | C ABI 契约：句柄、错误串、两趟缓冲调用 |
| `docs\device\README.md` | 设备与 PSO 缓存、命令列表与 render pass |
| `docs\c_abi\README.md` | 适配层规则与 `LNK2001` / `C5244` 的成因 |
| `docs\async\README.md` | draw 描述符堆的分段与 `syncRingCapacity()` |

## 文档维护

1. 改了 `mcdr_mesh_pipeline_desc` 或任何 mesh ABI 函数的签名，同一次改动里同步
   `include\mcdr_abi.h`、`src\c_abi\abi_mesh.cpp` 与本文 §2、§5，并跑一次布局自检（`mcdrABILayout`）。
2. 改了 §3 的任何一个数值（28 B 顶点、32 B 段 / meshlet、64 线程、16 quad、256 B 对齐），必须同时改
   JVM 侧的生成器与 HLSL —— 三端不一致时以 GPU 实际读到的结果为准。
3. 改了根签名的尾参布局（b8 / t14 / t15 / t16 的 register 或 range 数量），同步改
   `createMeshRootSignature` 的 `params` 推入顺序：`segmentParam` / `geometryParam` 由该顺序算出，
   不是写死的常量。
4. 新增 mesh 专属能力（payload 校验、AS 输出组数上限、多 node）时先补 native 校验再补本文 §6 的约束行；
   只写文档不写校验的约束一律视为不存在。
5. 本文与代码冲突时以代码为准，并顺手把这里改对；机制说明留在本文，源码注释只留"违反就会出错"的一句。
