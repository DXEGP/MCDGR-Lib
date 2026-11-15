# 日志通道、诊断开关与崩溃取证

本文讲 native 侧（`MCDR.dll`）怎么输出日志、级别由谁决定、诊断开关在哪里，以及进程崩掉时能留下什么证据。
读者是要从用户日志里定位 D3D12 问题的开发者，以及要指导用户"打开哪个开关、抓哪几个文件"的维护者。

相关文档：[ABI 契约](../abi/README.md) §7「日志与诊断」（导出函数的签名与调用约定）、
[适配层](../c_abi/README.md)（错误串与 `failVoid` 的落盘路径）、
[设备与命令层](../device/README.md)（`ensureDevice()` 的步骤编号）、
[交换链](../surface/README.md)（present 失败时的错误行）。

JVM 侧（`IllusionLogger`）是另一条出口，本文不展开；两条出口在同一个 `logs/latest.log` 里混排，
靠 §3 的前缀区分是谁写的。

## 1. 一条日志的完整路径

### 1.1 三个出口

| 出口 | 写入者 | 落点 | 谁能拿到 |
| --- | --- | --- | --- |
| 进程 `stderr` | `emitLogLine()` 里的 `std::fprintf(stderr, …)` | 启动器为子进程接的管道 / 控制台 | 启动器若把子进程 `stderr` 落盘，就进 `logs/latest.log`；否则只在控制台闪过 |
| 镜像文件 | `mirrorLogToFile()` | JVM 通过 `mcdrSetLogFile()` 给的路径，`fopen(path, "a")` | 玩家自己指定的文件，跨启动累积 |
| JVM log4j | `IllusionLogger` | `logs/latest.log` | 玩家默认会打包上传的那份 |

原生行**只有前两条出口**。`stderr` 是否进 `logs/latest.log` 由启动器决定，native 代码里没有这条路径
（代码中未明确）；部分启动器只收 `stdout`，所以镜像文件是"用户不装任何东西也能留下的副本"。

### 1.2 从调用点到文件

```
dbgLogInfo("device ready: FL=%s tier=%s", fl, tier)
      │
      │  注：可变参数在调用点已求值，级别过滤只省格式化，不省参数计算
      ▼
(1) 级别过滤       gLogLevel < 2 → return                   门在 dbgLogInfo 入口
      │
      ▼
(2) 正文格式化     char line[2048]; std::vsnprintf(line, …)  超长被截断，vsnprintf 保证 NUL
      │
      ▼
(3) 前缀           n = formatLogPrefix(full, 2304, "INFO", "dxr_mc")
      │            "[%02u%02u月%04u %02u:%02u:%02u.%03u] [thread-%lu/%s] [%s/]: "
      │            → "[2910月2026 14:04:29.123] [thread-18244/INFO] [dxr_mc/]: "
      ▼
(4) 拼整行         char full[2304]; std::snprintf(full + n, sizeof(full) - n, "%s", line)
      │            n <= 0 或 n >= 2304 → 整行丢弃，绝不写半行
      ▼
      ├────────────► (5a) std::fprintf(stderr, "%s\n", full) + std::fflush(stderr)
      │                     └─► 启动器管道 / 控制台（落盘时进 logs/latest.log）
      │
      └────────────► (5b) mirrorLogToFile(full)
                            ├ 无路径（配置为空且无 DX12_NATIVE_LOG_FILE）→ return
                            ├ gMirrorFile == nullptr → std::fopen(path, "a")，失败则 return
                            └ std::fprintf(gMirrorFile, "%s\n", full) + std::fflush
```

### 1.3 各阶段的实现与上限

| 阶段 | 代码位置（`src\core\d3d12_device.cpp`） | 缓冲 / 上限 | 失败时的行为 |
| --- | --- | --- | --- |
| 级别过滤 | `dbgLog` / `dbgLogError` / `dbgLogWarn` / `dbgLogInfo` / `dbgLogProgress` / `dbgLogDebug` | `gLogLevel` 单变量 | 静默 `return` |
| 正文格式化 | 同上，每个函数内 `char line[2048]` | 2048 字节 | 截断，不报错 |
| 前缀 | `formatLogPrefix()`（`:518`），`out` 由调用者给 | 传给它的 `outSize` | 返回 `n <= 0` 由调用者丢弃 |
| 拼行 | `emitLogLine()`（`:530`），`char full[2304]` | 2304 字节 | 前缀已占满则整行丢弃 |
| 写 stderr | `emitLogLine()` | — | 无返回值可查 |
| 镜像 | `mirrorLogToFile()`（`:486`） | 无大小上限 | `fopen` 失败则这一行只进 stderr |

前缀固定部分约 55–60 字节（全角「月」占 3 字节 UTF-8），所以 2304 的整行上限在正常 `tid` 下不会触发；
真正会先撞墙的是 2048 的正文缓冲（见 §8）。

## 2. 五个通道与级别门槛

### 2.1 `gLogLevel` 的语义

`gLogLevel` 是**全局单变量**门槛，整数 `0..3`，越大越吵；它没有 per-module / per-tag 过滤，
也没有"只开某一个子系统的日志"这种粒度。默认值 `0`：

```cpp
static int gLogLevel = 0;                    // d3d12_device.cpp:435
void setLogLevel(int level) {                // :599
    gLogLevel = (level < 0) ? 0 : (level > 3) ? 3 : level;
}
```

`setLogLevel()` 自己钳到 `0..3`，所以模块内**不存在**越界级别；越过 ABI 的脏值在进 native 的那一刻就被夹住。
`mcdr_device.cppm` 里与之对应的枚举是 `enum class DbgLogLevel : int { ERR = 0, WARN = 1, INFO = 2, DEBUG = 3 };`。

### 2.2 门槛表（级别 0..3 各自放行什么）

| `gLogLevel` | 名称 | `dbgLogError` | `dbgLogWarn` / `dbgLog` | `dbgLogInfo` | `dbgLogProgress` | `dbgLogDebug` |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | ERROR | 放行 | — | — | 仅当 `gShutdownTraceAlways`（默认真） | — |
| 1 | WARN | 放行 | 放行 | — | 仅当 `gShutdownTraceAlways` | — |
| 2 | INFO | 放行 | 放行 | 放行 | 放行 | — |
| 3 | DEBUG | 放行 | 放行 | 放行 | 放行 | 放行 |

表中的 `—` 表示该级别下被门挡掉（静默丢弃，不占文件）。判据原文，逐字：

| 函数 | 判据 |
| --- | --- |
| `dbgLogError`（`:549`） | `if (gLogLevel < 0) return;` —— 钳制后恒假，等价于无条件输出 |
| `dbgLog`（`:540`） | `if (gLogLevel < 1) return;` |
| `dbgLogWarn`（`:558`） | `if (gLogLevel < 1) return;` |
| `dbgLogInfo`（`:567`） | `if (gLogLevel < 2) return;` |
| `dbgLogProgress`（`:581`） | `if (!gShutdownTraceAlways && gLogLevel < 2) return;` |
| `dbgLogDebug`（`:590`） | `if (gLogLevel < 3) return;` |

`dbgLog` 与 `dbgLogWarn` 的门槛和标签（`WARN`）完全相同，`dbgLog` 是保留的旧名，两者可互换。
`dbgLogError` 里的 `< 0` 判断是死代码，但不要删：它记录了"ERROR 不被级别关掉"这个约定。

### 2.3 `setShutdownTraceAlways` 影响什么

它**只影响 `dbgLogProgress`**，默认 `true`：

```cpp
static bool gShutdownTraceAlways = true;                              // :477
void setShutdownTraceAlways(bool traceAlways) { gShutdownTraceAlways = traceAlways; }   // :601
```

| 取值 | 效果 | 用途 |
| --- | --- | --- |
| `true`（默认） | 阶段标记无视 `gLogLevel`，永远输出 | 玩家把级别压到 ERROR 时，仍能定位"关游戏卡在哪一步" |
| `false` | 阶段标记退化成 INFO，要 `gLogLevel >= 2` 才输出 | 想把日志压到最小、且不需要 shutdown 追踪时 |

阶段标记是"关闭路径的进度条"，全部是 `dbgLogProgress`，标签 `INFO`：

- `destroyDevice()`（`d3d12_device.cpp:2594-2620`）：`destroyDevice stage: enter` /
  `queue fence wait done` / `pending deletes flushed` / `dlss shutdown done` / `done`。
- `destroySurface()`（`d3d12_surface.cpp:1660-1731`）：`destroySurface stage: enter` /
  `waited render thread` / `device idle` / `parent painted` / `fence wait done` /
  `pending deletes flushed` / `deleting surface` / `surface deleted`
  （`deleting surface` 与 `surface deleted` 在两条分支里各出现一次）。

### 2.4 `DBG_LOG_DEBUG`：编译期第二道门

`include\_d3d12_module_prelude.h:58` 定义了一个只在编译时打开的宏：

```cpp
#ifndef DBG_LOG_DEBUG
#ifdef DX12_DEBUG_VERBOSE
#define DBG_LOG_DEBUG(...) ::d3d12_mc::dbgLogDebug(__VA_ARGS__)
#else
#define DBG_LOG_DEBUG(...) ((void)0)
#endif
#endif
```

| `DX12_DEBUG_VERBOSE` | 展开为 | 默认构建下的开销 |
| --- | --- | --- |
| 未定义（默认） | `((void)0)` | 零：参数不求值、整段代码被优化掉 |
| 定义 | `::d3d12_mc::dbgLogDebug(__VA_ARGS__)` | 仍受运行期 `gLogLevel >= 3` 约束 |

所以它是**双门**：编译期打开宏、并且运行期 `gLogLevel == 3`，才会真出字。宏体必须带全限定名
`::d3d12_mc::dbgLogDebug`，否则在 `DX12_DEBUG_VERBOSE` 下报 C3861（该 TU 在 namespace 之外使用宏）。
这个宏的唯一使用者是 `src\c_abi\abi_command.cpp`；另有整段诊断直接用 `#ifdef DX12_DEBUG_VERBOSE` 包住
（`d3d12_device.cpp:4583`）。普通诊断用 `dbgLogDebug` 即可，不要为它套宏。

## 3. 前缀格式规范

### 3.1 逐段说明

`formatLogPrefix(char* out, size_t outSize, const char* level, const char* logger)`（`:518`）生成的格式串是：

```
[%02u%02u月%04u %02u:%02u:%02u.%03u] [thread-%lu/%s] [%s/]: 
```

| 片段 | 格式串写法 | 取值来源 | 说明 |
| --- | --- | --- | --- |
| 时间 | `[%02u%02u月%04u %02u:%02u:%02u.%03u]` | `GetLocalTime()` 的 `wDay` / `wMonth` / `wYear` / `wHour` / `wMinute` / `wSecond` / `wMilliseconds` | 日-月-年倒序 + 全角「月」，毫秒定宽 3 位；不是 ISO 8601，改格式会让新旧日志无法按字面排序 |
| 线程 | `[thread-%lu/%s]` | `GetCurrentThreadId()`，`%s` 是级别 | 十进制 `tid`；native 拿不到 JVM 线程名，用 `thread-` 显式区分 |
| 级别 | 同一方括号里的 `%s` | 调它的 `dbgLog*` 传的固定串：`ERROR` / `WARN` / `INFO` / `DEBUG` | 不是调用者传的；正文里不要再写级别 |
| logger | `[%s/]: ` | `emitLogLine()` 固定传 `"dxr_mc"` | 形状对齐 log4j 的 `[logger/]: `，便于把两条出口排在同一时间轴上读 |
| 结尾 | `]: `（冒号 + 空格） | — | 正文紧跟其后，中间不再加分隔符 |

一行真实形状（`tid` 与时间随机器变化）：

```
[2910月2026 14:04:29.123] [thread-18244/INFO] [dxr_mc/]: device ready: FL=12_1 tier=2
```

读法：`2026-10-29 14:04:29.123`、线程 `18244`、INFO、logger 名 `dxr_mc`，正文从 `device ready:` 开始。

### 3.2 为什么前缀只在一处拼

`formatLogPrefix()` 是**唯一**产出这套形状的函数，理由有三条：

1. 两路出口（log4j 与 native）写进同一个文件时，只有形状严格一致才能按时间戳合并、按 `LEVEL` 过滤；
   任何一处自己拼标签，`grep '[dxr_mc/]'` 就会漏行。
2. 时间戳必须来自一个函数，否则不同调用点之间的毫秒对不齐（`GetLocalTime` 每次都是新采样）。
3. 崩溃旁路要在堆可能已坏的情况下出字，它直接调 `formatLogPrefix()` 拿前缀，而不是复用日志器。

它有四个调用点，`logger` 参数各不相同：

| 调用点 | `level` | `logger` | 覆盖的日志 | 进镜像文件 |
| --- | --- | --- | --- | --- |
| `emitLogLine()`（`d3d12_device.cpp:486`） | 由 `dbgLog*` 传 | `"dxr_mc"` | 全部 `dbgLog*` | 是 |
| `failVoid()`（`c_abi\abi_common.cpp:52`） | `"ERROR"` | `"mcdr"` | ABI 参数校验 / 状态失败 | 否 |
| `logFail()`（`c_abi\abi_async.cpp:28`） | `"ERROR"` | `"dx12"` | 异步设施的结构性失败 | 否 |
| `logAlignmentFail()`（`c_abi\abi_async.cpp:43`） | `"ERROR"` | `"dx12"` | CBV 对齐失败 | 否 |

三种 logger 名并存是当下的缺陷（§8 第 1 条）；新增日志一律走 `dbgLog*`，不要自己 `fprintf`。

### 3.3 截断与丢弃规则

- 正文先被 `std::vsnprintf` 截到 `line[2048]`，`vsnprintf` 保证 NUL 结尾，不会越界。
- `emitLogLine()` 用 `n = formatLogPrefix(...)` 的返回值做偏移；`n <= 0` 或 `n >= sizeof(full)` 时**整行丢弃**，
  理由是"半行前缀 + 正文"比没有更误导。
- 前缀约 60 字节 + 正文 2048 字节仍小于 2304，正常路径不会触发整行丢弃。

## 4. 级别如何从 JVM 传下来

### 4.1 ABI 入口

| 导出（`include\mcdr_abi.h`） | 入参 | 语义 | 返回 |
| --- | --- | --- | --- |
| `mcdrSetLogLevel`（:293） | `int32_t level` | 直通 `setLogLevel(level)`，由被调方钳到 `0..3` | — |
| `mcdrSetLogVerbose`（:290） | `int32_t enabled` | 非 0 → `setLogLevel(3)`；为 0 → `setLogLevel(1)` | — |
| `mcdrSetShutdownTraceAlways`（:298） | `int32_t traceAlways` | `setShutdownTraceAlways(traceAlways != 0)` | — |
| `mcdrSetLogFile`（:305） | `const char* utf8Path` | 记路径并关掉旧句柄；`nullptr` / 空串 = 关闭镜像 | `1` = 已配置，`0` = 已关闭 |

实现全在 `src\c_abi\abi_meta.cpp`（`mcdrSetLogVerbose` :276、`mcdrSetLogLevel` :283、
`mcdrSetShutdownTraceAlways` :289、`mcdrSetLogFile` :296），转发到 `mcdr.d3d12.device` 的 `setLogLevel()` /
`setShutdownTraceAlways()` / `setLogFile()`。返回值列填 `—` 的几项只表示"调用方不需要返回值"，
确切签名以 `include\mcdr_abi.h` 为准。

### 4.2 `mcdrSetLogLevel` 与 `mcdrSetLogVerbose` 的区别

| 维度 | `mcdrSetLogLevel(0..3)` | `mcdrSetLogVerbose(0/1)` |
| --- | --- | --- |
| 表达力 | 四档全覆盖 | 只有两档：`3` 与 `1` |
| 关掉 verbose 后的级别 | 显式给，可以是 `0` | 恒为 `1`（WARN），**不是**默认的 `0` |
| 用途 | 新代码、需要精确档位时 | 早期只有"开/关 verbose"的调用方，保留兼容 |

`mcdrSetLogVerbose(0)` 与"回到默认"不等价：默认 `gLogLevel = 0` 只出 ERROR，而它落到 `1` 会继续出 WARN。
要用最小日志，显式调 `mcdrSetLogLevel(0)`。

### 4.3 推送时机

JVM 在**创建设备之前**调这两个函数（`src\c_abi\abi_device.cpp` 里 `mcdr_dx12CreateDevice` 的注释写明
"JVM 先调 `mcdrSetLogVerbose()`"）。native 侧不再读任何"日志级别"环境变量，级别的唯一来源是 ABI 调用。
由此得到两条时序事实：

- `gLogLevel` 的默认 `0` 只覆盖"JVM 调 `mcdrSetLogLevel` 之前"那一段，通常是 DLL 加载到设备创建之间的空窗，
  期间只有 ERROR 出字。
- 级别是**运行期可变**的：再调一次立刻生效，镜像路径同理（见 4.4），不需要重启进程；
  但已经写出去的行不会重打。

### 4.4 镜像文件开关

`setLogFile(const char* utf8Path)`（`:501`）只做两件事：记下路径、关掉旧句柄。

| 行为 | 实现 | 后果 |
| --- | --- | --- |
| 记路径 | `gConfiguredLogPath = utf8Path`（空串 = 关） | `mcdrSetLogFile` 返回 `1` 只代表"路径已记" |
| 关旧句柄 | `gMirrorFile` 非空则 `fclose` + 置空 | 换路径后旧文件立刻封口 |
| 真正 `fopen` | 推到下一次写日志时，`mirrorLogToFile()` 里 `fopen(path, "a")` | 路径不可写时**当次不报错**，靠后续日志重试 |
| 打开模式 | `"a"`（追加）+ 每行 `fflush` | 跨启动累积、不覆盖；崩溃后已写的行不会丢在用户态缓冲里 |

路径优先级：`gConfiguredLogPath` 非空就用它；否则退回已弃用的 `getenv("DX12_NATIVE_LOG_FILE")`。
环境变量只是兼容层，新功能不得再依赖它；相对路径按**进程工作目录**解析（代码中未明确做了别的归一化）。

### 4.5 `mcdr_dx12SetEnvironment`：把 `getenv` 开关变成配置项

`mcdr_dx12SetEnvironment(const char* name, const char* value)`（`mcdr_abi.h:224`）让 JVM 在设备创建前把
`MCDR_DLSS_IMPL` 一类原来只能靠环境变量传的值送进来，native 侧据此写入自己的环境。
它存在的目的是让这些开关出现在配置界面里；**不要**在 native 新增 `getenv()` 读点，也不要为新功能发明环境变量。
设备创建前就要用的布尔开关**不要**走这里：`_putenv_s` 与 native 的 `getenv()` 不保证可见，应按
`[debug] debugLayer` 的做法新增专用 ABI（`mcdr_dx12SetDebugLayer`，见 §5.1）。

## 5. D3D12 调试层

### 5.1 何时启用

调试层不是编译期开关，在 `ensureDevice()` 内、选适配器之前判断一次。入口有两个：

```cpp
bool debugRequested = gDebugLayerRequested;                  // mcdr_dx12SetDebugLayer（配置项）
const char* dbgLayerEnv = std::getenv("DX12_DEBUG_LAYER");   // 已弃用的逃生口
```

配置项 `[debug] debugLayer` 走 `mcdr_dx12SetDebugLayer(int32_t)`（`mcdr_abi.h:229`），Kotlin 在
`D3D12Settings.pushToNative()` 里于设备创建前调用。**不要**再用 `mcdr_dx12SetEnvironment` 传它：JVM 侧的
`_putenv_s` 不保证对本模块的 `getenv()` 可见，配置曾因此静默失效（表现为日志里连调试层的告警都没有）。
环境变量只是兼容层，与默认值相同时视为未设置。

| 条件 | 行为 | 日志 |
| --- | --- | --- |
| 配置项为 `false` 且 `DX12_DEBUG_LAYER` 不以 `1` 开头 | 不开调试层，静默 | 无 |
| 请求了，且 `D3D12GetDebugInterface` 成功 | 显式载入 `MCDR.dll` 同目录的 `d3d12SDKLayers.dll` 后 `EnableDebugLayer()`；Agility 是否生效都走同一条路 | `dx12 debug layer: enabled via <path>` |
| 请求了，同目录没有 `d3d12SDKLayers.dll` | 不开调试层，继续跑 | `dx12 debug layer: <path> not found - validation stays off` |
| 请求了，文件在但载入失败 | 不开调试层，继续跑 | `dx12 debug layer: cannot load <path> (err=<n>)` |
| 请求了，拿不到 `ID3D12Debug` | 不开调试层，继续跑 | `dx12 debug layer: D3D12GetDebugInterface failed (agility applied=<0/1>)` |
| 开了调试层后建设备失败 | 关掉调试层、重选一次 | 见 §5.4 的失败路径 |

**必须显式 `LoadLibraryW` 同目录那份**：`D3D12GetDebugInterface` 按默认搜索路径找 layers DLL，而 Agility 的
`D3D12Core.dll` 在 `run\dxrmc\`、进程工作目录是 `Illusion\run`，找不到时它只返回失败，表现就是"配置打开了但
调试层不生效"。

**Agility 生效时也能开**：把 `MCDR\vcpkg_installed\x64-windows\debug\bin\d3d12SDKLayers.dll`（与
`D3D12Core.dll` 同版本，当前 `1.619.3.0`）拷到 `MCDR.dll` / `D3D12Core.dll` 所在目录（发行布局是
`<user.dir>\dxrmc\`）。版本不符时创建会失败，这正是上表最后一行兜底的用途。

不开 GPU-based validation：代码里没有 `SetEnableGPUBasedValidation` 一类调用，验证只来自 `EnableDebugLayer()`
的默认项。

### 5.2 消息回调

`ensureDevice()` 在拿到设备后取 `ID3D12InfoQueue1` 并注册回调（`:2704-2721`）：

```cpp
infoQueue1->RegisterMessageCallback(&dx12DebugMessageCallback,
                                    D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                                    nullptr, &gInfoQueueCookie);
```

| 结果 | 日志（逐字） |
| --- | --- |
| 成功 | `infoQueue: message callback registered cookie=%lu` |
| 失败 | `infoQueue: RegisterMessageCallback failed` |
| 拿不到 `ID3D12InfoQueue1` | 不注册，退回"只在失败点倒队列"（§5.3） |

`dx12DebugMessageCallback()`（`:604-620`）按 severity 分派，`kSev[] = {"CORRUPTION","ERROR","WARNING","INFO","MESSAGE"}`：

| `D3D12_MESSAGE_SEVERITY` | 数值 | 通道 | 行格式 |
| --- | --- | --- | --- |
| `CORRUPTION` | 0 | `dbgLogError` | `D3D12 %s #%d: %s`（severity 名、message id、描述） |
| `ERROR` | 1 | `dbgLogError` | 同上 |
| `WARNING` | 2 | `dbgLogWarn` | 同上 |
| `INFO` | 3 | `dbgLogInfo` | 同上 |
| `MESSAGE` | 4 | `dbgLogInfo` | 同上 |

CORRUPTION 与 ERROR 同级，INFO 与 MESSAGE 同级；降噪靠级别门槛（`gLogLevel < 2` 时 INFO/MESSAGE 不出字），
不是靠过滤规则。回调是**实时**的：每条消息产生时就打一行。

### 5.3 批量倒队列

`dumpInfoQueueMessages()`（`:338`）在失败点把队列里最近的 16 条倒出来，然后 `ClearStoredMessages()`（`:369`）：

| 条件 | 行 |
| --- | --- |
| `severity <= D3D12_MESSAGE_SEVERITY_ERROR` | `dbgLogError("InfoQueue[%s] %s", sev, desc)` |
| `== D3D12_MESSAGE_SEVERITY_WARNING` | `dbgLogWarn("InfoQueue[%s] %s", …)` |
| 其它 | `dbgLogInfo("InfoQueue[%s] %s", …)` |

调用点：

| 位置 | 触发条件 |
| --- | --- |
| `beginCommandListWithWait()`（`:3936`） | 每帧一次，但只在 `gLogLevel >= 3` 时 |
| 命令列表 `Close()` 失败（`:4083`） | 失败即倒 |
| `createComputePipeline()` 失败（`:5326`） | 失败即倒 |
| `createComputePipelineShadingRate()` 失败（`:5488`） | 失败即倒 |
| `smaaCreatePso()` 失败（`:8929`） | 失败即倒 |
| 管线创建失败的另一段（`:6173-6194`） | 倒最近 **64** 条，走 `dbgLogDebug`，仅 `gLogLevel >= 3` |

管线创建失败时的 64 条是"PSO 被拒"的唯一现场：`D3D12 validation messages:` 起头，逐条
`  [i] severity: description`。

### 5.4 错误怎么上报

调试层消息**从不抛异常**，只落日志。错误往上的通道是返回值 + 错误串：

| 通道 | 内容 |
| --- | --- |
| 日志 | 上面的 `dbgLogError` / `InfoQueue[...]` |
| `mcdrLastError()` | 失败路径用 `deviceStatusText()` 拼出的错误串 |
| HRESULT | 返回给 C ABI 调用方 |

`deviceStatusText()`（`:315`）拼的是 `GetDeviceRemovedReason=… | msg[i](desc)`，保留最近 24 条设备消息；
`logDeviceStatus(tag)`（`:376`）把它打成一行 `dbgLogError("%s: device status: %s", tag, s)`。
present 失败是唯一把两者一起用的地方（`d3d12_surface.cpp`）：

```cpp
dbgLogError("presentSurface: FAILED %s", hrText);        // :1972  总是打
// DXGI_ERROR_DEVICE_REMOVED / DXGI_ERROR_DEVICE_RESET 时再补：
logDeviceStatus("presentSurface: FAILED");               // :1977
```

### 5.5 队列什么时候清空

| 情况 | 是否清空 |
| --- | --- |
| `dumpInfoQueueMessages()` 走完 | 清（`ClearStoredMessages()`） |
| 回调已注册、且没有失败、`gLogLevel < 3` | **不清**，消息一直累积 |

回调注册之后，同一条消息既被实时打成日志、又留在队列里，下一次 dump 会把它再打一遍。

### 5.6 一份绘制只能用一个描述符堆

`SetGraphicsRootDescriptorTable` / `SetComputeRootDescriptorTable` 取的是**堆内偏移**：句柄来自哪个堆就用哪个堆解释。
同一次绑定期间只有 `SetDescriptorHeaps` 指定的那个堆有效，句柄来自别的堆时调试层报
`#708 ... descriptor heap ... containing handle is not the currently bound heap`，零售驱动下着色器读到的是错资源。

- 图形绘制的常量/纹理/缓冲表一律来自 `gCtx.drawHeap`（`pushDescriptors` 用 `ctx->drawHeapSlotBase + ctx->nextDrawSlot` 取槽）。
- SMAA 两张官方 LUT（AreaTex / SearchTex）的 SRV 写在 `drawHeap` **尾部两个保留槽**
  （`kSmaaLutSlots = 2`、`kDrawHeapSmaaBase = kDrawHeapPerFrame * kDrawHeapSections`），由 `smaaEnsureLuts` 写一次、
  `smaaBindLuts` 每帧挂表——它们和同一趟绘制的 edges 表同堆，因此不需要切堆。早先它们独占一个 2 槽小堆，
  绑定它就必然与 edges 表抢堆，既刷 `#708`，又让权重读到偏移处的垃圾描述符。
- compute / 光线追踪派发结束要 `restoreDrawHeap(ctx)`，否则后续图形绘制的堆状态是 compute 留下的那个。

调试层开启后（`DX12_DEBUG_LAYER=1` + 同版本 `d3d12SDKLayers.dll`）目前仍会看到的其余消息：

| 消息 | 含义 | 状态 |
| --- | --- | --- |
| `#820` | 颜色目标的 `ClearRenderTargetView` 清除值与创建时不同（同一物理纹理会按 descriptor 用不同 clearColor 复用） | 只影响速度；已按消息 ID 屏蔽（见下） |
| `#821` | 深度目标的清除值与创建时不同 | 已消除：RTA 纹理创建时带 `pOptimizedClearValue`，深度按 reverse-Z 清 0 |
| `#527` | 少数过渡屏障的 before 状态与实际不符（`COMMON` vs `NON_PIXEL_SHADER_RESOURCE`） | 只在有光追的世界会话出现，一次会话 6 条、成对紧贴 AS 构建（`as: accel struct view slot=…`）；`resourceState` 表以 `initialStateFor(heap)` 为锚点，而 `createAccelStructBuffer` 以 `RAYTRACING_ACCELERATION_STRUCTURE` 作初始状态，两者不一致是首要嫌疑。光追路径重做时一并处理 |
| `#1328` | 缓冲用 `GENERIC_READ` 作初始状态被忽略（缓冲实际从 `COMMON` 开始） | 无害，可改常量消音 |
| `#680` / `#679` | PSO 声明需要 DSV / RTV 槽而绘制时没绑，读写被丢弃 | 引擎既有路径，确认意图后消音 |

`ensureDevice()` 里给 InfoQueue 加了一条 DenyList，只屏蔽 `#820`、`#821` 这两个消息 ID：它们一次会话能刷数万行，
且 `#820` 在本架构里无法根治（清除值由上层 `RenderTargetDescriptor` 决定，同一物理纹理会被不同 clearColor 复用）。
`#821` 已通过给 RTA 纹理带 `pOptimizedClearValue`（深度清 0）真正消除，保留屏蔽只是为了绕过复用带来的残余。
要排查清除慢路径时，把 `d3d12_device.cpp` 里那段 DenyList 删掉即可。

## 6. 崩溃取证

### 6.1 装了什么

`ensureDevice()` 早段调 `installCrashHandlerOnce()`（`:754`，只装一次），内部只有一句
`SetUnhandledExceptionFilter(&dx12CrashFilter)`（`:764`）。没有的东西要明确记下来，免得有人以为有：

| 机制 | 是否存在 |
| --- | --- |
| 未处理 SEH 异常过滤器 | 有（`SetUnhandledExceptionFilter`） |
| `MiniDumpWriteDump` / `.dmp` 文件 | 没有 |
| `std::set_terminate` | 没有 |
| `_set_se_translator` | 没有 |
| Vectored Exception Handler | 没有 |

因此它只覆盖"没人接住的 SEH 异常"：`std::terminate`（未捕获 C++ 异常）与 fail-fast（`0xC0000409`）
都会绕过用户态过滤器。

### 6.2 FATAL 行

崩溃时全部走裸 `std::fprintf(stderr, …)`，行首固定 `FATAL `，末尾 `fflush(stderr)`，
返回 `EXCEPTION_EXECUTE_HANDLER` 让进程按原样死掉。逐字：

| # | 行（格式串） | 说明 |
| --- | --- | --- |
| 1 | `FATAL native exception code=0x%08X addr=%p tid=%lu module=%s` | 异常码、出错地址、线程 id、地址所属模块的完整路径 |
| 2 | `FATAL context: pendingDeletes=%lld openList=%d flushInProgress=%ld deletingObj=%p deletingIndex=%ld deletingTotal=%ld` | 崩溃瞬间的设备状态：待删队列长度、打开的命令列表数、是否在 flush、正在删的对象 |
| 3 | `FATAL backtrace: %u frames` | 后面跟 N 行帧 |
| 3.1 | `  [%02u] %s` | 本模块且能符号化：`文件:行 函数+0x偏移`（`file` 只留文件名） |
| 3.2 | `  [%02u] %s+0x%llx` | 只能给模块：`模块文件名+0xRVA` |
| 条件 A | `FATAL deletingObj: kind=%d descSlot=%d resource=%p sourceTexture=%p mappedPtr=%p size=%lld` | 仅当崩溃时正有对象在删 |
| 条件 B | `FATAL deviceRemovedReason=0x%08X` | 仅当 `gCtx.device` 非空（设备已被移除时值说明原因） |

第 2 行是这套取证里最有用的一行：它把"TDR / 设备移除"和"我们自己删错了对象"分开——
`deletingObj` 非空说明崩在删除路径，`flushInProgress` 非零说明崩在等待 GPU。

### 6.3 为什么绕过日志器

崩溃线程可能正持有 `gMirrorMutex`，或堆已经写坏。此时 `dbgLogError()` 会死锁或二次崩溃，
所以过滤器**不碰** `emitLogLine()` / `mirrorLogToFile()`，只调 `formatLogPrefix()`（纯栈上 `snprintf`）
然后 `fprintf(stderr)`。这也意味着崩溃行**不进镜像文件**：用户只给了镜像文件时看不到 FATAL 行。

### 6.4 backtrace 与符号

| 步骤 | 实现 |
| --- | --- |
| 取栈 | `RtlCaptureStackBackTrace(0, 48, frames, nullptr)`，最多 48 帧（`:722-723`） |
| 定位模块 | 每帧 `GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS \| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, …)`，再 `GetModuleFileNameA` |
| 偏移 | `RVA = 帧地址 - 模块基址` |
| 符号预热 | `prepareCrashSymbols(HMODULE)`（`:633`）在**安装处理器时**做完：`SymSetOptions(SYMOPT_UNDNAME \| SYMOPT_DEFERRED_LOADS \| SYMOPT_LOAD_LINES)`、`SymInitialize(process, <DLL 目录>, FALSE)`、`SymLoadModuleEx`、一次 `SymFromAddr` 预热并把结果记进 `gCrashSymbolsReady` |
| 崩溃时 | `describeOwnFrame()`（`:661`）只查已加载的表，`SymFromAddr` + `SymGetLineFromAddr64`，不做文件 I/O |
| 搜索路径 | 固定为 MCDR.dll 所在目录，不读 `_NT_SYMBOL_PATH`（避免首次建设备时卡在符号服务器上） |
| 没有 PDB | 退化成 `模块文件名+0x偏移`，仍能用 `dumpbin /disasm` 定位 |

Release 构建会在 MCDR.dll 旁边生成 `MCDR.pdb`，于是 `MCDR.dll+0x5dc6f` 能还原成
`d3d12_fsffg.cpp FsrFgFrameGenProvider::beforePresent+0x15` 这种形状；PDB 缺省时只有偏移。
`dbghelp` 的引入是两行：`d3d12_device.cpp #include <dbghelp.h>`、`:38 #pragma comment(lib, "dbghelp.lib")`。
预热的目的是把"读 PDB"从崩溃现场挪到启动时——崩溃现场做磁盘 I/O 很容易再次崩。

### 6.5 dump 相关

没有内存 dump。唯一的"dump"是像素回读：`dbgDumpPixelsToFile()`（`:7092`）写
`dx12_dump_<tag>.bmp` 到**进程当前工作目录**，只在 `gLogLevel >= 3` 时被调用（`:7084`）；
同一次回读还会用 `dbgLogDebug`（`:7224`）打一张 ASCII 缩略图。它不是崩溃取证，是渲染问题的取证。

## 7. PIX 抓帧

### 7.1 两种捕获

| 维度 | GPU 捕获（PIX 界面） | timing 捕获（进程内 API） |
| --- | --- | --- |
| 触发 | 在 PIX 里点 capture；native 只负责把**捕获层**加载进进程 | `mcdrStartPixTimingCapture(path)` / `mcdrStopPixTimingCapture()` |
| 依赖 DLL | `WinPixGpuCapturer.dll` | `WinPixTimingCapturer.dll` + `WinPixEventRuntime.dll`（或 `WinPixEventRuntime_OneCore.dll`） |
| 产物 | 由 PIX 决定 | 调用方给的路径，PIX 自己后台写 `.wpix` |
| 前置条件 | 必须在**任何** `D3D12CreateDevice` 之前载入 | 必须在**非渲染线程**发起（见 7.4） |
| 提权 | 不需要 | ETW 会话需要提权 |

### 7.2 捕获层什么时候载入

`ensurePixCapturerLoaded()`（`:1085`）幂等，只尝试一次；三个兜底调用点：`ensureDevice()`（`:2581`）、
适配器枚举路径（`:7351`）、`setPixCapture(true)`（`:1138`）。搜索顺序：

1. `gPixDirectory`（`[debug] pixDirectory`）：它指向 DLL 本身就直接用，指向目录就试
   `目录\WinPixGpuCapturer.dll`。
2. MCDR.dll 同目录。
3. `%ProgramFiles%\Microsoft PIX\<版本>` 里**含 `WinPixGpuCapturer.dll` 的最高版本**目录
   （`pixInstallDirectory()`，`:1030`）。

载入用 `LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)`；前三步都失败时再裸名
`LoadLibraryW(L"WinPixGpuCapturer.dll")` 交给系统搜索路径。日志：

| 结果 | 行（逐字） |
| --- | --- |
| 成功 | `PIX GPU capture: capture layer loaded from %s` |
| 成功（提醒） | `PIX GPU capture: PIX has to be running to save a capture; expect a small per-frame overhead while the layer is loaded` |
| 全失败 | `PIX GPU capture: enabled but WinPixGpuCapturer.dll was not found — searched [debug] pixDirectory (%s), %s and %s; this run captures nothing` |
| 配置关掉 | `PIX GPU capture: disabled by configuration` |

### 7.3 `pixDirectory` 的来源与返回值

`setPixDirectory(const char* utf8Path)`（`:1388`）由 JVM 在设备创建前推进来，存成 `std::wstring`：

| 输入 | 行为 | 返回 |
| --- | --- | --- |
| `nullptr` / 空串 | 清空，走默认位置 | `0`，日志 `PIX: [debug] pixDirectory is empty; searching the default locations` |
| 合法 UTF-8，是文件 | 记住文件路径 | `1`，日志 `PIX: [debug] pixDirectory -> file %s` |
| 合法 UTF-8，是目录 | 记住目录 | `1`，日志 `PIX: [debug] pixDirectory -> directory %s` |
| 不是合法 UTF-8 | 忽略 | `-1`，日志 `PIX: [debug] pixDirectory(%s) is not valid UTF-8; ignoring it` |
| 不存在 | 清空，回落默认 | `-1`，日志 `PIX: [debug] pixDirectory does not exist; falling back to the default locations` |

### 7.4 timing 捕获

`startPixTimingCapture(const char* utf8Path)`（`:1416`）：

| 情况 | 行为 |
| --- | --- |
| 已在录 | 返回 `0`，`PIX timing capture: already recording; mcdrStartPixTimingCapture ignored` |
| 运行库没载成功 | 返回 `E_FAIL` |
| 路径空 / 非法 | 返回 `E_INVALIDARG`，`PIX timing capture: empty or invalid output path` |
| 成功 | `CreateThread(pixBeginThread)`，主线程最多等 `kPixBeginWaitMs = 10000` ms；返回 `1`，`PIX timing capture: started (HRESULT 0x%08lX), writing %s` |
| 超时 | 返回 `HRESULT_FROM_WIN32(ERROR_TIMEOUT)` |
| 会话已存在（`ERROR_ALREADY_EXISTS`） | 提示 `logman query -ets` / `logman stop <name> -ets` |
| 其它失败 | 长 ERROR，说明 ETW 会话需要**提权**运行（`Performance Log Users`，S-1-5-32-559 不够）；`gradlew` 会复用未提权的 daemon，需先 `--stop` |

录制范围就是 `start` 到 `stop` 之间：捕获层进进程后由 PIX 按 ETW 会话采样。三条硬约束：

1. **必须在自建线程上发起**。PIX 运行库在载入时初始化 COM，从渲染线程调恒返回
   `RPC_E_CHANGED_MODE (0x80010106)`；native 用 `CreateThread` 出来的线程，再把结果传回主线程。
2. **不能等写盘**。`stopPixTimingCapture()`（`:1479`）未在录时返回 `0`，在录则另起线程收尾并立刻返回 `1`，
   日志 `PIX timing capture: stopping, PIX is writing %s`——文件从几百 MB 起，等它会把主线程拖死。
3. **捕获层不可撤销**。`setPixCapture(false)` 只打一行 `PIX GPU capture: disabled by configuration` 并返回 `0`，
   已经载入的 DLL 留在进程里；下一次 `setPixCapture(true)` 直接复用。

`PixCaptureParameters` 是按 SDK 结构逐字段手写的（前 14 个字段 + 16 个 `u64` 占位），工程里不引入 PIX SDK
头文件，所以结构体版本变化时要手工对齐。

## 8. 已知缺陷

> **缺陷**：前缀的 logger 名有三套并存——`emitLogLine()` 给 `dxr_mc`、`failVoid()` 给 `mcdr`、
> `logFail()` / `logAlignmentFail()` 给 `dx12`。按 `[dxr_mc/]` grep 会漏掉后两类 ERROR 行；
> `mcdr_device.cppm` 的示例注释里还写着旧形状 `[2026-10-15 …] [dx12/]`。

> **缺陷**：`failVoid()` / `logFail()` 绕过 `emitLogLine()` 直接 `fprintf(stderr)`，
> 所以它们写的 ERROR 行**不进镜像文件**。只拿到镜像文件的用户看不到 ABI 校验失败。

> **缺陷**：镜像文件无大小上限、无轮转、无"已存在则改名"逻辑，长时间开 DEBUG 会把盘写满。
> `fopen` 失败时 `gMirrorFile` 保持 `nullptr`，之后**每条日志都重试一次 `fopen`**。

> **缺陷**：InfoQueue 只在 `dumpInfoQueueMessages()` 里 `ClearStoredMessages()`。回调已注册、
> 且 `gLogLevel < 3`、且没有失败点时，消息队列只增不减；同一条消息也会既实时打一行、又在下次 dump 里再打一行。

> **已修**：过去 Agility core 生效时调试层被直接放弃（`agility: DX12_DEBUG_LAYER ignored …`）。现在无论
> Agility 是否生效都会显式载入同目录的 `d3d12SDKLayers.dll` 再 `D3D12GetDebugInterface`，失败才退成一条
> WARN（见 §5.1）。

> **缺陷**：命名三套且语义不同——`DX12_DEBUG_VERBOSE` 是**编译期**宏、`DX12_LOG_VERBOSE` 是**已废弃**的运行期
> 环境变量、实际运行期入口是 `mcdrSetLogLevel()`。多处注释仍写"需 `DX12_LOG_VERBOSE=1` 才输出"，
> 照注释设置环境变量没有任何效果。

> **缺陷**：`DBG_LOG_DEBUG` 只在 `DX12_DEBUG_VERBOSE` 定义时才有实体代码，但 `mcdr_device.cppm` 的注释宣称
> "`DX12_LOG_VERBOSE=1` 或编译时宏开启"。默认构建里把 `gLogLevel` 调到 3，被 `DBG_LOG_DEBUG` 包住的诊断
> 依然是空的。

> **缺陷**：正文缓冲固定 `line[2048]`，HLSL 反汇编、PSO 描述这类长正文会被截断且**没有任何提示**
> （`vsnprintf` 静默截断）。`emitLogLine()` 只检查前缀，不检查正文是否被截。

> **缺陷**：`dbgLog` 与 `dbgLogWarn` 门槛、标签完全相同，是两个名字一个函数；读代码时容易误以为
> `dbgLog` 是"通用入口、级别由参数决定"。

> **缺陷**：`mcdrSetLogVerbose(0)` 把级别设成 `1`（WARN），而"默认"是 `0`（ERROR）。想彻底安静必须调
> `mcdrSetLogLevel(0)`；调 `mcdrSetLogVerbose(0)` 之后仍会有 WARN 行。

> **缺陷**：崩溃取证只写 `stderr`，不进镜像文件，也没有 minidump。进程被 WER 接管、或启动器没有接住
> 子进程 `stderr` 时，`FATAL` 行会完全丢失。`SetUnhandledExceptionFilter` 也接不住 `std::terminate`
> 与 fail-fast（`0xC0000409`）。

> **缺陷**：PIX GPU 捕获层一旦载入**不可撤销**，`setPixCapture(false)` 只能阻止后续主动加载，
> 已载入的层继续每帧收开销（日志里那句 `expect a small per-frame overhead`）。

## 文件导航

| 路径（相对 `MCDR\`） | 内容 |
| --- | --- |
| `src\core\d3d12_device.cpp` | 日志器（`gLogLevel` / `formatLogPrefix` / `emitLogLine` / `mirrorLogToFile` / `setLogLevel` / `setLogFile`）、`dumpInfoQueueMessages`、调试层与 InfoQueue 回调、`dx12CrashFilter` + `prepareCrashSymbols`、PIX 载入与 timing 捕获 |
| `src\core\d3d12_surface.cpp` | present 失败的两行错误（`:1972` / `:1977`）、`destroySurface` 的阶段标记 |
| `src\module\core\mcdr_device.cppm` | `DbgLogLevel` 枚举、`dbgLog*` / `setLogLevel` / `setLogFile` / PIX 声明、`DBG_LOG_DEBUG` 声明处 |
| `include\_d3d12_module_prelude.h` | `DBG_LOG_DEBUG` 的编译期开关（`DX12_DEBUG_VERBOSE`） |
| `include\mcdr_abi.h` | `mcdrSetLogLevel` / `mcdrSetLogVerbose` / `mcdrSetShutdownTraceAlways` / `mcdrSetLogFile` / `mcdr_dx12SetEnvironment` / PIX 三个入口 |
| `src\c_abi\abi_meta.cpp` | 上述日志与 PIX 开关的 C 入口实现 |
| `src\c_abi\abi_common.cpp` | `failVoid()`：`logger = "mcdr"` 的错误行，不进镜像 |
| `src\c_abi\abi_async.cpp` | `logFail()` / `logAlignmentFail()`：`logger = "dx12"` 的错误行，不进镜像 |
| `docs\misc\debug-and-logging.md` | 本文 |

## 文档维护

1. 改 `formatLogPrefix()` 的格式串、或改 `emitLogLine()` 的缓冲尺寸，同一次改动里更新 §1.2 的图与
   §3.1/§3.3 的表——前缀是玩家 grep 与合并两条出口的唯一依据，格式和文档对不上等于两份日志。
2. 新增或改动 `dbgLog*` 的门槛，必须同步 §2.2 的两张表，并给出判据原文；新增一个通道要同时说明它
   属不属于 `dbgLogProgress` 那种"无视级别"的例外。
3. 新增诊断开关一律走 ABI 导出或配置项，不要新增 `getenv()` 读点；新增导出同步 §4.1 的表与
   [`abi/README.md`](../abi/README.md) 的 §7。
4. 改动 FATAL 行的任何字面量，逐字更新 §6.2——崩溃行是用户反馈里唯一能整段贴出来的证据，
   错一个字就没法反查。
5. 修掉 §8 里任意一条就把它删掉；新增一条当下成立的限制，按 `> **缺陷**：…` 的格式补上，
   写清触发条件与能观测到的日志串。
