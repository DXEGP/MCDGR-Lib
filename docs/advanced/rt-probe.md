# 光线追踪能力探测

本文只讲**探测与状态上报**：D3D12 基线能力（含 DXR tier）、第三方 SDK 有没有装好、光追可见性
分层当前处在哪一档，以及这些结论怎么变成 JVM 能读的文本 / JSON。这里没有光追渲染路径。

## 1. 三块互不相干的数据

| 数据 | 位置 | 回答什么 |
| --- | --- | --- |
| `DeviceContext::Capabilities` | `src\module\core\mcdr_device.cppm` 内的 POD | D3D12 **基线**：feature level、SM、各 tier、DXC 能否冒烟编译 |
| `mcdr::RtxSupport` | `include\mcdr_rtx_support.h` | 第三方 **SDK 装没装**：NGX / NRD / XeSS / FidelityFX |
| `mcdr_rt_state` | `include\mcdr_rt_state.h`（纯 C，4 个 atomic） | 可见性分层档位 + G-Buffer 就绪状态 |

`raytracingTier >= 1.0` 不代表画面里有光追，`dlssRayReconstruction = true` 也不代表本工程实现了
光线重建：三块数据分别是「D3D12 支持什么」「装了哪些 SDK」「此刻打算跑哪一档」。

```
JVM                      native
 │ mcdr_dx12CreateDevice ─▶ ensureDevice：D3D12CreateDevice → queryDeviceCapabilities
 │                          → EnumAdapterByLuid → wireDlssImpl → probeRtxSupport(adapter)
 │◀─ 文本：“适配器 (FL=…; caps…); selftest”
 │ mcdr_dx12GetAdapterInfo ─▶ adapterInfoJson（adapter 键 + caps 键 + rtx 键，平坦）
```

## 2. 探测时机

`ensureDevice`（`src\core\d3d12_device.cpp`）里的顺序是硬要求：

1. `D3D12CreateDevice(…, kRequiredFeatureLevel = 12_2)`：失败先退回系统默认适配器，再失败才报错；
2. `queryAdapterName` → **`queryDeviceCapabilities`** → `capabilitiesText` 打 `device ready:` 一行；
3. `logAgilityRuntime`、`applyStreamline`、`CreateDXGIFactory1` + `EnumAdapterByLuid` 填 `gCtx.adapter`；
4. **`wireDlssImpl()`** 必须在探测之前接好 DLSS 日志出口，否则 `mcdr_dlss_load_runtime()` 内部逐条
   `LoadLibrary` 尝试的日志被静默丢掉（幂等，`notifyDlssDeviceReady` 里还会再调）；
5. **`gCtx.rtx = probeRtxSupport(gCtx.adapter.Get())`** 必须在适配器就绪之后，随后
   `notifyDlssDeviceReady()`。

只有两条路径会探测，不要把它塞进每帧：

| 触发点 | 做什么 |
| --- | --- |
| `ensureDevice`（首次建 / 换设备） | 全量：D3D12 基线 + 四家 SDK |
| `queryDlss(char*, int)`（JVM 问 DLSS 状态） | 若 `!gCtx.rtx.dlssQueried && gCtx.rtx.ngxRuntime`：`resetRtxSupportCache()` → 重新探测 → `gDlssAvailabilityLogged = false` → `notifyDlssDeviceReady()` |

第二条的原因：NGX 在 `NVSDK_NGX_Init` 成功前对 `GetFeatureRequirements` 的回答可能不可用，而 Init
要等命令列表打开。

## 3. D3D12 基线能力

`queryDeviceCapabilities` 先 `caps = Capabilities{}` 再逐项查；`device == nullptr` 直接返回，
`queried` 保持 `false`。

| 字段 | 默认值 | 来源 |
| --- | --- | --- |
| `featureLevel` | `D3D_FEATURE_LEVEL_12_2` | 取 `gCtx.featureLevel`（创建时就是 12_2） |
| `shaderModel` | `D3D_SHADER_MODEL_5_1` | `D3D12_FEATURE_SHADER_MODEL`，从 `0x6a` 逐档降级问 |
| `meshShaderTier` / `samplerFeedbackTier` | 0 / 0 | `D3D12_FEATURE_D3D12_OPTIONS7` |
| `raytracingTier` | `D3D12_RAYTRACING_TIER_NOT_SUPPORTED` | `D3D12_FEATURE_D3D12_OPTIONS5`（**唯一** DXR tier 来源） |
| `shadingRateTier` / `shadingRateImageTileSize` / `additionalShadingRates` | 0 / 0 / false | `D3D12_FEATURE_D3D12_OPTIONS6` |
| `resourceBindingTier` / `enhancedBarriers` | 0 / false | `D3D12_FEATURE_D3D12_OPTIONS` / `_OPTIONS12` |
| `queried` | false | 进入函数后置 `true` |
| `dxcAvailable` / `meshShaderCompileOk` / `dxcPath` / `dxcNote` / `dxcProfile` | false / false / 空 / 空 / `ms_6_9` | `probeDxc`：动态加载 `dxcompiler.dll` + 一次 `ms_6_9` 冒烟编译 |

SM 的探法：把 `D3D12_FEATURE_DATA_SHADER_MODEL.HighestShaderModel` 先填上限，再按 `0x6a`
（6.10，SDK 头里没有枚举）→ `6_9` → `6_8` → … → `6_0` → `5_1` 逐个 `CheckFeatureSupport`，
第一个 `SUCCEEDED` 的就是真实最高档。从高往低问，避免老 runtime 对未知枚举返回 `E_INVALIDARG`
后被误判成 5.1。

每项独立判 `SUCCEEDED`，失败保持默认值——上层据此保守门控，不要假设「查询失败 = 支持」。
`raytracingTier` 写进 JSON 的是 `D3D12_RAYTRACING_TIER` 原始整数，**不是** `0 / 1 / 2`：

| 枚举 | 数值 | 含义 | 本工程怎么用 |
| --- | --- | --- | --- |
| `D3D12_RAYTRACING_TIER_NOT_SUPPORTED` | 0 | 设备 / 驱动不支持 DXR | 只上报，不影响设备创建 |
| `D3D12_RAYTRACING_TIER_1_0` | 10 | 支持 DXR 1.0（按 API 约定需 SM 6.3 与相应驱动） | 只上报 |
| `D3D12_RAYTRACING_TIER_1_1` | 11 | 支持 DXR 1.1（需 SM 6.5，以 `d3d12.h` 为准） | 只上报 |

全库没有任何代码按 `raytracingTier` 分支（`grep raytracingTier` 只有三处：赋值、`capabilitiesText`、
`capabilitiesJson`）。

## 4. 第三方 SDK 探测（`mcdr::RtxSupport`）

实现是 `src\advanced\d3d12_rtx_probe.cpp`，一个**普通 TU**：不 `import` 任何 `mcdr.*` 模块（否则与
设备模块互相 import 成环），只 include `mcdr_rtx_support.h`、`<windows.h>`、`<winver.h>`
（`GetFileVersionInfoW` / `VerQueryValueW`）与 nlohmann/json（必须在 `import std;` 之前 include）。

### 4.1 缓存与补探

```
probeRtxSupport(adapter)
 ├─ !s.dlssProbed                  ──▶ probeDlss(s, adapter)
 ├─ else adapter && !s.dlssQueried ──▶ 清 dlssQueried/dlssQueryNote，重跑 probeDlss
 ├─ s.nrdVersion.empty() / !s.xessProbed / !s.ffxProbed ──▶ probeNrd / probeXess / probeFidelityFx
 └─ return 常量引用
```

「探测过没有」记在**缓存字段本身**（`dlssProbed` / `nrdVersion.empty()` / `xessProbed` /
`ffxProbed`），不用函数局部 `static`，否则 `resetRtxSupportCache()`（即 `cache() = RtxSupport{}`）
清不掉。线程安全由调用方保证，只在设备初始化 / 能力查询路径调。

### 4.2 DLSS / NGX

1. `mcdr_dlss_load_runtime()` 找并加载 `nvngx_dlss.dll`（裸名含拓宽搜索路径 → JVM 厂商目录 →
   `MCDR.dll` 同目录 → cwd），失败再兜底 `LoadLibraryW(MCDR_NGX_RUNTIME_DLL)` 与模块目录。失败时
   `ngxRuntime=false`、`ngxNote = "nvngx_dlss.dll not found (GetLastError=0x…)"`、三个特性的 `…Why`
   统一 `runtime missing`；成功时路径取 `mcdr_dlss_runtime_path()`。
2. **必须先登记厂商目录**：`mcdr_dlss_register_vendor_dir(<dll 所在目录>, n)`。登记会建
   `NVSDK_NGX_FeatureCommonInfo::PathListInfo`；NGX 默认只在应用目录（开发时是 JDK 的 `bin\`）找
   特性 DLL，拿不到路径表就对 Init 与 `GetFeatureRequirements` 回
   `FAIL_InvalidParameter (0xBAD00005)`，症状是 `ngx=yes` 而 `dlssSupported=no`。
3. `adapter == nullptr` → `dlssQueried=false`，`dlssQueryNote = "no DXGI adapter supplied; feature
   support not queried yet"`，**不拿 nullptr 去问**（否则「没适配器」会显示成「不支持」）。
4. `ask(feature, outOk, outWhy)` 调 `NVSDK_NGX_D3D12_GetFeatureRequirements(adapter, &info, &req)`
   （不需要 `NVSDK_NGX_Init`，正适合探测）：`r != Success` → `"query failed: " + ngxResultString(r)`；
   `req.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported` → 支持并清空 why，否则
   `supportBitsText(...)`，`MinOSVersion[0] != 0` 时追加 `" (min os <MinOSVersion>)"`。
5. 依次问 `NVSDK_NGX_Feature_SuperSampling`、`_FrameGeneration`、`_RayReconstruction`。若超分那路
   以 `"query failed:"` 开头 → `dlssQueried=false`，`dlssQueryNote = "feature requirement query was
   inconclusive (…); the real gate is the NGX init"`。

真正的门是 NGX Init 是否成功（`mcdr_dlss_can_run`）加上 Init 之后 `queryDlss` 的补探；
`RtxSupport` 的三个布尔位只是「探测期能问到的最好答案」。

### 4.3 其余三家

| SDK | 探测方式 | 能报什么 |
| --- | --- | --- |
| NRD | `nrd::GetLibraryDesc()`（不需设备、不需实例） | `nrdVersion`（`4.18.0`）、`nrdVersionDate`、`nrdDenoisers`（如 `REBLUR` / `RELAX` / `SIGMA`） |
| XeSS | `libxess.dll` + 全局函数 `xessGetVersion`；另查 `libxess_fg.dll` / `libxell.dll` | `xessVersion`（`2.0.2`）、`xessFileVersion`（`2.0.2.68`）、`xessFrameGenPresent`、`xessLowLatencyPresent` |
| FidelityFX | 五个 DLL 按文件名探（`GetFileAttributesW` + 文件版本资源，不必加载）；loader 还要 5 个入口全部 `GetProcAddress` 成功 | `ffxLoaderVersion`（`2.3.0.0`）与四个 `…Present` / `…Version` |

- XeSS 两个版本号都报：包名 `XeSS_SDK_3.0.2` 与运行期自报 `2.0.2` 不一致。`xessFrameGenPresent` /
  `xessLowLatencyPresent` 只说明**运行时文件齐了**，不代表硬件跑得动、也不代表本工程接了这两个特性。
- FFX 五个文件是 `amd_fidelityfx_loader_dx12.dll`、`_upscaler_`、`_framegeneration_`、`_denoiser_`、
  `_radiancecache_`；版本先由文件资源拿到（loader 加载失败也能报「文件在，是 2.3.0.0」）；入口是
  `ffxCreateContext`、`ffxDestroyContext`、`ffxConfigure`、`ffxQuery`、`ffxDispatch`，缺任一 →
  `ffxLoaderLoaded=false`，`ffxNote = "loader loaded but not all of the 5 ffx entry points resolved"`。
  非 x64 时 `nrdVersion` / `xessNote` / `ffxNote` 一律写 `not built for this platform`。

## 5. 上报给 JVM

### 5.1 文本行

`capabilitiesText(caps)` 的格式串（`d3d12_device.cpp:2015`）：

```
FL=<name> SM=<name>(0x%02X)/eff=<profile> mesh=%d samplerFeedback=%d rt=%d rtMode=%d
vrs=%d(tile=%u,extra=%d) binding=%d enhancedBarriers=%d
dxc=<yes|no>(<profile>) meshSmoke=<ok|fail>ngx=… nrd=… xess=… ffx=…
```

`rt=` 是 `raytracingTier` 的整数值，`rtMode=` 读 `mcdr_rt_state_mode()`（§6），末尾接
`RtxSupport::summary()`：`ngx=yes(SR,FG) nrd=4.18.0[REBLUR,RELAX] xess=2.0.2.68[fg,ll]
ffx=2.3.0.0[sr,fg,dn,rc]`——某家不可用时该段为 `no` / `off`，方括号里是运行时文件齐不齐。

### 5.2 JSON

`capabilitiesJson(caps)` 返回**字段串**（无外层花括号、无前导逗号），键名：`shader_model`、
`shader_model_value`、`shader_model_effective`、`mesh_shader_tier`、`sampler_feedback_tier`、
`raytracing_tier`、`shading_rate_tier`、`shading_rate_image_tile_size`、`additional_shading_rates`、
`resource_binding_tier`、`enhanced_barriers`、`caps_queried`、`dxc_available`、
`dxc_mesh_shader_compile_ok`、`dxc_profile`、`dxc_path`、`dxc_note`，再接全部 `rtx_*` 键。刻意
**不含** `feature_level` / `feature_level_value`（`adapterInfoJson` 已有同名键）。

| SDK | `rtx_*` 键 |
| --- | --- |
| DLSS | `rtx_dlss_probed`、`rtx_dlss_runtime`、`rtx_dlss_sr`、`rtx_dlss_fg`、`rtx_dlss_rr`、`rtx_dlss_runtime_path`、`rtx_dlss_note`、`rtx_dlss_sr_why`、`rtx_dlss_fg_why`、`rtx_dlss_rr_why` |
| NRD | `rtx_nrd_linked`、`rtx_nrd_version`、`rtx_nrd_version_date`、`rtx_nrd_denoisers` |
| XeSS | `rtx_xess_probed`、`rtx_xess_runtime`、`rtx_xess_version`、`rtx_xess_file_version`、`rtx_xess_runtime_path`、`rtx_xess_note`、`rtx_xess_fg_present`、`rtx_xess_low_latency_present` |
| FFX | `rtx_ffx_probed`、`rtx_ffx_loader`、`rtx_ffx_loader_version`、`rtx_ffx_loader_path`、`rtx_ffx_note`、`rtx_ffx_upscaler_present`、`rtx_ffx_upscaler_version`、`rtx_ffx_framegen_present`、`rtx_ffx_framegen_version`、`rtx_ffx_denoiser_present`、`rtx_ffx_denoiser_version`、`rtx_ffx_radiancecache_present`、`rtx_ffx_radiancecache_version` |

`adapterInfoJson()` 先写 `name`、`vendor`、`vendor_id`、`device_id`、`subsys_id`、`revision`、
`feature_level` / `feature_level_value`、`driver_version` / `driver_description`、`device_type`
（`cpu` / `integrated` / `virtual` / `discrete`）、`vram_bytes`、`shared_bytes`、
`dedicated_system_bytes`、`luid`、`is_hardware`、`is_detachable`、`dxcore`，再用
`appendJsonFields` 把 `capabilitiesJson` 的字段插到结尾 `}` 之前。

### 5.3 C ABI 出口

| 函数 | 内容 |
| --- | --- |
| `mcdr_dx12CreateDevice(buf, cap)` | 成功：`<adapter> (<featureLevel>; <capabilitiesText>); <selfTest>`；失败：`"ERROR: " + 原因` |
| `mcdr_dx12GetAdapterInfo(buf, cap)` | `adapterInfoJson()`；无设备返回 0；`(nullptr, 0)` 返回所需字节数（含 NUL），写入后返回**不含 NUL** 的长度 |
| `mcdr_dx12EnumerateAdapters(buf, cap)` | `enumerateAdaptersJson()`，遵循 `writeString` 约定 |

`mcdr_dx12GetAdapterInfo` 的返回值约定与其他 `writeString` 路径**不同**（不含 NUL），JVM 侧按它
单独处理。

**没有跨语言结构体。** `mcdr_abi.h` 里没有 RT 相关的 struct：这一层跨 ABI 的载荷只有 `int32_t`
（tier / mode）与 UTF-8 文本 / JSON，所以没有字节数、字段顺序或对齐契约要维护，`mcdrABILayout()`
的布局自检表（`src\c_abi\abi_meta.cpp`）里也没有对应项。`mcdr::RtxSupport`
（`include\mcdr_rtx_support.h`）是 native 内部结构，含 `std::string` / `std::vector`，**不能**
跨语言传；JVM 只能看到 §5.2 的 JSON。

## 6. `mcdr_rt_state`

四个 `std::atomic<int32_t>`（匿名 namespace，全部 `memory_order_relaxed`）：

| 字段 | 语义 |
| --- | --- |
| `mode` | `0 = off`（默认）、`1 = ao`、`2 = ao_shadows`、`3 = full`；非光追机器上设置不会崩，只是不生效 |
| `gbuffer_ready` | G-Buffer 是否按当前尺寸创建成功；着色器注入与合成都要先看这一位 |
| `gbuffer_width` / `gbuffer_height` | 最近一次创建的 G-Buffer 尺寸，仅诊断 |

接口：`mcdr_rt_state_set_mode` / `mcdr_rt_state_mode`、`_set_gbuffer_ready` / `_gbuffer_ready`、
`_set_gbuffer_size` / `_gbuffer_width` / `_gbuffer_height`。写入路径只有 JVM 的
`mcdr_dx12SetRaytracingMode(int32_t)`（`src\c_abi\abi_surface.cpp`）→ `mcdr_rt_state_set_mode(mode)`
+ `dbgLog("raytracing mode=%d (0=off 1=ao 2=ao_shadows 3=full)")`：它**只存状态**，G-Buffer 与采样
资源的创建在设备侧；读取点只有 `capabilitiesText` 的 `rtMode=`。

## 7. 关键约束

1. **在适配器就绪之后再探测第三方 SDK。** `probeRtxSupport` 必须拿到 `gCtx.adapter`（`EnumAdapterByLuid`
   填好之后，`src\core\d3d12_device.cpp:2502`–`:2699`）；拿 `nullptr` 去问，NGX 只会回 `InvalidParameter`，
   那种结果比不查更糟。
2. **先 `wireDlssImpl()`，再探测。** 探测走 `mcdr_dlss_load_runtime()`，它用接好的日志出口报每一次
   `LoadLibrary` 尝试；接晚了只剩一句「没找到」，看不出找过哪条路径。
3. **不要绕过 `probeRtxSupport` 直接问 NGX。** 探测先 `mcdr_dlss_register_vendor_dir()` 建
   `PathListInfo`；漏掉这步，查询与 Init 一律 `FAIL_InvalidParameter (0xBAD00005)`。
4. **不要把 DXR tier 当「光追可用」。** `Tier 1` 只说明设备/驱动支持 DXR；仓库里没有光追渲染路径，
   也没有 G-Buffer。反过来 **`Tier 0` 不等于设备不可用**——它照样跑光栅路径。
5. **不要把 `dlssSuperResolution == false` 当「DLSS 不可用」。** 真正的门是
   `mcdr_dlss_can_run(device) == 1`（NGX Init，`src\core\d3d12_device.cpp`）。探测自认「答不了」
   时会把 `dlssQueried` 置 false 并写 `dlssQueryNote`，不要覆盖这个判断。
6. **`adapter` 传 `nullptr` 时不要解释成「不支持」。** 三个支持位全 false 的语义是「还没问」。
7. **探测只放在设备初始化与 `queryDlss` 的补探点。** 首次要 `LoadLibrary` 多个 DLL（有 loader 锁），
   不要在渲染线程逐帧调。
8. **不要在探测路径里 `FreeLibrary`。** 当前实现没有卸载逻辑，加载的 DLL 进程内常驻；新增卸载会与
   DLSS / XeSS 的全局状态冲突（本层未验证）。
9. **新增一个 SDK 字段时同时改三处**：`RtxSupport` 字段、`summary()`、`jsonFragment()`。只加字段，
   JVM 侧永远看不到。
10. **解析 `capabilitiesText` 用键名，不要用分隔符。** 当前有两处缺空格（§9）。
11. **`gbuffer_ready` 置位之前不要读 G-Buffer。** 它是着色器注入与合成的前置条件；当前没有写者，
    所以任何依赖它的代码都还不存在，也就没有自检。
12. **`mcdr_rt_state_set_gbuffer_size` 的两次 store 不是原子对。** 并发读者可能拿到「新宽 + 旧高」；
    需要一致就自己重试或另加序号，不要假设宽高一起可见。

## 8. 上层看到什么

| 情形 | 上层看到 |
| --- | --- |
| 无 D3D12 设备 | `mcdr_dx12CreateDevice` 返回 `ERROR: …`；`mcdr_dx12GetAdapterInfo` 返回 0（空串） |
| 设备可建但 DXR 不支持 | `raytracing_tier = 0`、`rt=0`，其余 caps 正常，没有单独的错误位 |
| `nvngx_dlss.dll` 不在 | `rtx_dlss_runtime=false`、`rtx_dlss_note` 带 `GetLastError`，`dlss_*` 全 false |
| 没拿到适配器就探测 | `rtx_dlss_probed=true`、`rtx_dlss_runtime` 可能为 true、`dlss_*` 全 false，note 说明未查硬件 |
| NGX 表态「查询失败」 | `rtx_dlss_sr_why` 以 `query failed:` 开头，`dlssQueryNote` 提示真正的门是 NGX Init |
| XeSS / FSR 运行库不全 | `rtx_xess_runtime` 或 `rtx_ffx_loader` 为 false、对应 `…note` 带 `GetLastError`；effect DLL 在但 loader 不在时仍会报 `rtx_ffx_upscaler_present=true` + 版本号 |

不要按「值为 false 就一定是硬件不支持」判断，先看对应的 `*_note` / `*_why`。

## 9. 已知限制与缺陷

> **缺陷**：`capabilitiesText` 的格式串在 `rtMode=%d` 与 `vrs=%d` 之间少了一个空格，输出实际是
> `rtMode=0vrs=1(tile=0,extra=0)`；用正则解析 `rtMode=` 的读方会连带吃掉 `vrs`。

> **缺陷**：`capabilitiesText` 末尾 `text += gCtx.rtx.summary()` 之前没有空格，而 `summary()` 以
> `ngx=` 开头（`src\advanced\d3d12_rtx_probe.cpp:267`），实际输出是 `meshSmoke=okngx=no nrd=off …`。

> **缺陷**：`device ready: …` 这行在 `probeRtxSupport` **之前**打印，此时 `gCtx.rtx` 还是默认值，
> 该行末尾的 SDK 段是默认的不可用态（`ngx=no`、`nrd=off` …），会被读成「这台机器什么都没装」；
> 真正的摘要在之后任何一次 `capabilitiesText` 调用里。

> **缺陷**：`mcdr_rt_state_set_gbuffer_ready` / `_set_gbuffer_size` 在 `src\` 下**没有调用方**
> （全文搜索只命中 `d3d12_rt_state.cpp` 自身）。G-Buffer 路径未接通，`gbuffer_ready` 与宽高恒为 0。

> **缺陷**：探测结果进程内缓存一次。换适配器（切换独显 / 核显后重建设备）不会自动失效，必须显式
> `resetRtxSupportCache()`；目前唯一会调它的路径是 `queryDlss` 的补探分支。同理
> `rtx_dlss_sr/fg/rr` 在 NGX Init 之前不可信——`false` 只表示「此刻问不到」。

> **缺陷**：XeSS 与 FidelityFX 只探**文件与入口是否存在**，不做设备能力查询
> （`xessD3D12CreateContext` / `ffxCreateContext` 要真实设备与命令列表，属于「真能跑」那一层）；
> `present` 不等于可用，`raytracing_tier` 也不代表本工程有光追渲染路径。

## 文件导航

| 路径（相对 `MCDR\`） | 负责什么 |
| --- | --- |
| `include\mcdr_rtx_support.h` | `mcdr::RtxSupport`、`summary()` / `jsonFragment()`、`probeRtxSupport` / `resetRtxSupportCache` 声明 |
| `include\mcdr_rt_state.h` | 可见性分层的纯 C 状态接口（mode + G-Buffer 就绪 / 尺寸） |
| `src\advanced\d3d12_rtx_probe.cpp` | 四家 SDK 的实际探测、缓存、单行摘要与 JSON 片段 |
| `src\advanced\d3d12_rt_state.cpp` | 四个 atomic 的实现（无 D3D12 依赖） |
| `src\core\d3d12_device.cpp` | `queryDeviceCapabilities`、`capabilitiesText` / `capabilitiesJson`、`adapterInfoJson`、`ensureDevice` 的探测时机 |
| `src\c_abi\abi_device.cpp` | `mcdr_dx12CreateDevice` / `mcdr_dx12GetAdapterInfo` / `mcdr_dx12EnumerateAdapters` |
| `src\c_abi\abi_surface.cpp` | `mcdr_dx12SetRaytracingMode`：把 JVM 的档位写入 `mcdr_rt_state` |
| `docs\device\README.md` | 设备模块、能力结构与资源生命周期 |
| `docs\abi\README.md` | C ABI 的字符串写出与错误状态约定 |

## 文档维护

- 新增一项 D3D12 能力：`Capabilities` 加字段 + `queryDeviceCapabilities` 查询 + 默认值（必须是安全的
  一侧），再同步 §3 的表、§5.1 格式串、§5.2 键名。
- 新增一家 SDK 探测：`RtxSupport` 加字段与 `*Probed`、写 `probeXxx`、补 `summary()` 与
  `jsonFragment()`，然后同步 §4 与 §5.2 的 `rtx_*` 表。
- 改探测时机时同步 §2，并确认 `wireDlssImpl()` 仍在 `probeRtxSupport` 之前。
- 改 `mcdr_rt_state` 的 mode 取值或新增字段，同步 §6 与 `mcdr_dx12SetRaytracingMode` 的日志文案。
- 修掉 §9 的某条缺陷时删掉对应警告块，不要留「已修复」的历史叙述。
