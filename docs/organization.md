# 当前 native 业务组织

当前 DLL 保留 C ABI 与 C++ 模块身份。实现按 backend、diagnostics、platform 和 render 业务域组织；当前结构没有实现新集中渲染器。

| 业务域 | 入口与状态 |
| --- | --- |
| backend/device | `d3d12_device.cpp` 持有 DeviceContext、资源退休、设备与命令协作 |
| backend/memory、backend/command | 描述符分配、async fence；executor/bundle 状态在镜像模块接口 |
| backend/shader | `d3d12_shader.cpp` 持有 DXC 单例与 DXIL 缓存逻辑 |
| diagnostics | `d3d12_diagnostics.cpp` 持有日志 sink、级别、崩溃符号状态；`d3d12_pix.cpp` 持有 PIX runtime/捕获状态 |
| platform | DllMain 与窗口宿主 |
| render/reconstruction | SR registry、DLSS/FSR/XeSS adapter |
| render/presentation | swapchain、HDR、framegen 与 provider |
| render/antialiasing | `d3d12_postprocess.cpp` 持有 blit/锐化/SMAA/TAA 实现与私有历史标志；`smaa_luts.cpp` 保留完整数据 |
| render/terrain、render/config | mesh 管线与 RT 选项 |

`src/module/` 镜像业务目录。设备模块中现有 GPU 资源仍由 DeviceContext 保存；本次组织不增加新的 ABI 或 SDK 类型公共契约。
`mcdr_device.cppm` 尾部非 export 协作声明供同模块实现单元使用，业务私有缓存不跨 TU 公开。
日志与崩溃/PIX 初始化顺序保持；崩溃处理调用设备的 pending-delete 诊断函数读取原有状态。

构建使用 Release/x64 与现有 SDK 版本。`MCDRDependencyRoot` 默认工程目录，允许显式复用同版本 external/lib/vcpkg 依赖；工程源码和对象仍来自当前工作树。

`MCDRDependencyDirectory` 将输入相对于工程目录规范化为绝对路径，并补齐末尾目录分隔符。`MCDRDependencyRoot` 可传不带末尾反斜杠的目录。
已有依赖的构建命令如下，在当前工程目录执行；路径应替换为实际工作区依赖根：

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' MCDR.vcxproj /p:Configuration=Release /p:Platform=x64 /p:MCDRDependencyRoot=D:/Projects/MinecraftMod/DXRMC/MCDR /p:VcpkgEnableManifest=false /p:VcpkgEnabled=false /v:minimal
```

该命令直接通过工程的 include/lib 路径消费已安装依赖，关闭 vcpkg 自动安装与集成。需要启用 vcpkg 集成时，可改传 `/p:VcpkgInstalledDir=<依赖根>/vcpkg_installed/ /p:VcpkgEnableManifest=false`，并保持所需 triplet 与已安装依赖一致；这条集成模式未作为本次成功构建的验证条件。

设备单元全局模块片段先包含 `initguid.h`，再包含 D3D12 前置头，为 Agility 的 `CLSID_D3D12SDKConfiguration` 提供 GUID 定义。其他模块单元不包含 `initguid.h`，只读取头文件声明，避免依赖 DXC 头文件的间接 GUID 定义。

## 当前重建输入与资源寿命

DLSS、FSR、XeSS 都消费 JVM 在世界 main pass 返回时复制的共享深度快照。NGX 不替换传入深度；
Streamline 在 tag 前将颜色、深度与 MV 转到 `NON_PIXEL_SHADER_RESOURCE`，返回后设备恢复调用方描述符 heap。
FSR/XeSS 的 FG 输入深度标记同样使用 `NON_PIXEL_SHADER_RESOURCE`，与实际资源状态一致。

`copyTextureToTexture` 对深度/MSAA 资源仅接受完整、同尺寸、同采样数的 subresource 拷贝，源 box 为 null。
普通颜色区域拷贝仍使用显式 box。深度源与快照保持同一 typeless 资源格式。

SR provider 所有权独立于 JVM 创建缓存。`srCreateProvider(ctx, NONE, ...)` 等待该 encoder 已提交的 fence
后释放，FSR/XeSS 切换与上下文重建遵守相同约束。等待超时保留旧资源并返回失败；不得在 GPU 仍可能使用
资源时强制销毁。无 context 的释放在 encoder 完成并销毁后调用，另检查队列 fence。

启动只做实际 SDK 初始化、设备挂接和能力查询。Streamline 不在设备初始化阶段提交虚构显示尺寸的 DLSS 参数，也不写独立 `sl-probe.txt`；真实 viewport/尺寸/曝光参数由重建 adapter 在实际调用时设置。测试替身、故障注入和行为探针放在独立验证程序，生产模块保留正常运行需要的错误和生命周期契约。
