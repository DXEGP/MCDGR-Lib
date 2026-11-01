# 构建系统与 C++23 模块组织

MCDR 是一个**全量 C++23 模块化**的 native DLL：`src\` 下没有自研 `.h`，接口单元是
`src\module\**\*.cppm`，实现单元散在 `src\core\`、`src\advanced\`，对外的唯一契约是
`include\mcdr_abi.h` 这份纯 C 头。本文讲三件事：

1. 一条 `MSBuild` 命令背后依次发生了什么、产物落在哪；
2. 9 个模块谁 import 谁、导出什么，以及违反模块规则时的确切报错；
3. 依赖从哪来（`external\` 子模块 vs `vcpkg`）、运行库为什么不进 `MCDR.dll`。

构建命令（仓库里没有 `.sln`，用工程文件直接构建）：

```powershell
MSBuild MCDR\MCDR.vcxproj /p:Configuration=Release /p:Platform=x64
```

---

## 1. 一次构建发生了什么

### 1.1 前置条件

| 条件 | 具体要求 | 缺了会怎样 |
| --- | --- | --- |
| 工具集 | MSVC **v145**（`PlatformToolset=v145`、`VCProjectVersion=18.0`） | 工程加载失败或回落到旧工具集，模块语义不可靠 |
| vcpkg | 由工程 `VcpkgEnableManifest=true` 自动驱动，无需手工 `vcpkg install` | `AdditionalIncludeDirectories` 里的 `vcpkg_installed\...` 路径不存在 → 头文件找不到 |
| `external\` 子模块 | `scripts\sync-external.ps1`（不带参数执行一次） | DLSS/NGX、NRD、XeSS、FidelityFX、Streamline、NRC 的头文件缺失 |
| `external\nrd\Lib\x64\<cfg>\NRD.lib` | 上游不带预编译静态库，需用 `external\nrd` 的 CMake 工程编译后放进去 | 链接期 `LNK1181: 无法打开输入文件“NRD.lib”` |
| `lib\*` 运行期 DLL | git 不记录，按 `scripts\sync-external.ps1` 准备（见 §6.3） | 只影响运行期，不影响构建 |

### 1.2 六个阶段

```text
 MSBuild MCDR.vcxproj /p:Configuration=Release /p:Platform=x64
        │
        ├─[1] 依赖还原  VcpkgEnableManifest=true
        │       vcpkg.json + vcpkg-configuration.json（baseline 52c9e08c…）
        │       → vcpkg_installed\<triplet>\      triplet 由 Platform 决定（x64-windows）
        │
        ├─[2] 模块依赖扫描  ScanSourceForModuleDependencies=true
        │       每个 ClCompile 项各产出一份 build\x64_Release\<名>.module.json
        │       （记录本 TU 导出了哪个模块、import 了哪些模块，供 MSBuild 排依赖）
        │
        ├─[3] 编译模块接口单元   9 个 .cppm → CompileAsCppModule
        │       build\x64_Release\<名>.cppm.ifc
        │
        ├─[4] 编译其余 31 个 .cpp
        │       22 个模块实现单元（首行 module <名>;）+ 9 个普通 TU
        │       build\x64_Release\<名>.obj
        │
        ├─[5] Link
        │       基础：d3d12.lib dxgi.lib d3dcompiler.lib dcomp.lib user32.lib version.lib
        │       x64 追加：nvsdk_ngx_d.lib（Debug 用 _d_dbg）NRD.lib ShaderMakeBlob.lib uuid.lib
        │
        └─[6] 输出
                target\x64_Release\MCDR.dll      ← 唯一被 JVM 侧加载的产物
                target\x64_Release\MCDR.lib / MCDR.exp / MCDR.pdb
```

第 [2] 阶段是可验证的：`build\x64_Release\` 下每个参与编译的源文件（包括 `dllmain.cpp`、
`smaa_luts.cpp` 这种普通 TU）都有一份同名的 `.module.json`。模块接口单元额外的 `.ifc`
落在同一目录；`.ifc` 与 `.module.json` 都随工具链版本失效，已在 `.gitignore` 里排除
（`*.ifc`、`*.module.json`、`*.ifc.dt`、`*.ifc.dtast`、`*.ifc.bgca.dt`）。

`build\x64_Release\microsoft\STL\std.ixx.ifc` 也会出现：两个普通 TU
（`src\advanced\d3d12_rtx_probe.cpp`、`src\advanced\d3d12_dlss.cpp`）写了 `import std;`，
需要 STL 模块单元；工程只在 `Debug|x64` 显式设了 `BuildStlModules=true`。

### 1.3 工程属性总表（`ItemDefinitionGroup` 无 Condition 段，四个配置共享）

| 属性 | 值 | 作用 / 为什么必须这样 |
| --- | --- | --- |
| `ConfigurationType` | `DynamicLibrary` | 产出 DLL |
| `TargetName` | `MCDR` | 产物名 `MCDR.dll` |
| `OutDir` / `IntDir` | `target\$(Platform)_$(Configuration)\` / `build\$(Platform)_$(Configuration)\` | 产物与中间件分离；两者都在 `.gitignore` 里 |
| `LanguageStandard` | `stdcpplatest` | C++23；**模块**只有在 `/std:c++latest` 下才可用 |
| `LanguageStandard_C` | `stdclatest` | `mcdr_abi.h` 被 C 侧使用时也能编译 |
| `WarningLevel` | `Level3` | 三级（`/W3`）；D3D12/DXGI 头在这一级下干净 |
| `TreatWarningAsError` | `true` | 任何告警都是构建失败——`C5244` 因此是硬错误 |
| `SDLCheck` / `ConformanceMode` | `true` / `true` | 安全开发生命周期检查 + `/permissive-` |
| `MultiProcessorCompilation` | `true` | `/MP`，多进程并发编译各 TU |
| `ScanSourceForModuleDependencies` | `true` | **本工程模块能工作的关键**：开启后才扫描 `import`/`export module` 并排依赖 |
| `PrecompiledHeader` | `NotUsing` | 全工程无 PCH；模块单元与 PCH 混用会互相干扰 |
| `AdditionalOptions` | `/utf-8` | 源文件里有中文注释，不加会按活动代码页误读 |
| `PreprocessorDefinitions` | `_CRT_SECURE_NO_WARNINGS;NOMINMAX;MCDR_EXPORTS;_WINDOWS;_USRDLL` | 见 §4.3 |
| `PreprocessorDefinitions`（仅 x64） | `MCDR_HAS_DLSS=1;MCDR_HAS_NRD=1;MCDR_HAS_XESS=1;MCDR_HAS_FFX=1;MCDR_HAS_NRC=1` | 五个 SDK 都是 x64-only；Win32 配置靠这些宏把用法整段关掉 |
| `AdditionalDependencies`（x64） | 见 §1.2 阶段 [5] | 只有 DLSS(ngx) 与 NRD 是**真链接**；XeSS/FidelityFX/NRC 走 `LoadLibrary` |
| `SubSystem` | `Windows` | 这是被 JVM 加载的 DLL，不是控制台程序 |
| `EnableSegmentHeap`（Manifest） | `true` | 段堆，降低多线程分配的争用 |
| `WholeProgramOptimization`（Release） | `true` | 配合 `EnableCOMDATFolding` / `OptimizeReferences` |
| `DebugInformationFormat`（Debug） | `ProgramDatabase`（`/Zi`） | **不要**改回默认 `/ZI`：`/ZI` + `/FS` + `/std:c++latest` 在 v145 上每个 PDB 写入都报 `C2471` |
| `VcpkgEnableManifest` | `true` | manifest 模式，依赖声明在 `vcpkg.json` 而不是全局 vcpkg 安装里 |

### 1.4 构建产物

| 路径 | 内容 |
| --- | --- |
| `target\x64_Release\MCDR.dll` | 交付物；JVM 侧 `gradlew copyNative` 把它放进开发客户端的 classpath |
| `target\x64_Release\MCDR.lib` / `.exp` | 链接器导入库与导出表（本工程没有外部 C++ 消费者，仅作取证/符号化用） |
| `target\x64_Release\MCDR.pdb` | 崩溃栈符号化要用它（`d3d12_device.cpp` 内的符号化路径），必须与 DLL 同目录 |
| `build\x64_Release\*.obj`、`*.ifc`、`*.module.json` | 中间件，可随时删 |
| `build\x64_Release\vc145.pdb` | 编译期 PDB（Debug 下容易出事的就是它） |

`target\x64_Release\` 里出现的 `libxess.dll`、`amd_fidelityfx_upscaler_dx12.dll` 之类**不是构建产物**，
是手工放进去做运行期探测的（该目录被 gitignore）。

---

## 2. 模块清单与依赖图

### 2.1 九个模块

「实现单元」列里，`module <名>;` 表示它是该模块的实现单元；标注「并入接口单元」表示定义直接写在
`.cppm` 里；标注「普通 TU」表示它**不属于**该模块，只是 `import` 消费者。

| 模块名 | 接口单元 | 实现单元 | import 的模块 | 导出什么 |
| --- | --- | --- | --- | --- |
| `mcdr.error` | `src\module\core\mcdr_error.cppm` | 并入接口单元（全 inline） | 无（叶子） | `Error`、`Result<T>`（`std::expected`）、`err()`、`ok()`、`describe()` |
| `mcdr.abi.common` | `src\module\c_abi\mcdr_abi_common.cppm` | `src\c_abi\` 全部 11 个 `.cpp` | `mcdr.error` | `setLastError` / `setLastErrorf` / `lastErrorText` / `failWith` / `failVoid` / `failWithPtr` / `failResult` / `failResultPtr` / `fromHandle<T>` / `toHandle` / `writeString`（`mcdr::abi::`） |
| `mcdr.d3d12.descriptor_allocator` | `src\module\core\d3d12_descriptor_allocator.cppm` | `src\core\d3d12_descriptor_allocator.cpp` | 无 | `SegmentedDescriptorAllocator` |
| `mcdr.d3d12.async_descriptor` | `src\module\core\d3d12_async_descriptor.cppm` | `src\core\d3d12_async_descriptor.cpp` | `mcdr.error` | `PartitionedDescriptorAllocator`；`kAsyncFrameSlots=4`、`kMaxAsyncWorkers=8`、`kAsyncSyncRingReserve=131072`、`kAsyncWorkerSlotsPerSection=24576` |
| `mcdr.d3d12.async_fence` | `src\module\core\d3d12_async_fence.cppm` | `src\core\d3d12_async_fence.cpp` | `mcdr.error` | `AsyncFenceManager` |
| `mcdr.d3d12.async_executor` | `src\module\core\d3d12_async_executor.cppm` | 并入接口单元 | `mcdr.error` | `MainCommandExecutor`（`kFramesInFlight=4`） |
| `mcdr.d3d12.async_bundle` | `src\module\core\d3d12_async_bundle.cppm` | 并入接口单元 | `mcdr.error` | `BundleRecorder`、`BundleRecorderPool`；`kBundleFrameSlots=4`、`kBundleBatchesPerFrame=16`、`kBundleSlotCount=64` |
| `mcdr.d3d12.device` | `src\module\core\mcdr_device.cppm` | `src\core\d3d12_device.cpp`、`src\core\d3d12_surface.cpp`、`src\advanced\d3d12_sr.cpp`、`d3d12_framegen.cpp`、`d3d12_fsr.cpp`、`d3d12_xess.cpp`、`d3d12_xessfg.cpp`、`d3d12_fsffg.cpp`（共 8 个） | `mcdr.error` | `DeviceContext`、`Capabilities`、`PipelineDesc`、`Dx12Pipeline`、`CommandContext`、`Dx12Surface`、`deviceContext()`；日志族 `dbgLog` / `dbgLogError` / `dbgLogWarn` / `dbgLogInfo` / `dbgLogDebug` / `dbgLogProgress` / `setLogLevel` / `setLogFile` / `formatLogPrefix` / `nowMs`；超分与插帧的后端接口 |
| `mcdr.d3d12.mesh` | `src\module\advanced\d3d12_mesh.cppm` | **无**——定义在普通 TU `src\advanced\d3d12_mesh_base.cpp` | `mcdr.d3d12.device`（只 `import`，不 `export import`）、`mcdr.error` | `MeshPipelineDesc`、`createMeshPipeline`（两个重载，返回 `Result<void*>`）、`createMeshRootSignature`、`releaseMeshRootSignature`、`meshPipelineSupported`、`dispatchMesh` |

三条派生事实：

- **`import` 不传递**。用到 `mcdr::Error` / `describe()` 的 TU 必须自己写 `import mcdr.error;`，
  `src\c_abi\*.cpp` 里那行注释就是这个意思。
- **适配层的 11 个 `.cpp` 不是普通 TU**，它们是 `mcdr.abi.common` 的实现单元（首行 `module;`、
  中间 `module mcdr.abi.common;`）。里面写的 `extern "C"` 函数不属于任何模块，是 DLL 的导出面。
- **实现单元不 import 自己所属的模块**。`src\core\d3d12_device.cpp:47` 写的是
  `module mcdr.d3d12.device;`，不是 `import mcdr.d3d12.device;`。

### 2.2 依赖图

```text
                        mcdr.error  （叶子：不 import 任何 mcdr.*）
                             ▲
        ┌────────────────────┼─────────────────────────────┐
        │                    │                             │
        │        ┌───────────┴────────────┐                │
        │        │                        │                │
        │  mcdr.d3d12.async_descriptor    │                │
        │  mcdr.d3d12.async_fence         │                │
        │  mcdr.d3d12.async_executor      │                │
        │  mcdr.d3d12.async_bundle        │                │
        │        │                        │         mcdr.abi.common
        │        │                        │       （11 个适配 TU 的实现单元）
        │        │                        │                │
        │        ▼                        ▼                ▼
        └──► mcdr.d3d12.device ◄───────────────────────────┘
                     ▲
                     │ import（不 export import）
             mcdr.d3d12.mesh

  mcdr.d3d12.descriptor_allocator —— 独立节点，当前没有任何 import 方（见 §8）
```

箭头方向 = 「A import B」。普通 TU 的 import 关系另记：

| 消费者 | import |
| --- | --- |
| `src\c_abi\abi_common.cpp`、`abi_meta.cpp` | `mcdr.d3d12.device` |
| `src\c_abi\abi_sr.cpp`、`abi_framegen.cpp` | `mcdr.d3d12.device` + `mcdr.error` |
| `src\c_abi\abi_device.cpp`、`abi_pipeline.cpp`、`abi_mesh.cpp` | `mcdr.d3d12.device` + `mcdr.d3d12.mesh` + 四个 async 模块 |
| `src\c_abi\abi_command.cpp`、`abi_draw.cpp`、`abi_surface.cpp`、`abi_async.cpp` | 上者再加 `mcdr.error` |
| `src\advanced\d3d12_mesh_base.cpp`（普通 TU） | `mcdr.d3d12.device` + `mcdr.d3d12.mesh` + `mcdr.error` |

为什么 `mcdr.d3d12.device` 不 import 四个 async 模块：`DeviceContext` 里没有任何异步成员，异步对象是
`d3d12_device.cpp` 里的文件级静态量。反过来 import 会与各 async 实现单元（它们要 `deviceContext()`）
形成 import 环。

---

## 3. 三条硬约束

违反任意一条，构建都会在 `/W3` + `TreatWarningAsError=true` 下直接失败，或链接出一个跑不起来的 DLL。

### 3.1 导出函数必须在本模块内定义

```text
现象   LNK2001: 无法解析的外部符号 "…?createMeshPipeline@d3d12_mc@@…"
       LNK2019: 无法解析的外部符号 …，函数 "…" 中引用了该符号
原因   export 声明的实体带模块归属；定义挪到普通 TU 后，定义侧算出的是另一个修饰名。
正确   .cppm 内 inline 定义，或写在 module <模块名>; 实现单元里。绝不挪到普通 TU。
```

例外只有一处，且是有意为之：`mcdr.d3d12.mesh` 的导出函数定义在**普通 TU**
`src\advanced\d3d12_mesh_base.cpp`（文件头注释写明：放实现单元会让签名里的 device 类型变成模块私有名，
放普通 TU 两侧都记公共名）。触发这个选择的正是 §3.2 —— 它的导出签名里有 `PipelineDesc::Binding`。
**不要**把这个例外推广到别的模块。

配套两条：

- 实现单元不要 `import` 自己所属的模块（冗余，且语义上自相矛盾）。
- 声明与定义必须在**同一个模块**里：`mcdr.d3d12.device` 有 8 个实现单元，它们都属于同一模块，
  彼此不能 `import` 对方。

### 3.2 导出签名里不得出现别的模块拥有的类型

```text
现象   LNK2001，符号名里带模块私有后缀，形如
         <类型名>@$$_A<模块名>
       实际修饰名形如 Dx12Pipeline@$$_Amcdr.d3d12.device
       消费方看到的是公共名 <类型名>@1，两个名字永远对不上。
原因   v145 在模块 purview 里给「别模块拥有的类型」加模块私有修饰，普通 TU 只按
       mcdr_abi.h 里能看到的前向声明算名字。
正确   跨模块/跨 TU 只传不透明句柄与 POD：void* / uintptr_t / uint64_t / 描述符结构。
       全库唯一的 reinterpret_cast 落点是 mcdr::abi::fromHandle<T> / toHandle。
```

现成的正例：`createMeshPipeline` 返回 `mcdr::Result<void*>`，`dispatchMesh` 收 `void* ctx`，
`mcdr_abi.h` 里的句柄一律是 `mcdr_object*` / `mcdr_command_ctx*` / `uint64_t`。

普通 TU 与模块单元之间唯一的合法桥是 `extern "C"`：同一个 C++ 函数在两边会算出不同修饰名，只有 C 链接
能对上同一个符号。`include\mcdr_dlss_impl.h` 整份头就是为这条规则存在的——签名里只有 `void*`、POD
与 `char*` 缓冲，**不要**把 `std::string` 放进去。

### 3.3 `#include` 只能放在全局模块片段

```text
现象   warning C5244: '#include' in the purview of module 'mcdr.abi.common' appears erroneous.
                        Consider moving that directive before the module declaration.
       本项目 TreatWarningAsError=true → 直接构建失败
原因   模块 purview 内文本包含头文件（尤其是会展开标准库/系统头的）不符合标准。
正确   系统头与内部 prelude 放在 module; 与 export module / module <名>; 之间。
```

配套两条：

- 全局模块片段里**只能有预处理指令**（`C5202`）。要放 `extern "C"` 声明就装进一个头文件再 `#include`
  （`mcdr_rtx_support.h`、`mcdr_dlss_impl.h` 就是这么用的）。
- 模块声明必须出现在 TU 开头或全局模块片段内（`C5201`）。把 `#include` 写在模块声明前面却没写
  `module;`，就是这个错。

判断标准是「谁在 purview 里展开出系统头」，不是「头文件在第几行」：`src\c_abi\*.cpp` 的形态是
prelude 在 `module;` 之后，而 `#include "mcdr_abi.h"` 与 `#include "mcdr_dlss_impl.h"` 在模块声明
**之后**——这两个头只含宏、`extern "C"` 声明与 POD，自身不展开标准库。新加头文件前先看它 include 了什么。

### 3.4 报错速查

| 诊断 | 在本工程里的含义 | 处置 |
| --- | --- | --- |
| `C5244` | 模块 purview 内 include 了会展开系统头/STL 的头 | 移到 `module;` 与模块声明之间 |
| `C5202` | 全局模块片段里出现了非预处理指令（裸 `extern "C"`、`import std;`） | 装进头文件再 include，或整个迁移到 `import std;` |
| `C5201` | 模块声明不在 TU 开头/全局模块片段内 | 在文件最前面补 `module;` |
| `LNK2001` | 模块内实体的修饰名在普通 TU 侧对不上（§3.1、§3.2） | 定义放回模块内，或改用不透明句柄 |
| `LNK2019` | 同上，但报错里带完整签名，便于看出是哪个参数类型不一致 | 同上 |
| `LNK2011` | 预编译对象未链接——**与模块无关**，本工程 `PrecompiledHeader=NotUsing` 不会出现 | 若出现，先查是不是有人把 PCH 重新打开了 |
| `C2471` | 无法更新 PDB | Debug 要 `/Zi`，不能是默认 `/ZI`（见 §1.3） |
| `LNK2038` | `_ITERATOR_DEBUG_LEVEL` / `RuntimeLibrary` 不匹配 | DLSS 导入库要按配置选 `nvsdk_ngx_d.lib`（`/MD`）或 `nvsdk_ngx_d_dbg.lib`（`/MDd`） |
| `LNK1181` | 打不开输入文件 | `external\nrd` 的 `NRD.lib` / `ShaderMakeBlob.lib` 还没编 |

---

## 4. 全局模块片段、前导头与导出宏

### 4.1 每个模块单元的标准骨架

```cpp
// 一句话职责
module;                                     // ← 全局模块片段开始

#include "_d3d12_module_prelude.h"        // windows.h / d3d12.h / dxgi*.h / STL

export module mcdr.d3d12.device;            // 接口单元（实现单元这里写 module mcdr.d3d12.device;）

import mcdr.error;                          // 模块之间的 import 只能在模块声明之后

export namespace d3d12_mc { /* … */ }
```

顺序是不可交换的：`module;` → `#include` → 模块声明 → `import` → `export`。

### 4.2 `include\_d3d12_module_prelude.h` 的职责

| 职责 | 具体内容 | 为什么必须在全局模块片段 |
| --- | --- | --- |
| 守住 `NOMINMAX` | `#ifndef NOMINMAX / #define NOMINMAX`，位置在一切 `windows.h` 之前 | `min`/`max` 宏与 `std::min` 相撞在 `/WX` 下是硬错误（`C2589`/`C2059`） |
| 系统头 | `windows.h`、`d3d12.h`、`d3d12sdklayers.h`、`d3dcompiler.h`、`dcomp.h`、`dxcore_interface.h`、`dxgi.h`/`1_2`/`1_4`/`1_6`、`wrl/client.h` | 这些头会展开系统实体，放 purview 里触发 `C5244` |
| C 运行库 | `cstdarg`、`cstddef`、`cstdint`、`cstdio`、`cstring`、`expected` | `mcdr::Result<T>` 依赖 `<expected>` |
| C++ 标准库 | `array`、`atomic`、`climits`、`functional`、`memory`、`mutex`、`string`、`thread`、`unordered_map`、`unordered_set`、`utility`、`vector` | 同上 |
| `DBG_LOG_DEBUG` | `DX12_DEBUG_VERBOSE` 打开时展开为 `::d3d12_mc::dbgLogDebug(...)`，否则 `((void)0)` | **具名模块不导出宏**，宏只能随全局模块片段走；唯一使用者是 `src\c_abi\abi_command.cpp` |

现成规则（照做即可）：

- **不要**把 prelude 改成 `import std;`。`import std;` 出现在全局模块片段里是 `C5202`，而 prelude 的全部
  内容都在那里面。
- 展开宏必须写全命名空间 `::d3d12_mc::dbgLogDebug`，否则 `DX12_DEBUG_VERBOSE` 打开时报 `C3861`。
- `DBG_LOG_DEBUG` 有 `#ifndef` 守卫：重复定义在 `TreatWarningAsError=true` 下是硬错误。

### 4.3 `MCDR_EXPORTS`、`MCDR_API`、`MCDR_CALL`

`include\mcdr_abi.h` 里：

```c
#if defined(_WIN32)
#if defined(MCDR_EXPORTS)
#define MCDR_API __declspec(dllexport)
#else
#define MCDR_API __declspec(dllimport)
#endif
#define MCDR_CALL __cdecl
```

- 工程在 `PreprocessorDefinitions` 里全局定义了 `MCDR_EXPORTS`（§1.3），所以本工程内每个 TU 看到的
  `MCDR_API` 都是 `dllexport`。
- `src\module\c_abi\mcdr_abi_common.cppm` 在 `#include "mcdr_abi.h"` 之前又加了一道
  `#ifndef MCDR_EXPORTS / #define MCDR_EXPORTS 1`，使这个接口单元脱离工程属性单独编译也不会退化成
  `dllimport`。
- `cpp.hint` 只是 VS 给 IntelliSense 用的宏提示（`MCDR_API` 的两种展开），**不参与编译**。
- 新增导出函数一律写 `MCDR_API <返回值> MCDR_CALL mcdrXxx(...)`，两个宏都别省。

### 4.4 `src\core\dllmain.cpp` 为什么必须是普通 TU

```cpp
extern "C" {
#include <windows.h>
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReasonForCall, LPVOID lpReserved) {
    if (ulReasonForCall == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hModule); }
    return TRUE;
}
}
```

两条理由都不可绕过：**模块无法导出 C 语言链接的实体**；而 `DllMain` 由 CRT 按导出名查找，C 链接才稳。
该文件没有 `module;`、没有模块声明，也不出现在任何模块的 import 图里——别把它并进 `.cppm`。

---

## 5. 编译单元顺序为什么这么排

`src\c_abi\*.cpp` 在 `MCDR.vcxproj` 里的顺序是：

```text
abi_common → abi_meta → abi_device → abi_command → abi_pipeline
→ abi_draw → abi_mesh → abi_surface → abi_async → abi_sr → abi_framegen
```

**这不是链接依赖声明**。三条理由：

1. `MultiProcessorCompilation=true`（`/MP`）下各 TU 并发编译；`ScanSourceForModuleDependencies=true`
   让 MSBuild 按每份 `.module.json` 里记录的 `import`/`export module` 排依赖，实际编译顺序由模块图决定，
   与 `ItemGroup` 里的行序无关。
2. 链接器按 `.obj` 集合解析符号，不关心它们来自哪一行。
3. 想验证的话：把两个相邻条目互换，构建结果（DLL 导出面与大小）不变。

那它真正的作用是什么：

| 作用 | 说明 |
| --- | --- |
| 功能分区而非阶段编号 | 公共设施 → ABI 自检 → 设备/资源 → 命令录制 → 图形管线 → 绘制 → mesh → 表面 → 异步 → 超分 → 插帧。新文件按同一套分区追加，读代码时相邻即相关 |
| 与 `.filters` 分组对应 | 「源文件\C ABI」组的显示顺序与工程文件一致，VS 里浏览时不会跳 |
| 让 diff 可读 | 新增一个适配单元总是追加在尾部（或紧邻同类），review 时不会淹没在无关行里 |
| 共享前言一致 | 11 个文件顶部前言完全相同（prelude → `module` 声明 → `#include "mcdr_abi.h"` → import），顺序固定便于 grep |

真正的依赖关系写在 §2.2；不要靠 `ItemGroup` 顺序去"修"链接错误，那是改错地方。

---

## 6. 依赖与外部 SDK

### 6.1 `external\`：七个 git 子模块

`.gitmodules` 有七项；`scripts\sync-external.ps1` 负责把它们的目录整理成工程期望的布局。

| 目录 | 上游 | 版本 | 进构建的方式 | 运行期 |
| --- | --- | --- | --- | --- |
| `external\dlss` | `NVIDIA/DLSS` | v310.9.1 | `include` 在包含路径；**链接** `nvsdk_ngx_d.lib`（Release）/ `nvsdk_ngx_d_dbg.lib`（Debug） | `nvngx_dlss*.dll`，来自 `lib\dlss\` |
| `external\nrd` | `NVIDIA-RTX/NRD` | v4.18.0 | `Include` 在包含路径；**链接** `NRD.lib` + `ShaderMakeBlob.lib` | 无（静态库） |
| `external\xess` | `intel/xess` | v3.0.2 | `inc`、`inc\xess` 在包含路径；**不链** `.lib` | `libxess*.dll`，来自 `lib\xess\` |
| `external\fidelityfx` | `FidelityFX-SDK` | v2.3.0 | 5 个 `Kits\FidelityFX\...\include` 目录；**不链** `.lib` | `amd_fidelityfx_*.dll`，来自 `lib\fidelityfx\` |
| `external\nrc` | `NVIDIA-RTX/NRC` | v0.16.0.0 | `Include` 在包含路径；**未链** `NRC_D3D12.lib` | `lib\nrc\` 的 DLL + CUDA 13 运行库 |
| `external\streamline` | `NVIDIA-RTX/Streamline` | v2.14.16 | `include` 在包含路径；`lib\streamline\lib\x64` 在库搜索路径上，但**没有**任何 streamline `.lib` 进 `AdditionalDependencies` | `sl.*.dll`，来自 `lib\streamline\` |
| `external\sharc` | `NVIDIA-RTX/SHARC` | 代码中未明确 | 只有 `include\` 与 `docs\`，**未进** `MCDR.vcxproj` | 无 |

只有 DLSS 与 NRD 是真链接。XeSS / FidelityFX / NRC 一律 `LoadLibrary` + `GetProcAddress`，
所以它们的头文件在包含路径上、`.lib` 故意不在 `AdditionalDependencies` 里——这样 `MCDR.dll` 在缺
厂商运行库时仍然能加载，只在能力查询里报「不可用」。

NRC 尤其不能硬链：它的 import library 在 DLL 加载期就是硬依赖（`NRC_D3D12.dll` + CUDA 13 运行库约
113 MB，且需要 Tensor Cores 的用户自备安装）。真要接进调用路径，必须同时加
`AdditionalLibraryDirectories`、`AdditionalDependencies` 与 `DelayLoadDLLs`（+ `delayimp.lib`），
让缺失的 DLL 变成「功能不可用」而不是 `MCDR.dll` 加载失败。

`ShaderMakeBlob.lib` 必须和 `NRD.lib` 一起链：`NRD.lib` 里五个 `.obj` 都要
`ShaderMake::FindPermutationInBlob()`，只链 `NRD.lib` 会报五条 `LNK2001`。

### 6.2 `vcpkg`：manifest 模式

| 机制 | 具体值 |
| --- | --- |
| 启停 | 工程属性 `VcpkgEnableManifest=true`，不依赖全局 `vcpkg integrate` |
| 依赖声明 | `vcpkg.json` |
| 注册表 | `vcpkg-configuration.json`：`https://github.com/microsoft/vcpkg`，baseline `52c9e08cdf8580d2d9762f547d22b96fd81e82f2` |
| overlay ports | `./vcpkg-overlay-ports`，当前只有 `directx-dxc`（`version-date 2026-05-27`，替换上游端口以拿到指定日期版本） |
| 安装目录 | `vcpkg_installed\<triplet>\`，triplet 由 `Platform` 推导（`x64-windows` / `x86-windows`，两者都已存在） |
| 包含路径 | `vcpkg_installed\$(VcpkgTriplet)\include` 与 `...\include\directx-dxc`（**仅这两个**） |

`vcpkg.json` 声明的包与实际被源码 include 的情况：

| 包 | 解析版本 | 源码是否引用 |
| --- | --- | --- |
| `directx12-agility` | 1.619.3 | 提供运行期 `D3D12Core.dll`（`vcpkg_installed\x64-windows\bin\`） |
| `directx-dxc` | 2026-05-27（overlay） | `include\directx-dxc` 提供 `dxcapi.h`/`d3d12shader.h`；`dxcompiler.dll`、`dxil.dll` 在 `bin\` |
| `directx-headers` | 1.619.1 | 装到 `include\directx\`，**不在**包含路径上；`#include <d3d12.h>` 仍走 Windows SDK |
| `nlohmann-json` | 3.12.0 | 是：`src\advanced\d3d12_rtx_probe.cpp`、`src\core\d3d12_device.cpp` |
| `tomlplusplus` | 3.4.0 | 否：只在 `include\mcdr_abi.h` 的注释里出现 |
| `directxtk12` / `directxtex` / `directxmesh` / `directxmath` / `shaderwriter` / `tinyexpr` / `llfio` / `zoe` / `minizip` | 见 `vcpkg_installed\vcpkg\info\` | 否：`src\` 与 `include\` 里零 include |

`include\directx-dxc` 单独挂在包含路径上，是因为 DXC 的头文件名（`dxcapi.h`）与 Windows SDK 里的
同名文件冲突，靠目录隔离来选择。

### 6.3 `MCDR.dll` 里不打包任何运行库

构建产物只有 `MCDR.dll` 与它的 `.lib`/`.exp`/`.pdb`。所有厂商运行库都在 `MCDR\lib\` 下按厂商分目录，
由 **JVM 侧**负责搬运：

| 目录 | 内容 | 谁负责最终就位 |
| --- | --- | --- |
| `lib\dlss\` | `nvngx_dlss.dll` / `nvngx_dlssd.dll` / `nvngx_dlssg.dll` / `nvngx_dlssnr.dll` | JVM 侧打包/解出 |
| `lib\streamline\` | `sl.interposer.dll`、`sl.dlss*.dll`、`sl.reflex.dll`、`nvngx_deepdvc.dll` 等 | 同上（不进发行 JAR 的那一类） |
| `lib\xess\` | `libxess.dll` / `libxess_fg.dll` / `libxell.dll`（`libxess_dx11.dll` 不用） | JVM 侧打包 |
| `lib\fidelityfx\` | `amd_fidelityfx_loader_dx12.dll` 等 | 开发期用，发行版按 JVM 侧清单取舍 |
| `lib\nrc\` | `NRC_D3D12.dll` + CUDA 13 运行库 | 不进发行包 |
| `vcpkg_installed\x64-windows\bin\` | `D3D12Core.dll`、`dxcompiler.dll`、`dxil.dll` | JVM 侧打包 |

`lib\` 整个目录在 `.gitignore` 里（`/lib/`），新克隆的仓库必须先跑 `scripts\sync-external.ps1` 才齐全。
native 侧不做任何 DLL 复制动作——`MCDR.vcxproj` 没有任何 post-build 步骤。

---

## 7. 新增东西时各改哪里

### 7.1 新增一个普通源文件（不属于任何模块）

```text
1. 放到 src\core\（D3D12 实现）、src\advanced\（高级特性）或 src\c_abi\（C ABI 适配）
2. src\ 下不要新增自研 .h；需要跨 TU 共享就放进 include\ 并作为 <ClInclude> 加进工程
3. MCDR.vcxproj → <ItemGroup> 的 <ClCompile Include="src\...\xxx.cpp" />，放到对应功能分区的尾部
4. MCDR.vcxproj.filters → 同名 <ClCompile> 条目 + <Filter>源文件\Core|Advanced|C ABI</Filter>
5. 不要写 module; 与模块声明（普通 TU 才不会被记成模块单元）
6. Release|x64 构建一次：Level3 + TreatWarningAsError 必须干净
```

### 7.2 新增一个模块

```text
1. 命名：渲染相关用 mcdr.d3d12.<域>，适配层用 mcdr.abi.<域>，无依赖工具模块放 mcdr.<域>
2. 接口单元 src\module\<组>\<名>.cppm：
     module;  → #include "_d3d12_module_prelude.h"  → export module mcdr.<…>;  → import … → export namespace …
3. 实现单元（可选）：src\core\<名>.cpp 或 src\advanced\<名>.cpp，首行 module <同一模块名>;
   定义直接写在 .cppm 里也可以（async_executor / async_bundle / mcdr.error 就这么做）
4. MCDR.vcxproj：接口单元与实现单元各一条 <ClCompile>；
   不需要 CompileAs metadata——stdcpplatest 下 .cppm 自动按 CompileAsCppModule 编译
5. MCDR.vcxproj.filters：接口单元归「模块接口\Core|Advanced|C ABI」，实现单元归「源文件\…」
6. import 图必须无环：新模块只能 import 叶子（mcdr.error）或更底层的模块；
   不要被 mcdr.d3d12.device 反向 import——device 是被最多方依赖的汇聚点
7. 导出签名遵守 §3.2：只出不透明句柄与 POD，不出现别模块的类型
8. 同步 AGENTS.MD 的模块表与 docs\c_abi\README.md 的模块清单
```

### 7.3 新增一个导出函数

```text
1. include\mcdr_abi.h：按功能分组加 MCDR_API <ret> MCDR_CALL mcdrXxx(...)，紧跟同族函数
2. 模块侧：在对应 .cppm 的 export namespace 里声明（或直接 inline 定义）；
   实现若走实现单元，必须与声明同模块（§3.1）
3. 适配单元：src\c_abi\abi_<功能>.cpp 的 extern "C" 块里实现，只做三件事——
   参数校验 → 句柄/描述符摊平 → 调模块；失败用 mcdr::abi::failWith / failResult 写错误槽
4. 签名里出现新 struct：同时进 include\mcdr_abi.h 与 JVM 侧的 MCDR_Layout.kt 镜像，
   并加进 mcdrABILayout() 的字段表（布局自检靠它）
5. 破坏性改动（改布局/改语义）：按 mcdr_abi.h 顶部注释的流程提升 MCDR_ABI_VERSION
   （当前 4u），并同步 JVM 侧常量；设备创建时会断言版本一致
6. 契约细节与错误码口径见 docs\abi\README.md
```

---

## 8. 已知问题

> **注意**：`MCDR.vcxproj` 与 `MCDR.vcxproj.filters` 必须同一次改动里一起更新。两边当前各 40 条
> `ClCompile`，一一对应；少一条不会影响编译，但该文件在 VS 里就没有分组。

> **缺陷**：`mcdr.d3d12.descriptor_allocator` 没有任何 `import` 方——`src\` 里搜不到
> `import mcdr.d3d12.descriptor_allocator`，`SegmentedDescriptorAllocator` 也只出现在它自己的接口单元
> 与实现单元里。它每次构建都编译，但当前是死代码（真正在用的是
> `mcdr.d3d12.async_descriptor` 的 `PartitionedDescriptorAllocator`）。

> **缺陷**：`vcpkg.json` 里有 10 个包在 `src\` 与 `include\` 里零 include：
> `directxtk12`、`directxtex`、`directxmesh`、`directxmath`、`shaderwriter`、`tinyexpr`、`llfio`、
> `zoe`、`minizip`、`tomlplusplus`（`toml` 只在 `include\mcdr_abi.h` 的注释里出现）。干净构建要为它们
> 付编译时间。

---

## 文件导航

以下路径相对 `MCDR\`。

| 路径 | 作用 |
| --- | --- |
| `MCDR.vcxproj` | 唯一的构建定义：属性、依赖项、链接输入 |
| `MCDR.vcxproj.filters` | VS 里的分组视图（不参与编译，见 §8） |
| `MCDR.slnx` | 只含这一个工程的解决方案文件 |
| `vcpkg.json` | vcpkg 直接依赖清单 |
| `vcpkg-configuration.json` | vcpkg 注册表 baseline 与 overlay ports |
| `vcpkg-overlay-ports\directx-dxc\` | DXC 端口的本地覆盖（`version-date 2026-05-27`） |
| `.gitmodules` | `external\` 七个 SDK 子模块 |
| `scripts\sync-external.ps1` | 子模块布局整理 + 运行时 DLL 就位检查（含版本事实） |
| `.github\workflows\msbuild.yml` | CI：`Release` 构建 |
| `include\mcdr_abi.h` | 唯一的公共契约（203 处 `MCDR_API` 声明，`MCDR_ABI_VERSION 4u`） |
| `include\_d3d12_module_prelude.h` | 全局模块片段专用前导头（§4.2） |
| `include\mcdr_dlss_impl.h` | 设备模块 ↔ DLSS 普通 TU 之间的 `extern "C"` 桥 |
| `include\mcdr_rtx_support.h` | SDK 可用性探测结果类型（实现是普通 TU `d3d12_rtx_probe.cpp`） |
| `src\module\c_abi\mcdr_abi_common.cppm` | `mcdr.abi.common` 接口单元 |
| `src\module\core\mcdr_error.cppm` | `mcdr.error`（叶模块） |
| `src\module\core\mcdr_device.cppm` | `mcdr.d3d12.device` 接口单元（最大的一份） |
| `src\module\core\d3d12_async_*.cppm` | 四个异步模块接口单元 |
| `src\module\advanced\d3d12_mesh.cppm` | `mcdr.d3d12.mesh` 接口单元 |
| `src\c_abi\abi_*.cpp` | 11 个适配实现单元（`mcdr.abi.common`） |
| `src\core\dllmain.cpp` | DLL 入口点（普通 TU，§4.4） |
| `docs\overview.md` | 文档地图 |
| `docs\abi\README.md` | ABI 契约（新增导出函数的另一半流程） |
| `docs\c_abi\README.md` | 适配层逐文件职责与错误协议 |

## 文档维护

1. 改了 `MCDR.vcxproj` 的语言标准、警告级别、输出目录、链接库或预处理器定义，同步改 §1.3 与 §6。
2. 增删模块、改模块名或改 `import` 关系，同步改 §2.1 的清单与 §2.2 的依赖图，并同步 `AGENTS.MD`
   与 `docs\c_abi\README.md` 的模块表。
3. 增删 `src\` 下的文件，检查 `MCDR.vcxproj` 与 `MCDR.vcxproj.filters` 两边条目数量与路径是否一致
   （两边当前各 40 条 `ClCompile`，必须一一对应）。
4. 升级 `external\` 子模块或改 `vcpkg.json`，同步改 §6.1、§6.2 的版本列与
   `scripts\sync-external.ps1` 文件头；运行库归属变化同步改 §6.3。
5. 发现新的构建期坑（新诊断码、新工具链行为），按「现象 → 原因 → 正确写法」补进 §3，
   并把一次性缺陷补进 §8。
