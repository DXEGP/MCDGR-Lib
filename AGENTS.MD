# MCDR

纯 C ABI + C++23 模块的 native DLL。构建：msvc **v145**（VS2026 / MSBuild）+
`/std:c++latest`，`WarningLevel=Level3` 且 `TreatWarningAsError=true`，`MCDR\MCDR.vcxproj`，
配置 Release|x64（Debug|x64 亦可）。

## 布局

```
include\mcdr_abi.h              唯一保留的头文件：extern "C" C ABI（JVM 侧经 FFM 调用）
include\_d3d12_module_prelude.h 各模块全局模块片段共用的系统头（含 NOMINMAX 保护、DBG_LOG_DEBUG）
src\module\c_abi\               mcdr.abi.common 模块接口单元
src\module\core\                mcdr.error 与 6 个 mcdr.d3d12.* 模块接口单元
src\module\advanced\            mcdr.d3d12.mesh 接口单元
src\c_abi\                      C ABI 适配 TU，按功能命名（见下表）
src\core\                       mcdr.d3d12.* 模块实现单元 + DLL 入口点 dllmain.cpp
src\advanced\                   mcdr.d3d12.device 的实现单元（DLSS / 帧生成 / 超分 / 光追探测）+ mesh 普通 TU + SMAA LUT
```

`src\` 下没有自研 `.h`；唯一保留的公共头是 `include\mcdr_abi.h`，模块前置头是 `include\_d3d12_module_prelude.h`。

| 模块名 | 接口单元 | 实现单元 |
| --- | --- | --- |
| `mcdr.error` | `src\module\core\mcdr_error.cppm` | —（并入接口单元） |
| `mcdr.abi.common` | `src\module\c_abi\mcdr_abi_common.cppm` | `src\c_abi\abi_common.cpp` |
| `mcdr.d3d12.descriptor_allocator` | `src\module\core\d3d12_descriptor_allocator.cppm` | `src\core\d3d12_descriptor_allocator.cpp` |
| `mcdr.d3d12.async_descriptor` | `src\module\core\d3d12_async_descriptor.cppm` | `src\core\d3d12_async_descriptor.cpp` |
| `mcdr.d3d12.async_fence` | `src\module\core\d3d12_async_fence.cppm` | `src\core\d3d12_async_fence.cpp` |
| `mcdr.d3d12.async_executor` | `src\module\core\d3d12_async_executor.cppm` | —（并入接口单元） |
| `mcdr.d3d12.async_bundle` | `src\module\core\d3d12_async_bundle.cppm` | —（并入接口单元） |
| `mcdr.d3d12.device` | `src\module\core\mcdr_device.cppm` | `src\core\d3d12_device.cpp`、`d3d12_surface.cpp`、`d3d12_sr.cpp`、`d3d12_framegen.cpp`、`d3d12_fsffg.cpp`、`d3d12_fsr.cpp`、`d3d12_xess.cpp`、`d3d12_xessfg.cpp` |
| `mcdr.d3d12.mesh` | `src\module\advanced\d3d12_mesh.cppm` | —（`src\advanced\d3d12_mesh_base.cpp` 是**普通 TU**，不是模块实现单元） |

`mcdr.error` 是 `Result<T>` / `err()` 的载体，所有模块都 `import mcdr.error`（import 不传递）。
`d3d12_mesh_base.cpp` 刻意留在普通 TU：定义放进模块实现单元会让签名里的 device 类型变成模块私有名，
而 `abi_mesh.cpp` 按 `mcdr_abi.h` 只看得到公共名 → `LNK2001`。

新增 `.cppm` 只需作为 `<ClCompile>` 项加入 vcxproj：`LanguageStandard=stdcpplatest` 会让
MSBuild 自动按 `CompileAsCppModule` 编译，并自动开启 `ScanSourceForModuleDependencies`。

## C ABI 适配层（`src\c_abi\`）

适配层按功能拆成下面这些 TU（每个 TU 只转发自己那组模块函数）：

| 文件 | 功能关键词 |
| --- | --- |
| `abi_common.cpp` | `mcdr.abi.common` 的实现：每线程错误槽与字符串写出（`lastErrorText`、`setLastError`、`setLastErrorf`、`failWith`、`failVoid`、`writeString`），**没有** `extern "C"` 导出 |
| `abi_meta.cpp` | ABI 元信息、布局自检、错误串与日志控制（`mcdrABIVersion`、`mcdrABILayout`、`mcdrLastError`、`mcdrSetLogVerbose`、`mcdrSetLogLevel`、`mcdrSetLogFile`） |
| `abi_device.cpp` | 设备、资源与隐藏窗口入口 |
| `abi_command.cpp` | 命令层：encoder 生命周期、拷贝、清屏、render pass、时间戳查询、异步渲染控制通道 |
| `abi_pipeline.cpp` | 图形管线创建/销毁 |
| `abi_draw.cpp` | render pass 内的绘制录制 |
| `abi_mesh.cpp` | P72 mesh 管线与 HLSL → DXIL 编译 |
| `abi_surface.cpp` | DXGI 交换链 surface：创建/配置/获取/Blit/Present、back buffer 句柄、像素回读 |
| `abi_async.cpp` | 异步渲染基础设施：分区描述符分配器、bundle 录制池、fence 管理器、主列表执行器、屏障预处理 |
| `abi_sr.cpp` | 超分：DLSS/FSR/XeSS 的分派与能力查询 |
| `abi_framegen.cpp` | 帧生成：DLSS-G / FSR-FG / XeSS-FG 的分派、状态查询与 UI 纹理 |

排序与包含关系：`abi_common` → `abi_meta` → `abi_device` → `abi_command` → `abi_pipeline`
→ `abi_draw` → `abi_mesh` → `abi_surface` → `abi_async` → `abi_sr` → `abi_framegen`（也是 vcxproj
中的顺序；顺序本身不是链接依赖，靠 `ScanSourceForModuleDependencies`）。

## 改 native 时的三条硬约束

1. `.cppm` 内声明的导出函数必须在**同一个模块单元**内定义（接口单元声明 + `module <名>;` 实现单元
   定义，或直接定义在 `.cppm` 里），否则定义侧与引用侧修饰名不一致 → `LNK2001`/`LNK2019`。
2. 导出签名里**不要出现别的模块拥有的类型**。v145 会把它们记成模块私有名
   （`Dx12Pipeline@$$_Amcdr.d3d12.device`），而普通 TU 消费方按 `mcdr_abi.h` 的前向声明只能得到公共名
   `@1`，两侧永远对不上。跨界只用不透明句柄（`void*` / `uintptr_t`），例如
   `createMeshPipeline`、`dispatchMesh`。
3. `#include` 一律放在全局模块片段（`module;` 与 `export module` 之间）。放进模块范围内会触发
   `warning C5244`，本项目 `TreatWarningAsError=true`，直接构建失败。内部统一走
   `_d3d12_module_prelude.h`。

`src\core\dllmain.cpp` 是刻意的**普通翻译单元**（`extern "C"` + `DllMain`），不属于任何命名模块：
模块无法导出 C 语言链接的实体，而 `DllMain` 由 CRT 按名字查找，C 链接才稳。别把它并进 `.cppm`。

## 文档

设计文档在 `docs\`，入口是 `docs\overview.md`：ABI 契约 [`docs\abi\README.md`](docs/abi/README.md)、
适配层与模块规则 [`docs\c_abi\README.md`](docs/c_abi/README.md)、
设备与资源 [`docs\device\README.md`](docs/device/README.md)、
交换链 [`docs\surface\README.md`](docs/surface/README.md)、
异步 [`docs\async\README.md`](docs/async/README.md)、
DLSS [`docs\advanced\dlss.md`](docs/advanced/dlss.md)、
帧生成 [`docs\advanced\framegen.md`](docs/advanced/framegen.md)、
超分 [`docs\advanced\upscalers.md`](docs/advanced/upscalers.md)、
mesh [`docs\advanced\mesh.md`](docs/advanced/mesh.md)、
光追能力探测 [`docs\advanced\rt-probe.md`](docs/advanced/rt-probe.md)、
构建与模块 [`docs\misc\build-and-modules.md`](docs/misc/build-and-modules.md)、
日志与调试 [`docs\misc\debug-and-logging.md`](docs/misc/debug-and-logging.md)。

源码注释只写"违反就会出错"的约束；解释、数据流与已知缺陷写在 `docs\` 里，
需要展开时在注释里给出 `// 见 docs\...` 指针。文档与代码冲突时以代码为准，并顺手把文档改对。
