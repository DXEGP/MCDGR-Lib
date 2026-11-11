# MCDR

纯 C ABI + C++23 模块的 native DLL。构建：msvc **v145**（VS2026 / MSBuild）+
`/std:c++latest`，警告级别 **/W3 且 `TreatWarningAsError=true`**，`MCDR\MCDR.vcxproj`，
配置 Release|x64（Debug|x64 亦可）。

## 布局

公共 JVM 契约为 `include/mcdr_abi.h`；`include/` 还包含模块前置头以及 DLSS、HDR、RT、SMAA、surface 和 window 内部桥接头。

| 目录 | 当前职责 |
| --- | --- |
| `src/c_abi/` | C ABI 薄适配 |
| `src/backend/device/` | 设备、能力、资源/命令协作 |
| `src/backend/memory/` | 描述符分配器 |
| `src/backend/command/` | async fence；executor/bundle 定义在镜像模块接口 |
| `src/backend/shader/` | DXC 加载、编译、DXIL 缓存、shader profile |
| `src/diagnostics/` | 日志、崩溃、PIX、RTX 支持探测 |
| `src/platform/` | DLL 入口、Windows 窗口宿主 |
| `src/render/reconstruction/` | SR registry 与 DLSS/FSR/XeSS adapter |
| `src/render/presentation/` | surface、HDR、framegen 与 adapter |
| `src/render/antialiasing/` | blit、锐化、SMAA、TAA 与 LUT 数据 |
| `src/render/terrain/` | mesh 管线 |
| `src/render/config/` | 当前 RT 选项状态 |
| `src/module/<domain>/` | 对应业务域的 C++ 模块接口；C ABI common 保留 `module/c_abi/` |

模块身份保持 `mcdr.error`、`mcdr.abi.common`、`mcdr.d3d12.device`、`mcdr.d3d12.mesh`、`mcdr.d3d12.descriptor_allocator` 与 `mcdr.d3d12.async_*`。
`mcdr_device.cppm` 尾部非 export 声明只供同模块实现协作；不要把实现私有缓存与 mutex 改成 extern。
设备调用诊断入口；诊断通过 `deviceContext()` 读取设备，pending-delete 崩溃快照由设备函数打印。
DXC 单元拥有编译器缓存；postprocess 单元拥有 shader、SMAA/TAA 私有状态，现有 GPU 资源仍由 `DeviceContext` 保存。
不要将这些现有实现描述为未来 RenderPipeline 已完成。

新增编译单元与移动路径必须同时更新 `MCDR.vcxproj` 和 `MCDR.vcxproj.filters`。
可用 `MCDRDependencyRoot` 指向同版本 external/lib/vcpkg 依赖根，默认工程目录；源码、模块与输出仍使用当前工作树。
复用已有依赖时传入 `/p:MCDRDependencyRoot=<依赖根目录> /p:VcpkgEnableManifest=false /p:VcpkgEnabled=false`。构建属性会规范化绝对路径并补齐目录分隔符。

## C ABI 适配层（`src\c_abi\`）

原先按提交顺序命名的 `abi_bridge.cpp` / `abi_bridge_p3..p7.cpp` / `abi_bridge_p33.cpp`
已按功能重新命名并合并（合并只做拼接，函数体逐字未改）：

| 文件 | 功能关键词 |
| --- | --- |
| `abi_common.cpp` | 公共错误状态与字符串写出（`mcdrLastError`、`mcdrSetLogVerbose`） |
| `abi_meta.cpp` | ABI 元信息与布局自检（`mcdrABIVersion`、`mcdrABILayout`） |
| `abi_device.cpp` | 设备、资源与隐藏窗口入口 |
| `abi_command.cpp` | 命令层：encoder 生命周期、拷贝、清屏、render pass、时间戳查询、异步渲染控制通道 |
| `abi_pipeline.cpp` | 图形管线创建/销毁 |
| `abi_draw.cpp` | render pass 内的绘制录制 |
| `abi_mesh.cpp` | mesh 管线与 HLSL → DXIL 编译 |
| `abi_surface.cpp` | DXGI 交换链 surface：创建/配置/获取/Blit/Present、back buffer 句柄、像素回读 |
| `abi_async.cpp` | 异步渲染基础设施：分区描述符分配器、bundle 录制池、fence 管理器、主列表执行器、屏障预处理 |

排序与包含关系：`abi_common` → `abi_meta` → `abi_device` → `abi_command` → `abi_pipeline`
→ `abi_draw` → `abi_mesh` → `abi_surface` → `abi_async`（也是 vcxproj 与 `.filters` 中的顺序）。

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

`src/platform/dllmain.cpp` 是刻意的**普通翻译单元**（`extern "C"` + `DllMain`），不属于任何命名模块：
模块无法导出 C 语言链接的实体，而 `DllMain` 由 CRT 按名字查找，C 链接才稳。别把它并进 `.cppm`。

文档入口为 `docs/overview.md`，集中渲染器目标契约见 `docs/rendering.md`。
新模块按该契约组织资源与 SDK 所有权，不复制来源分支的整体帧调度和全量几何镜像。
RenderPipeline 只组装调度；业务按 `src/module/render/<domain>/` 与 `src/render/<domain>/` 封装，SDK adapter 留在所属域内。
领域状态单一写者，跨模块只传版本化值/只读视图/图资源；禁止全局服务定位器、跨域可变 manager 和 SDK 类型进入公共契约。
SM6.9/WG/CoopVec 优先使用，实际能力由模块和选定图声明；配置、资源寿命、历史、生成资产和诊断一起交付。
