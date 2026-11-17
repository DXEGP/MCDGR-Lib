# MCDR 文档入口

| 文档 | 内容 |
| --- | --- |
| [organization.md](organization.md) | 当前 native 业务目录、状态所有权与模块协作 |
| [shader-bindings.md](shader-bindings.md) | naga 编译产物的资源绑定、R8_SINT 到 RAW SRV 与范围约束 |
| [terrain-mesh.md](terrain-mesh.md) | meshlet 静态驻留、16 B 描述符、动态实例与 16/32/64 quad 变体 |
| [oit-records.md](oit-records.md) | OIT RGB records v1 的 capture、GPU compact、状态与生命周期；尚未接入正常循环 |
| [oit-normalization.md](oit-normalization.md) | 原生四 lane tile 归一化、帧内共享资源与录制顺序；尚未接入完整 stage 执行器 |
| [README_ZH.MD](README_ZH.MD) | 项目说明、构建与基础依赖 |
| [rendering.md](rendering.md) | 新集中渲染器的业务边界、依赖/生命周期、能力/资产/信号与验证契约，尚未实现 |

设计与已实现行为分别标注；不把接口设计、SDK 可用性或外部样例当作当前生产能力。
