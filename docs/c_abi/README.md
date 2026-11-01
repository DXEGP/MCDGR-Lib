# C ABI 适配层（`src\c_abi\`）

本目录说明 `MCDR\src\c_abi\` 这 11 个普通翻译单元（TU）做什么、怎么写，以及它们为什么必须以现在这种形态存在于 C++23 模块工程里。阅读对象是要给 MCDR 增删 ABI 函数的人：JVM 侧（Kotlin + FFM）与 native 侧（模块）都算。

先给结论：这一层**不实现任何渲染逻辑**。它只做四件事 —— 把 `extern "C"` 的参数摊平成模块能吃的 POD、校验调用方给的东西是否自相矛盾、把句柄原样递进模块、把模块的 `std::expected` 失败翻译成 `mcdrLastError()`。真正的 D3D12 状态机在 `src\module\` 与 `src\core\` 的模块里。

---

## 1. 这层是什么

### 1.1 契约：`include\mcdr_abi.h`

全库只有一个对外头文件 [`include\mcdr_abi.h`](../../include/mcdr_abi.h)（1182 行，208 处 `MCDR_CALL`，其中 205 个是真实导出）。它的存在不是为了方便，而是硬约束：**C 语言链接的实体不能从 C++ 模块导出**。所以 Kotlin 侧 [`MCDR_ABI.kt`](../../../Illusion/src/main/kotlin/dev/nts/expdxr/d3d12/extern/MCDR_ABI.kt) 必须能按普通 C 头去理解这些符号，模块化只发生在 DLL 内部。

```c
#define MCDR_API  __declspec(dllexport)   // MCDR_EXPORTS 定义时；否则 dllimport
#define MCDR_CALL __cdecl
#define MCDR_ABI_VERSION 7u
```

头部注释规定的跨界规则（违反即静默出错，编译器不会报）：

| 规则 | 具体要求 |
| --- | --- |
| 语言链接 | 只允许 C linkage；一切导出函数 `extern "C"` + `MCDR_API` + `MCDR_CALL` |
| 聚合体 | 每个跨界 struct 在头里只定义一次，Java 侧用 `MemoryLayout.structLayout` 逐字段镜像（含 `_padN`） |
| 句柄 | 一律不透明：`mcdr_object*`、`mcdr_command_ctx*`、`uint64_t`；跨界不解引用 |
| 字符串 | 调用方分配 UTF-8 缓冲；被调方返回**所需字节数（含结尾 NUL）**，写入量不超过 `cap-1` |
| 异常 | 不跨界。失败只能通过返回值 + `mcdrLastError()` |
| 布尔入口 | `int32_t`，1/0 |
| 数组参数 | 一律 `(pointer, count)` 成对，不允许长度藏在元素里 |
| upcall | 全库只有一个回调类型 `mcdr_fence_callback_fn`，外加一个整数 user handle |

### 1.2 一次调用从 JVM 到 D3D12

```text
JVM (Kotlin)                                        MCDR.dll
──────────────────────────────────────────────      ────────────────────────────────────────────────
D3D12 后端调 MCDR_ABI.D3D12CreateTexture(...)
  └─ MH_createtexture.invokeExact(a, b, c, …)
       │  FFM downcall（Linker.nativeLinker()，
       │  参数静态类型必须与 FunctionDescriptor 精确一致）
       ▼
                                             include\mcdr_abi.h
                                               MCDR_API mcdr_object* MCDR_CALL
                                               mcdr_dx12CreateTexture(int32_t usage, …);
                                                     │  按符号名链接
                                                     ▼
                                             src\c_abi\abi_device.cpp        ← 普通 TU
                                               ① 参数自洽性校验
                                               ② POD → d3d12_mc::TextureDesc
                                               ③ 调 d3d12_mc::createTexture(...)
                                                     │  import mcdr.d3d12.device
                                                     ▼
                                             src\module\core\mcdr_device.cppm  ← 接口单元
                                               声明 createTexture(...)
                                                     │  同一模块单元内定义
                                                     ▼
                                             src\core\d3d12_device.cpp        ← 实现单元
                                               module mcdr.d3d12.device;
                                               ④ 解包句柄（Dx12Object::fromHandle）
                                               ⑤ 资源跟踪 / 延迟删除登记
                                               ⑥ ID3D12Device::CreateCommittedResource
```

### 1.3 失败路径

```text
模块返回 std::expected<T, mcdr::Error>；适配 TU 发现 !r
  │
  ├─ mcdr::abi::failWith / failWithPtr / failResult / failResultPtr
  │     └─ setLastError(what, message)
  │           └─ thread_local std::string gLastError = "<函数名>: <message>"
  │     └─ failVoid 才写错误日志：formatLogPrefix("ERROR", "mcdr") + fprintf(stderr) + fflush
  │
  └─ 返回失败值（0 / nullptr / -1 / 0ULL）
        │
        ▼
JVM: MCDR_ABI.D3D12LastError() → mcdrLastError(NULL, 0) 取所需长度
                               → mcdrLastError(buf, len)  取内容
                               → mcdr::abi::writeString（含结尾 NUL 的字节数）
```

两条要点：

- **错误槽是线程局部的**（[`abi_common.cpp`](../../src/c_abi/abi_common.cpp) 的 `thread_local std::string gLastError`）。渲染线程、描述符 worker、native 线程上的 FFM upcall 各报各的，不会互相覆盖；但**读取必须与触发发生在同一线程**，否则读到空串。
- **`setLastError` 只写槽，不打印**。打印只发生在 `failVoid` 里。少数路径（例如分配器 init 失败）会先手工 `setLastError` 再调自己的 `logFail`，所以日志有、返回值仍是 `nullptr`。

### 1.4 职责边界

| 环节 | 归属 | 说明 |
| --- | --- | --- |
| 参数自洽性（pointer/len 配对、count 上限、cap 为负） | 适配 TU | 越界/矛盾在进模块前就失败，错误消息里带函数名 |
| 语义合法性（这个 format 能不能建 Texture、tier 够不够） | 模块 | 适配层只做结构校验 |
| 句柄解包 | 模块 | 只有模块知道 `Dx12Object` 布局；适配层只做非空检查 |
| 生命周期（创建/销毁/延迟删除） | 模块 | 适配层不持有任何资源 |
| 错误文本 | 适配 TU | `r.error().message` 原样透传，不再自造措辞 |
| 日志 | 模块 + `failVoid` | 日志前缀只由 `formatLogPrefix()` 生成 |

### 1.5 这一层不做什么

| 不做 | 为什么 |
| --- | --- |
| 语义校验（格式能否当 render target、tier 够不够、尺寸是否超设备能力） | 那要问设备；放两份必然与模块漂移 |
| 资源所有权 / 引用计数 | 句柄由模块创建与销毁，适配层只转发 |
| 猜状态机（资源当前 state、命令列表开没开） | 状态在模块的 `ctx` 里；适配层猜一次就是 barrier 错配 |
| 跨帧缓存 | 唯一例外是 `thread_local` 的错误槽与 `gBindingScratch` 这类单帧 scratch |
| 正常日志 | 只有 `failVoid` 打错误行；进度与状态由模块自己 `dbgLogInfo` |
| 面向玩家的文案 | 错误文本是英文诊断，配置文案在 `IllusionConfig.kt` |

---

## 2. 逐文件职责

11 个文件合计 205 个导出函数。`abi_common.cpp` 是唯一的例外：它不含任何 `extern "C"` 定义，只提供模块内的错误/字符串工具。

| 文件 | 行数 | 导出 | 导出函数族关键词 | 依赖的模块 | 典型校验 |
| --- | ---: | ---: | --- | --- | --- |
| [`abi_common.cpp`](../../src/c_abi/abi_common.cpp) | 81 | 0 | 错误槽与字符串写出：`setLastError` / `setLastErrorf` / `lastErrorText` / `failWith` / `failVoid` / `failWithPtr` / `writeString` | `mcdr.d3d12.device`（仅取 `formatLogPrefix`） | `writeString` 恒返回 `s.size()+1`；`buf==nullptr` 或 `cap<=0` 时只回长度 |
| [`abi_meta.cpp`](../../src/c_abi/abi_meta.cpp) | 415 | 31 | ABI 元信息与布局自检、日志开关、着色率、抗锯齿、DLSS/Reflex/帧生成开关、PIX | `mcdr.d3d12.device`；普通 TU 头 `mcdr_dlss_impl.h` | `mcdrLastError` 空串回 0；`mcdrSetShadingRate` 未知值回 -1；`mcdr_dx12SetShadingRateNow` 回 -2 表示 ctx 空或无打开列表 |
| [`abi_device.cpp`](../../src/c_abi/abi_device.cpp) | 253 | 23 | 设备/资源/隐藏窗口：`CreateDevice`、`CreateTexture`、`CreateBuffer`、`CreateSampler*`、`CreateTextureView`、`DestroyResource`、`CreateComputePipeline*`、`MapBuffer`、`GetAdapterInfo`、`CreateHiddenWindow` | device, mesh, async_bundle, async_descriptor, async_executor, async_fence | `MapBuffer` 先把 `*outLength=0`；`GetAdapterInfo` 探针回 `size+1`、写入回 `size`；失败统一 `failWithPtr<d3d12_mc::Dx12Object>` |
| [`abi_command.cpp`](../../src/c_abi/abi_command.cpp) | 414 | 50 | 命令层：encoder 生命周期、提交与 fence、拷贝、清屏、render pass、查询池、异步渲染控制通道、compute 派发 | 上述六个 + `mcdr.error` | `ReadQueryValues(count<=0)` 回 1（no-op）；`WaitForFence` 把含 `"timed out"` 的错误当正常返回；`DispatchCompute` 透传 1/0/-2 |
| [`abi_pipeline.cpp`](../../src/c_abi/abi_pipeline.cpp) | 151 | 2 | 图形管线 `CreateGraphicsPipeline` / `DestroyPipeline` | 上述六个 | `colorCount ≤ 8`、`inputElementCount ≤ 64`、`bindingCount ≤ 64`；`vsData/vsLen` 配对；`semanticName` 空串保留为空（下游回退 `TEXCOORD`） |
| [`abi_draw.cpp`](../../src/c_abi/abi_draw.cpp) | 141 | 10 | render pass 内录制：`SetPipeline`、`SetScissor`、`SetViewport`、`SetVertexBuffer`、`SetIndexBuffer`、`PushDescriptors`、`Draw*` | 上述六个 + `mcdr.error` | `PushDescriptors(count<=0)` 直接回 1 且不碰 ctx；`bindings==nullptr` 才报错；逐字段拷进 `thread_local std::vector<DrawBinding>` |
| [`abi_mesh.cpp`](../../src/c_abi/abi_mesh.cpp) | 280 | 8 | mesh 管线与 HLSL → DXIL：`QueryMeshSupport`、`CompileShaderDxc`、`CreateMeshPipeline`、`CreateMeshRootSignature`、`ReleaseRootSignature`、`DispatchMesh`、`MeshDrawMeshlets` | 上述六个 | `msLen>0 && msData!=nullptr` 是硬条件；`sourceLen ≤ 4 MiB`；`DispatchMesh` 每维 1..65535 |
| [`abi_surface.cpp`](../../src/c_abi/abi_surface.cpp) | 428 | 20 | DXGI surface：创建/配置/present/Blit/back buffer 句柄/像素回读/交换链选项/HDR/RT | 上述六个 + `mcdr.error`；4 个普通 TU 状态头 | 回读要求句柄 `kind == Texture`、宽高 ≤ 16384、格式能映射 bpp；回读等待 fence 5 秒 |
| [`abi_async.cpp`](../../src/c_abi/abi_async.cpp) | 724 | 41 | 异步基础设施：分区描述符分配器、bundle 录制池、fence 管理器、主列表执行器、屏障预处理 | 上述六个 + `mcdr.error` | CBV 地址必须 256 字节对齐；`Allocate(count<=0)` 回 -1；`prepareObjects` 要求 `ctx->listOpen`；upcall 不得抛异常 |
| [`abi_sr.cpp`](../../src/c_abi/abi_sr.cpp) | 101 | 9 | 超分抽象层：`SrPick`、`SrQueryCaps`、`SrCreate`、`SrResize`、`SrEvaluate`、`SrJitter`、`SrJitterPhaseCount`、`SrDestroy`、`SrQuery` | `mcdr.d3d12.device` + `mcdr.error` | backend 必须在 `MCDR_SR_BACKEND_NONE..MCDR_SR_BACKEND_XESS`；输出缓冲两种非法形态（负 cap、cap>0 但指针空）都回 -1 |
| [`abi_framegen.cpp`](../../src/c_abi/abi_framegen.cpp) | 98 | 11 | 帧生成：`FgSetConstants`、`FgPick`、`FgQueryCaps`、`FgSetMode`、`FgPresented`、`FgStatus`、`FgState`、`FgSetUiTexture`、`FgSetUiComposition`、`FgUiCompositionReady`、`mcdr_fgTagDlssInputs` | `mcdr.d3d12.device` + `mcdr.error` | backend 必须在 `MCDR_FG_BACKEND_NONE..MCDR_FG_BACKEND_XESSFG`；`FgStatus` 与 `SrQuery` 同一套容量约定 |

两个后加的文件（`abi_sr.cpp`、`abi_framegen.cpp`）与其它 9 个结构完全相同：`module mcdr.abi.common;` 的实现单元、`extern "C"` 导出、`import mcdr.d3d12.device` 转发。它们的共同约定是**文件里不出现任何厂商名**，所有厂商分支（DLSS / XeSS / FSR …）都在 `src\advanced\` 的实现单元里。

---

## 3. 模块规则（三条硬约束）

MSVC v145 + `/std:c++latest` 下这三条不是风格问题，是构建成败问题。工程开了 `TreatWarningAsError=true`，任何一条踩到都是硬失败。

### 3.1 导出函数必须与声明在同一个模块单元内定义

必须：在 `.cppm` 里 `export` 声明的函数，就在**同一个模块单元**里定义 —— 可以写在 `.cppm` 里，也可以写在同一模块的实现单元（`module <名>;`）里，但**不能挪到普通 TU**。

原因：模块内实体的修饰名带模块私有后缀，普通 TU 按 `mcdr_abi.h` 的前向声明算出的是另一个名字，两边永远对不上，链接期报 `LNK2001`（未解析的外部符号）或 `LNK2019`（签名不匹配的未解析引用）。

必须：实现单元不要 `import` 自己所属的模块（`module mcdr.abi.common;` 的文件里再写 `import mcdr.abi.common;` 是错的）。

```text
src\module\c_abi\mcdr_abi_common.cppm   export module mcdr.abi.common;   声明 mcdr::abi::*
src\c_abi\abi_common.cpp                module mcdr.abi.common;         定义 mcdr::abi::*   ← 同一模块
src\c_abi\abi_device.cpp                module mcdr.abi.common;         extern "C" 导出（不属于模块）
```

### 3.2 导出签名不得出现别的模块拥有的类型

必须：模块导出的函数签名里只出现**本模块自己定义的类型**、基础类型，或者不透明句柄（`void*` / `uintptr_t` / `uint64_t`）。跨模块传递的对象一律走句柄。

原因不是洁癖：v145 会把「别的模块拥有的类型」在签名里记成模块私有名，例如

```text
Dx12Pipeline@$$_Amcdr.d3d12.device
```

而普通 TU（`abi_pipeline.cpp` 这种）按头文件里能看到的前向声明只能得到公共名 `Dx12Pipeline@1`。两侧修饰名不同 → 链接不上，或更糟：能链接但在错误的假设下跑。

现行做法：`mcdr::abi::fromHandle<T>` / `toHandle` 是全库唯一的 `reinterpret_cast` 落点（在 `mcdr_abi_common.cppm` 里），转换只发生在模块内部；`mcdr_abi.h` 里出现的句柄都是 `mcdr_object*`、`mcdr_command_ctx*`、`void*`、`uint64_t`。`createMeshPipeline`、`dispatchMesh` 这类跨模块入口就用 `uint64_t` / POD 描述符，不用模块类型。

普通 TU 与模块单元之间唯一的合法桥是 `extern "C"`：同一个 C++ 函数在两边会算出不同修饰名，只有 C 链接能对上同一个符号（`include\mcdr_dlss_impl.h` 整份头就是为这条规则存在的，签名里只有 `void*`、POD 和 `char*` 缓冲，**不要**把 `std::string` 放进去 —— 两个 TU 看到的不是同一个类型）。

### 3.3 `#include` 只能放在全局模块片段

必须：系统头与内部 prelude 放在 `module;` 与 `export module`（或 `module <名>;`）**之间**。

```cpp
module;                                   // ← 全局模块片段开始

#include "_d3d12_module_prelude.h"       // windows.h / d3d12.h / dxgi*.h / STL

module mcdr.abi.common;                   // ← 模块 purview 开始

#include "mcdr_abi.h"                     // 只有宏与 extern "C" 声明，自身不展开标准库
```

一旦在 purview 里包含会展开系统头/标准库的头，MSVC 报

```text
warning C5244: '#include' in the purview of module 'mcdr.abi.common' appears erroneous.
              Consider moving that directive before the module declaration.
```

`TreatWarningAsError=true` → 直接失败。`src\c_abi\*.cpp` 的现行形态是上面那段骨架：prelude 在 `module;` 之后、`mcdr_abi.h` 与 `mcdr_dlss_impl.h` 在模块声明之后（这两个头不含标准库实体）。**判断标准是「谁在 purview 里展开出系统头」，不是「头文件放在哪一行」**；新加头文件前先看它 `#include` 了什么。

配套的两条：

- prelude 里不能改成 `import std;` —— 那个结构式在全局模块片段里会被判为「仅可包含预处理器指令」（C5202）。
- 全局模块片段里裸写 `extern "C"` 声明同样触发 C5202；要带 C 声明就装进一个头文件再 `#include`（`mcdr_dlss_impl.h`、`mcdr_rtx_support.h` 都是这么用的）。

### 3.4 为什么 `src\core\dllmain.cpp` 是普通 TU

必须：`DllMain` 留在 [`src\core\dllmain.cpp`](../../src/core/dllmain.cpp)，不要并进任何 `.cppm`。

理由有两条，都不可绕过：模块无法导出 C 语言链接的实体；而 `DllMain` 由 CRT 按**导出名**查找，C 链接才稳。该文件全文只有 `extern "C" { #include <windows.h> … BOOL APIENTRY DllMain(...) }`，`DLL_PROCESS_ATTACH` 时调 `DisableThreadLibraryCalls(hModule)`。

### 3.5 模块清单

| 模块名 | 接口单元 | 实现单元 | C ABI 侧使用者 |
| --- | --- | --- | --- |
| `mcdr.abi.common` | `src\module\c_abi\mcdr_abi_common.cppm` | 无独立实现单元（`src\c_abi\*.cpp` 是它的实现单元） | 全部 11 个文件 |
| `mcdr.error` | `src\module\core\mcdr_error.cppm` | 无（叶模块） | `abi_command/surface/async/draw/sr/framegen` 显式 import |
| `mcdr.d3d12.device` | `src\module\core\mcdr_device.cppm` | `src\core\d3d12_device.cpp`、`d3d12_surface.cpp`、`d3d12_sr.cpp`、`d3d12_framegen.cpp`、`d3d12_fsffg.cpp`、`d3d12_fsr.cpp`、`d3d12_xess.cpp`、`d3d12_xessfg.cpp` | 全部 11 个文件 |
| `mcdr.d3d12.descriptor_allocator` | `src\module\core\d3d12_descriptor_allocator.cppm` | `src\core\d3d12_descriptor_allocator.cpp` | 间接（经 device） |
| `mcdr.d3d12.async_descriptor` | `src\module\core\d3d12_async_descriptor.cppm` | `src\core\d3d12_async_descriptor.cpp` | `abi_async`、`abi_command`、`abi_draw` 等 |
| `mcdr.d3d12.async_fence` | `src\module\core\d3d12_async_fence.cppm` | `src\core\d3d12_async_fence.cpp` | 同上 |
| `mcdr.d3d12.async_executor` | `src\module\core\d3d12_async_executor.cppm` | 并入接口单元 | 同上 |
| `mcdr.d3d12.async_bundle` | `src\module\core\d3d12_async_bundle.cppm` | 并入接口单元 | 同上 |
| `mcdr.d3d12.mesh` | `src\module\advanced\d3d12_mesh.cppm` | 无（`src\advanced\d3d12_mesh_base.cpp` 是普通 TU 消费者） | `abi_mesh`、`abi_device`、`abi_pipeline` |

`import` 不传递：用到 `mcdr::Error` 的 TU 必须自己 `import mcdr.error;`（`abi_draw.cpp` 第 21 行就带着这条注释）。

---

## 4. 适配层通用写法

### 4.1 文件骨架

11 个文件共用同一段前言，顺序固定，不要重排：

```cpp
// abi_xxx.cpp — 一句话职责
module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
// …按需

using namespace d3d12_mc;

namespace { /* 常量上限、thread_local scratch */ }

extern "C" {
/* 导出函数 */
}  // extern "C"
```

`using namespace d3d12_mc;` 是刻意保留的：适配层几乎每个表达式都要落到模块函数上，逐处加限定只会把调用行挤断。

### 4.2 参数校验模式

校验一律在**进模块之前**完成，且每条失败都要能指名道姓。四种固定形态：

| 形态 | 写法 | 实例 |
| --- | --- | --- |
| 指针配对 | `if (len < 0 \|\| (len > 0 && data == nullptr))` | `abi_pipeline.cpp`：`"vsData/vsLen mismatch"`、`"psData/psLen mismatch"` |
| 计数上限 | 命名常量 + `setLastErrorf(what, "bad colorCount %d", …)` | `kMaxColorTargets = 8`、`kMaxInputElements = 64`、`kMaxPipelineBindings = 64` |
| 写缓冲形态 | 负 cap，或 `cap > 0 && buf == nullptr` 都拒 | `abi_sr.cpp`：`mcdr_dx12SrQuery` 回 -1 |
| 早返回 no-op | 空集合不是错误，直接成功 | `abi_draw.cpp`：`count <= 0` → `return 1`；`abi_command.cpp`：`ReadQueryValues(count<=0)` → 1 |

有两条不要做：

- 不要在校验里重算语义合法性（例如「这个 `DXGI_FORMAT` 能不能当 render target」）。那属于模块，重复一份只会两边漂移。
- 不要为了凑消息自己造句。失败文本统一 `r.error().message`；只有结构错误（null、越界）才由适配层给短句，且必须是英文。

### 4.3 句柄与非空检查

适配层**不解包句柄**，只判断「有没有传」。解包发生在模块里（只有它知道 `Dx12Object` 的布局），所以 `mcdr_dx12DestroyResource(nullptr)` 这类调用是模块的幂等路径，不是适配层该拦的事故。

必须做的非空检查是**组合条件**类的入口：

| 函数 | 检查 |
| --- | --- |
| `mcdr_dx12DispatchMesh` | `ctx == nullptr` → `"command context is null"` |
| `mcdr_dx12MeshDrawMeshlets` | 四个 buffer 句柄任一为空 → `"buffer handle is null"` |
| `mcdr_dx12AsyncDescriptorCreate` | 模块报 `"device/drawHeap not ready"` 时由适配层 `setLastError` + `logFail` + 回 `nullptr` |
| `mcdr_dx12WriteCBV` | `slot/offset/size < 0` 回 0；地址未 256 字节对齐另报 `"CBV is not 256-byte aligned (buffer offset=%lld)"` |

### 4.4 错误落地：`setLastError` 的位置

`setLastError` / `setLastErrorf` 只写线程局部槽。什么时候调，只有三种正当场合：

1. **走 `failWith` 家族**（绝大多数）。`failWith` 返回 0、`failWithPtr` 返回 `nullptr`、`failVoid` 返回 `void`、`failResult`/`failResultPtr` 接 `mcdr::Error`，四个都会顺手把槽写好、并按 `failVoid` 的路径打错误日志。
2. **返回值不是 0/1 语义**，没法用 `failWith`：例如 `mcdr_dx12CreateMeshRootSignature` 要回 `uint64_t 0`，于是手工 `setLastErrorf(...)` 再 `return 0`。
3. **失败必须留日志但没错误值**：`mcdr_dx12AsyncDescriptorCreate` 里 `device/drawHeap not ready`。

约定：`failWith`/`failWithPtr`/`failResult*` **全部全限定写**（`mcdr::abi::failResultPtr<...>`）。`abi_mesh.cpp` 第 216 行记了原因：实参是 `std::unexpected<mcdr::Error>`，ADL 只会拉进 `std` 与 `mcdr`，不会把 `mcdr::abi` 带进来。

### 4.5 返回值约定

| 返回值 | 含义 | 代表函数 |
| --- | --- | --- |
| `1` | 成功 | `mcdr_dx12SetPipeline`、`mcdr_dx12PushDescriptors`、绝大多数 `int32_t` 入口 |
| `0` | 失败（已 `setLastError`） | `failWith` 的返回值、`mcdr_dx12BlitSurface` |
| `-1` | 参数非法 / 不支持 / 无句柄 | `mcdr_dx12SrQueryCaps`、`mcdr_dx12CreateDlssFeature`、`mcdr_dx12AsyncRegionBase` |
| `-2` | 命令列表没打开 | `mcdr_dx12DispatchCompute`、`mcdr_dx12SetShadingRateNow` |
| 正整数（不含 NUL 的字节数） | 尺寸探针 | `mcdr_dx12CompileShaderDxc` 回 `dxil.size()`；`mcdr_dx12GetAdapterInfo` 写入路径回 `size` |
| `size + 1` | 字符串探针（含 NUL） | `mcdr::abi::writeString`、`mcdrLastError`、`mcdr_dx12SrQuery`、`mcdr_dx12FgStatus` |
| `uint64_t 0` | 无效句柄 | `mcdr_dx12CreateMeshRootSignature`、`mcdr_dx12CreateHiddenWindow` |
| `~0ULL` | 无当前帧值 | `mcdr_dx12AsyncCurrentFrameValue(nullptr)` |
| `float*` 出参恒 0 返回 | 只有一个 jitter 源，无失败路径 | `mcdr_dx12SrJitter` |

> **缺陷**：长度基准不统一。`mcdr_dx12GetAdapterInfo` 的探针路径回 `size+1`、写入路径回 `size`（不含 NUL），而 `writeString` 家族两条路径都回 `size+1`。调用方必须按函数分别处理，不能套同一个「返回 NUL 及长度」假设。

### 4.6 线程约定

只以代码注释为准；注释没写的一律按「代码中未明确」处理，不要替它假设。

| 入口组 | 线程要求 | 依据 |
| --- | --- | --- |
| `abi_draw.cpp` 全部录制函数、`mcdr_dx12PushDescriptors` | 渲染线程；并行 bundle 录制时也在描述符 worker 上 | `thread_local gBindingScratch`，注释明确「两处都不会重入同一个 pushDescriptors」 |
| `mcdr_dx12AsyncFenceRegisterCallback` 的回调 | native monitor 线程，注册锁外触发，可重入 | 回调是 FFM upcall stub，**不得抛异常**；这是全库唯一 native 回调 JVM 的点 |
| `mcdr_dx12FgSetMode` / `mcdr_dx12SetDlssgMode` / `mcdr_dx12SetDlssConstants` | 任意线程调用，但**只记状态** | 真正的 `slDLSSGSetOptions` / `slSetConstants` 在渲染线程的下一次 evaluate / present 前下发 |
| `mcdr_dx12PresentSurface` | 主线程或渲染线程，二选一 | `gAsyncOwnsPresent` / `gAsyncRenderCtx` 仲裁，避免同一交换链双 Present |
| `mcdr_dx12AsyncPrepare*` | 主列表（渲染线程） | bundle 内不能含 `ResourceBarrier`，屏障只能由主列表预处理 |
| 设备/资源创建、管线创建、像素回读 | 代码中未明确 | 现有调用方都在初始化或帧边界，但没有注释与断言约束 |
| `mcdrLastError` | 任意线程，**但只能读到本线程的错误** | 错误槽是 `thread_local` |

### 4.7 字符串与尺寸探针协议

所有「调用方给缓冲」的入口共用一套两段式协议：先用空指针探长度，再按长度分配后取内容。

```text
调用方                                        被调方
mcdrLastError(NULL, 0)            ──────▶     只计算，不写内存，返回 need
allocate(need)
mcdrLastError(buf, need)          ──────▶     写入 need-1 字节 + '\0'，仍返回 need
```

必须遵守的三条：

1. **缓冲不足不是错误**。被调方发现 `cap < need` 时只回长度、不写、不 `setLastError`，所以调用方不能把「返回值 > cap」当失败。
2. **返回值口径按函数不同**。`mcdrLastError` / `mcdr_dx12SrQuery` / `mcdr_dx12FgStatus` 回 `size+1`（含 NUL）；`mcdr_dx12CompileShaderDxc` 回 `dxil.size()`（二进制，没有 NUL 概念）；`mcdr_dx12GetAdapterInfo` 探针回 `size+1`、写入回 `size`。
3. **必须有终止符**。`writeString` 写入 `min(size, cap-1)` 字节后强制补 `'\0'`；`CompileShaderDxc` 不做任何补零，缓冲区大小必须用探针值。

| 函数 | 探针返回 | 写入返回 | 备注 |
| --- | --- | --- | --- |
| `mcdrLastError` | `size+1`；无线程错误时 0 | 同左 | 读的是当前线程的错误槽 |
| `mcdr_dx12SrQuery` / `mcdr_dx12FgStatus` | `size+1`；`noteCap==0` 时只回长度 | 同左 | 负 cap 或 `cap>0 && buf==nullptr` 直接 -1 |
| `mcdr_dx12CompileShaderDxc` | `dxil.size()` | 同左 | 二进制；`outDxil==nullptr` 只允许配 `outCapacity==0` |
| `mcdr_dx12GetAdapterInfo` | `size+1` | `size` | 空 JSON 回 0（见 §4.5 的缺陷条目） |
| `mcdr_dx12SurfacePresentModes` | `total`（只写 `min(cap,total)` 项） | 同左 | 数组型，不是字符串 |

---

## 5. 编译单元顺序

`MCDR.vcxproj` 与 `.filters` 里 `src\c_abi\` 的顺序一致：

```text
abi_common → abi_meta → abi_device → abi_command → abi_pipeline → abi_draw
           → abi_mesh → abi_surface → abi_async → abi_sr → abi_framegen
```

| 位置 | TU | 在这一层里的角色 | 依赖层次 |
| ---: | --- | --- | --- |
| 1 | `abi_common.cpp` | 错误槽、失败助手、字符串写出 | 被所有文件使用，自己无 C 导出 |
| 2 | `abi_meta.cpp` | 版本、布局自检、日志与运行开关 | 只依赖 device；JVM 最先调 |
| 3 | `abi_device.cpp` | 设备、资源、适配器枚举、隐藏窗口 | 一切句柄的来源 |
| 4 | `abi_command.cpp` | encoder 生命周期、提交、拷贝、render pass、查询、异步控制 | 需要 ctx |
| 5 | `abi_pipeline.cpp` | 图形管线 | 需要 device |
| 6 | `abi_draw.cpp` | pass 内录制 | 需要 ctx + pipeline |
| 7 | `abi_mesh.cpp` | mesh 管线与 DXC | 需要 device + root signature |
| 8 | `abi_surface.cpp` | 交换链、present、回读 | 需要 device + queue + back buffer |
| 9 | `abi_async.cpp` | 分区描述符、bundle、fence、执行器 | 同时用到 ctx、bundle、descriptor、fence |
| 10 | `abi_sr.cpp` | 超分抽象层 | 建立在 device 之上，独立于异步组 |
| 11 | `abi_framegen.cpp` | 帧生成 | 建立在 SR/设备之上 |

这个顺序**不是链接依赖声明**：工程开着 `MultiProcessorCompilation=true` 与 `ScanSourceForModuleDependencies=true`，编译顺序由模块扫描决定，与列表顺序无关。它的实际作用是两条：

1. **阅读顺序 = 调用时序**。按 JVM 侧实际使用链路排：先错误槽（1），再自检（2），再建设备（3），再编码命令（4–7），再呈现（8），最后是异步与两个后加的子系统（9–11）。
2. **`.filters` 分组靠它对齐**。`src\c_abi\*.cpp` 全部落在「源文件\C ABI」这一个过滤器里，条目顺序与 vcxproj 一致；`abi_sr` / `abi_framegen` 排在末尾，因为它们是在异步组之后补齐的功能块。

必须同步的两处：

| 位置 | 内容 |
| --- | --- |
| `MCDR\MCDR.vcxproj` | `<ItemGroup>` 里的 `<ClCompile Include="src\c_abi\abi_xxx.cpp" />`，位置决定 IDE 与列表顺序 |
| `MCDR\MCDR.vcxproj.filters` | 同名条目 + `<Filter>源文件\C ABI</Filter>`；缺条目不影响编译，但在解决方案资源管理器里看不到该文件 |

`.cppm` 只作为 `<ClCompile>` 项加入即可，**不要**写 `CompileAs`：`LanguageStandard=stdcpplatest` 会让 MSBuild 自动按 `CompileAsCppModule` 编译（vcxproj 里 229–231 行的注释就是这条）。

`MCDR.vcxproj` 与 `MCDR.vcxproj.filters` 两边都列了 `abi_sr.cpp` 与 `abi_framegen.cpp`，后者排在最末；新增 TU 时两处都要补，否则 IDE 里看不到分组。

---

## 6. 新增一个 ABI 函数

以「新增 `mcdr_dx12Something(const mcdr_xxx_desc* desc) -> int32_t`」为例，八步，缺一步就有一侧静默失效。

| # | 动作 | 文件 | 要点 |
| ---: | --- | --- | --- |
| 1 | 声明导出 | `MCDR\include\mcdr_abi.h` | `MCDR_API int32_t MCDR_CALL mcdr_dx12Something(const mcdr_xxx_desc* desc);`；新 struct 连同 `_padN` 一起定义；**改动已有 struct 布局或返回值语义必须 bump `MCDR_ABI_VERSION`**（当前 `7u`） |
| 2 | 声明模块入口 | `MCDR\src\module\core\mcdr_device.cppm`（或对应模块接口单元） | 只在本模块声明的类型 + 基础类型；跨模块只传 `void*` / `uintptr_t` / `uint64_t` |
| 3 | 实现模块入口 | 同模块的实现单元：`MCDR\src\core\d3d12_device.cpp`、`src\core\d3d12_surface.cpp`、`src\advanced\d3d12_mesh_base.cpp` 等 | 必须是同一模块单元；返回 `std::expected<T, mcdr::Error>` 或 `mcdr::Result<...>` |
| 4 | 写适配导出 | `MCDR\src\c_abi\abi_<family>.cpp` | 用现成前言，不要重复 `#include`；校验 → 填 POD → 调模块 → `failWith*` 收尾 |
| 5 | 登记布局 | `MCDR\src\c_abi\abi_meta.cpp` 的 `layoutTable()` | 每个镜像字段一行 `MCDR_FIELD(T, F)`，**包括 `_padN`**；漏一个 → JVM 侧 `verify` abort |
| 6 | 加 Kotlin 镜像 | `Illusion\src\main\kotlin\dev\nts\expdxr\d3d12\extern\MCDR_Layout.kt` | `MemoryLayout.structLayout(...)` + `<NAME>_FIELD` 常量（经 `offset(layout, field)`）；字段名与 `mcdr_abi.h` 逐字一致 |
| 7 | 绑定 FFM 句柄 | `Illusion\src\main\kotlin\dev\nts\expdxr\d3d12\extern\MCDR_ABI.kt` | `private var MH_something: MethodHandle? = null`，在绑定块里 `MH_something = skip("mcdr_dx12Something", fd(MCDRLayout.I32, MCDRLayout.PTR))`（可选功能用 `skipOptional`），再包一个 `fun D3D12Something(...)` |
| 8 | 构建验证 | `MSBuild MCDR.vcxproj /p:Configuration=Release /p:Platform=x64` | 构建成功后 JVM 侧 `compileKotlin`；运行时 `verifyABI()` 会把 native 的 (name, offset, size) 表与 `MCDRLayout` 双向比对 |

第 6、7 步的两条细节：

- `MCDR_Layout.kt` 里 `MemoryLayout.structLayout` **不插入任何 padding**，C 编译器会。所以每个 `_padN` 都必须在 Kotlin 侧显式写出（这是 `layoutTable()` 也登记 `_padN` 的原因），并用 `verify()` 比对 offset + size 两个方向。
- `MCDR_ABI.kt` 的 `skip()` 要求符号存在，`skipOptional()` 允许缺失（旧 DLL 时单个功能降级）。`invokeExact` 的实参静态类型必须与 `FunctionDescriptor` 精确一致：MSVC 的 `long` 是 32 位，C 侧 `long` 在 Kotlin 侧就是 `MCDRLayout.I32`，不能写成 `I64`。

第 4 步的三条禁令：

- 不要在导出签名里出现模块类型（见 §3.2）。
- 不要把 `std::string` / `std::vector` 放进任何跨界头。
- 不要用环境变量当功能入口。`mcdr_dx12SetEnvironment` 只是既有 `getenv()` 读点的兼容层，新开关一律走 `IllusionConfig` + 一个专用 ABI 函数。

### 6.1 最小示例（只列新增点）

契约：

```c
// MCDR\include\mcdr_abi.h
typedef struct mcdr_xxx_desc {
    mcdr_object* texture;
    int32_t      samples;
    int32_t      _pad0;      /* Java 侧必须显式写这 4 字节 padding */
} mcdr_xxx_desc;

MCDR_API int32_t MCDR_CALL mcdr_dx12Something(const mcdr_xxx_desc* desc);
```

适配 TU（放在 `extern "C" { … }` 里，前言已由文件顶部提供）：

```cpp
int32_t MCDR_CALL mcdr_dx12Something(const mcdr_xxx_desc* desc) {
    constexpr const char* kWhat = "mcdr_dx12Something";

    if (!desc) {
        return mcdr::abi::failWith(kWhat, "descriptor is null");
    }
    if (desc->samples <= 0 || desc->samples > 8) {
        mcdr::abi::setLastErrorf(kWhat, "bad samples %d", static_cast<int>(desc->samples));
        return 0;
    }

    if (const auto r = d3d12_mc::something(desc->texture, desc->samples); !r) {
        return mcdr::abi::failWith(kWhat, r.error().message);
    }
    return 1;
}
```

Kotlin 镜像与绑定：

```kotlin
// MCDR_Layout.kt
val XXX_DESC: MemoryLayout = MemoryLayout.structLayout(
    PTR.withName("texture"),
    I32.withName("samples"),
    I32.withName("_pad0"),
).withName("mcdr_xxx_desc")

@JvmField val XXX_DESC_TEXTURE: Long = offset(XXX_DESC, "texture")
@JvmField val XXX_DESC_SAMPLES: Long = offset(XXX_DESC, "samples")

// MCDR_ABI.kt
private var MH_something: MethodHandle? = null
// 绑定块里：
MH_something = skip("mcdr_dx12Something", fd(MCDRLayout.I32, MCDRLayout.PTR))

fun D3D12Something(desc: MemorySegment): Boolean = MH_something!!.invokeExact(desc) as Int != 0
```

三处必须同时成立，否则运行时才炸：`mcdr_abi.h` 的字段与 `MCDR_FIELD` 登记一一对应、`MCDRLayout` 的 `structLayout` 字段顺序与 padding 完全一致、`fd(...)` 的参数类型与 C 原型逐位对应。

---

## 文件导航

相对 `MCDR\` 的路径：

| 路径 | 作用 |
| --- | --- |
| [`include\mcdr_abi.h`](../../include/mcdr_abi.h) | 唯一对外契约：`extern "C"` 导出、跨界 struct、`MCDR_ABI_VERSION` |
| [`include\_d3d12_module_prelude.h`](../../include/_d3d12_module_prelude.h) | 各模块全局模块片段共用的系统头 + `DBG_LOG_DEBUG` |
| [`include\mcdr_dlss_impl.h`](../../include/mcdr_dlss_impl.h) | DLSS 实现单元（普通 TU）与设备模块之间的 `extern "C"` 桥 |
| [`src\module\c_abi\mcdr_abi_common.cppm`](../../src/module/c_abi/mcdr_abi_common.cppm) | `mcdr.abi.common` 接口单元：错误槽、`failWith*`、`writeString`、`fromHandle`/`toHandle` |
| [`src\c_abi\abi_common.cpp`](../../src/c_abi/abi_common.cpp) | 错误槽与字符串写出的实现 |
| [`src\c_abi\abi_meta.cpp`](../../src/c_abi/abi_meta.cpp) | 版本、布局自检、日志开关、运行期开关 |
| [`src\c_abi\abi_device.cpp`](../../src/c_abi/abi_device.cpp) | 设备、资源、适配器、隐藏窗口 |
| [`src\c_abi\abi_command.cpp`](../../src/c_abi/abi_command.cpp) | encoder、提交、拷贝、清屏、render pass、查询、异步控制、compute |
| [`src\c_abi\abi_pipeline.cpp`](../../src/c_abi/abi_pipeline.cpp) | 图形管线 |
| [`src\c_abi\abi_draw.cpp`](../../src/c_abi/abi_draw.cpp) | pass 内录制 |
| [`src\c_abi\abi_mesh.cpp`](../../src/c_abi/abi_mesh.cpp) | mesh 管线与 HLSL → DXIL |
| [`src\c_abi\abi_surface.cpp`](../../src/c_abi/abi_surface.cpp) | 交换链、present、像素回读 |
| [`src\c_abi\abi_async.cpp`](../../src/c_abi/abi_async.cpp) | 分区描述符、bundle、fence、执行器、屏障预处理 |
| [`src\c_abi\abi_sr.cpp`](../../src/c_abi/abi_sr.cpp) | 超分抽象层 |
| [`src\c_abi\abi_framegen.cpp`](../../src/c_abi/abi_framegen.cpp) | 帧生成 |
| [`src\core\dllmain.cpp`](../../src/core/dllmain.cpp) | DLL 入口点（刻意的普通 TU） |
| [`MCDR.vcxproj`](../../MCDR.vcxproj) | 编译单元清单与全部语言/警告选项 |
| [`MCDR.vcxproj.filters`](../../MCDR.vcxproj.filters) | IDE 分组（`源文件\C ABI`） |

JVM 侧（相对仓库根）：

| 路径 | 作用 |
| --- | --- |
| [`MCDR_ABI.kt`](../../../Illusion/src/main/kotlin/dev/nts/expdxr/d3d12/extern/MCDR_ABI.kt) | `Linker.nativeLinker().downcallHandle` + `MethodHandle` 句柄表 + 各 `D3D12Xxx()` 包装 |
| [`MCDR_Layout.kt`](../../../Illusion/src/main/kotlin/dev/nts/expdxr/d3d12/extern/MCDR_Layout.kt) | `MCDRLayout`：每个跨界 struct 的 `MemoryLayout` 镜像与 `verify()` |

相邻主题：

- [`docs\overview.md`](../overview.md)：`docs\` 的总览与文档地图。
- [`docs\advanced\dlss.md`](../advanced/dlss.md)：`mcdr_dx12CreateDlssFeature` / `mcdr_dx12SetDlss*` 背后的实现单元。
- [`docs\advanced\framegen.md`](../advanced/framegen.md)：`abi_framegen.cpp` 那 11 个导出的完整语义。

## 文档维护

1. 改 `include\mcdr_abi.h` 里的 struct / 枚举 / 返回值语义时，同一次改动更新 §2 的「典型校验」与 §4.5 的返回值表；struct 变更还要三处同步：`MCDR_ABI_VERSION`、`layoutTable()`、`MCDRLayout`。
2. 增删或改名 `src\c_abi\` 的 TU 时，同一次改动更新 §2 表、§5 顺序表，并向 `MCDR.vcxproj` 与 `MCDR.vcxproj.filters` 各补一条。
3. 新增跨线程入口时，把线程要求写进代码注释并同步 §4.6；注释没写清的，文档只能标「代码中未明确」。
4. 本文件只写当下：不记设计史与排查过程；缺陷用 `> **缺陷**：…` 单列，修好一条删一条。
5. 代码与文档冲突时以代码为准，顺手把本文改对。
