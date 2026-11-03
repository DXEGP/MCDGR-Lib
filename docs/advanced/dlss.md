# DLSS / DLSS-G 的 native 实现

DLSS 的 native 部分全部收在 `MCDR\src\advanced\d3d12_dlss.cpp`（3397 行，普通翻译单元）。它是仓库里**唯一**同时 `#include nvsdk_ngx.h` 与 Streamline 头的地方：设备模块（`mcdr.d3d12.device`）永远看不到 NGX 类型，DLSS 的许可面与崩溃面都锁在这一个 TU 里。

对上，它只暴露 `MCDR\include\mcdr_dlss_impl.h` 的 `extern "C"` 函数（签名里只有 `void*`、POD 与 `char*`）；对下，它有两条互斥的实现路径：

| 路径 | 判定 | 超分由谁执行 | 帧生成由谁执行 |
| --- | --- | --- | --- |
| NGX 直连 | `dlssImplStreamline() == false` | 本单元直接调 `NVSDK_NGX_D3D12_*` | 不支持（`mcdr_dlss_set_fg_mode` 只写状态） |
| Streamline | 见 §1.4 | `sl.interposer.dll` 的 `slEvaluateFeature(kFeatureDLSS, …)` | `slDLSSGSetOptions` + 呈现链插帧 |

调用点固定：**世界画完、GUI 之前**（原 SMAA 的位置，见文件头注释）。DLSS 与 SMAA 不再互斥——两者都开时 SMAA 三趟先跑在低分辨率世界层上，再由 DLSS 升采样。

## 1. 一次超分调用的完整链路

### 1.1 调用方要做的事

1. 设备就绪时（一次）：`mcdr_dlss_install_log(info, warn, error)` → `mcdr_dlss_install_texture_ops(createFn, destroyFn)` → `mcdr_dlss_register_vendor_dir(utf8Dir, cap)`。三个都必须在任何其它入口之前完成，否则本层既不记日志、也建不出私有纹理。
2. 每帧世界渲染前：调用 `mcdr_dx12ReflexFrameBegin()`，使上帧快照失效；该失效不受 Reflex 开关影响。
3. main pass RETURN、清深度之前：可选 `mcdr_dx12SnapshotDlssDepth(ctx, depth)`；世界层画完、GUI 合成之前填 `mcdr_dlss_input` → `mcdr_dx12CreateDlssFeature(ctx, &in)` → `mcdr_dx12EvaluateDlss(ctx, &in)`，见 §4.4。
4. 设备销毁时：`mcdr_dlss_shutdown()`。这一步**必须在设备还活着时**调用，它要 `ReleaseFeature` / `Shutdown1` / 交回私有纹理。

### 1.2 时序图

```
JVM (Kotlin / FFM)
  │  mcdr_dx12CreateDlssFeature(ctx, &in)          src\c_abi\abi_command.cpp
  ▼
C ABI ── 参数 null 检查 ──► d3d12_mc::applyDlssToTexture(ctx, in)   d3d12_device.cpp:8639
  │
  ├─ gCtx.dlssEnabled ?  ──── 否 ──► return false   （ABI 返回 0，一帧无事发生）
  ├─ ctx->listOpen && commandList ──── 否 ──► false
  ├─ 4 个 pIn* 非空 ?  ──── 否 ──► false
  ├─ Dx12Object* → ID3D12Resource*（unwrapResource，4 个）
  ├─ mcdr_dlss_can_run(device) ──► 首次：loadRuntimeDll → ensureInit（只做一次，§2）
  ├─ 每帧重申：request_quality(tier) / request_preset(preset) / set_output_texture(override)
  ├─ mcdr_dlss_create(cmd, &in, tl, ctx)            d3d12_dlss.cpp:1573
  │     ├─ Streamline 路径：resolveQualityValue + streamlineApplyOptions → return 1
  │     └─ NGX 路径：ensureFeature → GetScratchBufferSize → NGX_D3D12_CREATE_DLSS_EXT
  ├─ ctx->inRenderPass ? ── 是 ──► endRenderPass(ctx) + WARN
  │                                 （NGX 要录 compute dispatch，绑着 RTV 会 0x887A0001）
  └─ mcdr_dlss_evaluate(cmd, &in, tl, ctx)           d3d12_dlss.cpp:2495
        ├─ Streamline：slSetTagForFrame → slSetConstants → slDLSSSetOptions
        │              → slEvaluateFeature → copy-back → slDLSSGSetOptions
        └─ NGX：输入转 NON_PIXEL_SHADER_RESOURCE → NGX_D3D12_EVALUATE_DLSS_EXT
                → copy-back → 输出转 RENDER_TARGET
  ▼
返回值 1 → ABI 1；0 或 -1 → ABI 0
```

### 1.3 两个 ABI 入口其实是同一段代码

| 入口 | 位置 | 实现 | 返回 |
| --- | --- | --- | --- |
| `mcdr_dx12CreateDlssFeature` | `src\c_abi\abi_command.cpp:197` | `applyDlssToTexture` | 1 成功 / 0 跳过或失败 / -1 参数为空 |
| `mcdr_dx12EvaluateDlss` | `src\c_abi\abi_command.cpp:206` | `applyDlssToTexture` | 同上 |
| `mcdr_dx12QueryDlss` | `src\c_abi\abi_command.cpp:214` | `queryDlss` | 见 §6.1 |

两个入口走同一个函数，因此：**create 可以重复调用**（尺寸/档位没变时 `ensureFeature` 直接判真，不重建）；**不调 create 直接 evaluate 也会把特征建出来**。create 返回 0 时调用方应当**跳过** evaluate——那是"这一帧没有超分能力"，不是"重试一次就能好"。

### 1.4 实现路径的选择

`dlssImplStreamline()`（`d3d12_dlss.cpp:700`）的判定顺序：

1. 环境变量 `MCDR_DLSS_IMPL`：等于 `streamline`（大小写不敏感）→ 走 Streamline；其它任何值 → 走 NGX。这一条**每次调用都重新读**，显式指定立即生效。
2. 旧变量 `MCDR_STREAMLINE=1` → Streamline。
3. 否则探测 `MCDR.dll` 同目录下的 `sl.interposer.dll`：见到即锁存为"走 Streamline"。**没见到不锁存**，下次再探——`sl.interposer.dll` 是 JVM 启动时解压到 `dxrmc\` 的，第一次调用可能早于解压完成。

两条路径的差异：

| 关注点 | NGX 直连 | Streamline |
| --- | --- | --- |
| `NVSDK_NGX_D3D12_Init*` | 调，且只调一次 | **绝不调**（同进程双 NGX 冲突），`s.failure = "streamline mode: native NGX init skipped by design"` |
| `mcdr_dlss_can_run` | 依赖 Init 结果 | 直接返回 1 |
| 特征创建 | `NGX_D3D12_CREATE_DLSS_EXT` | 无显式创建；选项经 `slDLSSSetOptions` 下发 |
| 选项下发时机 | 每次 `ensureFeature` | 只在**值变化**时；代码注释记录每帧无条件下发会让进程私有提交内存每分钟涨约 750 MB |
| HDR | `IsHDR` 不置位 | `opt.colorBuffersHDR = mcdr_hdr_state_active()` |
| 相机矩阵来源 | 不需要 | `mcdr_dlss_constants`（JVM 每帧经 `mcdr_dx12SetDlssConstants` 推来，列主序，native 逐元素转置） |
| 返回值 2（私有纹理） | 可能 | 不会 |

## 2. NGX 初始化

### 2.1 固定常量

| 常量 | 值 | 位置 |
| --- | --- | --- |
| `MCDR_NGX_PROJECT_ID` | `"7c1f4a52-9b3e-4d68-8f21-5a6c0e2d9b74"`（36 字符 GUID 串） | `d3d12_dlss.cpp` 文件头宏 |
| `MCDR_NGX_ENGINE_VERSION` | `"1.0"` | 同上 |
| `MCDR_NGX_APPDATA` | `L".\\ngx"` | 同上 |
| engine type | `NVSDK_NGX_ENGINE_TYPE_CUSTOM` | `ensureInit` |
| SDK 版本 | `NVSDK_NGX_Version_API`（`0x0000015` = 1.5.0） | `ensureInit`；**不做版本扫描** |
| creation / visibility node mask | `1u` / `1u` | `NGX_D3D12_CREATE_DLSS_EXT` |

### 2.2 需要哪些 DLL

| 文件 | 由谁提供 | 缺了会怎样 |
| --- | --- | --- |
| `nvngx_dlss.dll` | 开发目录 `MCDR\lib\dlss\`，随 `gradlew copyVendorDlls` 进 `dxrmc\`；**不进发行 JAR** | 所有候选路径都失败 → `loadRuntimeDll` 返回 NULL → `can_run` 0 → 每帧 ABI 0 |
| `nvngx.dll` | 显卡驱动 | NGX `Init` 失败（`0xBAD00005` 一类），驱动太旧时 `NeedsUpdatedDriver=1` |
| `sl.interposer.dll` | `MCDR\lib\streamline\`（开发目录） | 探测不到 → 回落到 NGX 直连 |
| `sl.dlss.dll` / `sl.dlss_g.dll` / `sl.reflex.dll` / `sl.common.dll` | 同上 | 对应 feature 加载失败，`mcdr_dlss_note_sl_loaded(0)` → `mcdr_dlss_available()` 报不可用 |

`nvngx_dlss.dll` 的导入表只有 `VERSION` / `ADVAPI32` / `USER32` / `KERNEL32`，没有额外依赖。

### 2.3 appData 目录与 DLL 搜索路径

`ensureInit` 先算 appData，三级回退（`buildAppDataPath`）：

1. `MCDR.dll` 所在目录 + `\ngx`（不存在就建）；
2. `.\ngx`（按需创建）；
3. `"."`。

`mcdr_dlss_register_vendor_dir` 做三件事，缺一不可：

1. `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS)`；
2. `SetDllDirectoryW(dir)`；
3. `AddDllDirectory(dir)`，cookie 记下来。

三件事各自结果打一行日志，然后 `rebuildCommonInfo(dir)` 把 `NVSDK_NGX_FeatureCommonInfo.PathListInfo.Path/Length` 指向该目录。**后一步是 Init 能成功的前提**：NGX 默认只在应用目录找依赖，而开发跑的"应用"是 JDK 的 `java.exe`（`...\bin\`），少了路径表 `Init` 与 `GetFeatureRequirements` 会直接返回 `0xBAD00005`。

`loadRuntimeDll`（`d3d12_dlss.cpp:343`）按序尝试：注册的搜索路径（或裸名）→ `vendorDir\nvngx_dlss.dll` → `<MCDR.dll 目录>\nvngx_dlss.dll` → `<cwd>\dxrmc\` → `<exe 目录>\dxrmc\` → `<cwd>`。每次失败都 WARN 并带 `err=%lu`（`ERROR_MOD_NOT_FOUND` / `ERROR_FILE_NOT_FOUND`），全失败再打一行总结。模块句柄缓存进 `s.runtimeModule`，路径记进 `s.runtimePath`（`mcdr_dlss_runtime_path()` 永不返回 NULL）。

### 2.4 初始化步骤

`ensureInit(device)`（`d3d12_dlss.cpp:845`）按序：

| # | 动作 | 失败处理 |
| --- | --- | --- |
| 1 | `s.initAttempted` 已置位 → 直接返回 `s.initOk` | 无（不重试） |
| 2 | `dlssImplStreamline()` 为真 → 写 `s.failure`，返回 0 | 本进程永不调 NGX Init |
| 3 | `buildAppDataPath()` | 回退 `"."` |
| 4 | `NVSDK_NGX_D3D12_Init_with_ProjectID(projectId, ENGINE_TYPE_CUSTOM, "1.0", appDataPath, device, commonInfoForNgx(), NVSDK_NGX_Version_API)` | WARN 一次 + `s.failure` + 返回 0 |
| 5 | `NVSDK_NGX_D3D12_GetCapabilityParameters(&s.params)` → `paramsOk` | 同上 |
| 6 | 读 `SuperSampling_Available` / `NeedsUpdatedDriver` / `MinDriverVersionMajor` / `MinDriverVersionMinor` / `FeatureInitResult`，打一行 `"NGX capabilities: ..."` | 无 |
| 7 | `available == 0 \|\| needsDriver == 1` → 额外 WARN `"NGX says DLSS-SR is NOT usable on this system ..."` | 无（Init 仍算成功，失败留给 CreateFeature 暴露） |
| 8 | `NGX_DLSS_GET_OPTIMAL_SETTINGS(params, 1280, 720, MaxQuality, …)` 打一行参考值 | 无 |

第 4 步的调用**必须写在一行**：拆行会让 MSVC 报 C2065（宏展开里的模板参数名）。

### 2.5 "只做一次"的约束

- `s.initAttempted` 一旦置位，**成败都不再重试**。想在同一进程里再来一次，只有 `mcdr_dlss_shutdown()` 把 `s = State{}` 清空（即设备重建）。
- `mcdr_dlss_request_quality` / `mcdr_dlss_request_preset` / `mcdr_dlss_set_enabled` 都**不会**重置初始化状态。
- Streamline 模式下 `initAttempted` 也被置位，含义是"本进程已决定不走 NGX"。

> 不要在 `can_run` 之外另开初始化入口。重复 Init 同一 device 会与已加载的 `nvngx.dll` 状态冲突，NGX 不保证行为。

### 2.6 失败后的可见表现

| 失败点 | native 日志 | `mcdr_dx12QueryDlss` 的 note | 上层表现 |
| --- | --- | --- | --- |
| 运行库找不到 | `nvngx_dlss.dll could not be loaded from any candidate ...` | `"ngx runtime not found"` | 每帧 create 返回 0，超分不发生，调用方走自己的兜底（低分辨率世界层直接上屏） |
| `Init` 失败 | `NVSDK_NGX_D3D12_Init_with_ProjectID failed: ...` | `"NGX init failed (see the DLSS WARN line)"`，适配器也报不支持时 `"NGX init failed and the adapter reports no DLSS: <原因>"` | 同上 |
| 驱动过旧 | `"NGX says DLSS-SR is NOT usable on this system ..."` | 同上（`Init` 可能仍成功） | 到 CreateFeature 才失败，失败一次后本进程永久禁用 |
| 设备未建 | — | `"device not created"` | — |
| Streamline 已加载但插件没起来 | `mcdr_dlss_note_sl_loaded(0)` 记录 | `"DLSS runtime not loaded"`（FG 侧） | 开关亮着但每帧被跳过 |

## 3. feature 创建

### 3.1 什么时候重建

`ensureFeature`（`d3d12_dlss.cpp:1112`）的比较表：

| 条件 | 比较的字段 | 后果 |
| --- | --- | --- |
| `sizeChanged` | `renderWidth/Height`、`targetWidth/Height` 四元组 | `destroyFeature()` → 重建 → `s.resetPending = true` |
| `qualityChanged` | 本帧解析出的 `resolvedTier` vs `s.effectiveQuality`（**接受后的档位**，0..4，-1 = 按比例选） | 同上 |
| `presetChanged` | `requestedPreset` vs `s.effectivePreset` | 同上 |
| 三者都没变且 `s.featureOk` | — | 直接返回 1，不碰 NGX |

`effectiveQuality` 存的是**接受后的档位**而不是 NGX 枚举：DLAA 被降级成 Balanced 之后基线就是 2，否则每帧都会重新判定"变了"并重建特征（实测 24 秒 480 次创建，每次 30–40 ms）。

`PerfQualityValue` 与 render preset 都是 `CreateFeature` 的输入，改它们必须重建——这是上面第三条比较存在的原因。

### 3.2 `mcdr_dlss_input` 字段

sizeof = 112、align = 8（`mcdr_abi.h`）。

| 偏移 | 字段 | 本层怎么用 | 合法范围 | 非法时 |
| --- | --- | --- | --- | --- |
| 0 | `renderWidth` | `NVSDK_NGX_Parameter_Width` / `Feature.InWidth` / `InRenderSubrectDimensions.x` | > 0，且 ≥ 该档名义输入（§3.4） | 太小 → NGX 回空纹理（黑屏），本层先 ERROR（§3.4）；= 0 会走进 `chooseQuality` 的守卫 |
| 4 | `renderHeight` | 同上（Height） | 同上 | 同上 |
| 8 | `targetWidth` | `OutWidth` / `InTargetWidth` / 窗口尺寸 | > 0 | 0 时 `chooseQuality` 直接回退 MaxQuality |
| 12 | `targetHeight` | 同上 | > 0 | 同上 |
| 16 | `jitterOffsetX` | `InJitterOffsetX`（原样透传） | render 像素，Halton(2,3) 落在 `[-0.5, 0.5)` | 不校验；NaN 未做检查（代码中未明确） |
| 20 | `jitterOffsetY` | `InJitterOffsetY` | 同上 | 同上 |
| 24 | `mvScaleX` | `InMVScaleX` | 像素/UV 的缩放（JVM 侧算好） | 不校验 |
| 28 | `mvScaleY` | `InMVScaleY` | 同上 | 不校验 |
| 32 | `preExposure` | `InPreExposure`；`0` → 用 `1.0f` | 任意 float | 0 视作 1.0 |
| 36 | `exposureScale` | `InExposureScale`，**仅当** `exposureMode == MCDR_DLSS_EXPOSURE_NONE` 时生效；`0` → `1.0f` | `> 0` | 0 视作 1.0 |
| 40 | `exposureMode` | `MCDR_DLSS_EXPOSURE_NONE(0)` / `_AUTO(1)` | 0 或 1 | 1 目前**保留**，本层不传曝光纹理 |
| 44 | `alphaUpscaling` | 两条路径都恒按关闭处理（NGX 不置 `AlphaUpscaling` flag，SL 侧 `alphaUpscalingEnabled = eFalse`） | — | 无 |
| 48 | `nearZ` | 只用于变更日志 | 任意 | 无 |
| 52 | `farZ` | 只用于变更日志 | 任意 | 无 |
| 56 | `depthInverted` | `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted` | 本项目 reverse-Z，**必须非 0** | 0 会让深度语义反掉，近处被当远处 |
| 60 | `reset` | `InReset`（与 `s.resetPending` 或运算） | 0 / 非 0 | 无 |
| 64 | `depthFormatOrdinal` | 只用于变更日志 | — | 无 |
| 72 | `pInColor` | `pInColor` | 模块解包后的裸 `ID3D12Resource*` | 空 → `applyDlssToTexture` 直接 false |
| 80 | `pInDepth` | `pInDepth`（有深度快照时被快照替换） | 同上 | 同上 |
| 88 | `pInMotionVectors` | `pInMotionVectors` | 同上 | 同上 |
| 96 | `pInOutput` | 决定"私有兜底纹理"的尺寸与格式 | 同上 | 同上 |

`pIn*` 传进来时是 JVM 的 `Dx12Object*` 包装句柄，**必须由设备模块解包**（`unwrapResource`）。实现单元是普通 TU，看不到 `Dx12Object` 的布局，直接把句柄当资源用会在第一次 `mv->GetDesc()` 上以 `EXCEPTION_ACCESS_VIOLATION reading 0x50` 崩掉。

### 3.3 质量档位表

档位是 JVM 传进来的整数 `tier`（`mcdr_dlss_request_quality`），本层映射成 NGX 枚举（`kTierToNgx[5]`，`d3d12_dlss.cpp:1204`）：

| tier | NGX `PerfQuality_Value` | 枚举值 | 名义倍率 `kTierNominalFactor` | 该档最小 render | Streamline 默认 preset |
| --- | --- | --- | --- | --- | --- |
| 0 | `UltraPerformance` | 3 | 3.00× | `ceil(target / 3.0)` | L |
| 1 | `MaxPerf` | 0 | 2.00× | `ceil(target / 2.0)` | M |
| 2 | `Balanced` | 1 | 1.72× | `ceil(target / 1.72)` | K |
| 3 | `MaxQuality` | 2 | 1.50× | `ceil(target / 1.5)` | K |
| 4 | `DLAA` | 5 | 1.00× | `target` 本身 | K |

- 名义倍率 = `target / render`。`UltraQuality`（枚举 4）在这张表里**没有位置**，本层不会请求它。
- `tier = -1` 表示"调用方没指定"：按 `target/render` 的实际比值挑最接近的名义倍率一档。
- `tier` 越界（<-1 或 >4）由 `mcdr_dlss_request_quality` 归一化，非法值返回 -1 且不改变现状。
- 显式档位与实际比值差超过 10%（`factor > nominal * 1.10`）时，改判到"名义倍率能覆盖它"的最紧一档，并打一条 ERROR（`tooSmallWarned` 只报一次）。例：请求 tier 1（2×）却给了 2.6× 的 render，会改判到 tier 0。
- `tier 4`（DLAA）要求 `render == target`：不等时**降级为 tier 2**，状态变化时打一条 ERROR。DLAA 建特征时 `Feature.InWidth/InHeight` 强制取 target 尺寸。

> 注意：`mcdr_dlss_impl.h` 的 `request_quality` 注释与设备模块 `setDlssQuality` 的 ERROR 文案都写 `0=Performance 1=Balanced 2=Quality 3=DLAA`，**与上表不符**，以 `kTierToNgx` 为准（见 §8）。

### 3.4 尺寸守卫（Dynamic scaling error 的唯一成因）

NGX 的动态分辨率区间是 `[target/nominal, target]`。`render` 比 `target/nominal` 还小时，`EvaluateFeature` 只写一行 `Dynamic scaling error ... outside of Min ... Max` 然后交出一张空纹理。

本层的两道处理（`resolveQualityValue`，`d3d12_dlss.cpp:1027`）：

```
needW = ceil(targetWidth  / nominal)
needH = ceil(targetHeight / nominal)
tooSmall(have, need) := have < need * 0.995      // 0.5% 容差
```

- 命中 `tooSmall` → ERROR：`render %ux%u is smaller than tier %d nominal input (%ux%u at %.2fx) — NGX answers with a dynamic scaling error and an empty texture, i.e. a black world with a working GUI; raise the render scale`。
- 没命中 → 清 `tooSmallWarned`，下次越界再报。
- 容差是 0.995 而不是 1.0：`854×480` 与名义 `853.33` 这种取整差异必须放过（差 0.08%），而 `844×440` 对 `1280×667` 那类 33% 的差距必须报。

### 3.5 feature flags

`computeFeatureFlags`（`d3d12_dlss.cpp:500`）只置三个位：

| 位 | 值 | 条件 | 说明 |
| --- | --- | --- | --- |
| `DepthInverted` | `1 << 3` | `in.depthInverted != 0` | 本项目 reverse-Z，必须置位 |
| `MVLowRes` | `1 << 1` | `renderWidth != targetWidth \|\| renderHeight != targetHeight` | MV 是 render 分辨率 |
| `AutoExposure` | `1 << 6` | 环境变量 `MCDR_DLSS_AUTOEXPOSURE=1` | 默认关，见 §8 |
| `MVJittered` | `1 << 2` | **不置位** | MV 已是真实运动，置位会让 NGX 减掉一个不存在的 jitter 差 |
| `IsHDR` | `1 << 0` | **不置位** | 内部主目标 8-bit UNORM；HDR 语义只在 Streamline 的 `colorBuffersHDR` 上 |
| `DoSharpening` | `1 << 5` | **不置位** | SDK 已标记 deprecated，`InSharpness` 固定 0 |

### 3.6 render preset（DLSS 模型）

`mcdr_dlss_request_preset(preset)` 接受 `NVSDK_NGX_DLSS_Hint_Render_Preset` 的值，合法 0..15，越界 WARN 并返回 -1。

| 值 | 名称 | 本层落到的 NGX 参数键（按当前档位） |
| --- | --- | --- |
| 0 | Default（交给 NGX 按档位决定） | 档位对应键，值写 0 |
| 5 / 6 | E / F（已弃用的 CNN 预设） | 同上 |
| 10 | J（transformer，残影少、闪烁多） | 同上 |
| 11 | K（transformer，推荐画质） | 同上 |
| 12 | L（Ultra Performance 默认） | 同上 |
| 13 | M（Performance 默认） | 同上 |

键的映射（`presetHintKey`）：

| 档位 | 参数键 |
| --- | --- |
| DLAA | `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA` |
| MaxQuality | `..._Preset_Quality` |
| Balanced | `..._Preset_Balanced` |
| MaxPerf | `..._Preset_Performance` |
| UltraPerformance | `..._Preset_UltraPerformance` |
| UltraQuality | 无键（返回 `nullptr`，NGX 自行决定） |

只写当前档位那**一个**键。写全部六个会把陈旧值留在未用键上，档位切换时被静默套用。

### 3.7 scratch buffer

`NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature_SuperSampling, params, &scratch)` 在**参数表填好之后**调用（`Width/Height/OutWidth/OutHeight/Feature_Create_Flags/PerfQualityValue/preset` 六个都在前面）。只设宽高会返回 `Success` + scratch=0。

- 返回值失败 → `createdWarned = true` + WARN，本帧失败。
- `scratch == 0` **不是错误**（现代 DLSS 内部自分配），但仍按 `64 MiB` 下限分配。
- 复用规则（`createScratchBuffer`，`d3d12_dlss.cpp:579`）：已有 buffer 且 `s.scratchBytes >= bytes` 就直接用；否则 `Release` 旧的再 `CreateCommittedResource`（DEFAULT heap、`BUFFER`、`ROW_MAJOR`、`FLAG_NONE`）。这是本层**唯一**自己 `Release` 的资源（§5）。

### 3.8 创建失败只报一次

`s.createdWarned` 为真时 `ensureFeature` 直接返回 false，不再尝试，也不再打日志。复位它只有两条路：`mcdr_dlss_request_quality` 或 `mcdr_dlss_request_preset` 的值发生变化（用户改了档位/模型）。尺寸变化**不**复位。

## 4. 每帧 evaluate

### 4.1 参数顺序约束

1. 所有 `NVSDK_NGX_Parameter_Set*` 必须在 `NVSDK_NGX_D3D12_GetScratchBufferSize` **之前**，也在 `NGX_D3D12_CREATE_DLSS_EXT` 之前。
2. 所有 `sl::DLSSOptions` / `NVSDK_NGX_DLSS_Eval_Params` 的赋值必须在 `EvaluateFeature` 调用**之前**；本层没有"设一半"的路径，构造完结构体一次性下发。
3. 命令列表必须先打开（`NGX_D3D12_CREATE_DLSS_EXT` 与 `EVALUATE` 都要录自己的命令）。
4. render pass 必须在 create 之前结束。`applyDlssToTexture` 完成绑定预检后调用 `endRenderPass(ctx)`，调用方后续重新开 pass。

### 4.2 jitter 约定

- 单位是**渲染像素**，不是 UV。
- 本层**原样透传** `in.jitterOffsetX/Y` 到 `InJitterOffsetX/Y`：不取负、不乘 2、不归一化。
- 相位由 JVM 侧推进（TAA 与 DLSS 共用同一条 Halton(2,3) 序列）；native 不维护相位计数，也没有"jitter 未推进"的检测。
- 采样点与投影矩阵必须同步：jitter 进了投影矩阵而 `InJitterOffset` 没给（或反过来）表现为画面整体轻微抖动 + 细节发糊，日志里看不出来。

### 4.3 motion vector 约定

| 项目 | 约定 |
| --- | --- |
| 内容 | 屏幕空间像素位移，不是 3D 世界速度（`mcdr_dlss_constants::motionVectors3D` 必须为 0） |
| 方向 | "当前像素在上一帧的位置 − 当前帧位置"，逐分量乘以屏幕尺寸；本层不校验、不取反 |
| 分辨率 | render 分辨率；`render != target` 时靠 `MVLowRes` flag 告知 NGX |
| 格式 | `DXGI_FORMAT_R16G16_FLOAT` 直接透传 |
| 其它格式 | 拷进本层建的中间纹理（按 `Width/Height` 复用），状态 `mv → COPY_SOURCE`、中间 → `COPY_DEST` → `PIXEL_SHADER_RESOURCE` |
| 缩放 | `InMVScaleX/Y = in->mvScaleX/Y`，原样；JVM 侧按"像素/UV"算好 |
| 缺失 | `pInMotionVectors == nullptr` → `applyDlssToTexture` 直接 false，并 WARN 一次提示打开速度矢量通道 |

方向写反的典型表现是整屏沿运动方向拖影；这种情况本层不会报错。

### 4.4 深度：快照与 reset

DLSS 的评估点在世界画完、GUI 之前，而 vanilla 在 `GameRenderer.render` 里已经把深度清成 0.0（reverse-Z 的"远"）。直接把 JVM 传来的深度喂进去等于没有深度，深度感知升采样退化成纯色彩升采样。

`mcdr_dlss_snapshot_depth`（`src/advanced/d3d12_dlss.cpp`）在世界渲染中途（录速度矢量那一处）调用，把有效深度拷进私有纹理：

| 项目 | 规则 |
| --- | --- |
| 复用条件 | `Width/Height/Format/MipLevels/DepthOrArraySize/SampleDesc.Count` 六项全一致 |
| 有效期 | 帧起点、每次捕获尝试开始、关闭 DLSS 与 shutdown 均失效；完整录制拷贝和源状态恢复后才记录本帧源资源 |
| 选择条件 | 本帧成功捕获、源资源与 `pInDepth` 相同、快照尺寸与 `renderWidth/renderHeight` 完全一致；否则两实现均使用本次 `pInDepth` |
| TYPELESS 映射 | `R32_TYPELESS→D32_FLOAT`、`R16_TYPELESS→D16_UNORM`、`R24G8_TYPELESS→D24_UNORM_S8_UINT`、`R32G8X24_TYPELESS→D32_FLOAT_S8X24_UINT` |
| 拷贝方式 | 六项形状匹配后使用 `CopyResource`，拷贝全部 mip、array layer 和 depth/stencil plane |
| 状态 | 源深度原状态由 `DlssQueryStateFn` 查出（缺省 `COMMON`），转 `COPY_SOURCE` 后拷贝并恢复原状态；快照 `COPY_DEST → NON_PIXEL_SHADER_RESOURCE` |
| 返回 | 1 = 已录制快照，0 = 参数不可用，-1 = 私有纹理创建失败；拷贝本身无同步返回值，须用 debug layer 与 fence 验证 |

调用方深度处于 `DEPTH_WRITE` 时也必须显式转 `COPY_SOURCE`，不能依赖 `COMMON` 的隐式提升。
归还原状态后，调用方可以继续清除或使用深度；NGX 与 Streamline 共用 `selectDlssDepth`，仅读取满足选择条件的独立快照。
JVM 在 `GameRenderer.render` HEAD 调用帧起点，必须早于 main pass RETURN 的捕获；不要在捕获后的清深度挂点重开本帧。
设备模块在校验捕获参数之前使快照失效，空 encoder 或空深度也不能让之前的成功捕获继续生效。
私有纹理由设备模块提供的回调创建，不以 NGX 专用的初始化状态作为快照前置条件。
实际选中的深度用于 SDK scope 与 Streamline tag；SR 完成后，FG provider 桥接也接收同一深度，
并在 NGX 使用 MV 中间纹理时接收该实际 MV，不能把原资源误声明为 SRV。
工作区离线检查 `python _port/rt_dlss_depth_check.py` 分别执行隔离 NGX 运行库与开发目录 Streamline，按
R32_FLOAT、D32_FLOAT、D24_UNORM_S8_UINT、D32_FLOAT_S8X24_UINT、R32_FLOAT、D32_FLOAT 的顺序提交。
每种格式四帧，另对 R32/D32 各执行捕获、漏捕获、空深度、空 encoder、不同源、320×180 快照、关闭再开启、重新捕获八帧。
每实现 40 次，共 80 次实际 SDK 求值；全部在求值前清零当前源深度。
有效快照的当前源状态保持原值；回退当前源时状态为 `NON_PIXEL_SHADER_RESOURCE`，从实际接管资源区分两条路径。
逐帧检查 SDK scope、后续颜色 compute 读回，并验证两实现合计 18,432,000 个源深度值为零，D3D12 corruption/error 为零。
此检查已接 GPU 总门；当前覆盖 2D 单 mip/单 layer/单 sample 的录制、格式往返与状态归属，
不直接读回快照内部 depth/stencil texel，也不证明 FG 实际消费、世界离开/分辨率切换的实机行为、世界内深度感知画质或 MSAA/数组/mip 的实际拷贝结果。

reset / history 的触发：

| 触发源 | 落点 | 生效时机 |
| --- | --- | --- |
| `in.reset != 0` | `InReset = 1` | 本次 evaluate |
| 特征重建（尺寸/档位/preset 变化） | `s.resetPending = true` | 下一次 evaluate（`InReset = (in->reset != 0 \|\| s.resetPending)`） |

成功 evaluate 后 `s.resetPending = false`。**不要**用 reset 代替重建：reset 只丢历史，换不掉 `PerfQualityValue`。

### 4.5 输出目标的三选一

| 顺序 | 目标 | 条件 | 状态切换 | 返回值 |
| --- | --- | --- | --- | --- |
| 1 | `s.externalOutput`（JVM 交付的 UAV） | `Width == targetWidth && Height == targetHeight && Format == out.Format` | `out → COPY_DEST`、external → `UNORDERED_ACCESS` | 1 |
| 2 | 私有 `dlssOutput`（本层申请、模块创建） | 无 handover 纹理 | 无（直接写） | 2 |
| 3 | 都没有 | 私有创建也失败 | — | 0（ERROR 一次） |

- 尺寸或格式不匹配时 ERROR 并**回退**到第 2 条，不会把不匹配的纹理硬转成 `UNORDERED_ACCESS`。
- 第 2 条之后本层自己做 copy-back：`ngxTarget → COPY_SOURCE`、`CopyResource(out, ngxTarget)`、`ngxTarget → UNORDERED_ACCESS`、`out → RENDER_TARGET`。
- 第 1 条成功后 `out` 仍是 `COPY_DEST`，本层把它转回 `RENDER_TARGET` 再交还。

### 4.6 返回值语义

| 值 | 含义 | 调用方该做什么 |
| --- | --- | --- |
| 1 | 结果已写进 handover 纹理（或直接写 `out`） | 正常继续，`out` 已是 `RENDER_TARGET` |
| 2 | 结果写进了**私有**纹理，没有拷回 | 不要当作"世界层已在 `out` 里"，那是黑屏路径 |
| 0 | 跳过（无特征/无参数/资源不可读） | 走自己的兜底 |
| -1 | 失败，或已 `disabled` | 走自己的兜底；不要期待下一帧自愈 |

设备模块把 2 也按 false 处理：`mcdr_dlss_evaluate(...) != 1` → `applyDlssToTexture` 返回 false → ABI 返回 0。所以 JVM 侧看到的只有 "1" 和 "0"。

### 4.7 每帧重申的三个设置

`applyDlssToTexture` 每次调用都重新下发 `mcdr_dlss_request_quality(gCtx.dlssQualityTier)`、`mcdr_dlss_request_preset(gCtx.dlssPreset)`、`mcdr_dlss_set_output_texture(overrideRaw)`。这三个都是幂等的，目的是**自愈**：`mcdr_dlss_shutdown()` 之后实现单元状态被清空，而设备模块的 `gCtx` 还留着旧值，不重申就会停在"没有 handover 纹理"的降级路径上。

### 4.8 SDK 资源归属

`applyDlssToTexture` 在任何转态、create 或求值前校验四张包装纹理，拒绝空资源、buffer、
输入/GUI 输出别名和正在被 SDK 接管的纹理。独立 handover 还必须有 UAV 标志且不与输入别名。
create 在 render pass 结束后执行，返回后恢复 draw heap 并清空 `cachedPso`。

实际求值使用 `DlssSdkResourceOps` 注入的 acquire/release 回调。
普通 TU 的 `DlssSdkResourceScope` 将真正选中的 color/depth/MV/output 裸资源交给设备模块；
设备模块以 `SdkRawResourceAccess` 创建统一 `SdkResourceScope`，记录 owner 为 `DlssSr`。
NGX 的深度快照、MV 中间纹理和私有/外部输出因此与其他 SDK 共用完整校验及归属机制。
Streamline 在 tagging 前接管，NGX 在 Evaluate 前接管；tag/options/evaluate 失败或提前返回均归还。
Streamline 要求 `slSetConstants` 入口存在；常量提交返回非 `eOk` 时，在 SDK 资源接管之前返回 0，
不执行后续 tag/evaluate、拷回或 FG 输入提交，输出交接状态为 0。
同一 frame token/viewport 的相同常量允许重复提交，不同常量由 SDK 返回 `eErrorDuplicatedConstants`（27/`0x1B`）。
常量错误日志前 3 次逐条输出，之后仅第 4、8、16…次输出；失败不锁死 DLSS，下一个新帧可正常重试。
tag 与 options 也必须成功后才执行 Evaluate；接管之后的失败由 scope 归还资源。

scope 归还时，实际输入保持 `NON_PIXEL_SHADER_RESOURCE`、实际输出保持 UAV；
后续宿主拷回可以继续调用跟踪式 transition，GUI 输出最终回到 `RENDER_TARGET`。
Streamline 的无 FG handover 在拷回后为 `COPY_SOURCE`，NGX handover 为 UAV。
NGX 使用 MV 中间纹理时，原 MV 因预备拷贝保持 `COPY_SOURCE`，实际中间纹理作为 SRV 归还。
这些是录制期间的归属规则，资源仍须保持有效直到提交 fence 完成；FG 的 Present 消费另行验证。

工作区 `rt_sr_signal_check.py --backends` 包含四帧光栅 DLSS SDR 检查，
固定 Performance 档、640×360 到 1280×720、独立 GUI 目标与 handover。
实际归还后查询状态、接管/归还/恢复堆计数，再 compute 读回输出；
常量 RGB 采样误差必须小于 0.08，D3D12 debug layer corruption/error 必须为空。
额外拒绝输入/输出别名、buffer 输出、handover 与输入别名和缺 UAV 标志。
`--raster-only --runtime-directory <dir>` 可在独立目录运行 NGX 实现，目录不放 Streamline DLL，
须包含匹配的 D3D12Core、d3d12SDKLayers、DXC、NGX SR 与 MCDR.dll。
无需环境变量或启动游戏；报告区分默认完整后端检查与独立光栅检查。
独立 NGX 检查增加 `--depth-snapshot` 可验证实际快照分支；四格式、有效期与回退检查见 §4.4。
`python _port/rt_dlss_failure_check.py` 在每次成功求值后，同 frame token 改变相机位置再求值，
要求实际 SDK 的 `0x1B` 诊断共四次，ABI 返回 0，五个宿主资源的状态与接管/归还/恢复堆计数完全不变。
每次拒绝后另建 frame token 并提交正确常量，要求恢复成功，随后实际 compute 读回颜色。
常量拒绝与求值拒绝检查已接 GPU 总门。
同一进程内，测试桥接使用 SDK 原始头编译，通过 `slGetFeatureFunction` 取得实际 `slDLSSSetOptions`，
临时将 viewport 0 设为 `eOff`；产品继续使用已有选项缓存，实际 Evaluate 返回 `eErrorInvalidIntegration`（19/`0x13`）。
每次该错误均须完整归还一次 scope，输入保持 SRV、实际输出保持 UAV、GUI 输出恢复 `RENDER_TARGET`，输出交接为 0。
失败后先执行实际 compute 并保存 GUI 输出读回，再恢复 SDK 选项、另建 frame token，要求重新求值成功。
成功求值次数只在 Evaluate 返回 `eOk` 后增加，失败不能计入 `mcdr_dx12QueryDlss` 的成功次数。
完整检查为四次常量拒绝、四次接管后 Evaluate 拒绝与十二次成功求值；接管/归还/恢复堆各 16 次。
恢复前的 compute 共检查 12,464 个颜色样本，最大误差 `0.0009765625`；D3D12 corruption/error 为零。
桥接由 runner 用 MSVC x64 `/std:c++latest` 自动编译到工作区临时目录，源代码、SDK 头、DLL 哈希均保存到报告。
此项不证明 tag/options 错误、SDK 部分写入 GPU 后的错误、NGX SDK 错误或 FG 失败收尾。
这组检查没有 HDR surface，光栅仅测 SDR；不代替非零 jitter、动态场景或窗口/世界内画质验收。

## 5. 输出资源的所有权

| 资源 | 谁创建 | 谁持有 | 谁销毁 | 入口 |
| --- | --- | --- | --- | --- |
| 私有输出 `dlssOutput` | 设备模块（`DlssCreateTextureFn`） | 模块持 `Dx12Object`，本层只存 `dlssOutputObject` + 裸指针 | 模块（延迟删除） | `destroyTrackedTexture` |
| 深度快照 `depthSnapshot` | 同上 | 同上 | 同上 | 同上 |
| MV 中间纹理 `mvBuffer` | 同上 | 同上 | 同上 | 同上 |
| scratch `depthStencil` | **本层**（`createScratchBuffer`） | 本层 | 本层 `Release()`（shutdown / 扩容时） | 无跟踪 |
| `externalOutput`（handover） | JVM（模块 `createTexture`） | JVM / 模块 | 本层**绝不**释放 | `mcdr_dlss_set_output_texture(nullptr)` 解除引用 |
| `s.feature` | NGX | 本层 | `NVSDK_NGX_D3D12_ReleaseFeature` | `destroyFeature` |

### 5.1 "先解绑再 close" 的硬约束

- 关命令列表之前，若调用了 `mcdr_dlss_set_output_texture(tex)`，必须在销毁 `tex` 之前调 `mcdr_dlss_set_output_texture(nullptr)`。本层持有裸 `ID3D12Resource*`，不做引用计数。
- 私有纹理一律走模块的 `destroyTrackedTexture`（内部进延迟删除队列），**禁止**在本层 `Release()`。唯一例外是 scratch buffer：它绕过模块自建，也必须由本层释放。
- `mcdr_dlss_shutdown()` 必须在设备还活着时调用；它把所有私有纹理交回模块（`ctx = nullptr`），再 `Shutdown1`。
- 违反后果：调试层报 `resource deleted prior to closing the command list`；release 下是 GPU 读写已释放内存 → 花屏、`RemoveDevice`、TDR 冻结。这类问题不会在崩溃点附近留下可读的日志。

### 5.2 状态契约

| 资源 | evaluate 结束后停在 | 依据 |
| --- | --- | --- |
| `out`（JVM 输出） | `RENDER_TARGET` | 本层最后一步 transition |
| handover 纹理 | Streamline：FG 开为 UAV、FG 关为 COPY_SOURCE；NGX：UAV | Streamline 由 `mcdr_dlss_output_state()` 回报，NGX 由资源跟踪查询 |
| `dlssOutput`（私有） | `UNORDERED_ACCESS` | copy-back 之后 |
| 深度快照 | `NON_PIXEL_SHADER_RESOURCE` | 供本次 evaluate 读 |
| scratch | 不转换 | 由 NGX 内部管理 |

`mcdr_dlss_output_state()` 返回上一帧结束时被 DLSS 写过的输出纹理的 `D3D12_RESOURCE_STATES` 整数，0 表示本帧没有 handover 纹理。JVM 做读回时必须用它当 `StateBefore`：按 `COMMON` 硬转会拿到未定义内容。

## 6. 能力查询

### 6.1 超分能力

`mcdr_dx12QueryDlss(note, noteCap)` → `queryDlss`（`d3d12_device.cpp:8839`）→ `mcdr_dlss_query`（`d3d12_dlss.cpp:1520`）。`note` 是唯一给 UI 的结论，`flags` 是机器可读的状态位：

| 位 | 名称 | 含义 |
| --- | --- | --- |
| 1 | `INIT_TRIED` | `ensureInit` 已执行过（不管成败） |
| 2 | `INIT_OK` | `NVSDK_NGX_D3D12_Init_with_ProjectID` 成功 |
| 4 | `PARAMS_OK` | `GetCapabilityParameters` 成功 |
| 8 | `OPTIMAL_OK` | `NGX_DLSS_GET_OPTIMAL_SETTINGS` 出过尺寸 |
| 16 | `FEATURE_OK` | 当前有一个活的 `NVSDK_NGX_Handle` |
| 32 | `DISABLED` | 已记过一次失败，永不重试 |

note 的判定顺序（先命中先返回）：

1. `disabled` → `"DLSS disabled, <failure> failed"`
2. 未尝试 → `"DLSS not initialised yet"`
3. Streamline 且 `!initOk` → `"Streamline owns NGX; DLSS switch is ON/OFF; tier=%u (%s), %llu evaluates so far"`
4. `!initOk` → `"NGX init failed (<failure>)"`
5. `!enabled` → `"NGX ready, DLSS switch is OFF ..."`
6. `!featureOk` → `"NGX ready, DLSS ON but feature not created yet (...)"`
7. 其它 → `"DLSS active, %llu frames submitted, render %dx%d -> target %dx%d, quality=%u (%s)"`

`queryDlss` 在 `!gCtx.rtx.dlssQueried && gCtx.rtx.ngxRuntime` 时会重探一次 RTX 支持（`resetRtxSupportCache` + `probeRtxSupport` + `notifyDlssDeviceReady`），否则直接用缓存的 `ngxNote`。

### 6.2 DLSS-G 能力（三态）

`mcdr_dx12FgQueryCaps(MCDR_FG_BACKEND_DLSSG, &caps)` 由 `d3d12_framegen.cpp` 的 `fillDlssGCaps` 填 `mcdr_fg_caps`（sizeof 80、align 4）：

| 字段 | 偏移 | DLSS-G 的判定 | 语义 |
| --- | --- | --- | --- |
| `runtimeLoaded` | 8 | `mcdr_dlss_available() != 0` | 运行库真的加载了（"头文件在"不算） |
| `deviceSupported` | 12 | `mcdr_dlss_streamline_requested() != 0` | 设备/驱动面能跑 FG 的必要条件：必须走 Streamline |
| `chainCompatible` | 16 | 同上 | 当前呈现链兼容：STREAMLINE 模式下建链一律升级成代理工厂，不依赖 FG 开关 |
| `available` | 4 | `dlssReady && streamline && reflexReady && srIsDlss` | 唯一结论位 |
| `needsReflex` | 20 | 恒 1 | FG 依赖 Reflex |
| `needsHudless` | 28 | 0 | HUDless 复用 SR 输出，不另开资源 |
| `needsUiResource` | 24 | 0 | UI 由应用自己合成回 backbuffer |
| `transitionsInputs` | 32 | 0 | SL 自己管 tag 资源的状态 |
| `maxGeneratedFrames` | 36 | 5 | 与 `mcdr_dlss_set_fg_mode` 的钳制区间 `[1,5]` 一致 |
| `latencyProvider` | 76 | `MCDR_FG_LATENCY_REFLEX` | 架构上"该配哪个"，不是"能不能用" |

`srIsDlss` 的判定用 `srSelectedBackend()`，没选过才用 `srActiveBackend()`，并接受 `MCDR_SR_BACKEND_NONE`。理由：100% 比例时 FSR/XeSS 本帧不跑，按 active 判会假放行。

note 的优先级：`"DLSS runtime not loaded"` → `"not on the Streamline path"` → `"needs Reflex, unavailable"` → SR 后端不是 DLSS 的短句。note 缓冲只有 28 字节，文案必须 ≤27 字符。

## 7. 错误码与 `mcdrLastError()`

### 7.1 NGX 结果码

`NVSDK_NGX_Result` 的失败段是 `0xBAD00000 | n`。本层用 `ngxResultString()` 打日志，格式 `"<text> (0xXXXXXXXX)"`：

| 值 | 名称 | `ngxResultText` | 常见触发点 |
| --- | --- | --- | --- |
| `0x00000001` | `Success` | `success` | — |
| `0xBAD00001` | `FAIL_FeatureNotSupported` | `feature not supported` | 显卡/驱动不支持 DLSS |
| `0xBAD00002` | `FAIL_PlatformError` | `platform error` | 驱动内部错误 |
| `0xBAD00003` | `FAIL_FeatureAlreadyExists` | —（只出 hex） | 重复 CreateFeature |
| `0xBAD00004` | `FAIL_FeatureNotFound` | `feature not found` | Release 一个失效句柄 |
| `0xBAD00005` | `FAIL_InvalidParameter` | `invalid parameter` | 参数表缺项、DLL 搜索路径没设、appData 不可写 |
| `0xBAD00006` | `FAIL_ScratchBufferTooSmall` | —（只出 hex） | scratch 小于 NGX 报的尺寸 |
| `0xBAD00007` | `FAIL_NotInitialized` | `not initialized` | 没 Init 就 Create |
| `0xBAD00008` | `FAIL_UnsupportedInputFormat` | —（只出 hex） | 颜色/深度/MV 格式不被接受 |
| `0xBAD00009` | `FAIL_RWFlagMissing` | —（只出 hex） | 输出纹理没有 `ALLOW_UNORDERED_ACCESS` |
| `0xBAD0000A` | `FAIL_MissingInput` | —（只出 hex） | 少给输入纹理 |
| `0xBAD0000B` | `FAIL_UnableToInitializeFeature` | —（只出 hex） | 特征初始化失败 |
| `0xBAD0000C` | `FAIL_OutOfDate` | `out of date` | 运行库/模型版本与驱动不匹配 |
| `0xBAD0000D` | `FAIL_OutOfGPUMemory` | —（只出 hex） | 显存不足 |
| `0xBAD0000E` | `FAIL_UnsupportedFormat` | —（只出 hex） | 目标格式不支持 |
| `0xBAD0000F` | `FAIL_UnableToWriteToAppDataPath` | —（只出 hex） | appData 目录不可写 |
| `0xBAD00010` | `FAIL_UnsupportedParameter` | —（只出 hex） | 参数键不被这一版接受 |
| `0xBAD00011` | `FAIL_Denied` | `denied` | 项目 ID 未授权 |
| `0xBAD00012` | `FAIL_NotImplemented` | `not implemented` | 该构建未实现此 feature |

`ngxResultText` 只覆盖 9 个（表中有文本的那些），其余只输出 `0x%08X`。`0xBAD00009`、`0xBAD00006` 是最容易遇到却没有文本的两个。

成功判定用 `NVSDK_NGX_SUCCEED(v)` = `(v & 0xFFF00000) != 0xBAD00000`，不要写 `== 0`（成功值是 1）。

### 7.2 返回值与 `mcdrLastError()` 的对应

| 入口 | 成功 | 跳过 | 失败 | 写 `mcdrLastError`？ |
| --- | --- | --- | --- | --- |
| `mcdr_dx12CreateDlssFeature` | 1 | 0 | -1（参数为空） | 否 |
| `mcdr_dx12EvaluateDlss` | 1 | 0 | -1 | 否 |
| `mcdr_dx12QueryDlss` | 1 | 0 | -1（`noteCap < 0`，或 `noteCap > 0` 而 `note` 为空） | 否 |
| `mcdr_dx12SnapshotDlssDepth` | 1 | 0 | -1 | 否 |
| `mcdr_dx12SetDlssQuality` / `SetDlssRenderPreset` / `SetDlssOutput` | 归一化值 / 1 | 0 | -1 | 否 |
| `mcdr_dlss_*` 内部函数 | 见 `mcdr_dlss_impl.h` | — | — | 否 |

> **缺陷**：DLSS 全链路**不写** `mcdrLastError()`。`setLastError` 只出现在 `abi_async.cpp` / `abi_mesh.cpp` / `abi_pipeline.cpp`，而 `gLastError` 是 `thread_local`。所以 DLSS 失败后读 `mcdrLastError()` 拿到的是**同一线程上别处的旧文本**（通常是空的）。失败原因只存在于 native 日志和 `mcdr_dx12QueryDlss` 的 note 里，机器可读的部分只有 `mcdr_dlss_query` 的 flags 位。

### 7.3 排错对照

| 现象 | 先说查什么 |
| --- | --- |
| 每帧 create 返回 0，日志无 NGX 行 | `nvngx_dlss.dll` 是否在 `register_vendor_dir` 报的那个目录里 |
| `Init` 返回 `0xBAD00005` | `PathListInfo` 是否指向 SDK 目录（§2.3）；appData 是否可写 |
| `GetScratchBufferSize` 返回 Success 但 size=0 | 正常，本层按 64 MiB 兜底 |
| 世界全黑、GUI 正常 | `Dynamic scaling error`：render 小于该档名义输入（§3.4） |
| 世界正常但整屏拖影 | MV 方向或 `mvScale` 反了（§4.3） |
| 画面轻微抖动 + 细节糊 | jitter 与投影矩阵不同步（§4.2） |
| `0xBAD00009` | 输出纹理没有 UAV 位 |
| `0xBAD0000C` | 运行库与驱动版本不匹配 |

## 8. 已知限制与缺陷

> **缺陷**：Dynamic scaling error 不是运行时可以兜住的错误。只要 `render < ceil(target / nominal) * 0.995`，NGX 就交出空纹理，表现为**世界全黑、GUI 正常**，而本层除了那条 ERROR 之外没有任何补救手段。选档时必须先算名义输入；`tier 0`（3.0×）最容易踩。

> **缺陷**：档位编号的文案与实现不一致。`include\mcdr_dlss_impl.h` 的 `request_quality` 注释与设备模块 `setDlssQuality` 的 ERROR 文案都写 `0=Performance 1=Balanced 2=Quality 3=DLAA`，实际映射是 `0=UltraPerformance 1=Performance 2=Balanced 3=Quality 4=DLAA`（§3.3）。以代码里的 `kTierToNgx` 为准。

> **缺陷**：`AutoExposure` 只能靠环境变量 `MCDR_DLSS_AUTOEXPOSURE=1` 打开，且打开后并不给 `pInExposureTexture`。这与"开关一律走配置"的约定冲突，且这一组合（声明自动曝光却不给曝光纹理）在本项目的 D3D12 环境里没有验证过。

> **缺陷**：一次失败即永久禁用。`s.disabled` 一旦由 evaluate 失败置位，`mcdr_dlss_evaluate` 立刻返回 -1，之后**永不重试**；`createdWarned` 让建特征失败也只报一次。恢复手段只有改档位/改 preset（复位 `createdWarned`）或重建设备。

> **缺陷**：没有 reactive mask / transparency mask。水面、玻璃、粒子这类"有颜色但运动矢量不可信"的像素只能靠 DLSS 自己的启发式，表现是实体/粒子边缘残影。`NVSDK_NGX_DLSS_Eval_Params` 里对应的指针本层全部留空。

> **验收限制**：`MVJittered` 保持关闭，调用方必须移除 MV 中的 jitter 差。RT 的产品 HLSL 数值对照与 RR/SR 偏移负例覆盖该约定；光栅 DLSS 的动态场景、残影与窗口切换仍需单独验收。

> **缺陷**：私有 UAV 兜底路径写不进像素。同源、同调用点、同一条命令列表的对照：写进模块建的目标读回 `mean=17.8/16.5/11.6/255.0`，写进本层自建的私有纹理读回 `mean=0.0/0.0/0.0/0.0`。所以 handover 纹理（`mcdr_dlss_set_output_texture`）是**唯一**可用路径，私有纹理只是历史兜底；返回 2 的那一帧等于黑屏。

> **缺陷**：DLAA 请求会被静默降级。`render != target` 时 tier 4 变成 tier 2，只在状态切换的那一次打 ERROR；之后日志安静，UI 上仍显示 DLAA。

> **缺陷**：Streamline 模式下 native NGX 永不初始化，`mcdr_dlss_query` 的 NGX 分支（`INIT_OK` / `PARAMS_OK` / `OPTIMAL_OK` 位与 `optimalText`）在这一模式下永远是 0/空。诊断只能看 Streamline 自己的日志。

> **缺陷**：DLSS-G 要求 HUDless 是 SR 的输出纹理。`outResource == out` 时（DLSS 直接写进 backbuffer）拿不到干净的 HUDless，本层直接不下发 FG 选项；`slDLSSGSetOptions` 也只在值变化时调用，关掉时必须真的发一次 `eOff`（否则 SL 继续插帧而调用方以为已经关了）。

> **缺陷**：`sl.interposer.dll` 的探测与 `MCDR_DLSS_IMPL` / `MCDR_STREAMLINE` 两个环境变量是本层唯一的功能入口，配置系统里看不到。`MCDR_DLSS_IMPL=ngx` 是强制回退到 NGX 直连的唯一手段。

其余限制（不是缺陷）：

- 内部主渲染目标恒为 8-bit UNORM，NGX 路径下 `IsHDR` 恒不置位；HDR 语义只在 Streamline 的 `colorBuffersHDR` 上。
- `InSharpness` 固定 `0.0f`。DLSS 的锐化已弃用，锐化由独立的后处理通道负责。
- `depthFormatOrdinal` 与 `nearZ` / `farZ` 在 NGX 路径下只用于变更日志，不参与 evaluate。
- 本层不做帧率限制、不做 present 拦截；DLSS-G 的插帧发生在 `Present`，由 Streamline 与交换链代理完成。

## 9. 文件导航

路径相对 `MCDR\`。

| 文件 | 内容 |
| --- | --- |
| `src\advanced\d3d12_dlss.cpp` | 全部实现：NGX 初始化、运行库发现、特征创建/销毁、evaluate、深度快照、Streamline 分支、DLSS-G 选项、状态查询 |
| `include\mcdr_dlss_impl.h` | 实现单元与设备模块的唯一接口（`extern "C"`）；`DlssTextureDesc`、`DlssTransitionFn`、`DlssQueryStateFn`、`DlssCreateTextureFn`、`DlssDestroyTextureFn` |
| `include\mcdr_abi.h` | `mcdr_dlss_input`（112 B）、`mcdr_dlss_constants`、`mcdr_fg_caps`（80 B）、`MCDR_DLSS_EXPOSURE_*`、`MCDR_FG_*` 常量与全部 `mcdr_dx12*` 导出声明 |
| `src\c_abi\abi_command.cpp` | `mcdr_dx12CreateDlssFeature` / `mcdr_dx12EvaluateDlss` / `mcdr_dx12QueryDlss` 三个薄转发 |
| `src\c_abi\abi_common.cpp` | `mcdrLastError` 与 `setLastError*`（DLSS 路径不调用） |
| `src\core\d3d12_device.cpp` | 设备侧闸门与解包：`applyDlssToTexture`、`queryDlss`、`setDlssQuality`、`setDlssRenderPreset`、`setDlssOutput`、`snapshotDlssDepth`、`wireDlssImpl` |
| `src\advanced\d3d12_framegen.cpp` | DLSS-G 能力表 `fillDlssGCaps` 与 `DlssGFrameGenProvider` |
| `external\dlss\include\nvsdk_ngx_defs.h` | NGX 结果码、`PerfQuality_Value`、`DLSS_Feature_Flags`、`Hint_Render_Preset` |
| `lib\dlss\` | `nvngx_dlss.dll` / `nvngx_dlssd.dll` / `nvngx_dlssg.dll` / `nvngx_dlssnr.dll`（开发目录，不入发行 JAR） |
| `lib\streamline\` | `sl.interposer.dll` / `sl.dlss.dll` / `sl.dlss_g.dll` / `sl.reflex.dll` / `sl.common.dll` 等 |

相关文档：`docs\advanced\upscalers.md`（超分抽象层与三条后端的取舍）、`docs\advanced\framegen.md`（帧生成抽象层、HUDless 捕获与相机常量）、`docs\abi\README.md`（C ABI 与结构体布局约定）、`docs\device\README.md`（设备模块、资源跟踪与延迟删除）。

## 10. 文档维护

1. 改 `kTierToNgx` / `kTierNominalFactor` / 尺寸守卫公式时，同一次提交里更新 §3.3、§3.4 与 §8 里关于档位文案的那条缺陷。
2. 改 `computeFeatureFlags` 或 `NVSDK_NGX_DLSS_Create_Params` 的字段时，更新 §3.5；注释与行为不一致时**以代码为准**并顺手改注释。
3. 新增/删除 `mcdr_dlss_impl.h` 的导出时，更新 §1.3、§5 的表与 §9 的导航；签名里只允许 `void*` / POD / `char*`。
4. 改 `mcdr_fg_caps` 的字段或 `fillDlssGCaps` 的判定时，更新 §6.2，并确认 `sizeof = 80` / `align = 4` 未变（Kotlin 侧 `MCDR_Layout.kt` 有镜像）。
5. 任何涉及动态分辨率、档位或输出纹理的改动，先确认手写表格里的百分比与代码里的常数一致；本层所有阈值（`0.995`、`1.10`、`64 MiB`、`[1,5]`、`1024` 字节日志缓冲）都在代码里，不要凭记忆写。

## 11. 光追 Ray Reconstruction

光追 RR 通过 `mcdr_dx12DlssdEval` 进入本单元的 `mcdr_dlssd_eval`，使用独立的 viewport 1。
光栅 DLSS-SR 与帧生成仍使用 viewport 0。八张纹理与 104-float 相机载荷遵守 [ABI 契约](../abi/README.md)。
反射重投影使用 `kBufferTypeSpecularHitDistance`，绑定 render 尺寸的 `R32_FLOAT` 世界单位距离，
并在 `DLSSDOptions` 中填写 `worldToCameraView/cameraViewToWorld`。
反射打空写 `1e9`；禁止复用 NRD 输入 alpha 中的粗糙度归一化距离。

- Streamline 与 NGX 探测使用相同的 Project ID `7c1f4a52-9b3e-4d68-8f21-5a6c0e2d9b74`、engine version `1.0`。
  engineVersion 非空时必须提供 projectId，保证 feature-requirements 查询和初始化使用相同身份。
- 输入为带噪、已重调制的线性 HDR 颜色，深度以 `kBufferTypeLinearDepth` 传正 viewZ，`depthInverted=eFalse`。
  法线使用原始世界空间 xyz，粗糙度放 w；禁止将 NRD 的八面体编码纹理当作 RR packed normal。
- 输入/输出尺寸变化时调用 `slDLSSDGetOptimalSettings`，确认输入位于 SDK 的动态范围。
  原生输入优先 DLAA，缩放输入按质量档依次查询；没有合法范围则拒绝求值。
- 相机位置、方向、pinhole、抖动、MV 比例与布尔标志必须完整填写。相机视图正逆矩阵每帧更新到 DLSSDOptions。
  `slSetConstants` 失败时停止求值；帧号保持递增，重置历史时禁止复用已提交的编号。
- 输入转 NPSR、输出转 UAV，连续调用 setTag/evaluate，完成后由设备模块恢复 COMMON 与绘制描述符堆。
  返回成功仅表示 SDK 接受命令；需结合后续帧无回退、调试层与世界画面验证。

输出是显示尺寸的线性最终颜色，合光禁止再次乘 albedo 或叠加 emissive。
RR 不可用时由 JVM 重打包 YCoCg 再调用 NRD；低倍率 NRD 输出允许继续调用独立 SR。

### 11.1 NRD 后独立 DLSS-SR

`mcdr_dx12RtSrEval` 转入 `mcdr_dlss_sr_eval`，固定使用 viewport 2。输入为重调制后的线性 HDR 颜色、
设备深度与像素 MV，输出为显示尺寸 RGBA16F。四张纹理顺序与返回值见 [ABI 契约](../abi/README.md)。
SR 的 `kBufferTypeDepth` 必须绑定设备深度，不能复用 RR 的正 viewZ 标签。
JVM 重调制 pass 用本帧投影从 viewZ 生成设备深度，`depthInverted` 根据投影方向填写。

SR 与 RR 共用 104-float 相机转换，矩阵保持无 jitter，68–71 传当前/上一帧屏幕内容偏移 xy。
`jitterOffset` 读取 68/69，单位为 render 像素；共享 Halton `(Jx,Jy)` 对应 `(Jx,-Jy)`。
四分量须有限且在 `[-0.5,0.5]`，两个入口均在接管资源前校验。
MV 已移除 jitter 差，`mvecScale=(1/renderWidth,1/renderHeight)`，
相机常量每帧更新。尺寸变化时查询 `slDLSSGetOptimalSettings` 的合法范围再设选项；
无合法范围时拒绝求值。资源状态与恢复规则和 RR 相同，两个入口使用独立的递增帧号。
JVM 在 Plan 变化时复位历史，失败后保留 NRD 合光，避免重复重调制和曝光。

### 11.2 RR 离线输入与历史对照

RR、NRD 与不同 SR provider 的最终画面按各自效果独立验收，不要求图像级或像素级一致。
以下误差、宽度和负例倍数属于指定合成场景的诊断配置，不作为 S4 的退出条件。
同一 SDK 的不同入口、历史路径或重建前后也不要求最终颜色逐位相等。
SDK 输入/标签/单位、编码、真实求值、资源归还、互斥和失败恢复是工程检查；主观画质由用户在世界内确认。
总门所用 RR core、共享 jitter 与曝光 ABI 报告的 `passed` / `engineering_passed` 只表示该探针的工程检查。
图像指标写入 `image_diagnostics`，汇总为 `image_diagnostics_passed`，未达到预算不改变工程退出码。
`world_quality=unverified` 保留世界内目视验收的状态，不能由离线报告自动置为通过。

在工作区根执行 `python _port/rt_rr_temporal_check.py --case core`，通过公开八纹理 ABI 求值实际 RR，
不启动游戏。使用 640×360 原生输入/输出，每组 24 帧，主进程 123 次求值与独立对照进程 122 次求值；
逐帧等 fence 后通过 compute 读回，要求全部输出分量有限。

| 检查 | 场景与对照 | 当前脚本诊断配置 |
| --- | --- | --- |
| HDR | 带噪线性 RGB `(4, 2, 0.5)` | 每通道相对 RMSE 小于 5%，R 通道 MSE 小于带噪输入的 1/4 |
| MV | 世界位置线性辐亮度渐变，相机 yaw 在 0/0.45 rad 间交替；像素 MV 对比反号/误喂 UV 单位 | 正确 R 通道 MSE 小于 0.005，两个负例均超过正确值 2 倍 |
| 历史 | 预热后信号从 0.8 变为 1.0；按需 reset 对比独立进程的每帧 reset | 对齐求值次数、SDK 帧号及输入，RGB 最大差小于 0.002；CONTINUE 的各通道 MSE 均超过 reset 2 倍 |

相机矩阵遵守与产品相同的行向量转换，MV 为 `prevUV-curUV` 的 render 像素位移。
历史对照必须同时对齐 SDK frameIndex 与实际求值次数；只对齐帧号或比较相邻两次 reset，
内部采样相位会产生差异。独立对照使用全程 reset 预热，不应描述成冷启动首帧对照。

结果写入工作区 `_tmp_s4_rr_temporal.json`，包含设备、运行库与探针 SHA-256、参数、阈值、
逐路误差和未覆盖项；`rt_check_all.py --gpu` 仅调用上述 `core` 检查。
`--case all` 另外执行遮挡平面的深度负例、曲面 GGX 高光的法线/粗糙度探索，
写入 `_tmp_s4_rr_exploratory.json`。这些场景的负例敏感度与过滤后高光宽度只作诊断，
不改变工程退出码，也不替代世界内画质验收。独立深度边缘/高光分析命令仍可返回诊断性的 `INCOMPLETE`。

当前玩家 RT RR 的 `DLSSDOptions.preExposure/exposureScale` 为 1，输入保持未曝光线性 HDR，
曝光与 tone map 在后级执行一次。HDR 读回不等于手动/自动曝光对照；RR 官方指南明确忽略
DLSS 的 `useAutoExposure` 选项，不能将该选项当作 RR 的曝光切换入口。
玩家 RT DLSS-SR 也使用 `preExposure/exposureScale=1`，RR/SR 均提交引擎测光的曝光资源 tag。
玩家 AUTO 是引擎生成显式倍率，SR 的 `useAutoExposure` 仍为 eFalse。
旧入口无资源时，Streamline SR 插件会设置 AutoExposure flag，即使 `useAutoExposure=eFalse`。
RR 插件读取可选曝光 tag 并交给 NGX，其创建 flags 未设置 AutoExposure；缺资源时 SDK 内部行为，代码中未明确。
所带 NGX 头将曝光资源定义为包含最终曝光倍率的 1×1 纹理，适配时使用 `R32_FLOAT`。
SR 的曝光 flag 在 feature 创建时确定，重建判断只有 mode、输出尺寸和 preset；
改变 `useAutoExposure` 必须同时明确 feature 生命周期，不能用 setOptions 成功代替实际模式切换验证。

ABI 23 的 `mcdr_dx12DlssdEvalExposure` / `mcdr_dx12RtSrEvalExposure` 通过独立两 float 参数段
传入 `preExposure / exposureScale`，可在原八/四张纹理后追加一张 1×1 `R32_FLOAT` 曝光资源。
标量范围为 `[2^-16,2^16]`，不改变 104-float 相机载荷。
新旧入口共同检查纹理形状、尺寸、读写能力、别名与归属，完整预检后才接管。
曝光 tag 使用 1×1 extent；SR 按资源有无明确设置 `useAutoExposure`，RR 仅提交可选资源与两个标量。
同一模式内标量变化会刷新 SR options；RR 每次设置 options。

每个 RT viewport 保存上次实际求值的 completion fence/value，SDK 返回错误也保留在途生命周期。
曝光资源有无变化时，先等上次提交完成，再 `slFreeResources`、清尺寸/选项缓存并设置 pending reset。
同一未提交列表内切换返回 0，诊断 `exposure: submit before switching modes`；其他未完成提交最多等 5 秒。
reset 只在实际求值成功后消费；曝光纹理返回后回 `COMMON`，资源须存活至对应 fence 完成。
玩家 RR/RT DLSS-SR 调用显式曝光入口，两个标量均为 1；1×1 R32_FLOAT 纹理由引擎测光生成。
NeoForge 配置 `exposureMode / exposureEv / exposureSpeed` 控制引擎手动/自动测光，
两种模式都保留资源，最终合光读取同一纹理并执行一次曝光；SDK 输入仍为未曝光线性 HDR。

`rt_exposure_abi_check.py` 经真实产品 ABI 执行旧入口、新默认参数及四段资源模式切换，
RR/SR 共 384 次求值与 344 次拒绝。旧/新默认输出逐帧逐位一致，切换组各段末八帧 MSE 小于 0.001；
预曝光 .25/4 及段内翻倍时核验实际 HDR 输入，逐帧验证曝光纹理未被修改。
父进程核验源码/SDK/运行库/输入哈希、模式序列，并重算二十四份分段末帧快照的曝光值与误差。
接管/归还/恢复堆计数、拒绝原子性、后续 compute 读回及 D3D12 debug layer 同时检查。
新增两个同队列跨 encoder 组各执行 164 次求值，检查另一 encoder 完成不能满足旧 encoder 的 fence。
旧列表未提交和已提交但 GPU queue 被 fence 阻塞时，均须约五秒超时且保持两套资源状态/归属/计数不变；
提交旧列表或解除阻塞后，同一新列表上的重试须成功。
另外四次双向资源模式切换观察至少 0.25 秒阻塞等待，解除阻塞前调用不得返回或接管资源。
每次恢复后再求值 31 帧，脚本对其 MSE 使用 0.001 诊断预算，重建 feature 后首帧使用 0.1；
图像指标未达到预算不阻塞阶段，重试成功、输出有限与资源归还仍须通过。
父进程复核新增二十四份完整快照、fence/时间/模式序列和两套 scope 计数。
总计 712 次实际 SDK 求值、348 次原子拒绝，纳入 `rt_check_all.py --gpu`；
`--cross-encoder` 只运行新增两组并复用已有 JVM 输入。
独立 GPU queue 与同时求值不在本组覆盖内；世界内画质仍待确认，曝光错配敏感度保留为可选诊断。

RR/RT DLSS SR 使用 [SDK 接管](../device/README.md#57-sdk-资源接管与归还)，返回后输入输出均回 `COMMON`。
core 探针逐次查询状态与接管/归还/恢复堆计数，检查四种非法绑定原子拒绝，
并要求 D3D12 debug layer 无 corruption/error、后续 compute 可读回。
光栅 DLSS、FG、可选 mask、跨 encoder 共享归属不在该检查范围内。
`rt_jitter_check.py` 独立执行 RR 原生尺寸与 RT DLSS SR 50% 放大的非零偏移对照。
各后端正确、反号与零偏移组使用独立进程及相同 SDK 帧号；脚本诊断配置为正确 MSE 小于 0.001、
两个错误组各大于正确值两倍，这些图像阈值不作阶段门。
非法偏移在接管前拒绝，资源归还与 D3D12 debug layer 同时检查。
全部历史失效原因、世界内切换与主观残影仍需分别验证。
