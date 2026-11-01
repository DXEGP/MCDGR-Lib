// mcdr_abi.h — public C ABI of the MCDR native render library: the only contract between the
// native library and the JVM side (FFM downcalls, no JNI). Full contract, return-value tables
// and struct mirrors: docs\abi\README.md.
//
// Violating any of these breaks the build or corrupts memory:
//   * `extern "C"` only; every aggregate is mirrored 1:1 by MemoryLayout (no packed blobs).
//   * Handles are opaque pointers; strings are caller-allocated UTF-8 and the callee returns the
//     required count including the NUL; arrays are (pointer, count) pairs.
//   * No exceptions cross: int32_t 1/0 plus mcdrLastError().
//   * MSVC `long` is 32-bit: int32_t here is JAVA_INT (int64_t/size_t/pointers are JAVA_LONG).

#ifndef MCDR_ABI_H
#define MCDR_ABI_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(MCDR_EXPORTS)
#define MCDR_API __declspec(dllexport)
#else
#define MCDR_API __declspec(dllimport)
#endif
#define MCDR_CALL __cdecl
#else
#define MCDR_API __attribute__((visibility("default")))
#define MCDR_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever ANY struct layout or signature below changes — including pure additions, because the Java
// side uses this number to tell whether the DLL has the new entry points. A mismatch aborts backend creation
// at load time, before the first draw. Bumping requires the five changes in docs\abi\README.md §4.1.
// 7 -> 8：新增 mcdr_rt_geometry 与七条加速结构入口（S3 的 BLAS/TLAS），旧入口签名一个都没改。
// 8 -> 9：新增 mcdr_rt_hit_group 与三条光线追踪状态对象入口（RTPS / 着色器标识符 / DispatchRays）。
// 9 -> 10：新增 mcdr_dx12GetBufferAddress（加速结构实例描述要的是缓冲的 GPU 地址，不是对象句柄）。
// 10 -> 11：新增 mcdr_dx12NrdInspect（读 NRD 自己算出来的 InstanceDesc，不进设备路径）。
// 11 -> 12：新增 mcdr_dx12NrdInit / mcdr_dx12NrdShutdown（NRD 实例、PSO、纹理池、描述符堆常驻）。
// 12 -> 13：新增 mcdr_dx12NrdDenoise（逐帧降噪派发）。
// 13 -> 14：mcdr_dx12NrdDenoise 多一个 resetHistory（1 = 这一帧丢历史）。
// 14 -> 15：新增 mcdr_dx12DlssdEval（DLSS-D 逐帧求值，走 Streamline）。
// 15 -> 16：mcdr_dx12DlssdEval 多一个 constants（slSetConstants 的 72 个浮点）。
#define MCDR_ABI_VERSION 16u

// Same value at runtime, so a stale Java constant fails at load time, not at first draw.
MCDR_API uint32_t MCDR_CALL mcdrABIVersion(void);

// ---- Opaque handles ------------------------------------------------------------
// Each typedef names the real C++ class, so native reinterpret_casts with no wrapper. Two traps: the tag must
// use the SAME class-key as its definition (MSVC C4099 is an error under TreatWarningAsError), and the forward
// declarations MUST live inside namespace d3d12_mc — a global `struct Dx12Object;` is a different type (C2664).
#ifdef __cplusplus
namespace d3d12_mc {
class PartitionedDescriptorAllocator;
class BundleRecorderPool;
class MainCommandExecutor;
class AsyncFenceManager;
struct Dx12Object;
struct CommandContext;
struct Dx12Pipeline;
struct Dx12Surface;
struct QueryPool;
}  // namespace d3d12_mc

typedef d3d12_mc::Dx12Object mcdr_object;       // texture / buffer / sampler / texture view
typedef d3d12_mc::CommandContext mcdr_command_ctx;  // one command encoder (3-deep list ring)
typedef d3d12_mc::Dx12Pipeline mcdr_pipeline;
typedef d3d12_mc::Dx12Surface mcdr_surface;
typedef d3d12_mc::QueryPool mcdr_query_pool;
typedef d3d12_mc::PartitionedDescriptorAllocator mcdr_desc_alloc;
typedef d3d12_mc::BundleRecorderPool mcdr_bundle_pool;
typedef d3d12_mc::MainCommandExecutor mcdr_executor;
typedef d3d12_mc::AsyncFenceManager mcdr_fence_mgr;
#else
typedef struct mcdr_object mcdr_object;
typedef struct mcdr_command_ctx mcdr_command_ctx;
typedef struct mcdr_pipeline mcdr_pipeline;
typedef struct mcdr_surface mcdr_surface;
typedef struct mcdr_query_pool mcdr_query_pool;
typedef struct mcdr_desc_alloc mcdr_desc_alloc;
typedef struct mcdr_bundle_pool mcdr_bundle_pool;
typedef struct mcdr_executor mcdr_executor;
typedef struct mcdr_fence_mgr mcdr_fence_mgr;
#endif

// ---- Cross-language aggregates -------------------------------------------------
// Offsets and sizes are asserted at runtime by mcdrABILayout(), never by hand.

// One pipeline binding slot, shared by PushDescriptors and the bundle recorder.
typedef struct mcdr_binding_slot {
    int32_t type;          // 0 = CBV, 1 = SRV (texture view), 2 = SRV (texel buffer)
    int32_t texelFormat;   // type 2 only: GpuFormat ordinal
    mcdr_object* buffer;        // type 0/2 only
    mcdr_object* view;          // type 1 only
    int64_t offset;        // type 0/2 only
    int64_t length;        // type 0/2 only
} mcdr_binding_slot;

// One draw's index/vertex geometry, used by the bundle recorder.
typedef struct mcdr_partition_draw {
    mcdr_object* indexBuffer;
    mcdr_object* vertexBuffer;
    int64_t vertexOffset;
    int32_t indexType;     // 0 = SHORT (R16_UINT), 1 = INT (R32_UINT)
    int32_t indexCount;
    int32_t firstIndex;
    int32_t baseVertex;
    int32_t vertexSlot;
    int32_t vertexStride;
    int32_t _pad0;
} mcdr_partition_draw;

// One colour target of a graphics pipeline; format == -1 means "unused slot".
typedef struct mcdr_color_target_desc {
    int32_t format;             // GpuFormat ordinal, or -1
    uint8_t writeMask;
    uint8_t blendEnabled;
    uint8_t srcColor;           // BlendFactor ordinal
    uint8_t dstColor;           // BlendFactor ordinal
    uint8_t colorOp;            // BlendOp ordinal
    uint8_t srcAlpha;           // BlendFactor ordinal
    uint8_t dstAlpha;           // BlendFactor ordinal
    uint8_t alphaOp;            // BlendOp ordinal
} mcdr_color_target_desc;

typedef struct mcdr_depth_state_desc {
    int32_t depthFormat;        // GpuFormat ordinal
    uint8_t depthWrite;
    uint8_t depthCompareOp;     // CompareOp ordinal
} mcdr_depth_state_desc;

// HLSL input element. semanticName is NUL-terminated; NULL or "" -> "TEXCOORD".
typedef struct mcdr_input_element_desc {
    int32_t location;
    int32_t binding;
    int32_t format;         // GpuFormat ordinal
    int32_t offset;
    int32_t stride;
    int32_t stepRate;       // 0 = per-vertex, 1 = per-instance
    int32_t _pad0;
    const char* semanticName;   // NUL-terminated; NULL or "" -> "TEXCOORD"
} mcdr_input_element_desc;

typedef struct mcdr_pipeline_binding_desc {
    uint8_t type;               // 0 = CBV, 1 = SRV + static sampler, 2 = SRV
    uint8_t reg;
} mcdr_pipeline_binding_desc;

// Full graphics-pipeline description. vsData/psData point at DXBC bytecode (no NUL terminator).
typedef struct mcdr_pipeline_desc {
    const void* vsData;
    int32_t vsLen;
    int32_t _pad0;
    const void* psData;
    int32_t psLen;
    int32_t colorCount;
    const mcdr_color_target_desc* colorTargets;
    int32_t hasDepth;
    int32_t _pad1;
    mcdr_depth_state_desc depth;
    int32_t topology;       // PrimitiveTopology ordinal
    int32_t cullEnabled;
    int32_t polygonMode;    // PolygonMode ordinal
    int32_t inputElementCount;
    const mcdr_input_element_desc* inputElements;
    int32_t bindingCount;
    int32_t _pad2;
    const mcdr_pipeline_binding_desc* bindings;
} mcdr_pipeline_desc;

// ---- Mesh-shader (amplification / mesh) pipeline description -------------------
// asData/msData/psData point at DXIL from DXC (as_6_x / ms_6_x / ps_6_x), NOT DXBC: mesh shaders cannot go
// through D3DCompile (SM 5.1 ceiling). msData is required, asData/psData may be NULL/0. Bindings reuse
// mcdr_pipeline_binding_desc with the SAME layout contract as mcdr_pipeline_desc, so PushDescriptors is unchanged.
typedef struct mcdr_mesh_pipeline_desc {
    const void* asData;         // DXIL amplification shader bytecode, or NULL
    int32_t asLen;
    int32_t _pad0;
    const void* msData;         // DXIL mesh shader bytecode (required)
    int32_t msLen;
    int32_t _pad1;
    const void* psData;         // DXIL pixel shader bytecode, or NULL
    int32_t psLen;
    int32_t colorCount;
    const mcdr_color_target_desc* colorTargets;
    int32_t hasDepth;               // same meaning as mcdr_pipeline_desc::hasDepth
    int32_t _pad2;
    mcdr_depth_state_desc depth;    // same layout as mcdr_pipeline_desc::depth
    int32_t cullEnabled;
    int32_t polygonMode;            // PolygonMode ordinal
    int32_t bindingCount;
    int32_t _pad3;
    const mcdr_pipeline_binding_desc* bindings;
} mcdr_mesh_pipeline_desc;

// ---- ABI self-check ------------------------------------------------------------
// One entry per mirrored struct field; the Java side walks the same list and fails on the first mismatch.
typedef struct mcdr_abi_field {
    const char* name;   // "mcdr_binding_slot.buffer"
    uint64_t offset;
    uint64_t size;
} mcdr_abi_field;

MCDR_API void MCDR_CALL mcdrABILayout(const mcdr_abi_field** fields, int32_t* count);

// ---- Diagnostics ---------------------------------------------------------------

// Copies the calling thread's last error into `buf` as UTF-8 and returns the required byte
// count including the NUL, or 0 when none is pending. buf == NULL / cap == 0: size query.
MCDR_API int32_t MCDR_CALL mcdrLastError(char* buf, int32_t cap);

// Verbosity for the native log, mirroring DX12_LOG_VERBOSE.
MCDR_API void MCDR_CALL mcdrSetLogVerbose(int32_t enabled);

// Native log level: 0=error, 1=warning, 2=info, 3=debug.
MCDR_API void MCDR_CALL mcdrSetLogLevel(int32_t level);

// Progress markers on the shutdown path are INFO lines. 1 (default) keeps them visible at any
// log level, 0 makes them plain INFO. Config: `[logging] shutdownTrace`.
MCDR_API void MCDR_CALL mcdrSetShutdownTraceAlways(int32_t always);

// 把每条 native 日志镜像到该文件（配置项 `[logging] nativeLogFile`）：UTF-8 路径、相对路径按进程
// 工作目录解析、追加写、逐行 flush；传 NULL 或空串关闭镜像。返回 1=已配置 0=已关闭（不同步 open）。
MCDR_API int32_t MCDR_CALL mcdrSetLogFile(const char* utf8Path);

/// Sets (or clears, with a null/empty value) one process-wide environment variable so the remaining getenv()-
/// driven native switches can be driven from config. Must be called before the first device is created.
MCDR_API void MCDR_CALL mcdr_dx12SetEnvironment(const char* name, const char* value);

// PIX GPU capture: load WinPixGpuCapturer.dll into this process. It hooks D3D12, so it must be in place BEFORE
// the first device is created; irreversible for the life of the process. Returns 1 = loaded, 0 = not requested,
// -1 = requested but not found/loadable (an ERROR lists the paths searched). Idempotent.
MCDR_API int32_t MCDR_CALL mcdrSetPixCapture(int32_t enabled);

// D3D12 debug layer (the [debug] debugLayer config). It is enabled once, while the first device is created, so
// this must arrive before that point; later calls change nothing. Use this instead of mcdr_dx12SetEnvironment:
// the JVM's _putenv_s is not guaranteed to be visible to this module's getenv(). Returns 1 = recorded.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDebugLayer(int32_t enabled);

// Where PIX's DLLs live (the [debug] pixDirectory config: a DLL or the directory holding them). Call it before
// mcdrSetPixCapture — the layer loads before the first device, so the path must be known that early. Empty
// string clears it and restores automatic detection. Returns 1 = accepted, 0 = cleared, -1 = invalid/missing.
MCDR_API int32_t MCDR_CALL mcdrSetPixDirectory(const char* utf8Path);

// PIX timing capture driven from inside the process: PIXBeginCapture(PIX_CAPTURE_TIMING, {file_name}) ...
// PIXEndCapture(0). The runtime and capturer DLLs are loaded by mcdrSetPixCapture, or lazily here.
//
// `utf8Path` is the full path of the .wpix; the caller creates the directory. Runs on its own thread: PIX's
// runtime sets COM up on load and the render thread's apartment is fixed (RPC_E_CHANGED_MODE).
// Returns 1 = started, 0 = one already running, otherwise the negative HRESULT as-is (E_FAIL, E_INVALIDARG
// for an empty path, HRESULT_FROM_WIN32(ERROR_TIMEOUT), E_ACCESSDENIED = not elevated, ERROR_ALREADY_EXISTS
// = a session from an earlier run is alive). Elevation is REQUIRED: `Performance Log Users` is not enough, and
// a gradle daemon keeps the non-elevated ancestry — use `--no-daemon`.
MCDR_API int32_t MCDR_CALL mcdrStartPixTimingCapture(const char* utf8Path);

// Stops the capture started above; PIX writes the file on its own thread, so this returns without waiting.
// Returns 1 = stop requested, 0 = nothing recording, negative HRESULT = the writer thread could not start.
MCDR_API int32_t MCDR_CALL mcdrStopPixTimingCapture(void);

// Variable-rate shading: the per-draw control point. `rate` is a raw D3D12_SHADING_RATE value (1X1=0x0,
// 1X2=0x1, 2X1=0x4, 2X2=0x5, 2X4=0x6, 4X2=0x9, 4X4=0xA), applied to every graphics list started afterwards;
// -1 turns it off (no RSSetShadingRate call at all). Requests before device creation are accepted and gated
// on the first command list with a WARN. Returns 1 = accepted, 0 = the device cannot honour it (no VRS, or
// 2X4/4X2/4X4 without AdditionalShadingRatesSupported), -1 = unknown rate.
MCDR_API int32_t MCDR_CALL mcdrSetShadingRate(int32_t rate);

// The **scoped** control point: issues RSSetShadingRate on `ctx`'s open list right now, for one of the seven
// raw values (-1 is *not* accepted). It neither reads nor writes the stored player setting, so the caller
// brackets exactly the draws that may be shaded coarsely and restores 1X1; legal mid-render-pass, because
// this module never uses BeginRenderPass. Returns 1 = issued, 0 = rate not honour-able or no
// ID3D12GraphicsCommandList5, -1 = unknown rate, -2 = ctx is null or has no open command list (caller bug).
MCDR_API int32_t MCDR_CALL mcdr_dx12SetShadingRateNow(mcdr_command_ctx* ctx, int32_t rate);

// The two shading-rate combiners used by every RSSetShadingRate here: PASSTHROUGH=0 OVERRIDE=1 MIN=2 MAX=3
// SUM=4; defaults {PASSTHROUGH, PASSTHROUGH}. Keep stage 0 at PASSTHROUGH until the mesh shader really writes
// SV_ShadingRate (a per-primitive 1X1 under MIN undoes every 2X2). Returns 1 = stored, -1 = outside 0..4.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetShadingRateCombiners(int32_t first, int32_t second);

// Anti-aliasing mode, same numbering as the Java-side option (0=OFF, 1=SMAA, 2=TAA, 3=DLAA). SMAA runs three
// full-screen passes over the world layer after the world and before the GUI (mcdr_dx12ApplySmaaToTexture);
// TAA keeps an RGBA16F history ping-pong in the device (mcdr_dx12ApplyTaaToTexture); DLAA is accepted but
// treated as OFF (a WARN names it). No restart needed — a change takes effect on the next frame. Returns 1 =
// in effect, 0 = accepted but not implemented, -1 = unknown mode. See docs\abi\README.md §6.2.
MCDR_API int32_t MCDR_CALL mcdrSetAntiAliasing(int32_t mode);

// DLSS upscaling on/off, independent of the anti-aliasing mode and of mcdr_dx12SetDlssVendorDir.
// 1 enables, 0 disables; idempotent and callable before the device exists (only the state is
// recorded). Returns the resulting state (1/0). Availability: mcdr_dx12QueryDlss.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssEnabled(int32_t enabled);

// Snapshot the world depth into a private D32 texture for DLSS, from the point where the depth still holds this
// frame's world depth — BEFORE vanilla's clearDepthTexture (GameRenderer.render) wipes it to 0.0. DLSS evaluates
// after the world, when the live depth is already cleared, which degenerates depth-aware upscaling into
// colour-only. `depth`'s state is restored on exit. 1 = taken/reused, 0 = unusable arguments, -1 = failed.
MCDR_API int32_t MCDR_CALL mcdr_dx12SnapshotDlssDepth(mcdr_command_ctx* ctx, mcdr_object* depth);

// The DLSS quality tier: 0 = Performance, 1 = Balanced, 2 = Quality (same order as the Java enum), default 2.
// Out of range is rejected with -1 and changes nothing. Idempotent, callable before the device exists. A change
// REBUILDS the feature even at an unchanged size: NVSDK_NGX_PerfQuality_Value is a CreateFeature input.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssQuality(int32_t tier);
// 帧生成（DLSS-G）。mode: 0=off 1=on（固定倍率） 2=dynamic；numFrames 1..5（见 slDLSSGGetState）。
// 选项在渲染线程的下一次 present 生效；Reflex 必须开启，否则 status 会带 eFailReflexNotDetectedAtRuntime。
MCDR_API void MCDR_CALL mcdr_dx12SetDlssgMode(int32_t mode, int32_t numFrames);

/* 上一次 Present 真正提交的帧数（1 = 没插帧）。F3 的 output 用它累积。 */
MCDR_API int32_t MCDR_CALL mcdr_dx12GetDlssgPresented(void);

// The DLSS *model*, exposed as NGX's render-preset hint.
//   preset: 0 = Default (NGX picks per tier: K for DLAA/Balanced/Quality, M for Performance, L for
//           Ultra Performance), 5 = E, 6 = F (deprecated CNN), 10 = J, 11 = K, 12 = L, 13 = M.
// Out of range is rejected with -1; otherwise the recorded value is returned. Idempotent, callable
// before the device exists. A change REBUILDS the feature (the preset is a CreateFeature input).
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssRenderPreset(int32_t preset);

// The UAV texture the Java side created, used as NGX's output. A texture this DLL creates itself cannot hold the
// write, so the Java side must supply a target-sized UAV texture. NULL clears the override and restores the
// private fallback; the resource is not owned natively. Size/format mismatch = one ERROR plus fallback.
// Returns 1 = accepted, 0 = cleared, -1 = not a live texture.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssOutput(mcdr_object* tex);

// ---- DLSS super-resolution (NVSDK_NGX): data plane + the real evaluate call ----
//
// Everything the per-frame Evaluate needs arrives in ONE struct. Two entry points:
// mcdr_dx12CreateDlssFeature (once per size tuple) and mcdr_dx12EvaluateDlss (per frame).
// Left at 0 on purpose: pInTransparencyMask (no per-pixel transparency pass), a GPU exposure texture
// (exposureMode = MCDR_DLSS_EXPOSURE_NONE, so exposureScale is used), per-object motion vectors (the velocity
// pass reprojects the camera only, so entities ghost slightly). Minecraft 26.x is REVERSE-Z (depth cleared to
// 0.0 = far, GREATER_THAN_OR_EQUAL everywhere), so depthInverted must be true. See docs\advanced\dlss.md.

// How the constant exposure value below is interpreted.
#define MCDR_DLSS_EXPOSURE_NONE 0u   // pure constant: exposureScale is the pre-exposure
#define MCDR_DLSS_EXPOSURE_AUTO 1u   // reserved: a GPU exposure texture (not available yet)

// Everything one DLSS Evaluate needs; pIn* are opaque mcdr_object* texture handles.
//
// LAYOUT (cl 19.51 / v145, x64): sizeof = 112, align = 8. _pad0 covers the bytes the compiler would insert
// before the first pointer, _pad1 keeps the total a multiple of 8; both are NAMED fields because MCDR_Layout.kt
// mirrors them and abi_check.py only sees named fields.
typedef struct mcdr_dlss_input {
    // --- identity / geometry of this frame ---
    int32_t renderWidth;   // INPUT size, px = window * dxrMcRenderScale (the DLSS render size)
    int32_t renderHeight;  // INPUT size, px
    int32_t targetWidth;   // OUTPUT size, px = window size = GUI target = swapchain (DLSS target size)
    int32_t targetHeight;  // OUTPUT size, px
// Jitter in pixels, RENDER space, not normalised and NOT negated (Halton(2,3), [-0.5,+0.5)).
    float jitterOffsetX;
    float jitterOffsetY;
// Motion-vector scale: multiplies the raw MV texture to get pixel-space motion (MV is already pixels -> 1.0).
    float mvScaleX;
    float mvScaleY;
// Pre-exposure: 1.0 = "already in display units", so keep 1.0.
    float preExposure;
// Constant exposure scale, read only when exposureMode == MCDR_DLSS_EXPOSURE_NONE.
    float exposureScale;
    uint32_t exposureMode;     // MCDR_DLSS_EXPOSURE_NONE (0) or _AUTO (1, reserved)
    uint32_t alphaUpscaling;   // 1 = upscale alpha too. False for the Minecraft world layer.
// Markers (diagnostics only): the JVM's authoritative values, cross-checked natively.
    float nearZ;               // camera near plane, from Projection.zNear()
    float farZ;                // camera far plane, from Projection.zFar()
    uint32_t depthInverted;    // 1 = reverse-Z (this renderer). Must stay 1.
    uint32_t reset;            // 1 = drop history this frame (first frame, resize, world switch)
    int32_t depthFormatOrdinal;  // GpuFormat ordinal of the depth attachment (diagnostics)
    int32_t _pad0;
    // --- the textures ---
    mcdr_object* pInColor;          // world layer, render size, colour (SRGB or UNORM)
    mcdr_object* pInDepth;          // main render target depth, render size, D32_FLOAT
    mcdr_object* pInMotionVectors;  // D3D12Velocity.kt output, render size, RG16_FLOAT
    mcdr_object* pInOutput;         // target-size colour target (the GUI's own target)
    int32_t _pad1;
    int32_t _pad2;   // trailing padding: keeps sizeof a multiple of the 8-byte alignment
} mcdr_dlss_input;

// Creates (or recreates, after a size change) the DLSS feature on the open list; call it before the
// first Evaluate for that size tuple. On failure it logs ONE warning and returns 0 for the rest of
// the process — keep the linear-blit path and do not retry. Returns 1 = feature handle ready.
MCDR_API int32_t MCDR_CALL mcdr_dx12CreateDlssFeature(mcdr_command_ctx* ctx,
                                                      const mcdr_dlss_input* in);

// Runs one DLSS Evaluate on the open list, after the world and before the GUI. Returns 1 = ran,
// 0 = skipped (mode is not DLSS, a size changed without a successful create, or a previous failure
// disabled DLSS), -1 = invalid arguments. Never blocks, never fails the frame (one WARN, then off).
MCDR_API int32_t MCDR_CALL mcdr_dx12EvaluateDlss(mcdr_command_ctx* ctx,
                                                 const mcdr_dlss_input* in);

// One-line DLSS availability query for the UI and the log; `note` gets a NUL-terminated reason when
// DLSS cannot run (NULL is fine). Returns 1 = can run, 0 = not available (or no device yet; `note`
// says why), -1 = invalid arguments. Safe before the device exists; only a one-time NGX init.
MCDR_API int32_t MCDR_CALL mcdr_dx12QueryDlss(char* note, int32_t noteCap);

// Tells native WHERE the vendor runtime DLLs live, as a UTF-8 absolute directory (`<user.dir>\dxrmc\`). That
// directory is NOT on the default DLL search path — the working directory is the game directory while the
// executable is the JDK's java.exe — so native widens the process search path (SetDefaultDllDirectories +
// SetDllDirectoryW + AddDllDirectory) before it loads the NGX runtime. Idempotent; 1 = accepted, 0 = rejected.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssVendorDir(const char* utf8Dir);

// ---- 超分（Super Resolution）抽象层 --------------------------------------------
// 不复用 mcdr_dlss_input：那张表的布局被 MCDR_Layout.kt 钉住且只服务 DLSS。这里把渲染/显示尺寸、jitter、
// MV 尺度、相机参数与可选资源收敛成一张与厂商无关的载荷，厂商差异下沉到 native 的 Provider。**纯追加**。
//
// 时序：SrPick（只记录）→ SrCreate（要求命令列表开着）→ SrEvaluate（逐帧）；SrQueryCaps / SrJitter /
// SrQuery 可随时调，SrDestroy 只允许在命令列表关闭时（帧首）。
//
// 资源指针一律是 **JVM 创建的纹理句柄**（Dx12Object*，即 D3D12GpuTexture.handle()），不是 ID3D12Resource*：
// 实现单元自建的纹理会掉出模块的 resourceState 表，读回全 0。传 0 = 没有。见 docs\advanced\upscalers.md。

#define MCDR_SR_BACKEND_NONE 0
#define MCDR_SR_BACKEND_DLSS 1
#define MCDR_SR_BACKEND_FSR 2
#define MCDR_SR_BACKEND_XESS 3

// 掩码能力位。三家语义不同（SL 的 ReactiveMaskHint、FFX 的 reactive/transparency、XeSS 的 responsive
// pixel mask），这里是并集，具体怎么喂由 Provider 决定。
#define MCDR_SR_MASK_REACTIVE 1
#define MCDR_SR_MASK_RESPONSIVE 2

// 后端能力面。sizeof = 80、align = 4（Kotlin 镜像：SR_CAPS）。note 是给 UI 与日志的短原因（NUL 结尾）。
typedef struct mcdr_sr_caps {
    int32_t backend;         // 0  = MCDR_SR_BACKEND_*
    int32_t available;       // 4  1 = 本机此刻可用
    int32_t coupledDenoise;  // 8  1 = 该后端自带降噪（DLSS-D / FSR4 ML）
    int32_t needsExposure;   // 12 1 = 需要曝光纹理
    int32_t takesMasks;      // 16 MCDR_SR_MASK_* 位集
    int32_t externalHeap;    // 20 1 = 接受调用方提供的临时堆
    int32_t jitterPhaseCount;// 24 0 = 无意见（沿用 D3D12Temporal 的 8）
    int32_t minRenderWidth;  // 28
    int32_t minRenderHeight; // 32
    int32_t maxRenderWidth;  // 36
    int32_t maxRenderHeight; // 40
    int32_t nativeAA;        // 44 1 = 允许 1.0x（DLAA 式）
    int32_t reserved;        // 48 保留，必须为 0（给后续 flags 留位，不动 sizeof）
    char note[28];           // 52 短原因，NUL 结尾（52 + 28 = 80）
} mcdr_sr_caps;

// 与厂商无关的每帧载荷。sizeof = 136、align = 8（Kotlin 镜像：SR_DISPATCH）。
//
// 约定（与 mcdr_dlss_input 一致，别改）：渲染尺寸是**渲染目标**尺寸、显示尺寸是**窗口**尺寸；jitter 是
// **渲染空间像素**、不归一化、不取负；motionVectorScale 在「MV 已是像素位移」时给 1.0。cameraAspectRatio /
// viewSpaceToMetersFactor 为 0 = "未填"（按窗口宽高比 / 1.0）；cameraVerticalFov 与 DLSS 的常量必须同源。
typedef struct mcdr_sr_dispatch {
    int32_t renderWidth;          // 0
    int32_t renderHeight;         // 4
    int32_t displayWidth;         // 8
    int32_t displayHeight;        // 12
    float jitterOffsetX;          // 16
    float jitterOffsetY;          // 20
    float motionVectorScaleX;     // 24
    float motionVectorScaleY;     // 28
    float preExposure;            // 32
    float frameTimeDeltaMs;       // 36
    float cameraNear;             // 40
    float cameraFar;              // 44
    float cameraVerticalFov;      // 48 弧度
    float sharpness;              // 52 0 = 不锐化
    int32_t reset;                // 56 1 = 丢弃历史
    int32_t hdrInput;             // 60 1 = 输入是 HDR 线性色
    int32_t backend;              // 64 MCDR_SR_BACKEND_*（0 = 用 SrPick 选的那个）
    int32_t flags;                // 68 保留，必须为 0
    void* pColorInput;            // 72 渲染尺寸颜色
    void* pColorOutput;           // 80 显示尺寸颜色（UAV 写）
    void* pDepth;                 // 88 渲染尺寸深度
    void* pMotionVectors;         // 96 渲染尺寸 RG16F
    void* pReactiveMask;          // 104 R8，可 0
    void* pExposure;              // 112 R32F，可 0
    int32_t depthFormatOrdinal;   // 120 与 mcdr_dlss_input 的同名字段同义
    float cameraAspectRatio;      // 124 显示尺寸的宽高比（FSR 可选、DLSS 常量里有同义字段）
    float viewSpaceToMetersFactor;// 128 1.0 = 一个世界单位算 1 米；0 = 未填（后端按 1.0 处理）
    int32_t _pad2;                // 132（补到 136 = align 8 的整数倍）
} mcdr_sr_dispatch;

// 记录期望后端。纯记录：不建、不销毁、不重建（重建在下一帧帧首）。1 = 已记录，0 = 编号未知，-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrPick(int32_t backend);

// 查询后端能力面。out 必须非空；*out 整体覆盖。1 = 已填（结论看 available），-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrQueryCaps(int32_t backend, mcdr_sr_caps* out);

// 建/唤醒后端，与 CreateDlssFeature 同点调用（命令列表必须开着）。maxRenderWidth/Height 是**上界**
// （热切换与 resize 时判要不要重建）。1 = 就绪，0 = 本机不可用（已 log），-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrCreate(mcdr_command_ctx* ctx, int32_t backend,
                                              int32_t maxRenderWidth, int32_t maxRenderHeight,
                                              int32_t displayWidth, int32_t displayHeight);

// 尺寸变化通知。返回 1 = 已接受，0 = 后端未就绪（忽略），-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrResize(mcdr_command_ctx* ctx, int32_t renderWidth,
                                              int32_t renderHeight, int32_t displayWidth,
                                              int32_t displayHeight);

// 逐帧提交。返回 1 = 已执行，0 = 跳过（后端未就绪 / 本帧不适用），-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrEvaluate(mcdr_command_ctx* ctx, const mcdr_sr_dispatch* desc);

// 该后端偏好的亚像素 jitter。1 = 已写 outX/outY，0 = 无意见（不修改，沿用 D3D12Temporal），-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrJitter(int32_t frameIndex, float* outX, float* outY);

// 该后端期望的 jitter 相位长度（0 = 不适用），参数非法返回 -1。DLSS/XeSS 报 8，FSR 报
// `8 * (displayWidth/renderWidth)^2` 并夹到 [1, 256]。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrJitterPhaseCount(int32_t backend, int32_t renderWidth,
                                                      int32_t displayWidth);

// 释放后端资源。只允许在没有打开命令列表时调用（帧首）；重复调用安全。
MCDR_API void MCDR_CALL mcdr_dx12SrDestroy(void);

// 一行可用性文本：当前/期望后端、各自 available、最近失败原因。1 = 已写，0 = 没有内容，-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrQuery(char* note, int32_t noteCap);

// ---- 帧生成（Frame Generation）抽象层 —— ABI v4 --------------------------------
// 与 SR 段同构（一张能力表 + 一份逐帧载荷），区别在于**没有 Evaluate**：帧生成的工作发生在 Present 的两侧，
// 不由我们的命令列表录制。DLSS-G（Streamline）占走 Present、链必须由 SL 代理工厂产出、必须 Reflex；
// FSR-FG / XeSS-FG **接管交换链**，与 SL 代理链二选一。见 docs\advanced\framegen.md。

#define MCDR_FG_BACKEND_NONE 0
#define MCDR_FG_BACKEND_DLSSG 1
#define MCDR_FG_BACKEND_FSRFG 2
#define MCDR_FG_BACKEND_XESSFG 3

// 呈现链当前归谁（可查询状态，不再是隐含的 if）。v1 里只有前两态可达。
#define MCDR_FG_CHAIN_APP_NATIVE 0
#define MCDR_FG_CHAIN_SL_PROXY 1
#define MCDR_FG_CHAIN_FSR_WRAP 2  // 占位：v1 不实现
#define MCDR_FG_CHAIN_XESS_WRAP 3 // 占位：v1 不实现

// 本后端架构上对应的低延迟方案（能力面，不是「运行时可用」）。Anti-Lag 2 与 XeLL 的契约没接，所以
// 这两位的位**恒为 0 / 原因写清**；Reflex 现役且 FG 依赖它。
#define MCDR_FG_LATENCY_NONE 0
#define MCDR_FG_LATENCY_REFLEX 1
#define MCDR_FG_LATENCY_ANTILAG2 2
#define MCDR_FG_LATENCY_XELL 3

// 后端能力面。sizeof = 80、align = 4（Kotlin 镜像：FG_CAPS）。note 是给 UI 与日志的短原因。
typedef struct mcdr_fg_caps {
    int32_t backend;            // 0  MCDR_FG_BACKEND_*
    int32_t available;          // 4  1 = 本机此刻可启用（唯一结论位）
    int32_t runtimeLoaded;      // 8  运行库已加载（头文件在 ≠ 这个）
    int32_t deviceSupported;    // 12 设备/驱动支持
    int32_t chainCompatible;    // 16 当前呈现链兼容
    int32_t needsReflex;        // 20 1 = 必须 Reflex 开（DLSS-G 恒 1）
    int32_t needsUiResource;    // 24 1 = 需要独立 UI 资源（FSR/XeSS）
    int32_t needsHudless;       // 28 1 = 需要独立 HUDless（FSR/XeSS）
    int32_t transitionsInputs;  // 32 1 = provider 自己转输入输出屏障
    int32_t maxGeneratedFrames; // 36 1..N（DLSS-G 当前 5）
    int32_t chainOwner;         // 40 MCDR_FG_CHAIN_*（当前实际状态，不是期望）
    int32_t reserved;           // 44 保留，必须为 0
    char note[28];              // 48 短原因，NUL 结尾（48 + 28 = 76）
// 本后端**架构上**该配哪个低延迟方案（MCDR_FG_LATENCY_*）；能不能用看 available / note。
    int32_t latencyProvider;    // 76 MCDR_FG_LATENCY_*
} mcdr_fg_caps;

// 与厂商无关的"设期望值"载荷。sizeof = 96、align = 8（Kotlin 镜像：FG_DISPATCH）。
//
// mode 与 mcdr_dx12SetDlssgMode 一致（0=off 1=固定倍率 2=dynamic；numFrames 只在 mode=1 有意义）。三轨帧号
// 不同源、别混用：appFrameId 应用帧、backendFrameId 给后端、presentId 给 present。资源指针是 **JVM 创建的
// 纹理句柄**（0 = 没有）；pHudlessColor 给 0 是 v1 常态（DLSS-G 复用 SR 的输出）。
/* 帧常量。归属 FG 抽象层自己（不是 DLSS 的 mcdr_dlss_constants）。矩阵按行主序（与 XeSS-FG 的
 * `xefg_swapchain_frame_constant_data_t` 一致），JVM 侧直接交。 */
typedef struct mcdr_fg_constants
{
    float view[16];
    float projection[16];
    float jitterX;
    float jitterY;
    float mvScaleX;
    float mvScaleY;
    int32_t resetHistory;
    float frameRenderTime;
} mcdr_fg_constants;
typedef struct mcdr_fg_dispatch {
    int32_t displayWidth;    // 0
    int32_t displayHeight;   // 4
    int32_t backend;         // 8  MCDR_FG_BACKEND_*（0 = 用 FgPick 记下的那个）
    int32_t mode;            // 12 0/1/2
    int32_t numFrames;       // 16 仅 mode=1 有意义
    int32_t reset;           // 20 1 = 丢弃历史
    uint64_t appFrameId;     // 24
    uint64_t backendFrameId; // 32
    uint64_t presentId;      // 40
    void* pBackbuffer;       // 48
    void* pHudlessColor;     // 56 可 0
    void* pDepth;            // 64
    void* pMotionVectors;    // 72
    void* pUiColor;          // 80 可 0
    int32_t _pad2[2];        // 88 / 92：align 8 要显式填满（只留一个 int32 会多出 4 B 尾填充）
} mcdr_fg_dispatch;

// 记录期望后端。纯记录：不建、不销毁（与 SrPick 一致）。返回 1 = 已记录，0 = 未知编号。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgPick(int32_t backend);

// 查询后端能力面。out 必须非空；*out 整体覆盖（先清零再填）。返回 1 = 已填，-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgQueryCaps(int32_t backend, mcdr_fg_caps* out);

// 设期望值（mode/numFrames/帧号/资源）。**只记录，不立即下发**：真正的 slDLSSGSetOptions 在渲染线程的
// 下一次 present 前下发，这样跨线程调用不会落到不确定的那次 Present 上。1 = 已记录 0 = 不可用 -1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgSetMode(const mcdr_fg_dispatch* desc);

// 上一次 Present 真正提交的帧数（1 = 没插帧，与 mcdr_dx12GetDlssgPresented 同义）。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgPresented(void);
MCDR_API void MCDR_CALL mcdr_dx12FgSetConstants(const mcdr_fg_constants* constants);

// 一行状态文本：期望/生效后端、可用性、链归属、最近一次失败原因。
// 返回 1 = 已写（NUL 结尾），0 = 没有内容，-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgStatus(char* note, int32_t noteCap);

// 帧生成的状态，压进一个 int32（位布局就是契约，JVM 侧按位读，不要解析文本）。
//   [ 0.. 7] desiredMode   0=关 1=开（固定倍率） 2=dynamic —— **native 侧的期望值**
//   [ 8..15] chainMode     链是在哪个 mode 下建成的（0=关的链 1=开的链）
//   [16..23] active        1 = 现在有一个活着的后端（0 = 没有后端，也就没有插帧）
//   [24..31] rebuilds      min(重建次数, 255)；native 的回滚会把 desiredMode 清成 0，别只看 options.txt。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgState(void);

/* 把 GUI 合成目标交给帧生成后端当「UI 纹理」用。包装型后端（XeSS-FG / FSR-FG）要把「无 HUD 的世界
 * 层」与「UI」分开插值，否则准星/文字会被插帧拉扯。`object` 是 `Dx12Object*` 句柄，传 0 = 撤销。
 * **只在 GPU 目标创建/重建时推一次**，不要每帧推。 */
MCDR_API void MCDR_CALL mcdr_dx12FgSetUiTexture(void* object);

/* hudless 呈现开关，以及"UI 会由 SDK 合成"的可查询状态。
 *
 * `mcdr_dx12FgSetUiComposition(1)`：应用接下来 Present 的 backbuffer **只含世界层**，UI 单独作为一张
 * "透明底 + 预乘 alpha"的纹理（就是 SetUiTexture 推过来的句柄）交给后端合成。0 = 老路径：应用自己把
 * GUI 画进 backbuffer，后端**不得**再注册 UI（否则同一份 HUD 会被合成两遍）。
 *
 * `FgUiCompositionReady()` 返回 1 才敢 Present 不含 UI 的图；判据是"链已包 + UI 纹理已交 + 已经发生过
 * 一次插帧 dispatch"——只包链不算数（界面会整层消失）；调用方要用它做**每帧一次**的 latch。 */
MCDR_API void MCDR_CALL mcdr_dx12FgSetUiComposition(int32_t enabled);
MCDR_API int32_t MCDR_CALL mcdr_dx12FgUiCompositionReady(void);

/* native **内部**调用口，不是给 JVM 的（JVM 不要绑这个符号）：给 DLSS 超分路径补上逐帧 tag。
 * 参数全是 POD（命令列表 + 三个资源 + 各自**当前**状态，即 `D3D12_RESOURCE_STATES` 的整数）。 */
MCDR_API void MCDR_CALL mcdr_fgTagDlssInputs(void* cmdList, void* color, int32_t colorState,
                                             void* depth, int32_t depthState,
                                             void* motionVectors, int32_t motionVectorState);

// ---- DLSS 每帧相机常量（Streamline sl::Constants 逐字段对应；ProgrammingGuide.md §2.11.1 /
//      ProgrammingGuideDLSS.md §6.0）
// 用**新导出 + 新结构**增量扩展，不塞进 mcdr_dlss_input（那个布局被钉住）。老 DLL 没有该导出时，JVM 侧按
// `hasDx12SetDlssConstants` 特性探测跳过。矩阵约定：三张都是**列主序**（JOML get(float[]) 的原生布局），
// native 喂给 sl::Constants（行主序）之前必须逐元素转置；viewProjection/prevViewProjection 是 world->clip
// 且已烘进相机平移，projection 是 view->clip 本身（塞 VP 会让反投影多带一层世界平移）。motionVectors3D
// 必须是 0：本项目 MV 是屏幕空间像素位移，不是 3D 世界速度。
typedef struct mcdr_dlss_constants {
    float viewProjection[16];
    float prevViewProjection[16];
    float jitterOffsetX;
    float jitterOffsetY;
    float mvecScaleX;
    float mvecScaleY;
    float cameraPosX;
    float cameraPosY;
    float cameraPosZ;
    float _pad0;
    float cameraUpX;
    float cameraUpY;
    float cameraUpZ;
    float _pad1;
    float cameraRightX;
    float cameraRightY;
    float cameraRightZ;
    float _pad2;
    float cameraFwdX;
    float cameraFwdY;
    float cameraFwdZ;
    float _pad3;
    float cameraNear;
    float cameraFar;
    float cameraFovRadians;
    float cameraAspectRatio;
    uint32_t depthInverted;
    uint32_t cameraMotionIncluded;
    uint32_t motionVectors3D;
    uint32_t reset;
    uint32_t renderingGameFrames;
    uint32_t motionVectorsJittered;
    float projection[16];
} mcdr_dlss_constants;

// 每帧一次、DLSS 评价之前调用；返回 1 已缓存，0 参数无效。幂等，设备创建前也可调用。
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssConstants(const mcdr_dlss_constants* constants);

// 帧开始（建本帧 token + eSimulationStart）与通用标记（取值见 sl::PCLMarker）。
MCDR_API void MCDR_CALL mcdr_dx12ReflexFrameBegin(void);
MCDR_API void MCDR_CALL mcdr_dx12ReflexMarker(int32_t marker);

// NVIDIA Reflex 模式（0=off 1=on 2=on+boost）与可用性（1=可用）。
MCDR_API void MCDR_CALL mcdr_dx12SetReflexMode(int32_t mode);
MCDR_API int32_t MCDR_CALL mcdr_dx12ReflexAvailable(void);
// 1 = DLSS 可用（Streamline 的 DLSS 插件已加载，或未探测到失败）；0 = 插件没有初始化（NGX 版本不匹配）。
MCDR_API int32_t MCDR_CALL mcdr_dx12DlssAvailable(void);

// Reflex 自己的限帧器（微秒/帧，0 = 关闭）。配置项 [dlss] reflexFrameLimitUs 的唯一入口。
MCDR_API void MCDR_CALL mcdr_dx12SetReflexFrameLimit(int32_t micros);

// ---- Device and resources ------------------------------------------------------

// Writes a human-readable device/adapter summary into `buf`.
MCDR_API int32_t MCDR_CALL mcdr_dx12CreateDevice(char* buf, int32_t cap);

// 释放设备上下文中的全部 D3D12/DXGI 对象。**必须在进程退出前调用**：这些 ComPtr 若留到 DLL_PROCESS_DETACH
// 才析构，Release() 会在 dxgi.dll 已经 detach 之后执行，产生原生异常（退出码 2170）。幂等。
MCDR_API void MCDR_CALL mcdr_dx12DestroyDevice(void);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateTexture(int32_t usage, int32_t format, int32_t width,
                                                       int32_t height, int32_t depthOrLayers,
                                                       int32_t mipLevels);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateBuffer(int32_t usage, int64_t size);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateSampler(int32_t addressU, int32_t addressV, int32_t minFilter,
                                                       int32_t magFilter, int32_t maxAnisotropy,
                                                       float maxLod);

// 带显式 mip LOD bias 的采样器，只给**地形图集**用；bias 由超分比例决定（log2(render/display) - 0.5），
// 不超分时必须传 0。合法区间 [-16, 15.99]，越界由 native 夹取。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateSamplerWithMipBias(
    int32_t addressU, int32_t addressV, int32_t minFilter, int32_t magFilter, int32_t maxAnisotropy,
    float maxLod, float mipLodBias);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateTextureView(mcdr_object* texture, int32_t baseMipLevel,
                                                           int32_t mipLevels);

MCDR_API void MCDR_CALL mcdr_dx12DestroyResource(mcdr_object* object);

// Returns the CPU-visible base address of a mapped buffer, or NULL on failure.
// `outLength` receives the number of bytes actually mapped.
MCDR_API void* MCDR_CALL mcdr_dx12MapBuffer(mcdr_object* buffer, int64_t offset, int64_t length, int32_t read,
                                            int32_t write, int64_t* outLength);

MCDR_API void MCDR_CALL mcdr_dx12UnmapBuffer(mcdr_object* buffer);

// Writes a JSON array describing every adapter into `buf`.
MCDR_API int32_t MCDR_CALL mcdr_dx12EnumerateAdapters(char* buf, int32_t cap);

// Writes a flat, single-line JSON object describing the adapter the ACTIVE device is bound to. Keys (always
// present): name / vendor / vendor_id / device_id / subsys_id / revision / feature_level / feature_level_value /
// driver_version / driver_description / device_type / vram_bytes / shared_bytes / dedicated_system_bytes /
// luid / is_hardware / is_detachable / dxcore. driver_* and is_* come from DXCore, loaded at run time (no
// link-time dependency); unavailable -> ""/false and dxcore false. Buffer convention: buf == nullptr or cap <= 0
// returns the required byte count INCLUDING the NUL; otherwise a NUL-terminated UTF-8 string is written and the
// bytes written EXCLUDING the NUL are returned. 0 = no active device.
MCDR_API int32_t MCDR_CALL mcdr_dx12GetAdapterInfo(char* buf, int32_t cap);

MCDR_API uint64_t MCDR_CALL mcdr_dx12GetQueueHandle(void);
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetDeviceHandle(void);

MCDR_API uint64_t MCDR_CALL mcdr_dx12CreateHiddenWindow(int32_t width, int32_t height);
MCDR_API void MCDR_CALL mcdr_dx12DestroyHiddenWindow(uint64_t hwnd);

// ---- Command layer -------------------------------------------------------------

MCDR_API int64_t MCDR_CALL MCDR_D3D12_get_timestamp_frequency(void);

MCDR_API mcdr_command_ctx* MCDR_CALL MCDR_D3D12_create_command_encoder(void);
MCDR_API void MCDR_CALL mcdr_dx12DestroyCommandEncoder(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12BeginCommandList(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12BeginCommandListWithWait(mcdr_command_ctx* ctx, uint64_t waitForValue);
MCDR_API void MCDR_CALL mcdr_dx12EndCommandList(mcdr_command_ctx* ctx);
// Fence 语义（per-command-ctx）：GetFenceValue 返回该 ctx 的 per-command-list 序号，WaitForFence 只等待该 ctx
// 自己的提交，Submit 返回同一条时间线上的序号。因此帧 encoder 的 fence token 不会被一次性 encoder 提前满足。
MCDR_API uint64_t MCDR_CALL mcdr_dx12Submit(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12WaitForFence(mcdr_command_ctx* ctx, uint64_t value, int64_t timeoutNs);
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetFenceValue(mcdr_command_ctx* ctx);
MCDR_API int64_t MCDR_CALL mcdr_dx12GetTimestampNow(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12IsListOpen(mcdr_command_ctx* ctx);

MCDR_API void MCDR_CALL mcdr_dx12CopyBuffer(mcdr_command_ctx* ctx, mcdr_object* src, int64_t srcOffset,
                                            mcdr_object* dst, int64_t dstOffset, int64_t size);

MCDR_API void MCDR_CALL mcdr_dx12WriteToTexture(mcdr_command_ctx* ctx, mcdr_object* stagingBuffer,
                                                int64_t stagingOffset, int32_t width, int32_t height,
                                                mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                int32_t dstX, int32_t dstY);

MCDR_API void MCDR_CALL mcdr_dx12CopyBufferToTexture(mcdr_command_ctx* ctx, mcdr_object* srcBuffer,
                                                     int64_t srcOffset, int32_t srcWidth, int32_t srcHeight,
                                                     mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                     int32_t dstX, int32_t dstY, int32_t width,
                                                     int32_t height);

MCDR_API void MCDR_CALL mcdr_dx12CopyTextureToBuffer(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                                     int32_t mip, int32_t layer, int32_t srcX, int32_t srcY,
                                                     int32_t width, int32_t height, mcdr_object* dstBuffer,
                                                     int64_t dstOffset);

MCDR_API void MCDR_CALL mcdr_dx12CopyTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                                      mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                      int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY,
                                                      int32_t width, int32_t height);

MCDR_API void MCDR_CALL mcdr_dx12ClearColorTexture(mcdr_command_ctx* ctx, mcdr_object* texture, float r,
                                                   float g, float b, float a);
MCDR_API void MCDR_CALL mcdr_dx12ClearDepthTexture(mcdr_command_ctx* ctx, mcdr_object* texture, double depth);
MCDR_API void MCDR_CALL mcdr_dx12ClearColorTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture, float r,
                                                         float g, float b, float a, int32_t x, int32_t y,
                                                         int32_t width, int32_t height);
MCDR_API void MCDR_CALL mcdr_dx12ClearDepthTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture,
                                                         double depth, int32_t x, int32_t y, int32_t width,
                                                         int32_t height);

// Clears only the stencil plane to `value`; the depth plane is untouched.
MCDR_API int32_t MCDR_CALL mcdr_dx12ClearStencilTexture(mcdr_command_ctx* ctx, mcdr_object* texture,
                                                        int32_t value);

// Apply SMAA to `texture` in place on the open command list: three full-screen passes, then the result is copied
// back and the texture is left in RENDER_TARGET. Call it after the world and before the GUI. No-op when the mode
// is not SMAA; a failure disables SMAA with one ERROR. 1 = ran this frame, 0 = skipped (not SMAA / unsuitable).
MCDR_API int32_t MCDR_CALL mcdr_dx12ApplySmaaToTexture(mcdr_command_ctx* ctx, mcdr_object* texture);

// Draw `src` onto `dst` (full-screen triangle + linear sampler, so sizes may differ) and leave `dst` in
// RENDER_TARGET. A 1:1 copyTextureToTexture cannot do it when the scale factor is not 1.
MCDR_API int32_t MCDR_CALL mcdr_dx12BlitTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                         mcdr_object* dst);
// 后置锐化（TAA / DLAA / DLSS 共用）。src -> dst 的 5-tap unsharp，amount 取 [0,1]；
// src == dst 会被拒绝（返回 0），调用方需准备第二张同尺寸纹理。
MCDR_API int32_t MCDR_CALL mcdr_dx12SharpenTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                        mcdr_object* dst, float amount);

// TAA resolve, native side. Same call point as ApplySmaaToTexture, but the cross-frame state lives
// here: two RGBA16F history textures (ping-pong), the resolve pipeline and the jitter/history
// bookkeeping belong to the device context. `motion` may be a texture view; jitter is in pixels and
// must already be baked into the projection; `reset` != 0 bypasses the blend; `blend` is the current
// frame's weight (1/16 usual); `sharpenAmount` <= 0 selects a plain blit. `scene` stays RENDER_TARGET.
// Returns 1 = ran, 0 = skipped (not TAA, unsuitable textures, disabled), -1 = failed (one ERROR).
MCDR_API int32_t MCDR_CALL mcdr_dx12ApplyTaaToTexture(mcdr_command_ctx* ctx, mcdr_object* scene,
                                                     mcdr_object* motion, float jitterX, float jitterY,
                                                     float prevJitterX, float prevJitterY, float blend,
                                                     int32_t reset, float sharpenAmount);

// `colorTextures` may contain NULL entries (unused attachments); they are
// skipped. `colorMips` is the per-attachment RTV mip slice. `colorClearFlags`
// is 0 = load, 1 = clear. `clearColors` holds 4 floats per attachment.
MCDR_API void MCDR_CALL mcdr_dx12BeginRenderPass(mcdr_command_ctx* ctx, mcdr_object* const* colorTextures,
                                                 int32_t colorCount, const int32_t* colorMips,
                                                 const int32_t* colorClearFlags, const float* clearColors,
                                                 mcdr_object* depthTexture, int32_t depthMip,
                                                 int32_t depthClearFlag, double depthClearValue, int32_t x,
                                                 int32_t y, int32_t width, int32_t height);

MCDR_API void MCDR_CALL mcdr_dx12EndRenderPass(mcdr_command_ctx* ctx);
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12GetActiveColorTexture(mcdr_command_ctx* ctx);

MCDR_API mcdr_query_pool* MCDR_CALL mcdr_dx12CreateQueryPool(int32_t size);
MCDR_API void MCDR_CALL mcdr_dx12DestroyQueryPool(mcdr_query_pool* pool);
MCDR_API void MCDR_CALL mcdr_dx12WriteTimestamp(mcdr_command_ctx* ctx, mcdr_query_pool* pool, int32_t index);
MCDR_API uint64_t MCDR_CALL mcdr_dx12ReadQueryValue(mcdr_query_pool* pool, int32_t index);
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadQueryValues(mcdr_query_pool* pool, int32_t start, int32_t count,
                                                    int64_t* out);

// Async renderer control channel.
MCDR_API void MCDR_CALL mcdr_dx12InitAsyncRenderer(int32_t workerCount);
MCDR_API void MCDR_CALL mcdr_dx12DestroyAsyncRenderer(void);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderBeginFrame(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitComplete(mcdr_command_ctx* ctx, int64_t timeoutMs);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderIsRecordingReady(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitRecordingReady(mcdr_command_ctx* ctx, int64_t timeoutMs);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncSendCommandsReady(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12ClearAsyncRenderCtx(void);

// ---- Pipeline ------------------------------------------------------------------

MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateGraphicsPipeline(const mcdr_pipeline_desc* desc);
MCDR_API void MCDR_CALL mcdr_dx12DestroyPipeline(mcdr_pipeline* pipeline);

// ---- Compute：最小通路（着色率图像 / NRC 的硬前置） ----------------------------
// 本模块没有 D3D12_COMMAND_LIST_TYPE_COMPUTE：compute 走普通的 DIRECT 列表（SetPipelineState(compute PSO) +
// SetComputeRootSignature + Dispatch），这也是同一条列表里既录图形又录 compute 的前提。根签名固定：[0]
// 描述符表（1×UAV, u0；目标纹理必须带 usage 位 32 = ALLOW_UNORDERED_ACCESS），[1] root constants（b0 ≤ 8 个
// int32）。**必须是描述符表**：typed UAV 挂 root UAV 会让 CreateComputePipelineState 返回 E_INVALIDARG。

// 从 DXIL 建 compute PSO（cs_6_8 起支持 Wave 内在函数）。失败返回 NULL，原因见 mcdrLastError()。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipeline(const void* dxil, int32_t dxilSize);

// 根签名 [0] SRV 表(t0..)、[1] UAV 表(u0..)、[2] CBV b0（整块常量缓冲，可传 NULL）、
// [3] root constants b1（8 个 int32）。srv/uav 各 0..16，不能同时为 0。
// cbvOffset 是相对缓冲首地址的字节偏移，必须是 256 的倍数（D3D12 常量缓冲对齐要求）。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineEx(const void* dxil, int32_t dxilSize,
                                                                 int32_t srvCount, int32_t uavCount);
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchComputeEx(
    mcdr_command_ctx* ctx, mcdr_object* pipeline, mcdr_object* const* srvs, int32_t srvCount,
    mcdr_object* const* uavs, int32_t uavCount, const int32_t* constants, int32_t constantCount,
    mcdr_object* cbv, uint64_t cbvOffset, int32_t x, int32_t y, int32_t z);

// 在已打开的命令列表上派发一次。内部把目标纹理 COMMON → UNORDERED_ACCESS、Dispatch、再转回 COMMON。
// 返回 1=已派发 0=参数或资源状态不对（写一条 ERROR） -2=没有打开的命令列表。
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchCompute(mcdr_command_ctx* ctx, mcdr_object* pipeline,
                                                     mcdr_object* uavTexture, const int32_t* constants,
                                                     int32_t constantCount, int32_t x, int32_t y, int32_t z);

// ---- 光线追踪加速结构（S3）：BLAS / TLAS ---------------------------------------
// 加速结构缓冲是**普通 buffer 资源**，但必须建在 DEFAULT 堆、带 ALLOW_UNORDERED_ACCESS、初始状态是
// RAYTRACING_ACCELERATION_STRUCTURE，而且不能进缓冲池（池按 (堆, 容量) 复用，会给它配上错误的状态）。
// 结果与 scratch 的大小只能由 GetRaytracingAccelerationStructurePrebuildInfo 给出，所以先查后建：
// 一次查询把 {结果上限字节, scratch 字节} 写进 out[2]，调用方按这两个数分配两块缓冲。

// 一条 BLAS 几何：顶点与索引都指向调用方已经上传到 GPU 的缓冲。
// 偏移是**字节**偏移（顶点为 StartVertexLocation * vertexStride）——JVM 侧只有 ByteBuffer 视角。
typedef struct mcdr_rt_geometry {
    mcdr_object* vertexBuffer;   // 必须是 default 堆的 buffer
    int64_t vertexStride;        // 地形是 28；R16G16B16A16_FLOAT 位置 + 2 个通道
    int64_t vertexCount;
    int64_t vertexOffset;        // 字节
    mcdr_object* indexBuffer;    // 必须是索引缓冲（DEFAULT 堆）
    int64_t indexCount;
    int64_t indexOffset;         // 字节
    int32_t flags;               // 0 = 不透明；1 = 需要 alpha 测试（GEOMETRY_FLAG_NONE，alpha 在着色器里判）
    int32_t indexStride;         // 索引宽度字节数：2 = R16_UINT，4 = R32_UINT
} mcdr_rt_geometry;

// 一个 hit group：名字 + 三个入口（三角形几何只需要 closestHit / anyHit；程序化体素才要 intersection）。
// 四条指针指向调用方持有的 UTF-8 字符串，只在 mcdr_dx12CreateRaytracingPipeline 调用期间读。
typedef struct mcdr_rt_hit_group {
    const char* name;
    const char* closestHit;
    const char* anyHit;
    const char* intersection;
} mcdr_rt_hit_group;

// 一次 CreateStateObject 最多接受的 hit group 数（与 cppm 的 kMaxRtHitGroups 一致）。
#define MCDR_RT_MAX_HIT_GROUPS 4

// 建一块可以直接当 BLAS/TLAS 用的缓冲（DEFAULT 堆，ALLOW_UNORDERED_ACCESS，初始状态 AS）。NULL = 失败。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateAccelStructBuffer(int64_t size);

// 查询 BLAS 预构建信息：out[0] = 结果最大字节数，out[1] = scratch 字节数（两者都需 256 字节对齐）。
// 返回 1 = 成功，0 = 失败（原因见 mcdrLastError()）。
MCDR_API int32_t MCDR_CALL mcdr_dx12GetBlasPrebuildInfo(const mcdr_rt_geometry* geometries,
                                                        int32_t count, uint64_t* out);

// 查询 TLAS 预构建信息，instanceCount 是实例数（每个实例 64 字节的 D3D12_RAYTRACING_INSTANCE_DESC）。
MCDR_API int32_t MCDR_CALL mcdr_dx12GetTlasPrebuildInfo(int32_t instanceCount, uint64_t* out);

// 在已打开的命令列表上构建/更新 BLAS。update != 0 时走 PERFORM_UPDATE（要求 dest 里已经是同一个拓扑的
// BLAS，否则结果未定义）。返回 1 = 已录制，0 = 失败（原因见 mcdrLastError()，含"没有打开的命令列表"）。
MCDR_API int32_t MCDR_CALL mcdr_dx12BuildBlas(mcdr_command_ctx* ctx, mcdr_object* dest,
                                              mcdr_object* scratch,
                                              const mcdr_rt_geometry* geometries, int32_t count,
                                              int32_t update);

// 在已打开的命令列表上重建 TLAS（每帧一次，PREFER_FAST_BUILD）。instances 是装满
// D3D12_RAYTRACING_INSTANCE_DESC 的缓冲，instanceCount 与它一致。返回值同上。
MCDR_API int32_t MCDR_CALL mcdr_dx12BuildTlas(mcdr_command_ctx* ctx, mcdr_object* dest,
                                              mcdr_object* scratch, mcdr_object* instances,
                                              int32_t instanceCount);

// 把 src 压缩拷贝到 dest（COPY_MODE_COMPACT）。紧凑版要等这次提交执行完才能当 SRV 用：调用方
// 在下一帧再切过去，否则读到的是还没写过的内存。
MCDR_API int32_t MCDR_CALL mcdr_dx12CopyAccelStruct(mcdr_command_ctx* ctx, mcdr_object* dest,
                                                    mcdr_object* src);

// 给加速结构建一个 SRV（RAYTRACING_ACCELERATION_STRUCTURE），可直接放进 compute 的 SRV 表给
// RaytracingAccelerationStructure 绑定。NULL = 失败。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateAccelStructView(mcdr_object* accel);

// 缓冲的 GPU 虚拟地址（失败返回 0）。D3D12_RAYTRACING_INSTANCE_DESC::AccelerationStructure 要的是
// 这个地址；写 mcdr_object 句柄等于把堆指针当显存地址，TLAS 里所有实例都指到不存在的地方。
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetBufferAddress(mcdr_object* buffer);

// 用 lib_6_x 编出来的 DXIL 库建光线追踪状态对象（D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE）。
// 全局根签名与 compute 共用同一套四参数（SRV 表 / UAV 表 / b0 CBV / b1 八 dword）；maxRecursion 1..8。
// 返回 mcdr_object*（NULL = 失败，原因见 mcdrLastError()，含"缺少某个导出名"）。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateRaytracingPipeline(
    const void* dxilLib, int64_t dxilSize, const char* raygen, const char* miss,
    const mcdr_rt_hit_group* groups, int32_t groupCount, int32_t srvCount, int32_t uavCount,
    int32_t maxRecursion);

// 取某个导出名的 32 B 着色器标识符（SBT 记录的前 32 字节）。返回 1=写入 out32，0=名字不存在。
MCDR_API int32_t MCDR_CALL mcdr_dx12GetShaderIdentifier(mcdr_object* pipeline, const char* exportName,
                                                void* out32);

// 在已打开的命令列表上 DispatchRays。偏移与步长都是字节数，起点必须是 32 的倍数、步长必须是 32 的倍数。
// 返回 1=已派发 0=参数或资源状态不对（写一条 ERROR） -2=没有打开的命令列表。
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchRays(
    mcdr_command_ctx* ctx, mcdr_object* pipeline, mcdr_object* shaderTable, int64_t raygenOffset,
    int64_t missOffset, int64_t missStride, int32_t missCount, int64_t hitOffset, int64_t hitStride,
    int32_t hitCount, mcdr_object* const* srvs, int32_t srvCount, mcdr_object* const* uavs,
    int32_t uavCount, const int32_t* constants, int32_t constantCount, mcdr_object* cbv,
    uint64_t cbvOffset, int32_t width, int32_t height);

// ---- Mesh shaders: amplification/mesh pipeline + HLSL->DXIL compilation --------
// Mesh shaders are unreachable through D3DCompile (SM 5.1 ceiling), so native compiles them with
// DXC, loaded dynamically from dxcompiler.dll.

// Compiles one HLSL entry point to DXIL. Returns the bytecode size, or 0 on failure (reason via
// mcdrLastError()). `profile` is e.g. "as_6_6" / "ms_6_6" / "ps_6_0"; an empty `entry` means "main".
// Pass (NULL, 0) — or a short buffer — to learn the required size first.
MCDR_API int32_t MCDR_CALL mcdr_dx12CompileShaderDxc(const char* source, int32_t sourceLen,
                                                     const char* entry, const char* profile, void* outDxil,
                                                     int32_t outCapacity);

// 能力与原因的统一查询（与 UI 置灰同源）。1 = 支持 0 = 不支持；outReason 写 UTF-8 原因（可为 NULL）。
MCDR_API int32_t MCDR_CALL mcdr_dx12QueryMeshSupport(char* outReason, int32_t capacity);

// Creates an amplification/mesh pipeline from DXIL. NULL plus last error when the device reports tier 0.
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateMeshPipeline(const mcdr_mesh_pipeline_desc* desc);

// 与 mcdr_dx12CreateMeshPipeline 相同，但复用调用方提供的根签名（ID3D12RootSignature* 的地址值，
// 0 = 退回内部按 bindings 新建）。用途：地形 mesh 变体复用现有地形的绑定点布局（b0..b3 / t4 / t5），
// JVM 侧补 b8 小常量与 t14/t15/t16 尾参，mesh PSO 与地形 PSO 才能共用同一套 draw 绑定。
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateMeshPipelineWithRootSignature(
    const mcdr_mesh_pipeline_desc* desc, uint64_t rootSignature);

// 按绑定点布局建根签名（返回 ID3D12RootSignature* 的地址值，失败返回 0）。meshExtras != 0 时追加 mesh
// 专用尾参：root CBV b8 + 第二个描述符表（t14/t15/t16，固定 3 个 range），且不能带
// ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT。调用方必须用 mcdr_dx12ReleaseRootSignature 释放。
MCDR_API uint64_t MCDR_CALL mcdr_dx12CreateMeshRootSignature(
    const mcdr_pipeline_binding_desc* bindings, int32_t count, int32_t meshExtras);
MCDR_API void MCDR_CALL mcdr_dx12ReleaseRootSignature(uint64_t rootSignature);

// DispatchMesh with thread-group counts. Requires a mesh pipeline bound through mcdr_dx12SetPipeline, an
// open render pass and descriptors pushed. Returns 1 on success.
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchMesh(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t z);

// meshlet 版地形派发（索引由 MS 算术生成）。绑定布局与 CreateMeshRootSignature(meshExtras=1) 一致：
// root CBV b8 = 小常量（uint meshletCount + 3 个 pad，256 B 对齐）；描述符表 = t14 顶点池（28 B/顶点）、
// t15 段表（32 B/段）、t16 meshlet 表（32 B/条）。三个缓冲都是 RAW SRV（ByteAddressBuffer + Load），
// 因此不需要 StructureByteStride。一次 DispatchMesh(meshletCount, 1, 1) 覆盖整批；段号写在记录里。
MCDR_API int32_t MCDR_CALL mcdr_dx12MeshDrawMeshlets(
    mcdr_command_ctx* ctx, mcdr_object* vertexBuffer, int64_t vertexOffset, mcdr_object* sections,
    int64_t sectionsOffset, int64_t sectionsLength, mcdr_object* meshlets, int64_t meshletsOffset,
    int64_t meshletsLength, mcdr_object* globals, int64_t globalsOffset, int32_t meshletCount);

// ---- Surface -------------------------------------------------------------------

MCDR_API mcdr_surface* MCDR_CALL mcdr_dx12CreateSurface(uint64_t hwnd);

// Writes the supported present-mode ordinals into `out` (up to `cap`) and
// returns the total count, so a caller can size the buffer first.
MCDR_API int32_t MCDR_CALL mcdr_dx12SurfacePresentModes(int32_t* out, int32_t cap);

MCDR_API int32_t MCDR_CALL mcdr_dx12ConfigureSurface(mcdr_surface* surface, int32_t width, int32_t height,
                                                     int32_t presentMode);
// 真独占全屏（1 = 进独占，0 = 退回窗口/无边框）。失败返回 0，画面不受影响。
MCDR_API int32_t MCDR_CALL mcdr_dx12SetExclusiveFullscreen(mcdr_surface* surface, int32_t on);

MCDR_API int32_t MCDR_CALL mcdr_dx12AcquireSurface(mcdr_surface* surface);
MCDR_API void MCDR_CALL mcdr_dx12BlitSurface(mcdr_command_ctx* ctx, mcdr_surface* surface,
                                             mcdr_object* texture);
MCDR_API void MCDR_CALL mcdr_dx12PresentSurface(mcdr_surface* surface);
// Returns the raw ID3D12Resource* of the acquired back buffer as an opaque integer (NOT a mcdr_object*).
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetBackBufferHandle(mcdr_surface* surface);
MCDR_API int32_t MCDR_CALL mcdr_dx12IsSurfaceSuboptimal(mcdr_surface* surface);

// （原生 HDR）：0 = auto（按窗口所在输出的 DXGI 色彩空间判定），1 = 强制开，2 = 强制关。
// 只改进程状态，真正生效在下次创建/重建交换链时（格式是建链参数，不能原地改）。
MCDR_API void MCDR_CALL mcdr_dx12SetHdrMode(int32_t mode);
MCDR_API int32_t MCDR_CALL mcdr_dx12GetHdrAppliedMode(void);

// 光追可见性分层总开关（0=off, 1=ao, 2=ao_shadows, 3=full）；不支持的设备上记下但不生效。
MCDR_API void MCDR_CALL mcdr_dx12SetRaytracingMode(int32_t mode);

// 光追 / Work Graph 的能力面。sizeof = 80、align = 4（Kotlin 镜像：RT_CAPS）。
// tier 值都是 D3D12 枚举原值（raytracing 0/10/11/12、mesh 0/10、workGraphs 0/10、VRS 0/1/2）。
// 两个 queried 位区分「问了但结果为 0」与「根本没问成」：OPTIONS21 只有 Agility 619 的 core 认，
// OPTIONS22 更晚，所以不要把 0 直接当成「这台机器不支持」。
typedef struct mcdr_rt_caps {
    int32_t raytracingTier;        // 0
    int32_t meshShaderTier;        // 4
    int32_t workGraphsTier;        // 8
    int32_t shadingRateTier;       // 12
    int32_t shaderModel;           // 16 D3D_SHADER_MODEL 原值（0x69 = 6.9）
    int32_t samplerFeedbackTier;   // 20
    int32_t options21Queried;      // 24 1 = workGraphsTier 可信
    int32_t options22Queried;      // 28 1 = serActuallyReorders 可信
    int32_t serActuallyReorders;   // 32 1 = SER 真的重排（tier 1.2 只是声明）
    int32_t enhancedBarriers;      // 36
    int32_t dxcAvailable;          // 40 1 = dxcompiler.dll 可用
    int32_t max1DDispatchSize;     // 44
    int32_t max1DDispatchMeshSize; // 48
    int32_t reserved;              // 52 必须为 0（留给后续 flags，不动 sizeof）
    char note[24];                 // 56 短原因，NUL 结尾（56 + 24 = 80）
} mcdr_rt_caps;

// 填能力面。返回 1 = 表已填（各字段仍要按 queried 位解读），-1 = 参数非法或设备未建。
MCDR_API int32_t MCDR_CALL mcdr_dx12QueryRtCaps(mcdr_rt_caps* out);

// 影子状态跟踪器的计数。mismatches 是"真的错"；promotions / widened 是 D3D12 允许的
// 隐式提升与组合掩码，单独计数，免得把合法写法算成 bug。
// sizeof = 92 / align = 4。
typedef struct mcdr_rt_track_stats {
    int32_t transitions;
    int32_t mismatches;
    int32_t promotions;
    int32_t widened;
    int32_t firstUse;
    int32_t uavBarriers;
    int32_t tracked;
    char note[64];
} mcdr_rt_track_stats;

MCDR_API int32_t MCDR_CALL mcdr_dx12QueryRtTrackStats(mcdr_rt_track_stats* out);

typedef struct mcdr_barrier_desc {
    mcdr_object* resource;
    mcdr_object* resourceAfter;
    int32_t kind;
    int32_t flags;
    uint32_t before;
    uint32_t after;
    uint32_t subresource;
} mcdr_barrier_desc;

MCDR_API int32_t MCDR_CALL mcdr_dx12Barrier(mcdr_command_ctx* ctx, const mcdr_barrier_desc* descs,
                                            int32_t count);

// 交换链建链选项，下次建链时生效：allowTearing 0 = 不建 ALLOW_TEARING 的链（强制 DWM 合成翻页），非 0 = 允许
// （默认）；forcePresentChild 非 0 = 优先把链建在自建呈现子窗口上，0 = 默认（顶层窗口）。
MCDR_API void MCDR_CALL mcdr_dx12SetSwapchainOptions(int32_t allowTearing, int32_t forcePresentChild);

MCDR_API void MCDR_CALL mcdr_dx12DestroySurface(mcdr_surface* surface);
MCDR_API mcdr_surface* MCDR_CALL mcdr_dx12GetActiveSurfaceHandle(void);

// Both readback entry points write 36 int32s (9 pixels x RGBA) into `out` and
// return 1 on success, 0 on failure.
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadbackSurfacePixels(mcdr_surface* surface, int32_t* out);
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadbackTexturePixels(mcdr_object* texture, int32_t* out);

/* 已知起始状态的读回。`fromState` 是这张纹理**当前真实所处**的 D3D12_RESOURCE_STATES 数值，不是"我希望它在
 * 哪"：传错则 barrier 的 StateBefore 不匹配，读回内容未定义。旧导出按 COMMON 处理，读回后恢复成 fromState。 */
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadbackTexturePixelsIn(mcdr_object* texture, int32_t fromState,
                                                            int32_t* out);

/* DLSS 输出纹理上一帧结束时停在哪个状态（D3D12_RESOURCE_STATES 数值）。
 * 0 (COMMON) = 这一帧没有输出纹理/未知 —— 调用方不要据此采样。 */
MCDR_API int32_t MCDR_CALL mcdr_dx12DlssOutputState(void);

MCDR_API void MCDR_CALL mcdr_dx12DumpTextureToFile(mcdr_object* texture, const char* tag);

// ---- Draw ----------------------------------------------------------------------

MCDR_API int32_t MCDR_CALL mcdr_dx12SetPipeline(mcdr_command_ctx* ctx, mcdr_pipeline* pipeline,
                                                int32_t hasDepth);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetScissor(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                               int32_t height);
// Sets the viewport explicitly (it used to be set only inside mcdr_dx12BeginRenderPass).
MCDR_API int32_t MCDR_CALL mcdr_dx12SetViewport(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                                int32_t height);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetVertexBuffer(mcdr_command_ctx* ctx, int32_t slot, mcdr_object* buffer,
                                                    int64_t offset, int32_t stride);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetIndexBuffer(mcdr_command_ctx* ctx, mcdr_object* buffer,
                                                   int32_t indexType);

// `bindings` holds `count` entries, one per pipeline binding in declaration
// order. Replaces the six parallel arrays of the JNI version.
MCDR_API int32_t MCDR_CALL mcdr_dx12PushDescriptors(mcdr_command_ctx* ctx, const mcdr_binding_slot* bindings,
                                                    int32_t count);

MCDR_API int32_t MCDR_CALL mcdr_dx12DrawIndexed(mcdr_command_ctx* ctx, int32_t indexCount,
                                                int32_t instanceCount, int32_t firstIndex, int32_t baseVertex,
                                                int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12Draw(mcdr_command_ctx* ctx, int32_t vertexCount, int32_t instanceCount,
                                         int32_t firstVertex, int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12DrawIndexedIndirect(mcdr_command_ctx* ctx, mcdr_object* commands,
                                                        int64_t offset, int32_t drawCount);
MCDR_API int32_t MCDR_CALL mcdr_dx12DrawIndirect(mcdr_command_ctx* ctx, mcdr_object* commands, int64_t offset,
                                                 int32_t drawCount);

// ---- Async: partitioned descriptor allocator -----------------------------------

MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncCurrentFrameSlot(mcdr_command_ctx* ctx);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncCurrentFrameValue(mcdr_command_ctx* ctx);

MCDR_API mcdr_desc_alloc* MCDR_CALL mcdr_dx12AsyncDescriptorCreate(int32_t workerCount);
MCDR_API void MCDR_CALL mcdr_dx12AsyncDescriptorDestroy(mcdr_desc_alloc* alloc);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorRegionBase(mcdr_desc_alloc* alloc, int32_t frameSlot,
                                                              int32_t worker);
// Returns the base descriptor slot, or -1 when the region is exhausted.
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorAllocate(mcdr_desc_alloc* alloc, int32_t frameSlot,
                                                            int32_t worker, int32_t count);
MCDR_API void MCDR_CALL mcdr_dx12AsyncDescriptorResetFrame(mcdr_desc_alloc* alloc, int32_t frameSlot);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncDescriptorGpuHandle(mcdr_desc_alloc* alloc, int32_t slot);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorWriteCBV(mcdr_desc_alloc* alloc, int32_t slot,
                                                            mcdr_object* buffer, int64_t offset,
                                                            int64_t size);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorWriteSRV(mcdr_desc_alloc* alloc, int32_t slot,
                                                            mcdr_object* view);

// ---- Async: bundle recorder pool -----------------------------------------------

MCDR_API mcdr_bundle_pool* MCDR_CALL mcdr_dx12AsyncBundlePoolCreate(int32_t workerCount);
MCDR_API void MCDR_CALL mcdr_dx12AsyncBundlePoolDestroy(mcdr_bundle_pool* pool);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleBegin(mcdr_bundle_pool* pool, int32_t worker,
                                                     int32_t frameSlot, uint64_t frameValue);
// Returns the recorded ID3D12GraphicsCommandList* as an opaque integer, 0 on failure.
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncBundleEnd(mcdr_bundle_pool* pool, int32_t worker);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetPipelineState(mcdr_bundle_pool* pool, int32_t worker,
                                                                mcdr_pipeline* pipeline, int32_t hasDepth);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetDescriptorTable(mcdr_bundle_pool* pool, int32_t worker,
                                                                  int32_t slot, uint64_t gpuHandle);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetVertexBuffer(mcdr_bundle_pool* pool, int32_t worker,
                                                               int32_t slot, mcdr_object* buffer,
                                                               int64_t offset, int32_t stride);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetIndexBuffer(mcdr_bundle_pool* pool, int32_t worker,
                                                              mcdr_object* buffer, int32_t indexType);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleDrawIndexed(mcdr_bundle_pool* pool, int32_t worker,
                                                           int32_t indexCount, int32_t instanceCount,
                                                           int32_t firstIndex, int32_t baseVertex,
                                                           int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleDraw(mcdr_bundle_pool* pool, int32_t worker,
                                                    int32_t vertexCount, int32_t instanceCount,
                                                    int32_t firstVertex, int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleDrawIndexedIndirect(mcdr_bundle_pool* pool, int32_t worker,
                                                                   mcdr_object* commands, int64_t offset,
                                                                   int32_t drawCount);

// The hot path: one call records a whole worker partition.
//
//   `bindings` — when `sharedBindings` is non-zero, `bindingCount` entries describing draw 0's block, reused
//                for every draw. Otherwise `drawCount * bindingCount` entries indexed as bindings[draw * N + slot].
//   `draws`    — `drawCount` entries, already advanced to the partition's first draw.
// Any invalid argument returns 0 and the Java side discards the bundle and re-records serially.
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleRecordPartition(mcdr_desc_alloc* alloc, mcdr_bundle_pool* pool,
                                                               int32_t worker, int32_t baseSlot,
                                                               int32_t bindingCount, int32_t sharedBindings,
                                                               const mcdr_binding_slot* bindings,
                                                               const mcdr_partition_draw* draws,
                                                               int32_t drawCount);

MCDR_API int32_t MCDR_CALL mcdr_dx12ExecuteBundle(mcdr_command_ctx* ctx, uint64_t bundle);

// Barrier pre-passes: a bundle may not contain barriers, so every CBV buffer,
// index buffer and texture view is transitioned on the main list first.
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareCBVBuffers(mcdr_command_ctx* ctx, mcdr_object* const* buffers,
                                                           int32_t count);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareIndexBuffers(mcdr_command_ctx* ctx,
                                                             mcdr_object* const* buffers, int32_t count);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareTextureViews(mcdr_command_ctx* ctx, mcdr_object* const* views,
                                                             int32_t count);

// ---- Async: executor -----------------------------------------------------------

MCDR_API mcdr_executor* MCDR_CALL mcdr_dx12AsyncExecutorCreate(void);
MCDR_API void MCDR_CALL mcdr_dx12AsyncExecutorDestroy(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorTryBeginFrame(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorAddBundle(mcdr_executor* executor, uint64_t bundle);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorAddTransition(mcdr_executor* executor, mcdr_object* object,
                                                               int32_t stateBefore, int32_t stateAfter);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncExecutorEndFrame(mcdr_executor* executor);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncExecutorLastFenceValue(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorIsFrameInProgress(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorWaitFrame(mcdr_executor* executor, uint64_t fenceValue,
                                                           int64_t timeoutMs);

// ---- Async: fence manager ------------------------------------------------------
//
// The only place native calls back into the JVM. `cb` is a single FFM upcall stub created once; `userHandle`
// identifies the Java-side registration, so no JVM object reference is retained. The callback runs OUTSIDE the
// registry lock, so it may re-register or unregister; timeoutMs == 0 waits forever.

typedef void(MCDR_CALL* mcdr_fence_callback_fn)(int64_t userHandle, uint64_t value, int32_t completed);

MCDR_API mcdr_fence_mgr* MCDR_CALL mcdr_dx12AsyncFenceCreate(void);
MCDR_API void MCDR_CALL mcdr_dx12AsyncFenceDestroy(mcdr_fence_mgr* manager);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceSignal(mcdr_fence_mgr* manager, uint64_t value);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceWait(mcdr_fence_mgr* manager, uint64_t value,
                                                   int64_t timeoutMs);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceIsComplete(mcdr_fence_mgr* manager, uint64_t value);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceRegisterCallback(mcdr_fence_mgr* manager, uint64_t value,
                                                               mcdr_fence_callback_fn cb, int64_t userHandle,
                                                               int64_t timeoutMs);

// ---- 着色率图像（tier 2 SRI） --------------------------------------------------
// compute 读上一帧深度（t0）与速度（t1），写一张 R8_UINT 的 tile 表（u0）：每个 texel 管一整块 tile，值是
// D3D12_SHADING_RATE_* 的枚举。图像尺寸是 **tile 数**（ceil(pixel / tile)），tile 边长看设备能力。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineShadingRate(const void* dxil,
                                                                         int32_t dxilSize);
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateShadingRateImage(int32_t width, int32_t height);

// 在已打开的命令列表上生成一次。srvDepth/srvMotion/srvPrevious 是 TextureView 句柄：t0 深度、t1 速度、
// t2 上一帧的着色率图像（读不到历史时可传同一张有效视图）。image 是要写的那张图像。
// 返回 1=已派发 0=参数或资源不对 -2=没有打开的命令列表。
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchComputeShadingRate(
    mcdr_command_ctx* ctx, mcdr_object* pipeline, mcdr_object* srvDepth, mcdr_object* srvMotion,
    mcdr_object* srvPrevious, mcdr_object* image, const int32_t* constants, int32_t constantCount,
    int32_t x, int32_t y, int32_t z);

// ---- 通用 compute 纹理 pass（3×SRV t0/t1/t2 + 1×UAV u0 + b0 的 8 个 int） ------
//
// 根签名与上面的着色率图像那条**完全相同**，只是用途泛化，实现直接复用那两个函数。image 必须带 usage
// 位 32（ALLOW_UNORDERED_ACCESS），否则返回 0。srv0/srv1/srv2 必须是带 CPU 描述符句柄的 TextureView；
// 用不到的槽传同一张有效视图（三张都必须有，这是固定根签名）。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineTexture3(const void* dxil,
                                                                     int32_t dxilSize);
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchComputeTexture3(
    mcdr_command_ctx* ctx, mcdr_object* pipeline, mcdr_object* srv0, mcdr_object* srv1,
    mcdr_object* srv2, mcdr_object* image, const int32_t* constants, int32_t constantCount,
    int32_t x, int32_t y, int32_t z);

// 把图像挂到列表的 SRI 槽位（image 传 NULL 摘掉）；内部把图像从 COMMON 转到 SHADING_RATE_SOURCE。
// 返回 1=已绑定 0=列表不是 ID3D12GraphicsCommandList5 -1=不是纹理 -2=没有打开的命令列表。
MCDR_API int32_t MCDR_CALL mcdr_dx12SetShadingRateImage(mcdr_command_ctx* ctx, mcdr_object* image);

// ---- NRD 实例清单（纯 CPU，不需要设备） --------------------------------------
//
// 建一个 NRD 实例读它算出来的 InstanceDesc，把 pipelines / CB 上限 / 纹理池 / 寄存器空间写进 outText
// （每行一条事实，UTF-8，'\n' 分隔，末尾也带 '\n'）。返回 1=成功 0=这份 DLL 没编进 NRD
// -1=建实例失败或没有描述（原因已在 outText 里）。调用方负责把每行打出去。
MCDR_API int32_t MCDR_CALL mcdr_dx12NrdInspect(char* outText, int32_t capacity);
// 建常驻的 NRD 上下文：实例 + 14 条 PSO + 两套纹理池 + 描述符堆。
// 返回 1=就绪 0=没编进 NRD -1=失败；outText 逐行写结论，失败时第一行就是失败的那一步。
MCDR_API int32_t MCDR_CALL mcdr_dx12NrdInit(int32_t width, int32_t height, char* outText,
                                            int32_t capacity);
MCDR_API int32_t MCDR_CALL mcdr_dx12NrdShutdown(void);

// 逐帧降噪派发（S4 4-c）：五张纹理按 motion / normal-roughness / viewZ / radiance-in /
// radiance-out 的顺序传，matrices 是 4 个列主序 4x4（当前视投影、上一帧视投影、当前世界到视图、
// 上一帧世界到视图）。返回派发的 pass 数，失败为 0 或 -2（没有打开的列表）。
MCDR_API int32_t MCDR_CALL mcdr_dx12NrdDenoise(mcdr_command_ctx* ctx, mcdr_object* const* textures,
                                               int32_t textureCount, const float* matrices,
                                               uint32_t frameIndex, uint32_t resetHistory, char* outText,
                                               int32_t capacity);

// ---- DLSS-D / Ray Reconstruction 逐帧求值（S4 4-d） ------------------------
//
// 游戏进程里 NGX 由 Streamline 持有，所以这里必须走 slSetTagForFrame + slEvaluateFeature，不能自己调
// NVSDK_NGX_D3D12_Init_*（同进程双 NGX 会撞 0xBAD00005）。textures 顺序：
//   [0] 带噪辐亮度（color 输入） [1] 输出颜色 [2] 深度 [3] 运动矢量
//   [4] 法线（packed 模式下是打包后的法线-粗糙度）
//   [5] 粗糙度（仅 unpacked 模式，textureCount == 6）
// width/height 是渲染尺寸，outputWidth/outputHeight 是输出尺寸（超分档位不同则两者不等）。
// constants 是 slSetConstants 的载荷，72 个浮点：四个行主序 4x4（viewToClip / clipToView /
// clipToPrevClip / prevClipToClip）+ near / far / fovY / aspect；行主序是 SL 的约定（float4x4 = row[4]）。
// 返回 1=SL 接受 0=未执行（SL 缺函数/参数不全）-1=求值失败 -2=没有打开的命令列表；
// outText 一行诊断，成功与失败都会写。
MCDR_API int32_t MCDR_CALL mcdr_dx12DlssdEval(mcdr_command_ctx* ctx, mcdr_object* const* textures,
                                              int32_t textureCount, uint32_t width, uint32_t height,
                                              uint32_t outputWidth, uint32_t outputHeight,
                                              uint32_t frameIndex, int32_t reset,
                                              const float* constants, char* outText,
                                              int32_t capacity);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MCDR_ABI_H