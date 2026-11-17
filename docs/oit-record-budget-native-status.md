# OIT 帧预算核对

此页保留 ABI19 的预算缺陷证据。当前 ABI20 已采用 144 B records / 8 B pixel，默认4K四个完整Frame加program为488.25 MiB；当前契约见 [OIT records](oit-records.md)，选择依据见 [4K存储方案](oit-4k-layout-proposal.md)。下文旧布局数字不适用于ABI20。

本轮保持公共 ABI、shader/CPU records 布局、records 容量、512 MiB 硬上限和等待策略不变。未启动游戏、未 GPU 采集、未截图，也未修改 `d3d12_oit_records.cpp`。

## 实际存活

BeginFrame 创建的 `Frame` 由当前 command-list ring 槽的 `recordingLeases` 持有。槽位只有在对应 fence 完成、allocator/list Reset 成功后才清空，因此三个槽直接持有当前录制帧和前两个 ring 槽帧。Resolve 读取 presented history 时，还把 history Frame 加入当前 recording 槽，避免 GPU 在提交完成前释放；最旧槽的历史读取引用可能额外保留第四个独立 Frame。pending/presented 只额外持有同一个 Frame，不复制 OIT buffers。

如果 Present 延迟、取消清理或 pending 票据长期未提交，pending map 会继续持有已经离开 ring 槽的 Frame。兼容 pending 会在新 frame 开始时保留，只有身份/配置不兼容才删除。实际存活需同时计算三个 recording 槽、它们读取的历史引用和延迟 pending，不能把三槽数量当成 Frame 总数上限；native 不等待未提交 fence，也不偷偷丢 pending 或旧透明颜色。

## 当前实际额度

默认 JVM 配置为 records=262144、perPixel=32、source=4096、primitive=65536、previous=65536。native 64 KiB 对齐后单个 Frame 预算如下：

| 分辨率 | 单 Frame | 2 Frame | 3 Frame |
| --- | ---: | ---: | ---: |
| 854×480 | 76.0625 MiB | 152.125 MiB | 228.1875 MiB |
| 1280×720 | 83.8125 MiB | 167.625 MiB | 251.4375 MiB |
| 1920×1080 | 101.4375 MiB | 202.875 MiB | 304.3125 MiB |
| 2560×1440 | 126 MiB | 252 MiB | 378 MiB |
| 3840×2160 | 196.3125 MiB | 392.625 MiB | 588.9375 MiB |

程序预算另占 4 MiB；draw 常量页按每 64 KiB 页面计费。4K 在程序预算后最多容纳两个完整 Frame，第三个 BeginFrame 会由共享 `reserve` 返回 `overBudget`。如果 pending 继续持有旧 Frame，拒绝会更早发生。4K 的根因是每个像素 16 B 的 pixel state、192 B record buffer、primitive/previous/source 元数据和对齐开销共同占用上限；单靠 draw 常量页已无法解决。

## JVM 失败路径

`OitRecordFrames.begin` 将 native `-3` 转为 `OitRecordFailure.OVER_BUDGET` 并抛出 `OitRecordException`。`OitReconstructionInputs.beginRecords` 在同一帧建立记录失败时不会发布 `recordFrames`，上层帧协调器的统一异常路径执行 `abortActiveFrame`，销毁/失效本帧 OIT 输入并结束当前世界帧。它不会继续使用半初始化的 OIT token，也不会把当前透明颜色替换成历史颜色。

## 后续可选方案

保留显式 admission failure 是当前 ABI19 的确定行为。要保证 4K 三个以上完整 Frame，需要重新设计跨文件的 pixel state 压缩、records 容量分档或 history 生命周期契约；这些都会改变 shader/CPU 布局或 pending 语义，不能在只改 `d3d12_oit_records.cpp` 的范围内安全完成。任何方案都必须继续让容量不足沿用现有 overflow/unknown 语义，并且禁止等待当前尚未提交的 fence。

Release x64 native build 已通过，0 warning / 0 error；产物为当前 ABI19 + ConstantsPage 版本。
