# MCDR 设计文档总览

本文是 `MCDR\docs\` 的入口。源码注释只保留"读代码时立刻要用到的约束"，
**用法、整条数据流、硬约束与已知缺陷**统一收在这里。

> 阅读顺序建议：先看下面的「文档地图」，再按你要改的子系统点进去。
> 每篇文档结尾都有「文件导航」表，用来在源码与文档之间来回跳。

## 这个项目是什么

MCDR 是 Minecraft 模组 DXRMC 的 **native 渲染库**：一个 C++23 动态库（`MCDR.dll`），
把 Direct3D 12 的设备、交换链、命令录制、异步资源与各种超分/插帧后端封装起来，
供 JVM 侧的模组 **Illusion** 调用。

- **只有一份 C ABI**：`include\mcdr_abi.h`。JVM 侧通过 **Java FFM API**（`java.lang.foreign`）
  直接 downcall 这些导出函数，**不使用 JNI**，也没有代码生成。
- **全量 C++23 模块**：`src\` 下没有自研 `.h`，接口单元是 `src\module\**\*.cppm`，
  实现单元在 `src\core\`、`src\advanced\`；唯一保留的头文件是那份 ABI 头。
  模块规则（哪些写法会 `LNK2001` / `C5244`）见 [`misc/build-and-modules.md`](misc/build-and-modules.md)。
- **构建**：MSVC **v145**（VS2026 / MSBuild）+ `/std:c++latest`，`WarningLevel=Level3` 且
  `TreatWarningAsError=true`，工程 `MCDR\MCDR.vcxproj`，配置 `Release|x64`。
- **能力范围**：设备与资源管理、交换链与 HDR、异步录制（描述符分区 / bundle / fence）、
  mesh shader 管线、SMAA 查找表、超分（DLSS / FSR / XeSS）、帧生成（DLSS-G / FSR-FG / XeSS-FG）、
  NVIDIA Reflex、PIX 抓帧、DXR 光追、native 几何快照及 G-Buffer/降噪/超分/合光帧资源。

## 文档地图

| 文档 | 主题 | 什么时候看 |
| --- | --- | --- |
| [`abi/README.md`](abi/README.md) | C ABI 契约：类型映射、句柄、错误与日志协议、版本与布局自检 | 新增或修改一个导出函数 |
| [`c_abi/README.md`](c_abi/README.md) | 适配层 11 个 TU 的职责、模块硬约束、编译单元顺序 | 排 `LNK2001` / `C5244`，加适配文件 |
| [`device/README.md`](device/README.md) | D3D12 设备、堆与描述符、命令列表、管线缓存、瞬态内存 | 改每帧提交流程、加资源类型 |
| [`surface/README.md`](surface/README.md) | 交换链、窗口宿主、HDR 与建链选项、像素回读 | 呈现异常、HDR 不对、resize |
| [`async/README.md`](async/README.md) | 异步录制：分区描述符分配、bundle 池、fence、执行器 | 多线程录制、fence 回收泄漏 |
| [`advanced/dlss.md`](advanced/dlss.md) | DLSS / DLSS-G（NGX 集成） | NGX 初始化失败、超分参数不对 |
| [`advanced/framegen.md`](advanced/framegen.md) | 帧生成三条后端：DLSS-G / FSR-FG / XeSS-FG | 插帧、hudless 捕获、相机常量 |
| [`advanced/upscalers.md`](advanced/upscalers.md) | 超分抽象层与 FSR / XeSS 后端 | 加超分后端、对不齐抖动 |
| [`advanced/mesh.md`](advanced/mesh.md) | mesh shader（meshlet）管线与 HLSL→DXIL | 地形管线、mesh PSO |
| [`advanced/rt-probe.md`](advanced/rt-probe.md) | 光线追踪能力探测与状态上报 | 光追开关、能力上报 |
| [`advanced/rt-frame.md`](advanced/rt-frame.md) | native 几何快照、场景发布、BLAS/TLAS、G-Buffer、追踪、曝光、降噪、独立 SR 与合光 | 候选续建、版本表、局部失效、几何/AS 共享、生产者 fence、SDK 历史、借用输出与退休 |
| [`advanced/nrd.md`](advanced/nrd.md) | NRD diffuse/specular 双通道与 SDK validation | 资源映射、诊断输出、在途槽位验证 |
| [`misc/build-and-modules.md`](misc/build-and-modules.md) | 构建配置、C++23 模块规则、依赖与 SMAA 查找表 | 模块编译报错、加模块、换依赖 |
| [`misc/debug-and-logging.md`](misc/debug-and-logging.md) | 日志通道与级别、FATAL 旁路、PIX 抓帧、诊断层 | 看日志、抓帧、排查崩溃 |

## 文档写作约定

- **语言**：简体中文；技术名词保留英文原文（ABI / FFM / PSO / SRV / UAV / DXIL / fence /
  bundle / swap chain …），不硬译。
- **面向人类读者**：先说"它是什么、怎么用"，再给架构与数据流，最后才给文件与函数级细节。
- **可查证**：结论必须来自代码。写具体数值、枚举名、宏名、函数名，而不是"某个阈值"。
  代码里没写清的地方标注「代码中未明确」，不要替它补脑。
- **只写当下**：不写设计史、方案对比与"当初为什么这样"；必须遵守的写法写成祈使句，
  已经发现的缺陷单独列成警告清单。
- **源码导航**：每篇结尾的表格用相对 `MCDR\` 的路径。
- **文档维护**：改了对应子系统的行为，同一次改动里更新文档；文档与代码冲突时以代码为准，
  并顺手把文档改对。
- **注释与文档的分工**：源码注释只写"违反就会出错"的约束（一两行），解释性内容放这里，
  注释里需要展开时给出指向本文档的路径。

## 相关文档

- `Illusion\docs\overview.md` —— JVM 侧（Kotlin）的设计文档；ABI 的另一端在那边。
- `MCDR\README.md` / `MCDR\README_ZH.MD` —— 项目简介与外部事实（徽章、致谢）。
- `MCDR\AGENTS.MD` —— 构建与模块布局速查、改 native 时的硬约束摘要。
