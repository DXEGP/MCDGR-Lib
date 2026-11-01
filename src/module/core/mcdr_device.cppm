// dx12-mc 原生 D3D12 层：设备 / 资源 / 命令 / 管线 / surface 的模块接口单元。
// 设备与 surface 必须同模块（device 写 gActiveSurface、surface 调 deviceContext()/allocRtvHandle，
// 拆开即 import 环）；异步三件套独立成模块且本模块**不** import。见 docs\device\README.md。

module;

#include "_d3d12_module_prelude.h"

// mcdr::RtxSupport：purview 外的声明不传给 import 方，故这里自己 include（实现单元与普通 TU 共用）；放全局模块片段以避免 C5244。
#include "mcdr_rtx_support.h"

// mcdr_dlss_input 等 ABI 结构；include 在 `module;` 与 `export module` 之间才不违 C5244。
#include "mcdr_abi.h"

// DLSS 实现单元的 C 接口（mcdr_dlss_impl.h，全局模块片段）：裸写 extern C 声明会触发 C5202 而 /WX 失败，且普通 TU 与模块单元的 C++ 修饰名不同。
#include "mcdr_dlss_impl.h"

export module mcdr.d3d12.device;

// 模块内部失败统一用 mcdr::Result<T>；mcdr.error 是叶子模块（只依赖标准库），import 不形成环。
import mcdr.error;

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

// 日志分级：ERROR/WARNING 始终输出；INFO/DEBUG 默认关闭，需 DX12_LOG_VERBOSE=1。
enum class DbgLogLevel : int {
    ERR = 0,
    WARN = 1,
    INFO = 2,
    DEBUG = 3,
};

// 毫秒时间戳（QPC），供诊断插桩打印精确阻塞点。
double nowMs();

// 统一的 native 日志前缀，与 JVM 侧 log4j 输出同形，两路日志按时间戳对齐。返回字符数；<=0 或 >= outSize 表示放不下，调用方必须丢弃整行。
int formatLogPrefix(char* out, size_t outSize, const char* level, const char* logger);

// 错误/警告（始终输出）
void dbgLog(const char* fmt, ...);
// 错误级别（与 dbgLog 同门槛，仅用于表达"这是错误"的语义）
void dbgLogError(const char* fmt, ...);
// 警告级别（WARN 及以上输出）
void dbgLogWarn(const char* fmt, ...);
// 信息级别（默认关闭）
void dbgLogInfo(const char* fmt, ...);
// 进度标记：默认恒定输出（不受 setLogLevel 影响，可被 setShutdownTraceAlways(false) 关掉）；只给「关停必须留痕、又不能冒充错误」的标记用。
void dbgLogProgress(const char* fmt, ...);
// 调试级别（默认关闭）
void dbgLogDebug(const char* fmt, ...);
// 设置日志级别（0=ERR 1=WARN 2=INFO 3=DEBUG）
void setLogLevel(int level);
// 进度标记是否恒定输出（配置 `[logging] shutdownTrace`，默认 true）。
void setShutdownTraceAlways(bool always);
// 把每条 native 日志镜像到该文件（`[logging] nativeLogFile`）；空指针/空串 = 关闭并释放句柄。相对路径按进程工作目录，追加写。
void setLogFile(const char* utf8Path);

// PIX GPU 捕获层：必须在**首个 D3D12 设备创建之前**调用，幂等。返回 1=已载入 0=未请求 -1=请求了但没载入。
int setPixCapture(bool enabled);

// 调试层（`[debug] debugLayer`）：只在首个设备创建时读一次，创建后再调用无效。返回 1=已记录。
bool setDebugLayerRequested(bool enabled);

// PIX 的 dll 在哪儿（`[debug] pixDirectory`）：dll 全路径或目录，空串 = 回自动检测（MCDR.dll 同目录 → %ProgramFiles% 最新版）。先于 setPixCapture()。返回 1=已设置 0=已清空 -1=无效。
int setPixDirectory(const char* utf8Path);

// PIX 程序化 timing 捕获：让 PIX 录一段并写成 .wpix（界面上的持续捕获不 Save 就没有文件）。路径是
// UTF-8 全路径。start 返回 1=已开始 0=已在录 负值=HRESULT（E_ACCESSDENIED = 没提权）；stop 返回
// 1/0/负值且**不等写盘**（PIX 在另一条线程落盘）。
int startPixTimingCapture(const char* utf8Path);
int stopPixTimingCapture();

// VRS 玩家档位（= 地形粗粒度上限）。rate = 原始 D3D12_SHADING_RATE（1X1=0x0 1X2=0x1 2X2=0x5 4X4=0xA
// 等），-1 = 关闭（一个 RSSetShadingRate 都不调）。只存进 DeviceContext 并做能力预检，真正下发走
// setShadingRateNow()。返回 1=接受（含设备还没建） 0=设备无法满足该档位 -1=未知速率。
int setShadingRate(int rate);

// scope 型应用点（ABI：mcdr_dx12SetShadingRateNow）：rate 只接受 0x0..0xA，立刻对 ctx 当前打开的
// 命令列表下发，不读也不写 requestedShadingRate。返回 1=已下发 0=不满足/列表非 List5 -1=未知速率
// -2=ctx 为空或没有打开的列表。
int setShadingRateNow(CommandContext* ctx, int rate);

// 两个着色率组合器（D3D12_SHADING_RATE_COMBINER 值 0..4），默认 PASSTHROUGH —— 逐图元速率开始写前不能把第一个改成 MIN（默认 1X1 会压回 2X2）。返回 1/-1。
int setShadingRateCombiners(int first, int second);

// 抗锯齿模式（Java 侧 dxrMcAaMode，与 VRS 一样**不需要重启**）。取值同 D3D12AntiAliasing：0=OFF
// 1=SMAA（就地三趟） 2=TAA（时域累积，历史由本模块持有） 3=DLAA（DLSS 特性；JVM 映射成 OFF 再传，
// 这里收到 3 只是「无 native 后处理」）。DLSS 已拆成独立开关。返回 1=接受 0=未实现 -1=非法取值。
int setAntiAliasing(int mode);
// 模式名（日志用）：OFF / SMAA / TAA / DLAA / UNKNOWN(n)。
const char* antiAliasingName(int mode);

// DLSS 升采样的**独立开关**（Java 侧 dxrMcDlss，不再借用 dxrMcAaMode）。默认关，与抗锯齿无关、可同时开；幂等。返回 1=开 0=关。
int setDlssEnabled(int enabled);

// 深度快照入口：JVM 在「世界深度还有效、vanilla clearDepthTexture 之前」调用。评估点在世界画完、GUI
// 之前，那时深度已被清成 0.0 ⇒ 深度感知升采样会退化成纯色彩升采样。depth 状态恢复成调用前的。
// 返回 1=已建立/复用 0=参数不可用 -1=失败（已 WARN 一次）。
int snapshotDlssDepth(CommandContext* ctx, mcdr_object* depth);

// DLSS 挡位显式指定：0=Performance 1=Balanced 2=Quality 3=DLAA；越界返回 -1（不改现状）。档位变化要
// 触发特征重建（还要复位实现单元的一次性失败闸门）。DLAA 复用同一条管线但要求 render == target，
// 不满足时打 ERROR 并退回 Quality，绝不生成假 DLAA。幂等，设备未建也能调。
int setDlssQuality(int tier);

// DLSS 模型（NGX render preset）：0=Default、5=E / 6=F（已弃用 CNN）、10=J / 11=K / 12=L / 13=M（transformer）。预设是 CreateFeature 的输入 ⇒ 变化要触发特征重建。越界 -1。
int setDlssRenderPreset(int preset);

// 登记 JVM 送来的 UAV 输出纹理：解包 `Dx12Object` 后把裸 `ID3D12Resource*` 交给实现单元（绝不 reinterpret JVM 句柄）。tex=nullptr 清除。返回 1=接受 0=清除 -1=不是纹理。
int setDlssOutput(mcdr_object* tex);

// 高频调试宏：默认静默，DX12_LOG_VERBOSE=1 或 DX12_DEBUG_VERBOSE 打开；宏展开必须写全 `::d3d12_mc::`，否则命名空间外使用报 C3861。
#ifdef DX12_DEBUG_VERBOSE
#define DBG_LOG_DEBUG(...) ::d3d12_mc::dbgLogDebug(__VA_ARGS__)
#else
#define DBG_LOG_DEBUG(...) ((void)0)
#endif

// 前向声明：DeviceContext 持有 std::unique_ptr<BlitPipeline>，析构需完整类型。
struct BlitPipeline;
struct SharpenPipeline;

// ---- 设备上下文（进程内单例，Java 侧首次调用 dx12CreateDevice 时创建） ----
struct DeviceContext {
    ComPtr<ID3D12Device> device;
    // mesh shader 管线必须用 D3D12_PIPELINE_STATE_STREAM_DESC（只在 ID3D12Device2 上；旧 DESC 无法表达 AS/MS 阶段），首次建管线时 QI 一次并缓存。
    ComPtr<ID3D12Device2> device2;

    ComPtr<ID3D12DescriptorHeap> srvHeap;      // SRV（SHADER_VISIBLE）
    // SRV 的 CPU-only 镜像堆（非 SHADER_VISIBLE）：CopyDescriptorsSimple 的源必须是 CPU-only 堆；view 的 cpuHandle 指向这里，srvHeap 的 GPU 句柄供绑定。
    ComPtr<ID3D12DescriptorHeap> srvCpuHeap;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;      // RTV（持久：surface backbuffer RTV）
    // 帧级瞬态 RTV 专用堆：每帧从 0 号槽复用。绝不能与 rtvHeap 混用 —— 覆盖持久的 backbuffer RTV 会让 blit 把颜色纹理当 RT 绑，表现是全程黑屏。
    ComPtr<ID3D12DescriptorHeap> frameRtvHeap; // 帧级瞬态 RTV（每帧从 0 复用）
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12DescriptorHeap> samplerHeap;  // Sampler（SHADER_VISIBLE）
    ComPtr<ID3D12DescriptorHeap> drawHeap;     // P6：每帧瞬时 CBV/SRV 描述符（SHADER_VISIBLE，ring x2）

    // compute 的 typed UAV 描述符堆（SHADER_VISIBLE，64 槽 ring，懒建）。root UAV 只支持 raw / structured
    // buffer，`RWTexture2D` 这类 **typed** UAV 必须走描述符表 —— 声明成 ROOT_PARAMETER_TYPE_UAV 只会
    // 拿到 E_INVALIDARG（0x80070057）。着色率图像本身就是 typed UAV（R8_UINT）。
    ComPtr<ID3D12DescriptorHeap> computeUavHeap;  // 懒建
    UINT computeUavStride = 0;
    UINT computeUavNext = 0;

    // 当前挂在命令列表 SRI 槽位上的着色率图像。SHADING_RATE_SOURCE 只有「绑定期间」合法：解绑/换绑时必须转回 COMMON，否则读回、拷贝、UAV 全错配。
    Dx12Object* shadingRateImageBound = nullptr;

    // 异步分区开启后：drawHeap 每帧段划出一部分给 worker，同步 ring 只能用每段起始的 syncRingReserve 个槽位。0 = 未开启（可用整段 kDrawHeapPerFrame）。
    UINT syncRingReserve = 0;

    // ExecuteIndirect 用的 command signature（DrawIndexedInstanced / DrawInstanced）
    ComPtr<ID3D12CommandSignature> cmdSigIndexed;
    ComPtr<ID3D12CommandSignature> cmdSigNonIndexed;

    UINT srvInc = 0, rtvInc = 0, dsvInc = 0, samplerInc = 0;
    UINT drawInc = 0;  // == srvInc（同属 CBV_SRV_UAV 堆类型）


    std::string adapterName;
    ComPtr<IDXGIAdapter> adapter;  // 与 device 绑定的 DXGI adapter（用于 CreateDXGIFactoryForAdapter）
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;

    // 设备能力基线：设备创建后立即用 CheckFeatureSupport 填好（见 queryDeviceCapabilities），既是适配器 JSON 的来源，也是 mesh / DXR / VRS 的门控依据（12_2 为下限）。
    struct Capabilities {
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_2;
        D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;  // 最高 SM；mesh shader 需 6_6+
        D3D12_MESH_SHADER_TIER meshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
        D3D12_SAMPLER_FEEDBACK_TIER samplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
        D3D12_RAYTRACING_TIER raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
        D3D12_VARIABLE_SHADING_RATE_TIER shadingRateTier = D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
        // Work Graphs 只在 D3D12_OPTIONS21 里报，且只有 Agility 619 的 core 会回填。系统自带的
        // d3d12.dll 不认识这个枚举值，CheckFeatureSupport 直接失败 —— 那种情况下保持 NotSupported，
        // 光追计划里的 B-WG 后端因此自然不会进入候选集。
        D3D12_WORK_GRAPHS_TIER workGraphsTier = D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED;
        bool options21Queried = false;  // false = CheckFeatureSupport(OPTIONS21) 失败，tier 不可信
        // D3D12_OPTIONS22：SER 有的驱动报 tier 却实现成 no-op（字段名就叫 ActuallyReorders），
        // 以及 1D dispatch 上限 —— 逐节点派发要按它切块。
        bool serActuallyReorders = false;
        UINT max1DDispatchSize = 0;
        UINT max1DDispatchMeshSize = 0;
        bool options22Queried = false;
        // VRS 的两个附加事实：tier 2 = 支持着色率图像（tile 大小决定其分辨率）；additional = 1x4/4x1 是否可用。门控以 shadingRateTier >= 1 为准。
        UINT shadingRateImageTileSize = 0;
        bool additionalShadingRates = false;
        D3D12_RESOURCE_BINDING_TIER resourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
        bool enhancedBarriers = false;
        bool queried = false;  // false = CheckFeatureSupport 未被调用/全部失败

        // DXC 可用性。D3DCompile 只到 SM 5.1 而本项目**没有 5.1 兜底**（图形管线硬要求 6.9，mesh 也要 DXC）：动态加载 dxcompiler.dll 并做 ms_6_9 冒烟编译。
        bool dxcAvailable = false;        // dxcompiler.dll 已加载且能拿到 IDxcCompiler3
        bool meshShaderCompileOk = false;  // 冒烟编译（目标 ms_6_9）是否成功
        std::string dxcPath;
        std::string dxcProfile;  // 冒烟编译使用的 target，恒为 "ms_6_9"
        std::string dxcNote;

        // 这台 DXC 是否真能产出 SM 6.9 的 DXIL（1.8 及更早对 6_9 报 invalid profile）。
        bool dxcSupportsSm69() const { return dxcProfile == "ms_6_9"; }

        constexpr bool meshShadersSupported() const {
            return meshShaderTier >= D3D12_MESH_SHADER_TIER_1;
        }

        // 每 draw 速率需要 tier 1；屏幕空间 VRS（着色率图像）需要 tier 2。
        constexpr bool shadingRateSupported() const {
            return shadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1;
        }
        constexpr bool shadingRateImageSupported() const {
            return shadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2;
        }

        // tier 1.0 就够跑光追计划里的分阶段图（递归深度、记录传递都在 1.0 里）。
        constexpr bool workGraphsSupported() const {
            return workGraphsTier >= D3D12_WORK_GRAPHS_TIER_1_0;
        }
    };
    Capabilities caps;

    // RTX（DLSS / NRD）可用性探测结果，设备与适配器就绪后填一次、之后只读。刻意不并进 Capabilities（那是「设备会什么」，这是「SDK 能不能用」）。
    mcdr::RtxSupport rtx;

    // VRS 玩家档位（= 地形粗粒度上限）；-1 = 关闭（默认，整条录制路径不产生 RSSetShadingRate）。是玩家想要的上限，不是每条列表都重放的速率（下发由 JVM 逐批次做）。
    int requestedShadingRate = -1;
    // 已下发过 RSSetShadingRate 的次数（首次下发打一行 INFO）。
    UINT64 shadingRateAppliedLists = 0;
    // 能力判负 / 拿不到 list5 各只 WARN 一次（scope 每帧会调很多次）。
    bool shadingRateUnsupportedLogged = false;
    bool shadingRateList5Logged = false;
    // 两个组合器（D3D12_SHADING_RATE_COMBINER 的整数值）。默认都 PASSTHROUGH。
    int shadingRateCombiner0 = static_cast<int>(D3D12_SHADING_RATE_COMBINER_PASSTHROUGH);
    int shadingRateCombiner1 = static_cast<int>(D3D12_SHADING_RATE_COMBINER_PASSTHROUGH);
    UINT64 timestampFrequency = 0;
    ComPtr<ID3D12CommandQueue> queue;

    // 全局队列 fence：每次 ExecuteCommandLists 后 Signal(queueFence, ++queueFenceValue)。vanilla 会在从不 submit 的 encoder 上建 token，用 per-ctx fence 会永不完成 → 黑屏冻结。
    ComPtr<ID3D12Fence> queueFence;
    HANDLE queueFenceEvent = nullptr;   // waitForQueueFenceValue 用（每调用新建 event，不共享）
    UINT64 queueFenceValue = 0;         // 每次 ExecuteCommandLists 后递增

    // 诊断（调试层）：设备移除时 GetDeviceRemovedReason + InfoQueue 消息定位根因
    ComPtr<ID3D12InfoQueue> infoQueue;

    // blit 全屏四边形管线（懒初始化，device 级单例）
    std::unique_ptr<BlitPipeline> blitPipeline;
    std::unique_ptr<SharpenPipeline> sharpenPipeline;

    // ---- SMAA 状态：全部懒初始化（第一次应用且 aaMode==SMAA 时建中间纹理与三条管线，源尺寸变化重建）。只作用在**世界层**，否则 GUI 一起发虚。 ----
    int aaMode = 0;  // 0=OFF 1=SMAA 2=TAA 3=DLAA（与 Java 的 D3D12AntiAliasing 同序）
    // DLSS 的独立开关（Java 侧 dxrMcDlss），默认关。与 aaMode 无关 —— 两者可同时开，那时先 SMAA（低分辨率世界层）再 DLSS 升采样。
    bool dlssEnabled = false;
    // DLSS 挡位显式指定（0=Performance 1=Balanced 2=Quality）；-1 = JVM 从未设过 ⇒ 退回 chooseQuality 的比例就近逻辑。
    int dlssQualityTier = 2;
    // DLSS 模型（NGX render preset）：0=Default，10=J / 11=K / 12=L / 13=M；语义见 setDlssRenderPreset。
    int dlssPreset = 0;
    // JVM 送来的 UAV 输出纹理（经模块 createTexture 建）：非空时 DLSS 写它而不是自建私有纹理。只登记句柄不持引用，实现单元拿到裸 ID3D12Resource*。
    mcdr_object* dlssOutputOverride = nullptr;
    // 中间纹理：edges（边检测，源分辨率 RG8）→ weights（混合权重，源分辨率 RG8）。
    ComPtr<ID3D12Resource> smaaEdges;
    ComPtr<ID3D12Resource> smaaWeights;
    D3D12_CPU_DESCRIPTOR_HANDLE smaaEdgesRtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE smaaWeightsRtv{};
    UINT64 smaaWidth = 0;
    UINT64 smaaHeight = 0;
    // 官方 LUT（AreaTex 160x560 R8G8_UNORM / SearchTex 64x16 R8_UNORM，字节在 include\mcdr_smaa_luts.h）；初始化失败时 smaaLutsReady 保持 false，权重趟退回解析式 PSO。
    ComPtr<ID3D12Resource> smaaAreaTex;
    ComPtr<ID3D12Resource> smaaSearchTex;
    // 两张 LUT 的持久 SRV：写在 drawHeap 尾部两个保留槽（kSmaaLutSlots），与绘制同堆，绑定不需切堆。
    D3D12_GPU_DESCRIPTOR_HANDLE smaaLutSrvGpu{};
    UINT smaaLutSrvGpuInc = 0;
    bool smaaLutsReady = false;
    // "用的是官方 LUT"那行 INFO 的一次性闸门（逐帧都会走到选路代码，否则刷屏）。
    bool smaaLutReadyLogged = false;
    // 两个候选权重 PSO：LUT 版与解析版，都在 smaaEnsurePipelines 里一次建好，绝不重试。
    ComPtr<ID3D12PipelineState> smaaWeightLutPso;
    bool smaaLutFallbackWarned = false;  // 「LUT 不可用、退回解析式」只 WARN 一次
    // 就地路径第三趟输出：同一资源不能既当 SRV 又当 RTV，故落到同尺寸同格式 scratch 再 CopyResource 拷回。
    ComPtr<ID3D12Resource> smaaWorldResult;
    D3D12_CPU_DESCRIPTOR_HANDLE smaaWorldResultRtv{};
    DXGI_FORMAT smaaWorldResultFormat = DXGI_FORMAT_UNKNOWN;
    // 管线：根签名 3 张 SRV 表（0=t0 源色 / 1=t1 辅助纹理 / 2=t2 SearchTex）+ 静态采样器 s0/s1，四条 PS 共用 blit VS。表数必须覆盖各趟声明的寄存器，否则 E_INVALIDARG。
    bool smaaPipelinesReady = false;
    bool smaaUnavailableLogged = false;  // 建管线失败只报一次，之后每帧静默跳过
    ComPtr<ID3D12RootSignature> smaaRootSig;
    ComPtr<ID3D12PipelineState> smaaEdgePso;
    ComPtr<ID3D12PipelineState> smaaWeightPso;
    ComPtr<ID3D12PipelineState> smaaBlendPso;
    UINT64 smaaAppliedFrames = 0;  // 已应用帧数（首帧打一行 INFO 便于实机确认）
    // SMAA 自己的 3 槽 RTV 堆（NONE-visibility）。不用 allocRtvHandle（持久单调分配，resize 重建时每轮泄漏槽位）。
    ComPtr<ID3D12DescriptorHeap> smaaRtvHeap;
    // 混合阶段的目标格式（blit 路径 = back buffer；就地路径 = 主渲染目标）；PSO 的 RTVFormats[0] 必须相等，格式变化时三条管线一起重建。
    DXGI_FORMAT smaaBlendFormat = DXGI_FORMAT_UNKNOWN;
    // 一次致命失败后彻底关闭 SMAA：避免每帧重试刷日志，也避免在状态不符的中间纹理上继续用。
    bool smaaDisabled = false;

    // ---- TAA 跨帧状态：历史纹理由**本模块**持有，按主目标尺寸懒建、尺寸变化整组重建；两张乒乓（read = 上一帧输出，write = 本帧目标）。 ----
    ComPtr<ID3D12Resource> taaHistory[2];
    D3D12_CPU_DESCRIPTOR_HANDLE taaHistoryRtv[2]{};
    UINT64 taaWidth = 0;
    UINT64 taaHeight = 0;
    // 两张历史纹理的 RTV 堆（2 槽、NONE-visibility）。不用 allocRtvHandle 的理由同 SMAA（resize 泄漏）。
    ComPtr<ID3D12DescriptorHeap> taaRtvHeap;
    // resolve 管线：根参数 0 = 12 个 32-bit 参数（10 有效 + 2 pad），根参数 1 = t1..t3 三张 SRV；PS 全用 Load + 手算双线性，没有采样器。
    ComPtr<ID3D12RootSignature> taaRootSig;
    ComPtr<ID3D12PipelineState> taaPso;
    int taaReadIndex = 0;
    int taaWriteIndex = 1;
    bool taaHistoryValid = false;  // false ⇒ 本帧 resolve 以 reset=1 开始（历史不可信）
    bool taaDisabled = false;      // 一次致命失败后永久关闭，避免每帧重试刷日志
    bool taaUnavailableLogged = false;  // 建管线失败 / 目标不适用只报一次
    bool taaSharpenFallbackWarned = false;  // 锐化趟失败退回纯 blit 只 WARN 一次
    UINT64 taaAppliedFrames = 0;            // 已应用帧数（首帧打一行 INFO 便于实机确认）
};

// ---- 资源对象（Java long 句柄 = Dx12Object*） ----
struct Dx12Object {
    // ComputePipeline 不是资源，只承载 compute PSO + 固定根签名（1 个 root UAV + 8 根常量）；销毁走 destroyObject 的延迟删除（可能被在飞的 list 引用）。
    // AccelStructView = 加速结构的 SRV（RAYTRACING_ACCELERATION_STRUCTURE）：自身没有资源，只占一个描述符槽。
    // RaytracingPipeline = 光线追踪状态对象（DXIL 库 + hit group + 全局根签名），没有资源，只承载 stateObject
    // 与取标识符用的 stateProps；同样走 destroyObject 的延迟删除。
    enum class Kind { Texture, Buffer, Sampler, TextureView, AccelStructView, ComputePipeline, RaytracingPipeline };

    Kind kind = Kind::Buffer;
    ComPtr<ID3D12Resource> resource;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;  // Texture 的格式（copy 计算行距用）
    void* mappedPtr = nullptr;
    int usage = 0;
    long long size = 0;
    // 描述符堆槽位索引（TextureView → SRV 槽；Sampler → sampler 槽；-1 = 未占用）。销毁时归还 free-list 复用，防止长会话耗尽堆。
    int descSlot = -1;
    // TextureView 指向的底层 Texture（采样前需 transition 到 PIXEL_SHADER_RESOURCE）；其它种类恒 nullptr。
    Dx12Object* sourceTexture = nullptr;
    // Kind::ComputePipeline 时有效；根签名与 PSO 一起建，调用方不需要（也无法）从外部传进来。
    ComPtr<ID3D12RootSignature> computeRootSignature;
    ComPtr<ID3D12PipelineState> computePipelineState;
    // Kind::RaytracingPipeline 时有效：状态对象与它的属性接口（GetShaderIdentifier 从这里取 32 B 标识符）。
    ComPtr<ID3D12StateObject> stateObject;
    ComPtr<ID3D12StateObjectProperties> stateProps;
};

// ---- 设备生命周期 ----
// 创建/复用全局设备上下文；失败时返回 mcdr::Error。设备必须建在支持 FL 12_2 的硬件适配器上（按 GPU
// 偏好顺序挑）：混合显卡上默认适配器常是核显，既没有 12_2，也让 CreateSwapChainForHwnd E_ACCESSDENIED。
mcdr::Result<void> ensureDevice();
// 销毁全局设备（幂等）：等 GPU 空闲后释放队列 / 堆 / 延迟删除队列与 SMAA·TAA 状态。
void destroyDevice();

// D3D_FEATURE_LEVEL → "12_2" 短名；未知值返回 "unknown"。
const char* featureLevelName(D3D_FEATURE_LEVEL level);
// 本工程允许的最高着色器模型 = 6.10（SDK 的 d3d12.h 只到 6_9，6_10 用 0x6a 补齐）；真正能用取驱动上报值与上限的较小者。
constexpr D3D_SHADER_MODEL kShaderModelCeiling = static_cast<D3D_SHADER_MODEL>(0x6a);
// "6.8" / "6.10"（日志与 JSON）与 "6_8" / "6_10"（DXIL target 后缀）两套写法。
const char* shaderModelName(D3D_SHADER_MODEL model);
const char* shaderModelProfileSuffix(D3D_SHADER_MODEL model);
// 驱动上报的最高 SM 与 kShaderModelCeiling 取小；着色器编译 / PSO 目标用它。
D3D_SHADER_MODEL effectiveShaderModel(const DeviceContext::Capabilities& caps);
// 查一遍 12_2 相关能力（SM / mesh shader tier / DXR / VRS / enhanced barriers）。
void queryDeviceCapabilities(ID3D12Device* device, DeviceContext::Capabilities& caps);
// 能力摘要（一行日志用）与 JSON 片段（无前导逗号，供适配器信息 JSON 拼接）。
std::string capabilitiesText(const DeviceContext::Capabilities& caps);
std::string capabilitiesJson(const DeviceContext::Capabilities& caps);

// 探测 DXC（运行时动态加载，绝不静态链接 dxcompiler.lib）并做 ms_6_9 冒烟编译，写回 caps 的 dxc* 字段。顺序：MCDR_DXC_DIR → MCDR.dll 同目录 → SDK 最新 x64。失败不致命。
void probeDxc(DeviceContext::Capabilities& caps);

// 运行期 HLSL → DXIL 入口（进程内缓存 dxcompiler.dll 与接口）；mesh shader（as_6_x / ms_6_x）只能由 DXC 产出。失败时 Error 带诊断首行。
mcdr::Result<void> compileHlslToDxil(const std::string& source, const std::string& entry,
                                    const std::string& profile, std::vector<uint8_t>& outDxil);

// compute 最小通路：根签名固定（1 个 root UAV(u0) + 8 个 int32 根常量(b0)），所以只要 DXIL 字节码，不需要绑定表。
mcdr::Result<void*> createComputePipeline(const void* dxil, int32_t dxilSize);

// 着色率图像派发（tier 2 SRI）：3 张 SRV（t0 深度 / t1 速度 / t2 上一帧图像）+ 1 张 UAV(u0) + b0 根常量。返回 1=已派发 0=状态不对 -2=没有打开的列表。
mcdr::Result<void*> createComputePipelineShadingRate(const void* dxil, int32_t dxilSize);
int dispatchComputeShadingRate(CommandContext* ctx, Dx12Object* pipeline, Dx12Object* srvDepth,
                               Dx12Object* srvMotion, Dx12Object* srvPrevious, Dx12Object* image,
                               const int32_t* constants, int32_t constantCount, int32_t x, int32_t y,
                               int32_t z);
mcdr::Result<void*> createShadingRateImage(int32_t width, int32_t height);
int setShadingRateImage(CommandContext* ctx, Dx12Object* image);
int dispatchCompute(CommandContext* ctx, Dx12Object* pipeline, Dx12Object* uavTexture,
                    const int32_t* constants, int32_t constantCount, int32_t x, int32_t y, int32_t z);

// descs 指向 mcdr_barrier_desc 数组（include\mcdr_abi.h）。1=已提交 0=参数错 -2=列表未开。
int submitBarriers(CommandContext* ctx, const void* descs, int32_t count);

mcdr::Result<void*> createComputePipelineEx(const void* dxil, int32_t dxilSize, int32_t srvCount,
                                           int32_t uavCount);
int dispatchComputeEx(CommandContext* ctx, Dx12Object* pipeline, Dx12Object* const* srvs,
                      int32_t srvCount, Dx12Object* const* uavs, int32_t uavCount,
                      const int32_t* constants, int32_t constantCount, Dx12Object* cbv,
                      uint64_t cbvOffset, int32_t x, int32_t y, int32_t z);
    int nrdDenoise(CommandContext* ctx, Dx12Object* const* textures, int32_t textureCount,
               const float* matrices, uint32_t frameIndex, uint32_t resetHistory, char* outText,
               int32_t capacity);
    // DLSS-D（S4 4-d）：5 张 = packed 法线-粗糙度，6 张 = 法线 + 粗糙度分开。
    int dlssdEval(CommandContext* ctx, Dx12Object* const* textures, int32_t textureCount,
                  uint32_t width, uint32_t height, uint32_t outputWidth, uint32_t outputHeight,
                  uint32_t frameIndex, int32_t reset, const float* constants, char* outText,
                  int32_t capacity);

// 光线追踪状态对象（S3 3-f）：lib_6_x 库 + raygen/miss + 1..kMaxRtHitGroups 个 hit group；全局根签名与
// compute 共用同一套四参数（SRV 表 / UAV 表 / b0 CBV / b1 八 dword）。
inline constexpr int32_t kMaxRtHitGroups = 4;
mcdr::Result<void*> createRaytracingPipeline(const void* dxilLib, int64_t dxilSize,
                                             const char* raygen, const char* miss,
                                             const mcdr_rt_hit_group* groups, int32_t groupCount,
                                             int32_t srvCount, int32_t uavCount,
                                             int32_t maxRecursion);
// 取某个导出名的 32 B 着色器标识符（SBT 记录的前 32 字节）；1=成功 0=名字不存在。
int32_t getShaderIdentifier(Dx12Object* pipeline, const char* exportName, void* out32);
// DispatchRays：1=已派发 0=参数错 -2=没有打开的列表。
int dispatchRays(CommandContext* ctx, Dx12Object* pipeline, Dx12Object* shaderTable,
                 int64_t raygenOffset, int64_t missOffset, int64_t missStride, int32_t missCount,
                 int64_t hitOffset, int64_t hitStride, int32_t hitCount, Dx12Object* const* srvs,
                 int32_t srvCount, Dx12Object* const* uavs, int32_t uavCount,
                 const int32_t* constants, int32_t constantCount, Dx12Object* cbv,
                 uint64_t cbvOffset, int32_t width, int32_t height);

// 供 C ABI 层读设备上下文（adapterName / featureLevel）。
DeviceContext& deviceContext();




// 释放延迟删除队列：默认只放 queue fence 已确认的条目（不阻塞），force=true 忽略 fence 全放（仅 GPU 已空闲的退出场景）。destroySurface 之后要调一次。
void flushPendingDeletes(bool force = false);

// ---- 把瞬时描述符直接写到 drawHeap 的「绝对槽位」（worker 在自己的分区区域内写 CBV / 纹理 SRV /
// texel buffer SRV）。与 pushDescriptors 的区别：**不做 transition**（bundle 内禁止 ResourceBarrier），
// 调用方须先在主列表上 transition；槽位由 PartitionedDescriptorAllocator 分配，互不重叠。 ----
mcdr::Result<void> writeCBVToDrawHeap(const UINT& slot, const Dx12Object*& buf, const long long& offset,
                                     const long long& size);
// 复制纹理视图已有的 SRV 描述符到 slot。
mcdr::Result<void> writeTextureSRVToDrawHeap(const UINT& slot, const Dx12Object*& view);
// buffer[offset, offset+length) 写为 texel buffer SRV（texelFormat = GpuFormat ordinal）。
mcdr::Result<void> writeBufferSRVToDrawHeap(UINT slot, Dx12Object* buf, long long offset, long long length,
                                           int texelFormat);

// 返回设备 / 队列的原始 COM 指针（供 Java 侧持有句柄）。
uintptr_t getDeviceHandle();
uintptr_t getQueueHandle();

// 创建一个隐藏的 Win32 窗口（测试渲染循环用）；返回 HWND (uintptr_t)。
uintptr_t createHiddenWindow(int width, int height);
// 销毁隐藏的 Win32 窗口。
void destroyHiddenWindow(uintptr_t hwnd);

// ---- 资源创建（返回 new 的 Dx12Object*，句柄 = 指针；失败返回 mcdr::Error） ----
// 句柄一律以 void* 返回：MSVC 19.51 无法为 std::expected<模块内类型*, mcdr::Error> 在模块边界外产生
// 一致的实例化符号（LNK2001）。
mcdr::Result<void*> createTexture(int usage, int format, int width, int height, int depthOrLayers,
                                  int mipLevels);
// 建 buffer（usage 位见 mcdr_buffer_usage）；失败返回 mcdr::Error。
mcdr::Result<void*> createBuffer(int usage, long long size);
mcdr::Result<void*> createSampler(int addressU, int addressV, int minFilter, int magFilter,
                                  int maxAnisotropy, float maxLod);
// 地形专用采样器。`mipLodBias` 只应在超分比例 != 1 时非 0（见 D3D12TerrainSampler）。
mcdr::Result<void*> createSamplerWithMipBias(int addressU, int addressV, int minFilter, int magFilter,
                                             int maxAnisotropy, float maxLod, float mipLodBias);
mcdr::Result<void*> createTextureView(Dx12Object* texture, int baseMipLevel, int mipLevels);
// 销毁任意 Dx12Object*（等到 GPU 不再引用后真正释放；nullptr 安全）。
void destroyObject(Dx12Object* obj);

// ---- 光线追踪加速结构（S3）：BLAS / TLAS ----
// 加速结构缓冲必须 DEFAULT 堆 + ALLOW_UNORDERED_ACCESS + 初始状态 RAYTRACING_ACCELERATION_STRUCTURE，
// 且不能进缓冲池（池按 (堆, 容量) 复用，会给它配上错误的状态）。
mcdr::Result<void*> createAccelStructBuffer(long long size);
// out[0] = 结果上限字节，out[1] = scratch 字节（均需 256 字节对齐）。
mcdr::Result<void> getBlasPrebuildInfo(const mcdr_rt_geometry* geometries, int count,
                                       unsigned long long* out);
mcdr::Result<void> getTlasPrebuildInfo(int instanceCount, unsigned long long* out);
// update != 0 时走 PERFORM_UPDATE；要求 dest 里已是同一拓扑的 BLAS。
mcdr::Result<void> buildBlas(CommandContext* ctx, Dx12Object* dest, Dx12Object* scratch,
                             const mcdr_rt_geometry* geometries, int count, bool update);
mcdr::Result<void> buildTlas(CommandContext* ctx, Dx12Object* dest, Dx12Object* scratch,
                             Dx12Object* instances, int instanceCount);
// COPY_MODE_COMPACT：紧凑版下一帧才能当 SRV 用。
mcdr::Result<void> copyAccelStruct(CommandContext* ctx, Dx12Object* dest, Dx12Object* src);
// RAYTRACING_ACCELERATION_STRUCTURE 的 SRV（进描述符堆，给 compute 的 t 槽用）。
mcdr::Result<void*> createAccelStructView(Dx12Object* accel);
// 缓冲的 GPU 虚拟地址：加速结构实例描述要写它，不能写对象句柄。
mcdr::Result<UINT64> bufferAddress(Dx12Object* buffer);

// ---- Buffer 映射（对应官方 GpuBuffer.map）：校验 usage 位后 Map，返回连续指针供 NewDirectByteBuffer。 ----
mcdr::Result<void*> mapBuffer(Dx12Object* buffer, long long offset, long long length, bool read, bool write);
// 解除 map（与 mapBuffer 成对）。
void unmapBuffer(Dx12Object* buffer);

// ---- 命令层：CommandContext 对应官方 VulkanCommandEncoder ----
// 官方用 timeline semaphore + 双 command pool；这里用 ID3D12Fence（timeline 语义）+ 三个 allocator，
// 三 ring：帧 N 用 index (N-1)%3，帧 N+3 复用时 submit 已等 N 完成（双 ring + 等 value-2 会差一帧）。
// Dx12Pipeline 的完整定义必须提前到这里：CommandContext 按值引用它，模块内前向声明会得到另一个模块
// 归属的类型（LNK2001）。
struct Dx12Pipeline {
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> withDepth;  // 总是创建；DSV=D32_FLOAT（镜像官方 depthAttachmentFormat=126）
    ComPtr<ID3D12PipelineState> withoutDepth;  // 仅 depthState==null 时创建；DSV=UNKNOWN
    int topology = 4;  // MC PrimitiveTopology ordinal（setPipeline 时 IASetPrimitiveTopology 用）
    // per-slot 修正 stride（由 inputElements 实际格式推算，覆盖 Java 侧 getVertexSize() 的错误值）
    std::unordered_map<int, UINT> vertexStrides;
    // mesh 管线（AS/MS）：没有 IA 阶段，setPipeline 不设 IASetPrimitiveTopology，PSO 里也不含输入布局。
    bool isMesh = false;
    // mesh 尾参在根签名里的参数索引（按 CBV 稠密编号 + 地形 SRV 表排版算出，经 createMeshPipeline 抄进来）；PushDescriptors 只设到地形 SRV 表，b8 与 t14/t15/t16 由 drawMeshlets 补。0 = 无尾参。
    UINT segmentParam = 0;
    UINT geometryParam = 0;
};


// 帧级瞬态 RTV/DSV 缓存键：(资源指针, mip) 唯一决定视图内容（见 makeRtvDesc）。
struct RtvDsvKey {
    ID3D12Resource* resource;
    UINT mip;
    bool operator==(const RtvDsvKey& o) const { return resource == o.resource && mip == o.mip; }
};
struct RtvDsvKeyHash {
    size_t operator()(const RtvDsvKey& k) const {
        // 黄金比例常数按 size_t 取：x64 下等价 0x9E3779B97F4A7C15ULL，x86 下让乘法落在 32 位域内以避免 C4244。
        return std::hash<void*>()(reinterpret_cast<void*>(k.resource))
               ^ (std::hash<UINT>()(k.mip) * static_cast<size_t>(0x9E3779B97F4A7C15ULL));
    }
};

struct CommandContext {
    ComPtr<ID3D12CommandAllocator> allocators[3];   // 三帧飞行，等待 value-2 后复用
    // command list 也是 3 路 ring，与 allocator 同槽位：录制帧 N 用帧 N-3 的槽位，只需等该槽位 fence。单条 list 会让主线程等上一帧 GPU 执行完才能 Reset。
    ComPtr<ID3D12GraphicsCommandList> commandLists[3];
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;              // 最近一次 Signal 的值（从 1 开始递增）
    HANDLE fenceEvent = nullptr;        // SetEventOnCompletion 用
    int listOpen = 0;
    int inRenderPass = 0;

    // 本帧使用的 ring 槽位（begin 时按 fenceValue%3 固定，submit 时据此记录 fence）。
    UINT currentListSlot = 0;
    // 各槽位最后一次提交的 per-list fence 值。begin 时等本槽位的值，保证该槽位 list/allocator 不再被 GPU 引用后再 Reset。
    UINT64 slotFence[3] = {0, 0, 0};
    // 指向 commandLists[currentListSlot] 的裸指针，供全部录制调用点直接用（保持 `ctx->commandList->` 写法）。
    ID3D12GraphicsCommandList* commandList = nullptr;

    // 当前渲染 pass 的活跃附件。RENDER_TARGET / DEPTH_WRITE 属**非可提升状态**，list 执行完不会隐式 decay ⇒ endRenderPass 必须自己回切。
    std::vector<Dx12Object*> activeColorTargets;
    // 标记每个 color target 在本 command list 中是否被 draw 写入过
    std::vector<bool> activeColorTargetsTouched;
    // 整个 command list 是否有过任何 draw 写入（endRenderPass 后仍有效，供 blitSurface 判断）。
    bool colorTargetsWritten = false;
    Dx12Object* activeDepthTarget = nullptr;

    // 本帧 drawHeap 瞬时描述符分配（ring：fenceValue%2 交替两个半区；帧 N+2 重写帧 N 半区时 submit 已等 N 完成，x2 足够）。
    UINT drawHeapSlotBase = 0;
    UINT nextDrawSlot = 0;

    // 同一 list 内「SRV 集合与上次完全一致」的连续 push 复用上次槽位：加载屏与图集上传会推上万个相同 SRV 表，每次新分配会耗尽每帧段（131072 槽）。槽位只增不减，复用安全。
    UINT lastSrvSlotOffset = 0;   // 上次分配的段内偏移（相对 drawHeapSlotBase）
    UINT lastSrvCount = 0;        // 上次分配的 SRV 个数（0 = 本列表还没推过）
    // 上次的 view 序列（指针 + 当时的描述符 CPU 句柄）。句柄必须一起比：view 释放后地址可能被复用，只比指针会把旧描述符当命中。
    std::vector<std::pair<Dx12Object*, SIZE_T>> lastSrvViews;

    // 上一次 submitCommandList 写入的 queue fence 值：beginCommandListWithWait 据此等 GPU 完成上一帧再 Reset allocator。0 = 无需等待。
    UINT64 lastSubmitQueueFence = 0;

    // 上一次 submit 写入的 per-list fence 值。queue fence 只表示已入队，per-list 表示 GPU **已执行完**；等它完成才 Reset allocator 是避免 hang 的关键。
    UINT64 prevFenceValue = 0;

    // 本 list 内已过渡的资源状态（资源 → 当前状态）。beginCommandList 清空（上一 list 结束后提升状态已隐式 decay 回 COMMON）；绝不在 list 内显式回退 COMMON。
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> resourceState;
    // 本 list 内被 transition 到 COPY_SOURCE 的 UPLOAD 堆 staging buffer；endCommandList 必须回切 GENERIC_READ（下一帧 begin 以此为初始假设）。
    std::unordered_set<ID3D12Resource*> stagingCopySourceResources;

    // 本 ctx 的帧级 RTV/DSV 缓存属于描述符堆哪一代（见 gFrameViewGeneration）；代不一致 ⇒ 句柄可能被别的 ctx 覆盖，必须整体重建。
    unsigned int frameViewGeneration = 0;

    
    // 当前绑定的管线，用于 setVertexBuffer 查找修正 stride。
    Dx12Pipeline* currentPipeline = nullptr;

    // setPipeline 快速路径：同一 list 内以相同 PSO 重复调用时省 3 次驱动调用。ExecuteBundle 后置空
    // （bundle 内设置的 PSO 不回滚到父列表，父列表状态可能已变）。
    ID3D12PipelineState* cachedPso = nullptr;

    // 帧级瞬态 RTV/DSV 描述符缓存：同一 (资源, mip) 在本 list 内只建一次视图。beginCommandList 清空
    // （与 gNextFrameRtv / gNextDsv 同点清零，句柄随之失效）。
    std::unordered_map<RtvDsvKey, D3D12_CPU_DESCRIPTOR_HANDLE, RtvDsvKeyHash> rtvCache;
    std::unordered_map<RtvDsvKey, D3D12_CPU_DESCRIPTOR_HANDLE, RtvDsvKeyHash> dsvCache;

    ComPtr<ID3D12CommandAllocator>& currentAllocator() { return allocators[currentListSlot]; }
};

// 创建命令上下文：3 个 allocator + command list + fence + event。
mcdr::Result<void*> createCommandEncoder();
// 销毁 encoder（等自身 fence 完成后释放 allocator / list / event；幂等）。
void destroyCommandEncoder(CommandContext* ctx);

// 开始录制：先等 GPU 完成 waitForValue 对应的提交（>0 时），再 Reset 当前 allocator + list —— allocator
// 被还在使用的 list 引用时覆写就是 use-after-free。0 跳过等待（兼容单线程与 createBuffer 一次性路径）。
mcdr::Result<void> beginCommandList(CommandContext* ctx);
// 等 GPU 完成 waitForValue 后开始录制（异步提交路径；0 = 不等待）。
mcdr::Result<void> beginCommandListWithWait(CommandContext* ctx, UINT64 waitForValue);
// 结束录制：Close list（之后可提交）。
mcdr::Result<void> endCommandList(CommandContext* ctx);

// 把主描述符堆（本帧的 drawHeap）无条件重新绑到命令列表上。SetDescriptorHeaps 是命令列表级状态，不会
// 随根签名 / PSO / 根描述符表复位，而厂商 SDK 的 Evaluate 与 dispatchCompute 都可能绑走自己的堆 ——
// 之后任何走瞬时 SRV 的绘制都会撞 descriptor heap 不匹配。重复绑同一张堆合法且开销可忽略，故不做查询。
void restoreDrawHeap(CommandContext* ctx);
// 提交：ExecuteCommandLists + Signal(fence, ++fenceValue)。非阻塞（GPU 异步执行）；fence 信号值
// 同时写入当前 active surface 的 per-backbuffer 记录。返回本次提交的 fence value。
mcdr::Result<UINT64> submitCommandList(CommandContext* ctx);
// 等待 fence 值达到 value；timeoutNs 为纳秒。返回是否完成。
mcdr::Result<void> waitForFenceValue(CommandContext* ctx, UINT64 value, UINT64 timeoutNs);
// 当前 fence value（Java 侧 createFence 记录用）。
UINT64 currentFenceValue(CommandContext* ctx);
// 等待**该 ctx 自己**的 per-command-list fence 达到 value（Kotlin GpuFence.awaitCompletion 的落点）。
// token 只由这个 ctx 的提交推进，与其它 encoder 解耦；超时返回 err，由调用方决定是否当作失败。
mcdr::Result<void> waitForContextFenceValue(CommandContext* ctx, UINT64 value, UINT64 timeoutNs);

// ---- 独立 C++ 渲染线程（渲染工作从主线程剥离） ----
// 渲染线程只做 GPU 同步（Close / ExecuteCommandLists / Signal / Present），主线程只 push 命令。四阶段
// 握手：BEGIN_FRAME（主）→ RECORDING_READY（渲染）→ COMMANDS_READY（主）→ SUBMIT_DONE（渲染）。
// allocator->Reset() 与 list 的 begin 只能由主线程做。
bool initAsyncRenderer(UINT workerCount);
// 停止渲染线程并释放其同步对象（幂等）。
void destroyAsyncRenderer();
// 等待渲染线程完成当前帧（不销毁线程，仅同步到 gEvtSubmitDone）。
void waitForRenderThreadSubmit();
// 等待所有已提交帧完成 Present（acquireSurface 调 GetCurrentBackBufferIndex 之前必须调）：Present 未
// 完成时 index 不轮转，提前 acquire 会拿到同一个 back buffer，blit 会写到已翻转走的 buffer 上，D3D12
// 直接以 DXGI_ERROR_ACCESS_DENIED (0x887A0005) 移除设备。返回 true = 已同步。
bool waitForPendingPresents(unsigned timeoutMs = 100);

// 把 GetDeviceRemovedReason() 与 InfoQueue 最近若干条消息打进日志（设备移除时调用）。不开调试层
// 时日志里只剩 "presentSurface: FAILED DXGI_ERROR_DEVICE_REMOVED"，看不到真正原因。
void logDeviceStatus(const char* tag);
// 设备是否已被移除/重置（GetDeviceRemovedReason() != S_OK，或设备尚未创建）。帧循环在调 D3D12 /
// Streamline / FidelityFX 之前用它跳过已失效的活 —— 设备移除后继续调用会解引用失效对象。
bool deviceRemoved();
// 非阻塞：请求渲染线程开始下一帧。false = 正在处理上一帧。
mcdr::Result<void> asyncRenderBeginFrame(CommandContext* ctx);
// 阻塞：等到本帧提交完成（或超时 ms）。
mcdr::Result<void> asyncRenderWaitComplete(CommandContext* ctx, UINT64 timeoutMs);
// 非阻塞：查询渲染线程是否已到达 "recording ready" 阶段。
bool asyncRenderIsRecordingReady(CommandContext* ctx);
// 阻塞：等待渲染线程到达 "recording ready" 阶段（超时 ms）。取代 Java 侧 Thread.sleep(1) 轮询。
bool asyncRenderWaitRecordingReady(CommandContext* ctx, UINT64 timeoutMs);
// 主线程通知渲染线程：所有命令已入队（设置 gEvtCommandsReady）。
void asyncSendCommandsReady(CommandContext* ctx);
// 主动清除 gAsyncRenderCtx（供 Java 侧超时降级时同步调用）。
void clearAsyncRenderCtx();
// 查询命令列表是否已打开。
bool isListOpen(CommandContext* ctx);

// 当前帧使用的 drawHeap 段号（= fenceValue % kDrawHeapSections）。Java 侧据此把 worker 的
// (frameSlot, worker) 描述符区域对齐到同一段，避免与同步 ring 重叠。
UINT currentDrawFrameSlot(CommandContext* ctx);

// ---- 全局队列 fence（对应官方共享 encoder 的 submit index） ----
// 当前全局队列 fence 值（createFence 记录用：目标 = 当前值 + 1，下一次任意 ctx 的提交完成后达成）。
UINT64 currentQueueFenceValue();
// 等待全局队列 fence 达到 value（等待对象是设备级 queueFence，而非 per-ctx fence）。
mcdr::Result<void> waitForQueueFenceValue(UINT64 value, UINT64 timeoutNs);

// 立即读 GPU 时间戳（临时 query + 提交 + 读回）。
mcdr::Result<long long> getTimestampNow(CommandContext* ctx);

// ---- 命令录制：copy / clear / render pass / timestamp ----
// buffer → buffer 拷贝（CopyBufferRegion）。
mcdr::Result<void> copyBufferToBuffer(CommandContext* ctx, Dx12Object* src, long long srcOffset, Dx12Object* dst,
                                     long long dstOffset, long long size);

// 状态追踪的纹理过渡：以本 list 已跟踪状态为准，**绝不回退 COMMON**（decay 由命令列表完成时隐式
// 处理）。blitSurface 也用它把渲染后的源纹理过渡到 COPY_SOURCE。
void transitionTextureTo(CommandContext* ctx, Dx12Object* tex, D3D12_RESOURCE_STATES to);

// buffer 版本（初始锚点 = initialStateFor(heapType)）。异步路径为主列表批量预过渡 CBV/SRV 时复用。
void transitionBufferTo(CommandContext* ctx, Dx12Object* buf, D3D12_RESOURCE_STATES to);

// 底层过渡：直接接受 ID3D12Resource*，用于 swapchain back buffer 等不在 Dx12Object 里的资源。
void transitionTo(CommandContext* ctx, ID3D12Resource* res, D3D12_RESOURCE_STATES fromInitial,
                  D3D12_RESOURCE_STATES to);

// clear 颜色纹理（整纹理，RENDER_ATTACHMENT）。
mcdr::Result<void> clearColorTexture(CommandContext* ctx, Dx12Object* tex, float r, float g, float b, float a);
// clear 深度纹理（整纹理）。
mcdr::Result<void> clearDepthTexture(CommandContext* ctx, Dx12Object* tex, double depth);

// clear 颜色纹理矩形区域：GuiItemAtlas 槽位 STALE 重绘前只清该槽位 —— 整图清空会把其它槽位已
// 烘好的物品图标一并抹掉，翻页回来后这些区域不再触发重绘 ⇒ 物品消失。
mcdr::Result<void> clearColorTextureRegion(CommandContext* ctx, Dx12Object* tex, float r, float g, float b,
                                          float a, int x, int y, int w, int h);
// clear 深度纹理矩形区域（同上）。
mcdr::Result<void> clearDepthTextureRegion(CommandContext* ctx, Dx12Object* tex, double depth, int x, int y,
                                          int w, int h);
// clear stencil 位（MC 26.2 的 clearStencilTexture）：只清 STENCIL 位并写入 value，深度位保持不动
// （ClearDepthStencilView 只带 STENCIL flag 时 depth 参数被忽略）。
mcdr::Result<void> clearStencilTexture(CommandContext* ctx, Dx12Object* tex, int value);

// ---- 纹理拷贝：CopyTextureRegion 系列（copyBufferToTexture 的 srcWidth/srcHeight 为源 buffer 的
// 行宽/行高 texel，w/h 为实际拷贝尺寸） ----
mcdr::Result<void> copyBufferToTexture(CommandContext* ctx, Dx12Object* srcBuf, long long srcOffset,
                                      int srcWidth, int srcHeight, Dx12Object* dstTex, int mip, int layer,
                                      int dstX, int dstY, int w, int h);

// texture 子区域 → readback buffer。
mcdr::Result<void> copyTextureToBuffer(CommandContext* ctx, Dx12Object* srcTex, int mip, int layer, int srcX,
                                      int srcY, int w, int h, Dx12Object* dstBuf, long long dstOffset);

// texture → texture 子区域。
mcdr::Result<void> copyTextureToTexture(CommandContext* ctx, Dx12Object* srcTex, Dx12Object* dstTex, int mip,
                                       int layer, int srcX, int srcY, int dstX, int dstY, int w, int h);

// 设备 timestamp 频率（GetTimestampFrequency；0 = 未知/未初始化）。
unsigned long long getTimestampFrequency();

// 渲染 pass 生命周期（OMSetRenderTargets + viewport/scissor + clear）。colorViews 允许空指针占位；
// colorMips 是每附件要绑的 mip slice（null=0）—— 图集按 mipViews[level] 逐级发起 render pass，恒绑
// mip0 会让 1+ 级内容从未写入（黑色方块）。colorClearFlags 0=Load 1=clear；clearColors 每附件 4 个
// float；depth* 四个参数同理；x/y/w/h = renderArea。
mcdr::Result<void> beginRenderPass(CommandContext* ctx, Dx12Object* const* colorViews, int colorCount,
                                  const int* colorMips, const int* colorClearFlags, const float* clearColors,
                                  Dx12Object* depthView, int depthMip, int depthClearFlag, double depthClearValue,
                                  int x, int y, int w, int h);
// 结束渲染 pass（附件回切 COMMON、关 scissor）；必须与 beginRenderPass 成对。
mcdr::Result<void> endRenderPass(CommandContext* ctx);

// ---- Timestamp query pool ----
struct QueryPool {
    ComPtr<ID3D12QueryHeap> heap;
    int size = 0;
};

// 建时间戳 query pool（size = 条目数）。
mcdr::Result<void*> createQueryPool(int size);
// 销毁 query pool。
void destroyQueryPool(QueryPool* pool);
// 录制 EndQuery(TIMESTAMP) 到 pool[index]。
mcdr::Result<void> writeTimestampToPool(CommandContext* ctx, QueryPool* pool, int index);
// 阻塞读回单个 timestamp（等待 GPU 完成）。
mcdr::Result<void> readQueryValue(QueryPool* pool, int index, long long& out);
// 阻塞读回连续 count 个 timestamp；会等 GPU，仅诊断与一次性路径用。
mcdr::Result<void> readQueryValues(QueryPool* pool, int start, int count, long long* out);

// ---- 格式映射（官方 GpuFormat 枚举值 → DXGI_FORMAT） ----
DXGI_FORMAT toDxgiFormat(int gpuFormat);
// 顶点输入布局专用：RGB32_* 用精确的三分量格式（R32G32B32_*，DXGI 合法输入格式）；纹理用的
// toDxgiFormat 会把 RGB32 加宽成 RGBA32，输入布局不能加宽。
DXGI_FORMAT toDxgiVertexFormat(int gpuFormat);

// ---- 图形管线：DXC(vs_6_9/ps_6_9, DXIL) + root signature + 双 PSO。**硬要求 SM 6.9**，无 5.1 兜底。
// desc 由 Java 侧 dx12CreateGraphicsPipeline 解析（字段语义见 Dx12Native Javadoc）。 ----
struct PipelineDesc {
    struct ColorTarget {
        int format = 0;  // GpuFormat ordinal；-1 = 未使用槽位
        uint8_t writeMask = 0;
        bool blendEnabled = false;
        uint8_t srcColor = 0, dstColor = 0, colorOp = 0;
        uint8_t srcAlpha = 0, dstAlpha = 0, alphaOp = 0;
    };
    struct InputElement {
        int location = 0, binding = 0, format = 0;
        int offset = 0, stride = 0, stepRate = 0;
        std::string semanticName;  // HLSL 声明的语义名（如 "POSITION"、"TEXCOORD0"）；空串则回退到 "TEXCOORD"
    };
    struct Binding {
        uint8_t type = 0;  // 0=CBV，1=SRV+static sampler，2=SRV
        uint8_t reg = 0;
    };

    std::vector<uint8_t> vsBytes;
    std::vector<uint8_t> psBytes;
    int colorCount = 0;
    std::vector<ColorTarget> colorTargets;
    bool hasDepth = false;
    int depthFormat = 0;
    bool depthWrite = false;
    int depthCompareOp = 0;
    int topology = 0;
    bool cullEnabled = false;
    int polygonMode = 0;  // PolygonMode ordinal（0=FILL, 1=WIREFRAME）
    std::vector<InputElement> inputElements;
    std::vector<Binding> bindings;  // 按 shader 声明顺序（register = 序号）
};

// 创建管线（DXC 编译 + root signature + 双 PSO）；失败返回 mcdr::Error。
mcdr::Result<void*> createGraphicsPipeline(const PipelineDesc& desc);
// 销毁管线（PSO 与根签名进延迟删除队列）。
void destroyPipeline(Dx12Pipeline* pipeline);

// ---- Blit 管线（全屏四边形绘制，绕过 CopyTextureRegion 格式不兼容问题） ----
// 源纹理 → backbuffer，格式转换由 GPU 光栅器自动完成，无需手动 resolve。
struct BlitPipeline {
    ComPtr<ID3D12RootSignature> rootSig;  // SRV(t0) + sampler(s0) + 32-bit constants(b1)
    ComPtr<ID3D12PipelineState> pso;  // noDepth, noBlend, noCull —— RTV = R8G8B8A8_UNORM（SDR）
    // HDR 变体：RTV = R16G16B16A16_FLOAT（scRGB），PS 把 sRGB 采样值解码成线性再乘 SDR 白点权重
    // （SDRWhiteLevel/80 —— scRGB 的 1.0 定义为 80 nits）。
    ComPtr<ID3D12PipelineState> psoHdr;
    ComPtr<ID3D12Resource> vertBuf;
    ComPtr<ID3D12Resource> idxBuf;
    D3D12_VERTEX_BUFFER_VIEW vbView{};
    D3D12_INDEX_BUFFER_VIEW ibView{};
};

// 懒初始化 blit 管线（线程安全：第一次调用创建，后续直接返回）。
mcdr::Result<void> initBlitPipeline();
const BlitPipeline* getBlitPipeline();

// sharpen pipeline (TAA / DLAA / DLSS post pass). Reuses the blit VS + vertex buffer, adds one
// 32-bit constant block (rcpSize.xy + amount).
struct SharpenPipeline {
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> vertBuf;
    ComPtr<ID3D12Resource> idxBuf;
    D3D12_VERTEX_BUFFER_VIEW vbView{};
    D3D12_INDEX_BUFFER_VIEW ibView{};
};
// 懒建 sharpen PSO（TAA / DLSS 后处理用；失败退回纯 blit）。
mcdr::Result<void> initSharpenPipeline();
const SharpenPipeline* getSharpenPipeline();
// 在 srvHeap 为 srcTex 分配 SRV 槽位并设置根描述符表（srvGpu 已绑定）。调用方无需关心 srvHeap 细节。
mcdr::Result<void> blitBindSourceTexture(CommandContext* ctx, Dx12Object* srcTex, ID3D12GraphicsCommandList* cmd);

// SMAA：对一张已渲染完成的纹理**就地**跑三个全屏阶段（边检测 → 混合权重 → 邻域混合 + 拷回）。调用点
// 在世界层画完、GUI 之前，所以界面保持锐利。只在 aaMode == 1 时工作，**不再**与 DLSS 互斥（两者都开
// 时这三趟先跑在低分辨率世界层上，再由 applyDlssToTexture 升采样）。返回 true = 已应用。
bool applySmaaToTexture(CommandContext* ctx, Dx12Object* tex);

// TAA（时域抗锯齿），与 applySmaaToTexture 同一调用点；状态在**本模块**跨帧保留。两趟：① scene +
// history[read] + motion → history[write]（RGBA16F，YCoCg 空间 variance clipping）；② history[write]
// 回填 scene（锐化量 > 0 走 sharpen PSO，否则纯采样 blit —— 16F 与 8 位主目标不能 CopyResource）。
// jitter 是投影矩阵里**已加进去**的偏移，本函数不改矩阵。返回 1=已应用 0=跳过 -1=失败。
int applyTaaToTexture(CommandContext* ctx, Dx12Object* scene, Dx12Object* motion, float jitterX,
                      float jitterY, float prevJitterX, float prevJitterY, float blend, int reset,
                      float sharpenAmount);

// DLSS 的跨帧上采样入口（真实 NVSDK_NGX 评价调用），与 applySmaaToTexture **同一调用点**：把 render 尺寸
// 的世界层升采样进 target 尺寸的目标纹理，顺带取代那次线性 blit。输入全部来自 in（mcdr_dlss_input）。
// 成功返回后 pInColor 停在 PIXEL_SHADER_RESOURCE、pInOutput 停在 RENDER_TARGET，GUI 的 pass 可接着用。
// 任何一步失败都只报一次、永久关掉 DLSS 并返回 false —— 调用方保持线性 blit，绝不重试、绝不黑屏。
bool applyDlssToTexture(CommandContext* ctx, const mcdr_dlss_input& in);

// DLSS 可用性与状态的单行查询（UI / 日志用）：输出写进 out（NUL 结尾；可为 nullptr）。返回 1=可用
// 0=不可用 -1=参数非法。首次查询会做一次 NGX init（成败都只做一次），所以它同时是探测入口。
int queryDlss(char* out, int cap);

// ---- 超分（SR）Provider 接口层 ----
// 把「一次超分提交」从厂商实现里剥出来：ABI 入口只做参数校验与填表，真正干活的对象按 backend id 在本
// 模块内选出来（DLSS / FSR / XeSS 各一个实现单元，新增后端不必动 ABI 编号）。为什么放在本模块：
// provider 要结束还开着的 render pass、按跟踪状态转 barrier、解包 `Dx12Object`。签名只用 POD。
enum class SrBackendId : int32_t {
    None = 0,
    Dlss = 1,
    Fsr = 2,
    XeSS = 3,
};

class IUpscaleProvider {
public:
    IUpscaleProvider() = default;
    IUpscaleProvider(const IUpscaleProvider&) = delete;
    IUpscaleProvider& operator=(const IUpscaleProvider&) = delete;
    virtual ~IUpscaleProvider() = default;

    virtual SrBackendId backend() const = 0;
    // 填能力表（`note` 由实现写；清零与 `backend` 字段由注册表负责）。
    virtual void fillCaps(mcdr_sr_caps& out) const = 0;
    // 建。失败时不得留下任何设备状态改动（调用方会保持既有路径）。`seed` 带首批尺寸：DLSS 的 feature
    // 只能建在打开的命令列表上，真正创建推迟到第一次 evaluate，这里只记上限尺寸。
    virtual bool initialize(CommandContext* ctx, const mcdr_sr_dispatch& seed) = 0;
    virtual void onResize(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                          int32_t displayHeight) = 0;
    // 资源与屏障的责任分工（实现方必须遵守）：
    //   1) **过渡归 provider**：必须走 transitionTextureTo / transitionTo，不许写裸 ResourceBarrier ——
    //      endRenderPass 每次都把附件回切 COMMON，实现方猜不到 Before 状态。
    //   2) **返回时 pColorOutput 必须处于 RENDER_TARGET**（GUI 的 pass 紧接着以它重新 attach）。
    //   3) 描述符堆由调用方收尾；私有纹理必须由 provider 在**模块内**创建并登记到跟踪表。
    virtual bool evaluate(CommandContext* ctx, const mcdr_sr_dispatch& desc) = 0;
    // 释放 provider 持有的全部资源（幂等；注册表随后把生效后端置 None）。
    virtual void shutdown() = 0;
};

// ---- 注册表（期望 → 生效两级）。返回 1=记住 0=非法 id（会把原因记进 status 行）。 ----
int32_t srPickBackend(int32_t backendId);
// 用户选中的后端（期望值；设备未建也可读，真正生效看 srActiveBackend）。
int32_t srSelectedBackend(void);
// 现在**跑着**的是哪一家（还没 create 时是 None；JVM 只调 srCreateProvider，pick 与生效可能不同）。帧生成
// 用它做互斥判断（SR 不是 DLSS 时 DLSS-G 拿不到 ScalingOutputColor tag）。
int32_t srActiveBackend(void);
// 填能力表；`backendId` 必须是 0..3。不改注册表状态，只用一次查询。
void srFillCaps(int32_t backendId, mcdr_sr_caps& out);
// 返回 1=已生效 0=后端不可用 -1=参数非法。重复调用同一个后端是幂等的。
int32_t srCreateProvider(CommandContext* ctx, int32_t backendId, int32_t maxRenderWidth,
                         int32_t maxRenderHeight, int32_t displayWidth, int32_t displayHeight);
int32_t srResizeProvider(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                         int32_t displayHeight);
int32_t srEvaluateFrame(CommandContext* ctx, const mcdr_sr_dispatch& desc);
// 后端报告它期望的 jitter 相位长度（0 = 不适用）。上层据此决定是"让后端接受我们的序列"还是"切到
// 后端序列"（见 D3D12Temporal.adoptPhaseCount）。
int32_t srJitterPhaseCount(int32_t backendId, int32_t renderWidth, int32_t displayWidth);
// 释放当前 provider（幂等）。调用后生效后端回到 None。
void srReleaseProvider(void);
// 单行状态摘要（生效后端 / 失败原因，供诊断行）。
std::string srStatusText(void);

// ---- 后端工厂：每个后端一个实现单元，注册表按需懒建。工厂 nullptr = 运行库不在（原因进 status 行，
// Evaluate 走「没有超分」的老路径） ----
std::unique_ptr<IUpscaleProvider> makeFsrProvider();
// caps 的 FSR 分支。**不建上下文**：caps 只回答"这个后端能不能用"。
void fillFsrCaps(mcdr_sr_caps& out);
// 运行库是否载入成功。给"没有上下文也要回答"的地方用（例如相位长度 / 诊断行）。
bool fsrRuntimeAvailable();
// XeSS（src\core\d3d12_xess.cpp）。
std::unique_ptr<IUpscaleProvider> makeXessProvider();
// caps 的 XeSS 分支（同 FSR 语义，不建上下文）。
void fillXessCaps(mcdr_sr_caps& out);
// XeSS 运行库是否载入。
bool xessRuntimeAvailable();

// ---- 帧生成（FG）Provider 接口层 ----
// 与 SR 同模块同接口单元：provider 必须在 Present 两侧插钩子（读交换链、解包 Dx12Object、改 present 的
// 同步间隔），只有本模块能做。与 IUpscaleProvider 的差异：没有 evaluate（FG 不往命令列表录东西），
// 多一对 beforePresent / afterPresent。
enum class FgBackendId : int32_t {
    None = MCDR_FG_BACKEND_NONE,
    DlssG = MCDR_FG_BACKEND_DLSSG,
    FsrFg = MCDR_FG_BACKEND_FSRFG,
    XessFg = MCDR_FG_BACKEND_XESSFG,
};

// 帧生成输入的中性快照（与 SR 后端解耦）：逐帧 tag 原只有一个触发点（srEvaluateFrame 成功后），而本仓
// DLSS 超分走 mcdr_dlss_evaluate 不经那条路径，于是 XeSS-FG 收不到颜色/深度/MV。资源状态必须由调用方
// 给：SDK 按 incomingState 记屏障，写错会让实际状态与跟踪表错位。
struct FgFrameInputs {
    ID3D12GraphicsCommandList* cmdList = nullptr;
    void* pColorOutput = nullptr;  // 显示分辨率的世界层（无 HUD）
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    void* pDepth = nullptr;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_READ;
    void* pMotionVectors = nullptr;
    D3D12_RESOURCE_STATES motionVectorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    int32_t renderWidth = 0;
    int32_t renderHeight = 0;
    int32_t displayWidth = 0;
    int32_t displayHeight = 0;
    // 相机与时间：FSR-FG 的 PrepareV2 需要（DLSS-G 走 Streamline 常量，自己不用）。0 = 上游没给。
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraVerticalFov = 0.0f;  // 弧度
    float frameTimeDeltaMs = 0.0f;
    float viewSpaceToMeters = 0.0f;
};

class IFrameGenProvider {
public:
    IFrameGenProvider() = default;
    IFrameGenProvider(const IFrameGenProvider&) = delete;
    IFrameGenProvider& operator=(const IFrameGenProvider&) = delete;
    virtual ~IFrameGenProvider() = default;

    virtual FgBackendId backend() const = 0;
    // 填能力表（`note` 由实现写；清零与 backend 字段由注册表负责）。
    virtual void fillCaps(mcdr_fg_caps& out) const = 0;
    // 设期望值。**只记录**：真正下发发生在下一次 beforePresent（渲染线程）。返回 true = 已接受（不
    // 代表已生效，生效看 presented()）。
    virtual bool setMode(const mcdr_fg_dispatch& desc) = 0;
    // 帧首（acquireSurface 消费完挂起的链重建之后、本帧还没有开画）。XeSS-FG 的 XeLL marker 必须
    // 在这里发，且帧号要与后面的 SetPresentId 一致。默认空实现。
    virtual void frameStart() {}
    // Present 之前 / 之后各一次，都在渲染线程。**不许在这里 Present**：链的所有权见 chainOwner()。
    virtual void beforePresent() = 0;
    virtual void afterPresent() = 0;
    // 上一次 Present 真正提交的帧数（1 = 没插帧）。
    virtual int32_t presented() const = 0;
    virtual void onResize(int32_t displayWidth, int32_t displayHeight) = 0;
    // 关。**必须先显式下发 eOff 再允许链回原生**，否则会留下「界面说关了、后端还在插帧」。实现不得
    // 改动链本身的所有权。
    virtual void shutdown() = 0;

    // ---- 包装型后端（FSR-FG / XeSS-FG）----
    // DLSS-G 是代理工厂 + SL 在 Present 内部插帧，链仍是应用那条；FSR-FG / XeSS-FG **包装交换链**：把应用链
    // 交给 SDK，之后所有 Present 必须走 SDK 换回来的那条。链的字段归 surface 所有，provider 只回答要换成
    // 哪条。默认不包装（0）；返回 1 时 *outPresentChain 必须是已 AddRef 的接口。
    virtual int32_t wrapSwapChain(ID3D12Device* device, ID3D12CommandQueue* queue,
                                  IDXGISwapChain* appChain, int32_t width, int32_t height,
                                  DXGI_FORMAT format, IDXGISwapChain** outPresentChain) {
        static_cast<void>(device);
        static_cast<void>(queue);
        static_cast<void>(appChain);
        static_cast<void>(width);
        static_cast<void>(height);
        static_cast<void>(format);
        static_cast<void>(outPresentChain);
        return 0;
    }
    // 解包：链要重建 / 后端要换人时先调它（SDK 的 context 必须先于链释放）。
    virtual void unwrapSwapChain() {}

    // 本帧的输入 tag：SR 已跑完、render pass 已结束、命令列表还开着。默认空实现（DLSS-G 自己交 tag）。
    // UI 纹理（GUI 合成目标）：传 nullptr 撤销，只在目标重建时调。
    virtual void setUiTexture(void* object) { static_cast<void>(object); }
    // hudless 呈现开关：1 = 应用 Present 的世界层**不含 UI**，UI 单独作为一张 UI-only 纹理交给后端合成
    // （FSR-FG 的 RegisterUiResource）；0 = 老路径（后端不得再注册 UI，否则 HUD 画两遍）。默认空实现。
    virtual void setUiComposition(int32_t enabled) { static_cast<void>(enabled); }
    // 后端是否真的会替我们把 UI 合成到 presented 帧上。只有它返回 true，应用才敢 Present 一张不含 UI
    // 的图。默认 false = "没人合成 UI"。
    virtual bool uiCompositionReady() const { return false; }
    // 帧常量（View/Projection 等）由 FG 载荷带进来，provider 自己决定怎么喂给 SDK。
    virtual void setFrameConstants(const mcdr_fg_constants& constants) { (void)constants; }

    // 本帧的输入 tag（命令列表还开着、SR 已跑完）。载荷是 `FgFrameInputs`，SR 通用路径与 DLSS 路径
    // 都能交。默认空实现。
    virtual void tagFrameInputs(const FgFrameInputs& inputs) { static_cast<void>(inputs); }
};

// ---- 注册表（与 SR 同风格：期望 → 生效两级）。返回 1=记住 0=非法 id（原因进 status 行）。 ----
int32_t fgPickBackend(int32_t backendId);
// 填能力表；`backendId` 必须是 0..3。不改注册表状态。
void fgFillCaps(int32_t backendId, mcdr_fg_caps& out);
// 懒建当前生效后端。返回 1=就绪 0=不可用（原因进 status 行）。重复调用幂等。
int32_t fgEnsureProvider(void);
// 记下期望值并转给 provider（provider 未建时按需建）。返回 1=已接受 0=不可用 -1=参数非法。
int32_t fgSetMode(const mcdr_fg_dispatch& desc);
// 由 presentSurface 在 Present 前后调用（渲染线程）。没有生效后端时是空操作。fgFrameStart 挂在
// acquireSurface 帧首（XeSS-FG 的 XeLL marker 需要帧起点语义）。
void fgFrameStart(void);
void fgBeforePresent(void);
void fgAfterPresent(void);
// 上一次真正提交的帧数；没有生效后端时返回 1（= 没插帧）。
int32_t fgPresented(void);
void fgOnResize(int32_t displayWidth, int32_t displayHeight);
// 三轨帧号的中间那条：provider 真正提交了一帧时调一次，把**自己**的严格 +1 计数写进注册表（present id
// 跳号会让 FSR-FG / XeSS-FG 静默丢帧）。DLSS-G 的帧 token 由 SL 管，从不调这里。
void fgNoteBackendFrame(uint64_t backendFrameId);
// 释放（幂等）。只等已提交的 fence 值，不等 +1。
void fgReleaseProviderWithWait(CommandContext* ctx);
std::string fgStatusText(void);
// 状态打包成 int32（位布局见 include\mcdr_abi.h 的 mcdr_dx12FgState）：回滚会在 native 侧把期望的
// mode 清成 0，而 JVM 的 options.txt 仍写着「开」，只读 options.txt 的界面会说假话。
int32_t fgStatePacked(void);

// ---- 链归属：d3d12_surface.cpp 在 DXGI 工厂升级成 SL 代理工厂成功之后写一次，provider 改不动它 ----
void fgNoteChainOwner(int32_t owner);
int32_t fgChainOwner(void);
// 链归属的**无锁镜像**，供 `fillCaps` 这类"可能已经持有注册表锁"的路径用。权威值仍是 fgChainOwner()；
// 读侧不加锁是为了避免 std::mutex 不可重入造成的每帧死锁。
int32_t fgChainOwnerFast(void);

// 包装型后端的链交接：fgWrapPresentChain 只在 picked 是包装型后端时动手（返回 1 = *outPresentChain 已
// 指向代理链，调用方负责持有）；fgUnwrapPresentChain 必须在销毁/重建链之前调用（context 绑在那条链上）。
int32_t fgWrapPresentChain(void* device, void* queue, void* appChain, int32_t width, int32_t height, int32_t format,
                           void** outPresentChain);
// 拆掉包装链并把链归属记回应用原生；销毁 / 重建链之前必须调。
void fgUnwrapPresentChain(void);

// 把本帧的输入 tag 交给包装型 provider。注册表侧持锁调用，provider 内部不得回头调注册表（std::mutex
// 不可重入）。两个入口：SR 通用路径把 mcdr_sr_dispatch 折成 FgFrameInputs；DLSS 路径自己填。
FgFrameInputs fgMakeFrameInputs(CommandContext* ctx, const mcdr_sr_dispatch& sr);
void fgTagFrameInputs(const FgFrameInputs& inputs);

// 登记/撤销 UI 纹理句柄（Dx12Object*）。注册表侧持锁保存，provider 在 tag 时读取。
void fgSetUiTexture(void* object);
void fgSetFrameConstants(const mcdr_fg_constants* constants);

// hudless 呈现模式：只记期望值并转给当前 provider；登记表在 provider 后建时回放（与 uiTexture 同一
// 条纪律）。
void fgSetUiComposition(int32_t enabled);
// 1 = provider 在 && hudless 模式在 && 它确认"UI 会被合成"。拿不准时必须返回 0，否则 UI 会整帧消失。
int32_t fgUiCompositionReady(void);

// ---- 链重建请求：每次开关 DLSS-G 都要重建 swapchain，否则关掉之后 SL 的呈现路径上还留着一层开销。
// fgSetMode 只挂请求（任意线程），acquireSurface（帧首）取走并重建，再 fgNoteChainRebuildDone 记账。
// 重建必须在**帧首** —— Present 时本帧 back buffer 刚画完。 ----
bool fgChainRebuildPending(void);
// ok != 0：链已按新的开关状态重建（chainMode 追平期望值、rebuilds +1）；ok == 0：重建失败，请求撤销
// 但 chainMode 不动 —— 下次用户改开关时会再试。
void fgNoteChainRebuildDone(int32_t ownerAfterRebuild, int32_t ok, const char* reason);

// 包装失败后的收场：此时应用链已被 SDK 拿走并销毁（Detach 之后回不来），照旧返回错误会让 surface 停在
// 「没有链」（游戏在跑、画面不上屏）。先调这里：关期望、链归属记回原生、拆 provider，再重建纯应用链。
void fgAbortWrapAfterFailure(const char* reason);

// 链重建属于**帧边界**职责，不能只挂在 acquireSurface 上 —— 那个函数由 JVM 在特定渲染路径才调，
// rebuildPending 置真之后可能永远没人消费。
mcdr::Result<void> rebuildSwapChainForFg(Dx12Surface* s, const char* why);

// ---- 后端工厂：每家一个实现单元，注册表按需懒建。DLSS-G 的 provider 是对既有
// mcdr_dlss_set_fg_mode / mcdr_dlss_fg_presented 的薄包装（行为等价性优先）。 ----
std::unique_ptr<IFrameGenProvider> makeDlssGProvider();
// caps 的 DLSS-G 分支。**不建上下文**：只回答"能不能用、为什么不能"。
void fillDlssGCaps(mcdr_fg_caps& out);

// 包装型后端的工厂与能力表，每家一个实现单元：
//   src\advanced\d3d12_xessfg.cpp —— XeSS-FG（libxess_fg.dll + libxell.dll），包装应用链；
//   src\advanced\d3d12_fsffg.cpp —— FSR-FG（amd_fidelityfx_framegeneration_dx12.dll）。
// 这两家要**接管 Present 的链**，只能在应用原生链上用（chainOwner == MCDR_FG_CHAIN_APP_NATIVE）：SL
// 代理工厂建的链不能交给它们，两种集成方式互斥。
std::unique_ptr<IFrameGenProvider> makeXessFgProvider();
std::unique_ptr<IFrameGenProvider> makeFsrFgProvider();
// 包装型后端的 caps 分支（不建上下文）。
void fillXessFgCaps(mcdr_fg_caps& out);
void fillFsrFgCaps(mcdr_fg_caps& out);
// 运行库是否已加载（只回答"DLL 在不在"，不代表可用）。用于诊断与 caps.runtimeLoaded。
bool xessFgRuntimeAvailable();
// FSR-FG 运行库是否载入。
bool fsrFgRuntimeAvailable();

// ---- 链策略：当前 pick 的后端（0=NONE）。改链前必须先问它：包装型后端要求应用原生链。 ----
int32_t fgPickedBackend(void);
// 1 = 当前后端要求「应用原生链」（FSR-FG / XeSS-FG）。createDxgiFactory 据此决定要不要升级成 SL 代理
// 工厂 —— 一旦选中包装型后端，代理工厂必须让位，否则 SDK 拿到的是 SL 的链、包装会失败。
bool fgWantsAppNativeChain(void);

// ---- Draw 命令录制：渲染 pass 内的 draw 全链路（对应官方 VulkanRenderPass）----
// setPipeline / bindTexture / setUniform / enableScissor / setVertexBuffer / setIndexBuffer /
// drawIndexed / draw / multiDraw* / drawIndexedIndirect / drawIndirect。
mcdr::Result<void> setPipeline(CommandContext* ctx, Dx12Pipeline* pipeline, bool hasDepth);
// RSSetScissorRects(1)（x,y,w,h）。
mcdr::Result<void> setScissor(CommandContext* ctx, int x, int y, int w, int h);
// RSSetViewports(1)（x,y,w,h；depth 恒 0..1）。MC 26.2 把 viewport 提升为 RenderPassBackend 上的独立
// 方法 setViewport(int,int,int,int)，原先只有 beginRenderPass 会设视口，这里补一个等价入口。
mcdr::Result<void> setViewport(CommandContext* ctx, int x, int y, int w, int h);
// IASetVertexBuffers：slot 0..15；stride 来自管线的 vertex format。
mcdr::Result<void> setVertexBuffer(CommandContext* ctx, int slot, Dx12Object* buffer, long long offset,
                                   int stride);
// IASetIndexBuffer（indexType：0=SHORT(R16_UINT)，1=INT(R32_UINT)）。
mcdr::Result<void> setIndexBuffer(CommandContext* ctx, Dx12Object* buffer, int indexType);
// 瞬时描述符绑定（对应官方 pushDescriptors）：
//   type 0 = CBV（buffer + offset + length，offset 须 256 对齐）
//   type 1 = SRV（复制 texture view 的现有描述符）
//   type 2 = SRV（texel buffer，按 texelFormat 建 Buffer SRV）
struct DrawBinding {
    uint8_t type = 0;
    Dx12Object* buffer = nullptr;
    long long offset = 0;
    long long length = 0;  // CBV length（内部向上取整 256）
    int texelFormat = 0;  // TEXEL SRV 的 GpuFormat ordinal
    Dx12Object* view = nullptr;
};
// CBV 走 root descriptor（SetGraphicsRootConstantBufferView，不占堆槽位），SRV 写入本帧 drawHeap
// 瞬时槽位并 SetGraphicsRootDescriptorTable(cbvCount)。
mcdr::Result<void> pushDescriptors(CommandContext* ctx, const std::vector<DrawBinding>& bindings);

mcdr::Result<void> drawIndexedInstanced(CommandContext* ctx, UINT indexCount, UINT instanceCount,
                                        INT startIndexLocation, INT baseVertexLocation,
                                        UINT startInstanceLocation);
mcdr::Result<void> drawInstanced(CommandContext* ctx, UINT vertexCount, UINT instanceCount, UINT firstVertex,
                                 UINT startInstanceLocation);
// ExecuteIndirect（DrawIndexedInstanced / DrawInstanced command signature）。
mcdr::Result<void> drawIndexedIndirect(CommandContext* ctx, Dx12Object* commands, long long offset,
                                       UINT drawCount);
mcdr::Result<void> drawIndirect(CommandContext* ctx, Dx12Object* commands, long long offset, UINT drawCount);

// meshlet 版地形派发。绑定布局 = createMeshRootSignature(meshExtras=1)：b8 小常量、t14 顶点池（28 B/
// 顶点）、t15 段表（32 B/段）、t16 meshlet 表（32 B/条），三个都是 RAW SRV；一次 DispatchMesh 覆盖整批。
mcdr::Result<void> drawMeshlets(CommandContext* ctx, Dx12Object* vertexBuffer, long long vertexOffset,
                                Dx12Object* sections, long long sectionsOffset, long long sectionsLength,
                                Dx12Object* meshlets, long long meshletsOffset, long long meshletsLength,
                                Dx12Object* globals, long long globalsOffset, int meshletCount);

// ---- Surface：DXGI swapchain（镜像官方 VulkanGpuSurface）。PresentMode 序数 = 官方 ordinal：
// IMMEDIATE=0, MAILBOX=1, FIFO=2, FIFO_RELAXED=3；DXGI FLIP 支持 {0, 2, 3}（MAILBOX 无直接对应）。 ----
constexpr UINT kSurfaceBufferCount = 3;  // 镜像官方 minImageCount max(3, min)

struct Dx12Surface {
    ComPtr<IDXGISwapChain3> swapChain;
    uintptr_t hwnd = 0;  // 重建 swapchain 时需要；ResizeBuffers 失败时销毁重建用
    // RTSS/OSD 修复：GLFW 主窗口一旦用 GL 呈现过画面，DXGI 就永久拒绝对该 HWND 创建 flip-model swapchain
    // （E_ACCESSDENIED 0x80070005）。故呈现改用本 surface 自建的子窗口（无 GL、不含 CS_OWNDC），同时让
    // OSD 注入器能从 GetDesc1().OutputWindow 拿到句柄。nullptr = 旧行为。
    HWND presentWindow = nullptr;
    // 承载 swapchain 的子窗口宿主（mcdr_window_host.h 的不透明句柄）。非空表示 presentWindow 是它
    // 创建的、从未接触过 GL 的子窗口；销毁 surface 时 release。
    void* host = nullptr;
    // 宿主子窗口是否已经显示（第一帧 Present 成功后置真）。
    bool hostRevealed = false;
    UINT width = 1;
    UINT height = 1;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    // 创建 swapchain 时真正用的格式。ResizeBuffers / 重建都必须传同一个值，否则 DXGI_ERROR_INVALID_CALL。
    DXGI_FORMAT swapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    int presentMode = 2;  // 默认 FIFO
    int currentImageIndex = -1;  // 最近 acquire 的 back buffer
    // 最近一次 blit 写入的 back buffer 下标。present 后 currentImageIndex 被重置为 -1，而 readback 要
    // 读"本帧已 blit 的 buffer"；用 GetCurrentBackBufferIndex() 兜底会读到下一帧尚未写入的（假黑屏）。
    int lastBlitIndex = -1;
    // 本帧是否真的把内容写进过当前 back buffer。presentSurface 据此决定是否 Present —— vanilla 的
    // blit/present 门与我们后端是否 blit 成功脱钩，跳过 blit 仍会 present ⇒ flip 到本帧从未写过的 buffer。
    // 不变式：acquire 成功置 false → blit 成功置 true → present 后置 false。必须是原子量（主线程写、
    // 渲染线程读）。
    std::atomic<bool> blittedThisFrame{false};
    // 本 surface 是否真的持有「已 acquire 但尚未 present」的 back buffer：acquire 成功置 true、present 末尾置
    // false；configureSurface 只在 exchange(false) 为 true 时才用 Present(0,0) 释放。不能用 currentImageIndex
    // 判断（渲染线程写 -1、主线程读是数据竞争，可能对已 present 的 buffer 再 Present 一次 → OSD 画两遍）。
    std::atomic<bool> bufferHeld{false};
    // 最近一次 Present 报告 suboptimal（OCCLUDED / MODE_CHANGED 等），上层据此重建 surface。
    bool suboptimal = false;
    // 是否处于真独占全屏。destroySurface 必须先 SetFullscreenState(FALSE)，否则释放链之后显示模式
    // 会停在独占状态，其它程序黑屏 / 分辨率不回退。
    bool exclusive = false;
    // 原生 HDR：是否以 scRGB（R16G16B16A16_FLOAT + G10 color space）呈现。hdrSdrWhiteScale =
    // SDRWhiteLevel/80（scRGB 里 1.0 = 80 nits），由 blit 的根常量喂给 PS；hdrMaxLuminance 仅用于日志。
    bool hdrActive = false;
    float hdrSdrWhiteScale = 1.0f;
    float hdrMaxLuminance = 0.0f;
    // 是否允许 tearing（创建链时带 ALLOW_TEARING；ResizeBuffers 的 flags 必须与之一致，否则 INVALID_CALL）。
    bool allowTearing = false;
    // fallback：CreateSwapChainForHwnd 被拒（E_ACCESSDENIED 0x80070005）时改用 CreateSwapChainForComposition
    // + DirectComposition 呈现；此时不支持 ALLOW_TEARING。声明顺序保证析构先释放 visual/target/device 再放链。
    bool compositionMode = false;
    ComPtr<IDCompositionDevice> dcompDevice;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;
    std::vector<ComPtr<ID3D12Resource>> backBuffers;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
    // per-backbuffer fence 值：submitCommandList 记录本帧写入的值，acquireSurface 据此判断重用的
    // back buffer 是否仍被 GPU 使用。
    std::vector<UINT64> surfaceFences;
    // per-surface present fence：每次 presentSurface() 后递增并 signal。IMMEDIATE + FLIP_DISCARD 下
    // deviceWaitIdle **不等** display controller，靠它避免 backbuffer 被 DWM 读着时释放（CORRUPTION）。
    std::vector<UINT64> surfacePresentFences;
    // 需要回读画面时用 ABI 的 mcdr_dx12ReadbackSurfacePixels 手动触发，不要往每帧路径里塞诊断代码。
};

// 当前 active surface（单例，渲染线程访问）。submit 时写入 per-backbuffer fence。
extern Dx12Surface* gActiveSurface;
inline void setActiveSurface(Dx12Surface* s) { gActiveSurface = s; }
inline Dx12Surface* getActiveSurface() { return gActiveSurface; }
// async 渲染线程 handle（供 destroySurface 在关闭渲染线程后等待 GPU 空闲使用）。
extern std::thread gRenderThread;
// 渲染线程运行标志（供 destroySurface 判断是否需要等待线程退出）。
extern bool gRenderRunning;
// 当前正在处理的 ctx（nullptr = 无活动帧），供诊断读回检测 ASYNC 管道活跃。
extern CommandContext* gAsyncRenderCtx;
// 本帧的 Present 是否已由渲染线程执行（true = Java 侧 present() 必须跳过，否则重复 Present → 窗口闪色）。
// 渲染线程 present 后置位，Java present() 消费后清位。
extern bool gAsyncOwnsPresent;

// 从 rtvHeap 分配一个 RTV CPU 句柄（surface 的 back buffer 用）。gNextRtv 不回收。
mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> allocRtvHandle();

// 创建 swapchain（FLIP_DISCARD + ALLOW_TEARING，1x1 占位，configure 时 ResizeBuffers）。
mcdr::Result<void*> createSurface(uintptr_t hwnd);
// 取该 surface 支持的 present modes（官方枚举序数数组，MAILBOX 不支持）。
std::vector<int> surfacePresentModes();
// ResizeBuffers + 重新取 back buffers + 重建 RTV。force=true 时即使尺寸没变也重建（进出真独占后
// 必须 ResizeBuffers，否则 Present 仍返回 S_OK 但 flip 链停在旧帧 —— 画面冻结）。
mcdr::Result<void> configureSurface(Dx12Surface* s, int width, int height, int presentMode,
                                    bool force = false);

// 真独占全屏开关（Java 侧把 Window.isExclusiveFullscreen() 推过来）。
mcdr::Result<void> setExclusiveFullscreen(Dx12Surface* s, bool on);

// ---- 原生 HDR：0 = auto（按 DXGI 输出色彩空间判定），1 = 强制开，2 = 强制关。必须在第一次 createSurface
// 之前调用（格式在创建 swapchain 时就定了）。 ----
void setHdrMode(int32_t mode);
// 最近一次创建的 surface 是否真的以 scRGB 呈现（0/1）。DLSS 的 colorBuffersHDR 用它。
int32_t hdrActiveFlag();
// 获取当前 back buffer index（DXGI FLIP 模型下 Present 内部同步）。
mcdr::Result<void> acquireSurface(Dx12Surface* s);
// 录制 blit（源纹理 → back buffer），在 command list 录制状态下调用；源与目标格式须一致
// （RGBA8/BGRA8 族可直接拷贝）。返回 Error 表示未录制。
mcdr::Result<void> blitSurface(CommandContext* ctx, Dx12Surface* s, Dx12Object* srcTex);
// 把一张纹理画到另一张纹理上（整屏三角形 + 线性采样，允许两边尺寸不同）：GUI 的独立全尺寸目标不能走
// 1:1 的 copyTextureToTexture。结束状态：源回 RENDER_TARGET，目标停在 RENDER_TARGET。
mcdr::Result<void> blitTextureToTexture(CommandContext* ctx, Dx12Object* src, Dx12Object* dst);
// src -> dst 5-tap unsharp。amount <= 0 时调用方应改用 blitTextureToTexture。
mcdr::Result<void> sharpenTextureToTexture(CommandContext* ctx, Dx12Object* src, Dx12Object* dst,
                                          float amount);
// 取（或创建）纹理的帧级瞬态 RTV —— blitTextureToTexture 要把目标当渲染目标用。
mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> acquireTextureRtv(CommandContext* ctx, Dx12Object* tex);
// Present(interval, flags)：FIFO=Present(1,0)；IMMEDIATE=Present(0,0)；FIFO_RELAXED=Present(1,ALLOW_TEARING)。
void presentSurface(Dx12Surface* s);
// 销毁 surface（先关全屏、等 GPU 完成，再释放交换链 / RTV / 窗口宿主）。
void destroySurface(Dx12Surface* s);
// 仅清理表面，不调用 deviceWaitIdle（供 C++ 内部在 render thread 已停止后使用）。
void destroySurfaceNoWaitIdle(Dx12Surface* s);
// 把当前 back buffer 读回 CPU 并打印 3x3 采样点 RGBA（内部先等 GPU 空闲），用于确认画面实际颜色。
mcdr::Result<void> readbackSurfacePixels(Dx12Surface* s);
// 返回当前 acquire 的 back buffer 原始指针，供 Java 侧直接用于 blit 源或 readback（不要用缓存的旧
// lastColorTextureHandle）。
uintptr_t getBackBufferHandle(Dx12Surface* s);
// 返回当前渲染 pass 中第一个活跃颜色附件的纹理句柄；不在 pass 中返回 0。避免 resize 后句柄指向
// 旧纹理。
uintptr_t getActiveColorTextureHandle(CommandContext* ctx);
// 读回任意纹理 3x3 采样点 RGBA（tag 标注来源，用于区分 draw 未写入 vs blit 丢失）。
void dbgReadbackTexturePixels(Dx12Object* tex, const char* tag);
// 把 RGBA8 像素存成 BMP（32bpp，覆盖写当前工作目录 dx12_dump_<tag>.bmp）并在日志打印 ASCII 缩略图。
void dbgDumpPixelsToFile(const uint8_t* rgba, UINT w, UINT h, UINT64 pitch, const char* tag);
// 读回 buffer 内容（前若干 float + hex）：UPLOAD 直接 Map，DEFAULT 经 readback staging。
void dbgReadbackBufferBytes(Dx12Object* buf, long long offset, int len, const char* tag);

// 阻塞等待 GPU 队列上所有已提交命令执行完（销毁 swapchain / 设备前必须调，否则资源被 GPU 用着时
// 释放 → DXGI_ERROR_DEVICE_REMOVED）。
mcdr::Result<void> deviceWaitIdle();

// 自检：创建 texture/buffer/sampler/view 各一，验证资源层可用后销毁。返回描述字符串（成功/失败明细）。
std::string runResourceSelfTest();

// 枚举所有 D3D12 适配器，返回 JSON 数组：
// [{"name":"...","luid":0x...,"vid":0x...,"did":0x...,"vram_gb":N,...},...]
std::string enumerateAdaptersJson();

// 活跃设备所绑定适配器的完整描述，单行扁平 JSON（键见 mcdr_abi.h 的 mcdr_dx12GetAdapterInfo）。没有
// 活跃设备时返回空串。驱动版本 / 分类来自 DXCore，不可用时回落 ""/false，其余来自 DXGI。
std::string adapterInfoJson();
}  // namespace d3d12_mc
