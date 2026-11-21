// mcdr_abi.h — public C ABI of the MCDR native render library: the only contract between the native
// library and the JVM side (FFM, no JNI, no name mangling, no exceptions across the boundary).
// Boolean entry points return int32_t 1/0 and mcdrLastError() carries the message. Strings are
// caller-allocated UTF-8 buffers and the callee returns the required byte count (including the NUL).
// Windows x64 mapping: MSVC `long` is 32-bit, so every int32_t here is JAVA_INT on the Java side
// (only int64_t/size_t/pointers are JAVA_LONG/JAVA_ADDRESS).

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

// Bumped whenever ANY struct layout or signature below changes; the Java side asserts equality at load
// time and aborts backend creation on mismatch. Any new aggregate or signature must update the matching
// Kotlin mirrors and native metadata; the loader rejects a partial update before device creation.
#define MCDR_ABI_VERSION 34u

// The same value at runtime, so a stale Java-side constant cannot be optimised away and a mismatched
// pair fails at load time rather than at first draw.
MCDR_API uint32_t MCDR_CALL mcdrABIVersion(void);

// Opaque handles: the Java side only ever sees a MemorySegment address and must never invent one.
// C++ forward declarations MUST live inside namespace d3d12_mc and use the SAME class-key as the
// definition — a global-scope `struct Dx12Object;` is a *different type*, so the typedef would name
// an incomplete global type and every adapter call site would fail with C2664; a mixed class/struct
// key is C4099, a hard error under TreatWarningAsError=true.
#ifdef __cplusplus
namespace d3d12_mc {
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
#else
typedef struct mcdr_object mcdr_object;
typedef struct mcdr_command_ctx mcdr_command_ctx;
typedef struct mcdr_pipeline mcdr_pipeline;
typedef struct mcdr_surface mcdr_surface;
typedef struct mcdr_query_pool mcdr_query_pool;
#endif

typedef struct mcdr_desc_alloc mcdr_desc_alloc;
typedef struct mcdr_bundle_pool mcdr_bundle_pool;
typedef struct mcdr_executor mcdr_executor;
typedef struct mcdr_fence_mgr mcdr_fence_mgr;

// Every struct below has an exact Java mirror built from MemoryLayout; offsets and sizes are asserted
// at runtime by mcdrABILayout(), never by hand.

// One pipeline binding slot, shared by mcdr_dx12PushDescriptors and the bundle recorder.
typedef struct mcdr_binding_slot {
    int32_t type;          // 0 = CBV, 1 = SRV (texture view), 2 = SRV (texel buffer)
    int32_t texelFormat;   // type 2: GpuFormat ordinal, -1 = RAW SRV (offset aligned to 4 bytes)
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

// One colour target of a graphics pipeline. format == -1 means "unused slot" (DXGI_FORMAT_UNKNOWN).
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

// HLSL input element. `semanticName` is a NUL-terminated ASCII string.
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

// Full graphics-pipeline description. `vsData`/`psData` point at HLSL source with entry `main`
// (not NUL-terminated).
typedef struct mcdr_pipeline_desc {
    const void* vsData;
    int32_t vsLen;
    int32_t _pad0;
    const void* psData;
    int32_t psLen;
    int32_t colorCount;
    const mcdr_color_target_desc* colorTargets;
    int32_t hasDepth;
    uint32_t pipelineFlags;
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

#define MCDR_OIT_LANE_DEPTH_COPY 1u
#define MCDR_OIT_LANE_BOUNDS_COPY 2u
#define MCDR_OIT_LANE_BOUNDS_CULL 3u
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateOitLaneOperationPipelineV1(
    const mcdr_pipeline_desc* descriptor, uint32_t kind, uint64_t programGeneration, uint32_t projectionBinding);
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateOpaqueSeedPipelineV1(
    const mcdr_pipeline_desc* descriptor, int32_t pushConstantBytes, const void* tileVertex, int32_t tileVertexBytes,
    const void* tileFragment, int32_t tileFragmentBytes, uint64_t programGeneration);
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateOitTileIAPipelineV1(
    const mcdr_pipeline_desc* descriptor, int32_t pushConstantBytes, const void* tileVertex, int32_t tileVertexBytes,
    const void* tileFragment, int32_t tileFragmentBytes, uint64_t programGeneration,
    const uint32_t* laneRegisters, uint32_t stage);
MCDR_API int32_t MCDR_CALL mcdr_dx12AttachOitTileIARouteV1(mcdr_pipeline* source, mcdr_pipeline* base);

// Mesh-shader (amplification / mesh) pipeline description. `asData`/`msData`/`psData` point at DXIL
// bytecode produced by DXC (as_6_x / ms_6_x / ps_6_x) — NOT HLSL and NOT DXBC (mesh shaders cannot be
// compiled by D3DCompile/fxc). `msData` is required; `asData` may be NULL/0 (no amplification stage) and
// `psData` may be NULL/0 (no pixel shader, in which case nothing is written to the colour targets).
// Bindings reuse mcdr_pipeline_binding_desc with the SAME layout contract as mcdr_pipeline_desc
// (CBV -> root descriptor, SRV -> the single descriptor table at root index cbvCount).
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
    int32_t rasterMotion;
    int32_t _pad4;
} mcdr_mesh_pipeline_desc;

// ABI self-check: the native side publishes one entry per mirrored struct field and the Java side walks
// the same list from its own MemoryLayouts, failing fast on the first mismatch.
typedef struct mcdr_abi_field {
    const char* name;   // "mcdr_binding_slot.buffer"
    uint64_t offset;
    uint64_t size;
} mcdr_abi_field;

MCDR_API void MCDR_CALL mcdrABILayout(const mcdr_abi_field** fields, int32_t* count);

// Copies the calling thread's last error message into `buf` as UTF-8 and returns the required byte count
// including the NUL terminator, or 0 when no error is pending. buf == NULL / cap == 0 = size query only.
MCDR_API int32_t MCDR_CALL mcdrLastError(char* buf, int32_t cap);

// Verbosity for the native log, mirroring DX12_LOG_VERBOSE.
MCDR_API void MCDR_CALL mcdrSetLogVerbose(int32_t enabled);

// Native log level: 0=error, 1=warning, 2=info, 3=debug.
MCDR_API void MCDR_CALL mcdrSetLogLevel(int32_t level);

// Progress markers on the shutdown path (destroySurface/destroyDevice) are INFO lines: 1 (default) keeps
// them visible at any log level, 0 makes them plain INFO (filtered by the level). Config: `[logging] shutdownTrace`.
MCDR_API void MCDR_CALL mcdrSetShutdownTraceAlways(int32_t always);

// 把每条 native 日志同时镜像到该文件（配置项 `[logging] nativeLogFile`）。`utf8Path` 为 UTF-8 路径，相对
// 路径按进程工作目录（游戏目录）解析，追加写、逐行 flush；NULL 或空串 = 关闭镜像并释放已打开的句柄。
// 返回 1=已配置 0=已关闭（能否真的落盘要到下一次写日志时才知道，这里不做同步 open）。
MCDR_API int32_t MCDR_CALL mcdrSetLogFile(const char* utf8Path);

// Set (or, with a null/empty value, clear) one process-wide environment variable, so the handful of native
// switches still read with getenv()/_wgetenv() (DX12_DEBUG_LAYER, MCDR_DLSS_IMPL, DX12_NATIVE_LOG_FILE, ...)
// can be driven from config/dxr_mc-client.toml instead of the launcher environment. Must be called before
// the first device is created; later calls only affect reads that happen afterwards.
MCDR_API void MCDR_CALL mcdr_dx12SetEnvironment(const char* name, const char* value);

// PIX GPU capture: load Microsoft PIX's capture layer (WinPixGpuCapturer.dll) into this process. The layer
// hooks D3D12, so it has to be in place BEFORE the first device is created; irreversible for the process.
// Returns 1 when loaded, 0 when capture was not requested, -1 when it was requested but the DLL could not
// be found or loaded (an ERROR line lists the searched paths). Idempotent.
MCDR_API int32_t MCDR_CALL mcdrSetPixCapture(int32_t enabled);

// Where PIX's DLLs live (a DLL itself or the directory holding them); the mod's [debug] pixDirectory.
// Call it before mcdrSetPixCapture, because the layer must be loaded before the first device is created.
// An empty string clears the setting and restores automatic detection (next to MCDR.dll, then the newest
// %ProgramFiles%\Microsoft PIX\<version>). 1 = accepted, 0 = cleared, -1 = not valid UTF-8 or missing.
MCDR_API int32_t MCDR_CALL mcdrSetPixDirectory(const char* utf8Path);

// PIX timing capture, driven by this process instead of by PIX's UI: PIXBeginCapture(PIX_CAPTURE_TIMING,
// {file_name}) ... PIXEndCapture(0). The runtime and capturer DLLs are loaded by mcdrSetPixCapture, or
// lazily here.
// `utf8Path` is the full path of the .wpix to write; the caller creates the directory. Call it on a thread
// of its own: PIX's runtime sets up COM on load and the render thread's apartment is already fixed
// (calling from there answers RPC_E_CHANGED_MODE).
// Returns 1 = started, 0 = already running, otherwise the negative HRESULT that explains it (E_FAIL =
// runtime DLLs missing, E_INVALIDARG = empty path, HRESULT_FROM_WIN32(ERROR_TIMEOUT) = PIX did not answer,
// E_ACCESSDENIED = not elevated); mcdrLastError() carries the readable reason. Requires an elevated process.
MCDR_API int32_t MCDR_CALL mcdrStartPixTimingCapture(const char* utf8Path);

// Stops the capture that mcdrStartPixTimingCapture started. PIX writes the file out on its own thread, so
// this returns without waiting. Returns 1 = stop requested, 0 = nothing was recording, negative HRESULT
// when the writer thread could not be started.
MCDR_API int32_t MCDR_CALL mcdrStopPixTimingCapture(void);

// Variable-rate shading: the shading-rate control point. `rate` is a raw D3D12_SHADING_RATE value
// (1X1=0x0, 1X2=0x1, 2X1=0x4, 2X2=0x5, 2X4=0x6, 4X2=0x9, 4X4=0xA) and applies to every graphics command
// list the renderer starts from then on; -1 turns the control off — no RSSetShadingRate call at all, the
// device keeps its default 1X1 rate.
// Returns 1 = accepted, 0 = the device cannot honour it (no VRS at all, or 2X4/4X2/4X4 without
// AdditionalShadingRatesSupported — only answerable once the device exists), -1 = unknown rate value. A
// request that arrives before the device is created is accepted and gated later, on the first command list.
MCDR_API int32_t MCDR_CALL mcdrSetShadingRate(int32_t rate);

// The **scoped** shading-rate control point: `rate` is one of the seven raw D3D12_SHADING_RATE values
// (0x0/0x1/0x4/0x5/0x6/0x9/0xA; -1 is *not* accepted here), issued as `RSSetShadingRate` on `ctx`'s
// currently open command list, right now. Unlike mcdrSetShadingRate it neither reads nor writes the stored
// setting: the caller brackets exactly the draws that may be shaded coarsely and restores 1X1 afterwards.
// Legal in the middle of a "pass" — a pass here is just OMSetRenderTargets, never BeginRenderPass.
// Returns 1 = issued, 0 = this device cannot honour the rate (VRS tier 0, or 2X4/4X2/4X4 without
// AdditionalShadingRatesSupported) or the list has no ID3D12GraphicsCommandList5, -1 = unknown rate value,
// -2 = ctx is null or has no open command list (caller bug; WARN once).
MCDR_API int32_t MCDR_CALL mcdr_dx12SetShadingRateNow(mcdr_command_ctx* ctx, int32_t rate);

// The two shading-rate combiners used by every RSSetShadingRate this module issues; each takes a
// D3D12_SHADING_RATE_COMBINER value (PASSTHROUGH=0 OVERRIDE=1 MIN=2 MAX=3 SUM=4). Stage 0 combines the
// per-draw rate with the per-primitive rate (a mesh shader's SV_ShadingRate), stage 1 combines that result
// with the shading-rate image (tier 2). Defaults are {PASSTHROUGH, PASSTHROUGH}. Returns 1 = stored,
// -1 = a value outside 0..4.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetShadingRateCombiners(int32_t first, int32_t second);

// Anti-aliasing mode, same numbering as the Java-side option (0=OFF, 1=SMAA, 2=TAA, 3=DLAA). SMAA runs
// three full-screen passes over the world layer right after the world and before the GUI
// (mcdr_dx12ApplySmaaToTexture); applying it at the final blit would soften the GUI too. TAA keeps an
// RGBA16F history ping-pong inside the device and runs at the same point. DLAA is accepted but treated as
// OFF (a WARN names it). No restart: a mode change takes effect on the next frame.
// Returns 1 = mode in effect (OFF, SMAA, TAA), 0 = accepted but not implemented (DLAA), -1 = unknown mode.
MCDR_API int32_t MCDR_CALL mcdrSetAntiAliasing(int32_t mode);

// DLSS upscaling on/off, independent of the anti-aliasing mode and of mcdr_dx12SetDlssVendorDir; the Java
// side owns it as its own option, not as a value of the AA mode. Idempotent and callable before the device
// exists (only the state is recorded). Returns 1 = enabled afterwards, 0 = disabled afterwards; availability
// is reported by mcdr_dx12QueryDlss, and a missing / older DLL simply keeps this switch inert.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssEnabled(int32_t enabled);

// Snapshot the world depth into a private D32 texture for DLSS. Call it from the point where the depth
// buffer still holds this frame's world depth — alongside the motion-vector capture, BEFORE vanilla's
// `clearDepthTexture` wipes it to 0.0 (reverse-Z "far"); a cleared depth makes DLSS's depth term constant.
// The state of `depth` is restored to whatever it was on entry, so the caller's world-render state machine
// is not disturbed.
// Returns 1 = snapshot taken/reused, 0 = unusable arguments (evaluation still uses the caller's depth),
// -1 = creation/copy failed (one WARN, caller's depth used instead). Never fails the frame.
MCDR_API int32_t MCDR_CALL mcdr_dx12SnapshotDlssDepth(mcdr_command_ctx* ctx, mcdr_object* depth);

// The DLSS quality tier: 0 = Performance, 1 = Balanced, 2 = Quality (same order as the Java enum), default
// 2. Out-of-range is rejected with -1 and changes nothing; otherwise the normalised tier is returned.
// Idempotent and callable before the device exists (only the state is recorded). A change REBUILDS the DLSS
// feature even when the render/target sizes are unchanged, because the perf-quality value is an input to
// CreateFeature, not an Evaluate parameter.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssQuality(int32_t tier);
// 帧生成（DLSS-G）。mode: 0=off 1=on（固定倍率） 2=dynamic；numFrames 1..5（见 slDLSSGGetState）。
// 选项在渲染线程的下一次 present 生效；Reflex 必须开启，否则 status 会带 eFailReflexNotDetectedAtRuntime。
MCDR_API void MCDR_CALL mcdr_dx12SetDlssgMode(int32_t mode, int32_t numFrames);

// 上一次 Present 真正提交的帧数（1 = 没插帧）。F3 的 output 用它累积。
MCDR_API int32_t MCDR_CALL mcdr_dx12GetDlssgPresented(void);

// The DLSS *model*, exposed as NGX's "render preset" hint.
//   preset: a NVSDK_NGX_DLSS_Hint_Render_Preset value —
//           0  = Default (NGX picks per tier: K for DLAA/Balanced/Quality, M for Performance,
//                L for Ultra Performance),
//           5  = E, 6 = F   (deprecated CNN presets),
//           10 = J, 11 = K, 12 = L, 13 = M   (transformer presets).
// Out-of-range is rejected with -1 and changes nothing; otherwise the recorded value is returned.
// Idempotent and callable before the device exists (only the state is recorded); a change REBUILDS the
// DLSS feature even when the render/target sizes are unchanged, because the preset is an input to
// CreateFeature, not an Evaluate parameter.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssRenderPreset(int32_t preset);

// Hand DLSS the UAV texture the Java side created through the module's own texture path, to be used as
// NGX's output (a target-sized UAV texture, `USAGE_STORAGE = 32` = ALLOW_UNORDERED_ACCESS).
// Passing NULL clears the override and restores the private fallback. The resource is NOT owned by the
// native side; a size/format mismatch against the render target is an ERROR and falls back.
// Returns 1 = accepted, 0 = cleared, -1 = the handle is not a live texture.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssOutput(mcdr_object* tex);

// DLSS super-resolution (NVSDK_NGX) data plane: everything one per-frame Evaluate needs arrives in ONE
// struct, so the Java side fills it in once and makes one call.
// pInTransparencyMask stays NULL (Minecraft has no reactive-mask pass); exposureMode stays
// MCDR_DLSS_EXPOSURE_NONE (vanilla tone-maps in-shader and writes no exposure buffer), so DLSS uses the
// constant exposureScale. Per-object motion vectors do not exist (the velocity pass reprojects the camera
// only, so moving entities ghost slightly) — that is accepted, not a bug in this call.
// Minecraft 26.x is REVERSE-Z, so depthInverted must be true.

// How the constant exposure value below is interpreted.
#define MCDR_DLSS_EXPOSURE_NONE 0u   // pure constant: exposureScale is the pre-exposure
#define MCDR_DLSS_EXPOSURE_AUTO 1u   // current-frame GPU exposure texture in pInExposure

// Everything one DLSS Evaluate needs. pIn* fields are opaque mcdr_object* texture handles; every other
// field is a plain value. sizeof = 112, alignment = 8 — MCDR_Layout.kt mirrors it offset by offset.
// _pad0 covers the four bytes the compiler would otherwise insert between depthFormatOrdinal and the first
// pointer; it is a named field on purpose — binary layout checks can only see named members, so do not
// remove it.
typedef struct mcdr_dlss_input {
    int32_t renderWidth;   // INPUT size, px = window * dxrMcRenderScale (the DLSS render size)
    int32_t renderHeight;  // INPUT size, px
    int32_t targetWidth;   // OUTPUT size, px = window size = GUI target = swapchain (DLSS target size)
    int32_t targetHeight;  // OUTPUT size, px
    float jitterOffsetX;   // pixels, RENDER space, not normalised and NOT negated (Halton(2,3), [-0.5,+0.5))
    float jitterOffsetY;
    float mvScaleX;        // multiplies the raw MV texture; D3D12Velocity.kt writes pixels, so 1.0/1.0
    float mvScaleY;
    float preExposure;     // scales the rendering before DLSS; constant exposure means keep 1.0
    float exposureScale;   // read only when exposureMode == MCDR_DLSS_EXPOSURE_NONE; 1.0 = already exposed
    uint32_t exposureMode;     // MCDR_DLSS_EXPOSURE_NONE (0) or _AUTO (1)
    uint32_t alphaUpscaling;   // 1 = upscale alpha too. False for the Minecraft world layer.
    float nearZ;               // camera near plane, from Projection.zNear()
    float farZ;                // camera far plane, from Projection.zFar()
    uint32_t depthInverted;    // 1 = reverse-Z (this renderer). Must stay 1.
    uint32_t reset;            // 1 = drop history this frame (first frame, resize, world switch)
    int32_t depthFormatOrdinal;  // GpuFormat ordinal of the depth attachment (diagnostics)
    int32_t _pad0;
    mcdr_object* pInColor;          // world layer, render size, colour (SRGB or UNORM)
    mcdr_object* pInDepth;          // main render target depth, render size, D32_FLOAT
    mcdr_object* pInMotionVectors;  // D3D12Velocity.kt output, render size, RG16_FLOAT
    mcdr_object* pInOutput;         // target-size colour target (the GUI's own target)
    mcdr_object* pInExposure;
} mcdr_dlss_input;

// One-line DLSS availability/state query for the UI and the log. `note` receives a NUL-terminated
// human-readable status (the reason when DLSS cannot run); NULL is fine.
// Returns 1 = DLSS can run (NGX runtime present, adapter supports super sampling, NGX initialised, optimal
// -setting query succeeded), 0 = not available or no device yet (`note` says why), -1 = invalid arguments.
// Safe to call before the device exists (returns 0 with "device not created").
MCDR_API int32_t MCDR_CALL mcdr_dx12QueryDlss(char* note, int32_t noteCap);

// Where the vendor runtime DLLs live, as a UTF-8 absolute directory (`<user.dir>\dxrmc\`, the directory the
// Java side extracted MCDR.dll and the vendor DLLs into). That directory is NOT on the default DLL search
// path, so the native side widens it (SetDefaultDllDirectories + SetDllDirectoryW + AddDllDirectory) before
// it loads the NGX runtime. Idempotent; safe before the device exists.
// Returns 1 = accepted and the search path widened, 0 = input rejected or empty.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssVendorDir(const char* utf8Dir);

// 超分（Super Resolution）抽象层的 v3 契约：厂商无关载荷，厂商差异下沉到 native Provider。
// 调用时序（与 DLSS 路径同点：世界层画完、GUI 之前，命令列表开着）：
//   mcdr_dx12SrPick() 只记录期望后端（重建发生在下一帧帧首）；mcdr_dx12SrQueryCaps() 无命令列表也可调；
//   mcdr_dx12SrCreate() 要求命令列表开着；mcdr_dx12SrResize() 渲染/显示尺寸变化时通知；mcdr_dx12SrJitter()
//   0 = 无意见，沿用 D3D12Temporal；mcdr_dx12SrDestroy() 只能在帧首且无打开命令列表时调用；
//   mcdr_dx12SrQuery() 一行可用性文本（日志与 UI）。
// 资源指针一律是 **JVM 创建的纹理句柄**（Dx12Object*，即 D3D12GpuTexture.handle() 的值），不是
// ID3D12Resource*；传 0 表示「这张资源没有」。

#define MCDR_SR_BACKEND_NONE 0
#define MCDR_SR_BACKEND_DLSS 1
#define MCDR_SR_BACKEND_FSR 2
#define MCDR_SR_BACKEND_XESS 3

// mcdr_sr_caps.takesMasks / mcdr_sr_dispatch 的掩码能力位。三家语义不同（SL 的 kBufferTypeReactiveMaskHint、
// FFX 的 reactive/transparencyAndComposition、XeSS 的 responsive pixel mask），这里是并集，具体怎么喂由 Provider 决定。
#define MCDR_SR_MASK_REACTIVE 1
#define MCDR_SR_MASK_RESPONSIVE 2
#define MCDR_SR_MASK_TRANSPARENCY_AND_COMPOSITION 4

// 后端能力面。sizeof = 80、align = 4（Kotlin 侧镜像见 MCDR_Layout.kt 的 SR_CAPS）；
// note 是给 UI 与日志的短原因（UTF-8，NUL 结尾），不是给机器判断的。
typedef struct mcdr_sr_caps {
    int32_t backend;         // MCDR_SR_BACKEND_*
    int32_t available;       // 1 = 本机此刻可用
    int32_t coupledDenoise;  // 1 = 该后端自带降噪（DLSS-D / FSR4 ML）
    int32_t needsExposure;   // 1 = 需要曝光纹理
    int32_t takesMasks;      // MCDR_SR_MASK_* 位集
    int32_t externalHeap;    // 1 = 接受调用方提供的临时堆
    int32_t jitterPhaseCount;// 0 = 无意见（沿用 D3D12Temporal 的 8）
    int32_t minRenderWidth;
    int32_t minRenderHeight;
    int32_t maxRenderWidth;
    int32_t maxRenderHeight;
    int32_t nativeAA;        // 1 = 允许 1.0x（DLAA 式）
    int32_t reserved;        // 保留，必须为 0（给后续 flags 留位，不动 sizeof）
    char note[28];           // 短原因，NUL 结尾
} mcdr_sr_caps;

// 与厂商无关的每帧载荷。sizeof = 144、align = 8（composition mask 是末尾追加字段），Kotlin 侧镜像见 SR_DISPATCH。
// 约定（与 mcdr_dlss_input 一致，别改）：渲染尺寸是**渲染目标**尺寸，显示尺寸是**窗口**尺寸；jitter 是
// **渲染空间像素**、不归一化、不取负；motionVectorScale 在「MV 已是像素位移」时给 1.0。
// `cameraAspectRatio` / `viewSpaceToMetersFactor` 是可选字段 —— 0 一律表示"未填"，后端按安全默认处理
// （aspect = displayWidth/displayHeight、meters = 1.0）。`cameraVerticalFov` 就是 FSR 的
// `cameraFovAngleVertical`（弧度），两处必须同源：取 `GameRenderer.getFov` 的实际值，不用视场设置的标称值。
struct mcdr_raster_camera;

typedef struct mcdr_sr_dispatch {
    int32_t renderWidth;
    int32_t renderHeight;
    int32_t displayWidth;
    int32_t displayHeight;
    float jitterOffsetX;
    float jitterOffsetY;
    float motionVectorScaleX;
    float motionVectorScaleY;
    float preExposure;
    float frameTimeDeltaMs;
    float cameraNear;
    float cameraFar;
    float cameraVerticalFov;      // 弧度
    float sharpness;              // 0 = 不锐化
    int32_t reset;                // 1 = 丢弃历史
    int32_t hdrInput;             // 1 = 输入是 HDR 线性色
    int32_t backend;              // MCDR_SR_BACKEND_*（0 = 用 SrPick 选的那个）
    int32_t flags;                // 保留，必须为 0
    void* pColorInput;            // 渲染尺寸颜色
    void* pColorOutput;           // 显示尺寸颜色（UAV 写）
    void* pDepth;                 // 渲染尺寸深度
    void* pMotionVectors;         // 渲染尺寸 RG16F
    void* pReactiveMask;          // R8，可 0
    void* pExposure;              // R32F，可 0
    int32_t depthFormatOrdinal;   // 与 mcdr_dlss_input 的同名字段同义
    float cameraAspectRatio;      // 显示尺寸的宽高比（FSR 可选）
    float viewSpaceToMetersFactor;// 1.0 = 一个世界单位算 1 米；0 = 未填（后端按 1.0 处理）
    int32_t _pad2;                // 补到 136 = align 8 的整数倍
    void* pTransparencyAndComposition; // FSR transparency-and-composition mask，可 0
    const struct mcdr_raster_camera* pCamera;
} mcdr_sr_dispatch;

// 记录期望后端。纯记录：不建、不销毁、不重建（重建在下一帧帧首）。返回 1 = 已记录，0 = 后端编号未知，
// -1 = 参数非法。Never throws.
MCDR_API int32_t MCDR_CALL mcdr_dx12SrPick(int32_t backend);

// 查询后端能力面。out 必须非空；*out 会被整体覆盖（先清零再填）。
// 返回 1 = 已填（available 字段才是结论），-1 = 参数非法（含 backend 越界）。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrQueryCaps(int32_t backend, mcdr_sr_caps* out);

// 建/唤醒后端。必须在命令列表打开时调用。maxRenderWidth/Height 是**上界**（热切换与 resize 时用来判
// 要不要重建），displayWidth/Height 是本帧的窗口尺寸。
// 返回 1 = 就绪（可以接着 Evaluate），0 = 本机不可用（已 log 一次），-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrCreate(mcdr_command_ctx* ctx, int32_t backend,
                                              int32_t maxRenderWidth, int32_t maxRenderHeight,
                                              int32_t displayWidth, int32_t displayHeight);

// 尺寸变化通知。返回 1 = 已接受，0 = 后端未就绪（忽略），-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrResize(mcdr_command_ctx* ctx, int32_t renderWidth,
                                              int32_t renderHeight, int32_t displayWidth,
                                              int32_t displayHeight);
typedef enum mcdr_sr_prepare_result {
    MCDR_SR_PREPARE_READY = 1,
    MCDR_SR_PREPARE_NOT_READY = 0,
    MCDR_SR_PREPARE_INVALID_ARGUMENT = -1,
    MCDR_SR_PREPARE_REJECTED = -2,
    MCDR_SR_PREPARE_COMPLETION_UNKNOWN = -3
} mcdr_sr_prepare_result;

MCDR_API int32_t MCDR_CALL mcdr_dx12SrPrepare(mcdr_command_ctx* ctx,
                                               const mcdr_sr_dispatch* description,
                                               int32_t dlssQuality, int32_t dlssPreset,
                                               uint64_t* token);
MCDR_API int32_t MCDR_CALL mcdr_dx12SrCommit(uint64_t token);
MCDR_API int32_t MCDR_CALL mcdr_dx12SrAbort(uint64_t token);

// 该后端偏好的亚像素 jitter。返回 1 = 已写 outX/outY，0 = 无意见（沿用 D3D12Temporal 的
// Halton(2,3)，这时 outX/outY 不被修改），-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrJitter(int32_t frameIndex, float* outX, float* outY);

// 该后端期望的 jitter 相位长度（0 = 不适用），参数非法返回 -1。
// DLSS / XeSS 报 8；FSR 报 `8 * (displayWidth / renderWidth)^2` 并夹到 [1, 256]。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrJitterPhaseCount(int32_t backend, int32_t renderWidth,
                                                      int32_t displayWidth);

// 释放后端资源。只允许在没有打开命令列表时调用（帧首）；重复调用安全。
MCDR_API void MCDR_CALL mcdr_dx12SrDestroy(void);

// 一行可用性文本：当前/期望后端、各自 available、最近一次失败原因。
// 返回 1 = 已写（buf 为 NUL 结尾），0 = 没有内容，-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrQuery(char* note, int32_t noteCap);

// 帧生成（Frame Generation）抽象层。与 SR 段同构（能力表 + 逐帧载荷 + 入口），区别在于**没有 Evaluate**：
// 帧生成发生在 Present 的两侧，不由我们的命令列表录制，所以只有「设期望值」与「问上次真提交了几帧」。
// DLSS-G（Streamline）占走 Present、链必须由 SL 代理工厂产出、必须 Reflex，HUDless 复用 SR 的输出；
// FSR-FG / XeSS-FG **接管交换链**（Wrap/New 或 InitFromSwapChain），与 SL 代理链二选一，v1 只有编号与能力位。

#define MCDR_FG_BACKEND_NONE 0
#define MCDR_FG_BACKEND_DLSSG 1
#define MCDR_FG_BACKEND_FSRFG 2
#define MCDR_FG_BACKEND_XESSFG 3

// 呈现链当前归谁（可查询状态，不再是隐含的 if）。v1 里只有前两态可达。
#define MCDR_FG_CHAIN_APP_NATIVE 0
#define MCDR_FG_CHAIN_SL_PROXY 1
#define MCDR_FG_CHAIN_FSR_WRAP 2  // 占位：v1 不实现
#define MCDR_FG_CHAIN_XESS_WRAP 3 // 占位：v1 不实现

// 本后端架构上对应的低延迟方案（能力面，不是「运行时可用」）：Reflex 现役且 FG 依赖它；Anti-Lag 2 只有
// 头文件、XeLL 的 DLL 在但契约没接，所以后两者的位**恒为 0**，原因写进 note。
#define MCDR_FG_LATENCY_NONE 0
#define MCDR_FG_LATENCY_REFLEX 1
#define MCDR_FG_LATENCY_ANTILAG2 2
#define MCDR_FG_LATENCY_XELL 3

// 后端能力面。sizeof = 80、align = 4（Kotlin 侧镜像见 MCDR_Layout.kt 的 FG_CAPS）；
// 与 mcdr_sr_caps 一样：note 是给 UI 与日志的短原因（UTF-8，NUL 结尾），机器判断只看数值。
typedef struct mcdr_fg_caps {
    int32_t backend;            // MCDR_FG_BACKEND_*
    int32_t available;          // 1 = 本机此刻可启用（唯一结论位）
    int32_t runtimeLoaded;      // 运行库已加载（头文件在 ≠ 这个）
    int32_t deviceSupported;    // 设备/驱动支持
    int32_t chainCompatible;    // 当前呈现链兼容
    int32_t needsReflex;        // 1 = 必须 Reflex 开（DLSS-G 恒 1）
    int32_t needsUiResource;    // 1 = 需要独立 UI 资源（FSR/XeSS）
    int32_t needsHudless;       // 1 = 需要独立 HUDless（FSR/XeSS）
    int32_t transitionsInputs;  // 1 = provider 自己转输入输出屏障
    int32_t maxGeneratedFrames; // 1..N（DLSS-G 当前 5）
    int32_t chainOwner;         // MCDR_FG_CHAIN_*（当前实际状态，不是期望）
    int32_t reserved;           // 保留，必须为 0
    char note[28];              // 短原因，NUL 结尾
    int32_t latencyProvider;    // MCDR_FG_LATENCY_*：架构上该配哪个，不是「能不能用」（看 available / note）
} mcdr_fg_caps;

// 与厂商无关的"设期望值"载荷。sizeof = 96、align = 8，Kotlin 侧镜像见 MCDR_Layout.kt 的 FG_DISPATCH。
// mode 与 mcdr_dx12SetDlssgMode 完全一致：0=off 1=固定倍率 2=dynamic；numFrames 只在 mode=1 时有意义，
// mode=2 由 provider 自己算。三轨帧号不同源，别混用：appFrameId 是应用帧、backendFrameId 给后端用
// （严格 +1）、presentId 给 present 用（严格 +1）。
// 资源指针是 **JVM 创建的纹理句柄**，与 SR 段一致；0 表示"这张没有"。pHudlessColor 给 0 是 v1 的常态
// （DLSS-G 复用 SR 的输出当 HUDless），只有 FSR/XeSS 才需要外部喂。
// 帧常量（View/Projection 是 XeSS-FG 的必需输入）：归属 FG 抽象层自己，不去翻 DLSS 的
// mcdr_dlss_constants；矩阵按行主序（与 XeSS-FG 的 `xefg_swapchain_frame_constant_data_t` 一致）。
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
// 下一次 present 前下发，这样跨线程调用不会落到不确定的那次 Present 上。
// 返回 1 = 已记录 0 = 后端不可用（原因进 FgStatus）-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgSetMode(const mcdr_fg_dispatch* desc);
typedef enum mcdr_fg_prepare_result {
    MCDR_FG_PREPARE_READY = 1,
    MCDR_FG_PREPARE_NOT_READY = 0,
    MCDR_FG_PREPARE_INVALID_ARGUMENT = -1,
    MCDR_FG_PREPARE_RESTART_REQUIRED = -2,
    MCDR_FG_PREPARE_REJECTED = -3
} mcdr_fg_prepare_result;

// NOT_READY 仅用于未结束的帧、录制或 GPU 工作；SDK/设备不支持请求时返回 REJECTED。
// 成功返回非零 token；调用方必须提交或中止，其他结果不会转移已生效配置的所有权。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgPrepare(const mcdr_fg_dispatch* description, uint64_t* token);
MCDR_API int32_t MCDR_CALL mcdr_dx12CommitPreparedConfiguration(uint64_t srToken,
    uint64_t fgToken, int32_t antiAliasing);
MCDR_API int32_t MCDR_CALL mcdr_dx12FgAbort(uint64_t token);

// 上一次 Present 真正提交的帧数（1 = 没插帧，与 mcdr_dx12GetDlssgPresented 同义）。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgPresented(void);

// 一行状态文本：期望/生效后端、可用性、链归属、最近一次失败原因。
// 返回 1 = 已写（NUL 结尾），0 = 没有内容，-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgStatus(char* note, int32_t noteCap);

// 帧生成的状态，压进一个 int32（位布局就是契约，JVM 侧按位读，不要解析文本）：
//   [ 0.. 7] desiredMode 0=关 1=开（固定倍率） 2=dynamic —— **native 侧的期望值**
//   [ 8..15] chainMode   链是在哪个 mode 下建成的（0=关的链 1=开的链）
//   [16..23] active      1 = 现在有一个活着的后端（0 = 没有后端，也就没有插帧）
//   [24..31] rebuilds    min(重建次数, 255)
// native 侧的回滚可能把 desiredMode 清成 0 而 options.txt 仍写着「开」，所以界面必须读这里而不是只读配置。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgState(void);

// 把 GUI 合成目标交给帧生成后端当「UI 纹理」用，否则准星/文字会被插帧拉扯（包装型后端要把「无 HUD 的
// 世界层」与「UI」分开插值）。
// `object` 是 `Dx12Object*` 句柄（就是 `D3D12GpuTexture.handle()` 那个值），传 0 = 撤销。**只在 GPU 目标
// 创建/重建时推一次**，不要每帧推。
MCDR_API void MCDR_CALL mcdr_dx12FgSetUiTexture(void* object);

// hudless 呈现开关。1 = 应用接下来 Present 的 backbuffer **只含世界层**（不含 GUI），UI 单独作为一张
// "透明底 + 预乘 alpha" 的纹理交给后端合成 —— 就是 mcdr_dx12FgSetUiTexture 推过来的那张；0 = 老路径，
// 应用自己把 GUI 画进 backbuffer，后端**不得**再注册 UI（否则同一份 HUD 会被合成两遍）。
// Ready 返回 1 时才敢 Present 不含 UI 的图，0 = 没人替我们合成 UI、必须继续 Present 合成层；它**不保证**
// 这一帧 UI 已登记（登记发生在同一个 Present 之前），所以按**每帧一次**的 latch 用，别在帧中途换模式。
MCDR_API void MCDR_CALL mcdr_dx12FgSetUiComposition(int32_t enabled);
MCDR_API int32_t MCDR_CALL mcdr_dx12FgUiCompositionReady(void);

// DLSS 的每帧相机常量（Streamline 的 sl::Constants 逐字段对应）。矩阵全部 **行主序** 且 **不含 jitter**
// （jitter 单独给）。三张矩阵是 JOML 列主序、列向量表示，Streamline 的行向量矩阵直接复制这16个元素即得
// 数学转置，**不得**再按行列下标重排。
// viewProjection/prevViewProjection 是同一相对原点的 world->clip（含相机平移，原点差在 double 里算完再写
// float）；projection 是 view->clip 本身，塞 VP 会让 SL 的反投影多带一层世界平移。
// `motionVectors3D` 必须是 0：本项目 MV 是屏幕空间像素位移，不是 3D 世界速度。
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

// PCL stats —— 帧开始（建本帧 token + eSimulationStart）与通用标记（eSimulationEnd=1 /
// eRenderSubmitStart=2 等，取值见 sl::PCLMarker）。
MCDR_API void MCDR_CALL mcdr_dx12ReflexFrameBegin(void);
MCDR_API void MCDR_CALL mcdr_dx12ReflexMarker(int32_t marker);

// NVIDIA Reflex 模式（0=off 1=on 2=on+boost）与可用性（1=可用）。
MCDR_API void MCDR_CALL mcdr_dx12SetReflexMode(int32_t mode);
MCDR_API int32_t MCDR_CALL mcdr_dx12ReflexAvailable(void);
// 1 = DLSS 可用（Streamline 的 DLSS 插件已加载，或非 Streamline 路径未探测到失败）；
// 0 = 明确探测到 Streamline 的 DLSS 插件没有初始化（NGX 版本不匹配时），界面据此置灰。
MCDR_API int32_t MCDR_CALL mcdr_dx12DlssAvailable(void);

// Reflex 自己的限帧器（微秒/帧，0 = 关闭）。配置项 [dlss] reflexFrameLimitUs 的唯一入口。
MCDR_API void MCDR_CALL mcdr_dx12SetReflexFrameLimit(int32_t micros);

// 把设备/适配器摘要写进 `buf`（UTF-8，NUL 结尾）；buf == NULL / cap == 0 时返回所需字节数。
// 返回 -1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12CreateDevice(char* buf, int32_t cap);

// 释放设备上下文中的全部 D3D12/DXGI 对象（device / queue / 描述符堆 / DXGI adapter）。**必须在进程退出前
// 调用**：这些 ComPtr 若留到 DLL_PROCESS_DETACH 才析构，Release() 会在 dxgi.dll 已 detach 之后执行，
// 产生原生异常并以异常码 2170 作为进程退出码。幂等，重复调用安全。
MCDR_API void MCDR_CALL mcdr_dx12DestroyDevice(void);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateTexture(int32_t usage, int32_t format, int32_t width,
                                                       int32_t height, int32_t depthOrLayers,
                                                       int32_t mipLevels);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateBuffer(int32_t usage, int64_t size);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateSampler(int32_t addressU, int32_t addressV, int32_t minFilter,
                                                       int32_t magFilter, int32_t maxAnisotropy,
                                                       float maxLod);

// 带显式 mip LOD bias 的采样器。只给**地形图集**（vanilla LevelRenderer 的 chunkLayerSampler）用；bias 由
// 超分比例决定（log2(render/display) - 0.5），不超分时调用方必须传 0，保持与 mcdr_dx12CreateSampler 完全
// 一致。合法区间 [-16, 15.99]，越界由 native 夹取。
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

// 把每个适配器的 JSON 数组写进 `buf`，缓冲区约定同 mcdr_dx12GetAdapterInfo。
MCDR_API int32_t MCDR_CALL mcdr_dx12EnumerateAdapters(char* buf, int32_t cap);

// 把当前 ACTIVE 设备绑定的那个适配器写成单行 JSON 对象，键恒齐全：
//   name / vendor / vendor_id / device_id / subsys_id / revision /
//   feature_level / feature_level_value / driver_version / driver_description /
//   device_type / vram_bytes / shared_bytes / dedicated_system_bytes / luid /
//   is_hardware / is_detachable / dxcore
// `vendor` 走原版 VulkanPhysicalDevice.java 的同一张厂商表；`driver_version` / `driver_description` /
// `is_hardware` / `is_detachable` 来自运行时加载的 DXCore（本 DLL 不链接 dxcore.dll），DXCore 不可用时
// 回落成 ""/false 且 `dxcore` 为 false。
// size-query：buf == nullptr 或 cap <= 0 返回所需字节数（**含**结尾 NUL）；否则写入 NUL 结尾 UTF-8 并
// 返回写入字节数（**不含** NUL）。没有 ACTIVE 设备（尚未创建或已销毁）返回 0。
MCDR_API int32_t MCDR_CALL mcdr_dx12GetAdapterInfo(char* buf, int32_t cap);

MCDR_API uint64_t MCDR_CALL mcdr_dx12GetQueueHandle(void);
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetDeviceHandle(void);

MCDR_API uint64_t MCDR_CALL mcdr_dx12CreateHiddenWindow(int32_t width, int32_t height);
MCDR_API void MCDR_CALL mcdr_dx12DestroyHiddenWindow(uint64_t hwnd);

MCDR_API int64_t MCDR_CALL MCDR_D3D12_get_timestamp_frequency(void);

MCDR_API mcdr_command_ctx* MCDR_CALL MCDR_D3D12_create_command_encoder(void);
MCDR_API void MCDR_CALL mcdr_dx12DestroyCommandEncoder(mcdr_command_ctx* ctx);
typedef enum mcdr_command_destroy_result_v1 {
    MCDR_COMMAND_DESTROY_INVALID = -1,
    MCDR_COMMAND_DESTROY_DESTROYED = 1,
    MCDR_COMMAND_DESTROY_OWNERSHIP_PENDING = 2,
    MCDR_COMMAND_DESTROY_SUBMISSION_UNKNOWN = 3
} mcdr_command_destroy_result_v1;
MCDR_API int32_t MCDR_CALL mcdr_dx12DestroyCommandEncoderCheckedV1(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12BeginCommandList(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12BeginCommandListWithWait(mcdr_command_ctx* ctx, uint64_t waitForValue);
MCDR_API void MCDR_CALL mcdr_dx12EndCommandList(mcdr_command_ctx* ctx);
// Fence 语义（per-command-ctx）：mcdr_dx12GetFenceValue 返回该 ctx 的 per-command-list 序号，
// mcdr_dx12WaitForFence 只等该 ctx 自己的提交，mcdr_dx12Submit 返回的也是同一条时间线上的序号 ——
// 帧 encoder 的 fence token 不会被一次性 encoder（createBuffer(data) 的临时上传）提前满足。
MCDR_API uint64_t MCDR_CALL mcdr_dx12Submit(mcdr_command_ctx* ctx);
typedef enum mcdr_command_outcome_v1 {
    MCDR_COMMAND_OUTCOME_INVALID = -1,
    MCDR_COMMAND_OUTCOME_READY = 0,
    MCDR_COMMAND_OUTCOME_SUBMITTED = 1,
    MCDR_COMMAND_OUTCOME_RECORDING_REJECTED = 2,
    MCDR_COMMAND_OUTCOME_OWNERSHIP_PENDING = 3,
    MCDR_COMMAND_OUTCOME_SUBMISSION_UNKNOWN = 4
} mcdr_command_outcome_v1;
MCDR_API int32_t MCDR_CALL mcdr_dx12CommandRecordingStatusV1(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12SubmitV1(mcdr_command_ctx* ctx, uint64_t* submittedFence);
MCDR_API int32_t MCDR_CALL mcdr_dx12SettleCommandSubmissionV1(mcdr_command_ctx* ctx,
    uint64_t timeoutMs, uint64_t* submittedFence);
MCDR_API int32_t MCDR_CALL mcdr_dx12WaitForFence(mcdr_command_ctx* ctx, uint64_t value, int64_t timeoutNs);
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetFenceValue(mcdr_command_ctx* ctx);
MCDR_API int64_t MCDR_CALL mcdr_dx12GetTimestampNow(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12IsListOpen(mcdr_command_ctx* ctx);

MCDR_API void MCDR_CALL mcdr_dx12CopyBuffer(mcdr_command_ctx* ctx, mcdr_object* src, int64_t srcOffset,
                                            mcdr_object* dst, int64_t dstOffset, int64_t size);
MCDR_API int32_t MCDR_CALL mcdr_dx12CopyBufferChecked(mcdr_command_ctx* ctx, mcdr_object* src,
    int64_t srcOffset, mcdr_object* dst, int64_t dstOffset, int64_t size);

MCDR_API void MCDR_CALL mcdr_dx12WriteToTexture(mcdr_command_ctx* ctx, mcdr_object* stagingBuffer,
                                                int64_t stagingOffset, int32_t width, int32_t height,
                                                mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                int32_t dstX, int32_t dstY);
MCDR_API int32_t MCDR_CALL mcdr_dx12WriteToTextureChecked(mcdr_command_ctx* ctx, mcdr_object* stagingBuffer,
    int64_t stagingOffset, int32_t width, int32_t height, mcdr_object* dstTexture,
    int32_t mip, int32_t layer, int32_t dstX, int32_t dstY);

MCDR_API void MCDR_CALL mcdr_dx12CopyBufferToTexture(mcdr_command_ctx* ctx, mcdr_object* srcBuffer,
                                                     int64_t srcOffset, int32_t srcWidth, int32_t srcHeight,
                                                     mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                     int32_t dstX, int32_t dstY, int32_t width,
                                                     int32_t height);
MCDR_API int32_t MCDR_CALL mcdr_dx12CopyBufferToTextureChecked(mcdr_command_ctx* ctx, mcdr_object* srcBuffer,
    int64_t srcOffset, int32_t srcWidth, int32_t srcHeight, mcdr_object* dstTexture,
    int32_t mip, int32_t layer, int32_t dstX, int32_t dstY, int32_t width, int32_t height);

MCDR_API void MCDR_CALL mcdr_dx12CopyTextureToBuffer(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                                     int32_t mip, int32_t layer, int32_t srcX, int32_t srcY,
                                                     int32_t width, int32_t height, mcdr_object* dstBuffer,
                                                     int64_t dstOffset);

MCDR_API void MCDR_CALL mcdr_dx12CopyTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                                      mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                      int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY,
                                                      int32_t width, int32_t height);
MCDR_API int32_t MCDR_CALL mcdr_dx12CopyTextureToTextureChecked(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
    mcdr_object* dstTexture, int32_t mip, int32_t layer, int32_t srcX, int32_t srcY,
    int32_t dstX, int32_t dstY, int32_t width, int32_t height);

MCDR_API void MCDR_CALL mcdr_dx12ClearColorTexture(mcdr_command_ctx* ctx, mcdr_object* texture, float r,
                                                   float g, float b, float a);
MCDR_API void MCDR_CALL mcdr_dx12ClearDepthTexture(mcdr_command_ctx* ctx, mcdr_object* texture, double depth);
MCDR_API void MCDR_CALL mcdr_dx12ClearColorTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture, float r,
                                                         float g, float b, float a, int32_t x, int32_t y,
                                                         int32_t width, int32_t height);
MCDR_API void MCDR_CALL mcdr_dx12ClearDepthTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture,
                                                         double depth, int32_t x, int32_t y, int32_t width,
                                                         int32_t height);

// 只清 stencil 面到 `value`，depth 面不动。
MCDR_API int32_t MCDR_CALL mcdr_dx12ClearStencilTexture(mcdr_command_ctx* ctx, mcdr_object* texture,
                                                        int32_t value);

// 在打开的列表上就地给 `texture` 做 SMAA：三个全屏 pass（边缘检测、混合权重、邻域混合）后把结果拷回，
// 纹理由此停在 RENDER_TARGET 以便继续当 attachment。必须在世界渲染结束、GUI 绘制之前调用 —— 只对世界层
// 抗锯齿，界面文字与面板保持锐利（放最终 blit 上会把 GUI 一起糊掉）。非 SMAA 模式是 no-op；失败会禁用
// SMAA 并记 ERROR。返回 1 = 本帧三个 pass 真的跑了，0 = 跳过（模式不是 SMAA / 纹理不是单采样 2D /
// 之前的失败已禁用 SMAA）。
MCDR_API int32_t MCDR_CALL mcdr_dx12ApplySmaaToTexture(mcdr_command_ctx* ctx, mcdr_object* texture);

// 把 `src` 画到 `dst`（全屏三角形 + 线性采样器，两者尺寸可以不同），`dst` 停在 RENDER_TARGET。GUI 的
// 窗口尺寸目标用它接管世界层——renderScale 不是 100% 时世界层在缩放后的主目标里，1:1 的
// copyTextureToTexture 做不到（缩放系数不是 1）。
MCDR_API int32_t MCDR_CALL mcdr_dx12BlitTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                         mcdr_object* dst);
// 后置锐化（TAA / DLAA / DLSS 共用）：src -> dst 的 5-tap unsharp，amount 取 [0,1]；
// src == dst 会被拒绝（返回 0），调用方需准备第二张同尺寸纹理。
MCDR_API int32_t MCDR_CALL mcdr_dx12SharpenTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                        mcdr_object* dst, float amount);

// `colorTextures` 里可以有 NULL（未使用的 attachment），会被跳过；`colorMips` 是每个 attachment 的 RTV
// mip slice；`colorClearFlags` 0 = load、1 = clear；`clearColors` 每个 attachment 占 4 个 float。
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

MCDR_API void MCDR_CALL mcdr_dx12InitAsyncRenderer(int32_t workerCount);
MCDR_API int32_t MCDR_CALL mcdr_dx12DestroyAsyncRenderer(void);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderBeginFrame(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitComplete(mcdr_command_ctx* ctx, int64_t timeoutMs);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderIsRecordingReady(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitRecordingReady(mcdr_command_ctx* ctx, int64_t timeoutMs);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncSendCommandsReady(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12ClearAsyncRenderCtx(void);

MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateGraphicsPipeline(const mcdr_pipeline_desc* desc);
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateGraphicsPipelineWithConstants(const mcdr_pipeline_desc* desc, int32_t byteSize);
MCDR_API int32_t MCDR_CALL mcdr_dx12PushGraphicsConstants(mcdr_command_ctx* context, const void* data, int32_t byteSize);
MCDR_API int32_t MCDR_CALL mcdr_dx12EnsureGraphicsState(mcdr_command_ctx* context);
MCDR_API void MCDR_CALL mcdr_dx12DestroyPipeline(mcdr_pipeline* pipeline);

// 本模块没有 D3D12_COMMAND_LIST_TYPE_COMPUTE：compute 走普通的 DIRECT 列表（SetPipelineState(compute
// PSO) + SetComputeRootSignature + Dispatch），这也是"同一条列表里既录制图形又录制 compute"的前提。
// 根签名固定：[0] descriptor table（1×UAV, u0）= 目标纹理，创建时必须带 usage 位 32
// （ALLOW_UNORDERED_ACCESS）。**必须是描述符表**：root descriptor 的 UAV 只支持 raw / structured buffer，
// typed UAV（RWTexture2D / R8_UINT 的 SRI）挂 root UAV 会让 CreateComputePipelineState 返回
// E_INVALIDARG。描述符堆由模块自己懒建（64 槽 ring），调用方不用也拿不到堆。
// [1] root constants（b0）至多 8 个 int32。调用方只需要「DXIL 字节码」和「一张带 UAV 位的纹理」。

// 从 DXIL 建 compute PSO。DXIL 由 mcdr_dx12CompileShaderDxc(..., "cs_6_x", ...) 产出。
// 失败返回 NULL，原因见 mcdrLastError()。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipeline(const void* dxil, int32_t dxilSize);

// Terrain tile binning compute PSO. Root parameters are fixed to space3: b0 draw constants,
// b1 batch constants, b2 binning constants, t8 envelopes, u0..u3 masks/prefixes/commands/control.
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateTerrainTileBinningPipeline(const void* dxil,
                                                                          int32_t dxilSize);

// 在已打开的命令列表上派发一次。内部把目标纹理 COMMON → UNORDERED_ACCESS、Dispatch、再转回 COMMON
// （跨列表的后续拷贝因此仍按 COMMON 处理）。
// 返回 1=已派发 0=参数或资源状态不对（会写一条 ERROR 说明是哪一条） -2=没有打开的命令列表。
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchCompute(mcdr_command_ctx* ctx, mcdr_object* pipeline,
                                                     mcdr_object* uavTexture, const int32_t* constants,
                                                     int32_t constantCount, int32_t x, int32_t y, int32_t z);

// Mesh shaders (amplification/mesh pipeline + HLSL->DXIL compilation)。mesh 走不到 D3DCompile
// （SM 5.1 上限），native 侧用 DXC 编译，dxcompiler.dll 从库旁或已安装的 Windows SDK 动态加载。

// Compiles one HLSL entry point to DXIL. `profile` is e.g. "as_6_6" / "ms_6_6" / "ps_6_0"; an empty
// `entry` means "main". Returns the bytecode size in bytes, or 0 on failure (reason via
// mcdrLastError()). size-query: pass (NULL, 0) -- or a buffer smaller than the result -- to learn the
// required size first, then call again with a buffer of that size.
MCDR_API int32_t MCDR_CALL mcdr_dx12CompileShaderDxc(const char* source, int32_t sourceLen,
                                                     const char* entry, const char* profile, void* outDxil,
                                                     int32_t outCapacity);

// 能力与原因的统一查询（与界面置灰的判据同源）。返回 1=支持 0=不支持；
// outReason 写入 UTF-8 原因（可为 NULL / capacity=0，只查能力）。
MCDR_API int32_t MCDR_CALL mcdr_dx12QueryMeshSupport(char* outReason, int32_t capacity);

// Creates an amplification/mesh pipeline from DXIL bytecode. Returns NULL and sets the last error
// when the device does not support mesh shaders (tier 0).
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateMeshPipeline(const mcdr_mesh_pipeline_desc* desc);
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateMeshPipelineWithConstants(const mcdr_mesh_pipeline_desc* desc, int32_t byteSize);
MCDR_API uint64_t MCDR_CALL mcdr_dx12CreateMeshRootSignatureWithConstants(const mcdr_pipeline_binding_desc* bindings, int32_t count, int32_t meshExtras, int32_t byteSize);
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateMeshPipelineWithRootConstants(const mcdr_mesh_pipeline_desc* desc, uint64_t rootSignature, int32_t byteSize);

// 与 mcdr_dx12CreateMeshPipeline 相同，但复用调用方提供的根签名（ID3D12RootSignature* 的地址值，
// 0 = 退回内部按 bindings 新建）。地形 mesh 变体要复用现有地形的绑定点布局（b0..b3 / t4 / t5），再由
// JVM 补 b8 小常量与 t14/t15/t16 的 mesh 尾参；两边共用同一套 draw 绑定才能一起用。
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateMeshPipelineWithRootSignature(
    const mcdr_mesh_pipeline_desc* desc, uint64_t rootSignature);

// 按绑定点布局建根签名（返回 ID3D12RootSignature* 的地址值，失败返回 0）。
// meshExtras != 0 时追加 mesh 专用尾参：root CBV b8（小常量/段表基址）+ 第二个描述符表
// （t14 顶点 / t15 段表 / t16 meshlet 表，固定 3 个 range），且**不能**带
// ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT（非 meshExtras 时必须带）。
// 调用方必须用 mcdr_dx12ReleaseRootSignature 释放。
MCDR_API uint64_t MCDR_CALL mcdr_dx12CreateMeshRootSignature(
    const mcdr_pipeline_binding_desc* bindings, int32_t count, int32_t meshExtras);
MCDR_API void MCDR_CALL mcdr_dx12ReleaseRootSignature(uint64_t rootSignature);

// DispatchMesh with thread-group counts. Requires a mesh pipeline bound through mcdr_dx12SetPipeline,
// an open render pass, and bindings pushed with mcdr_dx12PushDescriptors. Returns 1 on success.
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchMesh(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t z);

// meshlet 版地形派发（索引由 MS 算术生成）。绑定布局与
// mcdr_dx12CreateMeshRootSignature(meshExtras=1) 一致：root CBV b8 = 小常量（uint meshletCount + 3 个
// pad，256 B 对齐）；描述符表 = t14 顶点池（28 B/顶点）、t15 段表（32 B/段）、t16 meshlet 表（32 B/条）。
// 三个缓冲都是 RAW SRV（ByteAddressBuffer 用 Load 读取），结构体步长由 HLSL 自己按 32 B 解析，
// 不需要 StructureByteStride。一次 DispatchMesh(meshletCount, 1, 1) 覆盖整批；段号写在 meshlet 记录里。
MCDR_API int32_t MCDR_CALL mcdr_dx12MeshDrawMeshlets(
    mcdr_command_ctx* ctx, mcdr_object* vertexBuffer, int64_t vertexOffset, mcdr_object* sections,
    int64_t sectionsOffset, int64_t sectionsLength, mcdr_object* meshlets, int64_t meshletsOffset,
    int64_t meshletsLength, mcdr_object* globals, int64_t globalsOffset, int32_t meshletCount);

MCDR_API mcdr_surface* MCDR_CALL mcdr_dx12CreateSurface(uint64_t hwnd);

// Writes the supported present-mode ordinals into `out` (up to `cap`) and returns the total count,
// so a caller can size the buffer first.
MCDR_API int32_t MCDR_CALL mcdr_dx12SurfacePresentModes(int32_t* out, int32_t cap);

MCDR_API int32_t MCDR_CALL mcdr_dx12ConfigureSurface(mcdr_surface* surface, int32_t width, int32_t height,
                                                     int32_t presentMode);
// 真独占全屏（1 = 进独占，0 = 退回窗口/无边框）。失败返回 0，画面不受影响。
MCDR_API int32_t MCDR_CALL mcdr_dx12SetExclusiveFullscreen(mcdr_surface* surface, int32_t on);

MCDR_API int32_t MCDR_CALL mcdr_dx12AcquireSurface(mcdr_surface* surface);
MCDR_API void MCDR_CALL mcdr_dx12BlitSurface(mcdr_command_ctx* ctx, mcdr_surface* surface,
                                             mcdr_object* texture);
MCDR_API void MCDR_CALL mcdr_dx12PresentSurface(mcdr_surface* surface);
// Returns the raw ID3D12Resource* of the currently acquired back buffer as an
// opaque integer (NOT a mcdr_object* -- the Java side only forwards it).
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetBackBufferHandle(mcdr_surface* surface);
MCDR_API int32_t MCDR_CALL mcdr_dx12IsSurfaceSuboptimal(mcdr_surface* surface);

// 原生 HDR：0 = auto（按窗口所在输出的 DXGI 色彩空间判定），1 = 强制开，2 = 强制关。
// 只改进程状态，真正生效在下次创建/重建交换链时（格式是建链参数，不能原地改）。
MCDR_API void MCDR_CALL mcdr_dx12SetHdrMode(int32_t mode);
MCDR_API int32_t MCDR_CALL mcdr_dx12GetHdrAppliedMode(void);

// 光追可见性分层总开关（0=off, 1=ao, 2=ao_shadows, 3=full）；不支持的设备上记下但不生效。
MCDR_API void MCDR_CALL mcdr_dx12SetRaytracingMode(int32_t mode);

// 交换链建链选项，下次建链时生效：
//   allowTearing       0 = 不建 ALLOW_TEARING 的链（强制 DWM 合成翻页），非 0 = 允许（默认）。
//                      某些覆盖层（RTSS/Afterburner 的 OSD）只在合成翻页下画得出来。
//   forcePresentChild  非 0 = 优先把链建在自建呈现子窗口上，0 = 默认（顶层窗口）。
MCDR_API void MCDR_CALL mcdr_dx12SetSwapchainOptions(int32_t allowTearing, int32_t forcePresentChild);

MCDR_API void MCDR_CALL mcdr_dx12DestroySurface(mcdr_surface* surface);
MCDR_API int32_t MCDR_CALL mcdr_dx12DestroySurfaceChecked(mcdr_surface* surface);
MCDR_API int32_t MCDR_CALL mcdr_dx12DestroyDeviceChecked(void);
MCDR_API int32_t MCDR_CALL mcdr_dx12InvalidateReconstructionInputs(mcdr_command_ctx* ctx);
MCDR_API mcdr_surface* MCDR_CALL mcdr_dx12GetActiveSurfaceHandle(void);

// Both readback entry points write 36 int32s (9 pixels x RGBA) into `out` and
// return 1 on success, 0 on failure.
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadbackSurfacePixels(mcdr_surface* surface, int32_t* out);
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadbackTexturePixels(mcdr_object* texture, int32_t* out);

/* 已知起始状态的读回。`fromState` 是这张纹理**当前真实所处**的 D3D12_RESOURCE_STATES 数值，不是"我希望
 * 它在哪"：传错 → barrier 的 StateBefore 不匹配 → 读回内容未定义（恒 0）。上面那个不带状态的旧导出按
 * COMMON 处理，只对"建完没被 GPU 写过"的纹理成立；DLSS 的输出纹理在帧末停在 COPY_SOURCE（FG 关）或
 * UNORDERED_ACCESS（FG 开），必须走这一条。读回结束后纹理被恢复成 fromState。 */
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadbackTexturePixelsIn(mcdr_object* texture, int32_t fromState,
                                                            int32_t* out);
MCDR_API int32_t MCDR_CALL mcdr_dx12CaptureSampledTextureContentV1(
    mcdr_command_ctx* context, mcdr_object* view, uint64_t totalBudgetBytes,
    uint64_t* ownerIdentity, uint64_t* ownerGeneration, uint64_t* contentVersion,
    uint32_t* firstMip, uint32_t* mipCount, uint32_t* firstLayer, uint32_t* layerCount);

/* DLSS 输出纹理上一帧结束时停在哪个状态（D3D12_RESOURCE_STATES 数值）。
 * 0 (COMMON) = 这一帧没有输出纹理/未知 —— 调用方不要据此采样。 */
MCDR_API int32_t MCDR_CALL mcdr_dx12DlssOutputState(void);

MCDR_API void MCDR_CALL mcdr_dx12DumpTextureToFile(mcdr_object* texture, const char* tag);

MCDR_API int32_t MCDR_CALL mcdr_dx12SetRasterMotionTarget(
    mcdr_command_ctx* ctx, mcdr_object* worldColor, mcdr_object* worldDepth, mcdr_object* motion);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetAdditionalColorTarget(mcdr_command_ctx* ctx, mcdr_object* target);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetPipeline(mcdr_command_ctx* ctx, mcdr_pipeline* pipeline,
                                                int32_t hasDepth);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetScissor(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                               int32_t height);
// 显式设视口；不调用时视口由 mcdr_dx12BeginRenderPass 按 pass 区域设好。
MCDR_API int32_t MCDR_CALL mcdr_dx12SetViewport(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                                int32_t height);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetVertexBuffer(mcdr_command_ctx* ctx, int32_t slot, mcdr_object* buffer,
                                                    int64_t offset, int32_t stride);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetVertexBufferRangeV1(mcdr_command_ctx* ctx, int32_t slot,
    mcdr_object* buffer, int64_t offset, int64_t length, int32_t stride);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetIndexBuffer(mcdr_command_ctx* ctx, mcdr_object* buffer,
                                                   int32_t indexType);

// `bindings` holds `count` entries, one per pipeline binding in declaration order.
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

// 热路径：一次调用录完一个 worker 分区。
// `bindings` —— `sharedBindings` 非 0 时只含 draw 0 的那一组（`bindingCount` 条），被每个 draw 复用；
// 否则是 `drawCount * bindingCount` 条，按 bindings[draw * bindingCount + slot] 索引。
// `draws` —— 该分区的 `drawCount` 条，已前进到分区第一个 draw。
// 语义与逐调用序列完全一致：任何参数非法返回 0，Java 侧丢弃 bundle 并改为串行重录。
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleRecordPartition(mcdr_desc_alloc* alloc, mcdr_bundle_pool* pool,
                                                               int32_t worker, int32_t baseSlot,
                                                               int32_t bindingCount, int32_t sharedBindings,
                                                               const mcdr_binding_slot* bindings,
                                                               const mcdr_partition_draw* draws,
                                                               int32_t drawCount);

MCDR_API int32_t MCDR_CALL mcdr_dx12ExecuteBundle(mcdr_command_ctx* ctx, uint64_t bundle);

// Barrier pre-passes: a bundle may not contain barriers, so every CBV buffer, index buffer and texture
// view is transitioned on the main list first.
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareCBVBuffers(mcdr_command_ctx* ctx, mcdr_object* const* buffers,
                                                           int32_t count);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareIndexBuffers(mcdr_command_ctx* ctx,
                                                             mcdr_object* const* buffers, int32_t count);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareTextureViews(mcdr_command_ctx* ctx, mcdr_object* const* views,
                                                             int32_t count);

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

// The only place the native layer calls back into the JVM. `cb` is a single FFM upcall stub created
// once for the whole library; `userHandle` identifies the Java-side registration, so no JVM object
// reference is ever retained here. The callback is invoked OUTSIDE the registry lock, so it may
// re-register or unregister. `timeoutMs == 0` means wait forever.

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

// 着色率图像（tier 2 SRI）：compute 读上一帧深度（t0）与上一帧速度（t1），写一张 R8_UINT 的 tile 表（u0），
// 每个 texel 管一整块 tile，值是 D3D12_SHADING_RATE_* 的枚举（0=1X1 … 0xA=4X4）。图像尺寸是 **tile 数**
// （宽高各自 ceil(pixel / tile)），tile 边长看设备能力（NVIDIA 16、AMD 8）。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineShadingRate(const void* dxil,
                                                                         int32_t dxilSize);
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateShadingRateImage(int32_t width, int32_t height);

// 在已打开的命令列表上生成一次。srvDepth/srvMotion/srvPrevious 是 TextureView 句柄
// （mcdr_dx12CreateTextureView 的产物）：t0 深度、t1 速度、t2 上一帧的着色率图像
// （时间钳制用，读不到历史时可传同一张或任意有效视图，并在 constants 里关掉钳制）。
// image 是要写的那张图像。
// 返回 1=已派发 0=参数或资源不对 -2=没有打开的命令列表。
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchComputeShadingRate(
    mcdr_command_ctx* ctx, mcdr_object* pipeline, mcdr_object* srvDepth, mcdr_object* srvMotion,
    mcdr_object* srvPrevious, mcdr_object* image, const int32_t* constants, int32_t constantCount,
    int32_t x, int32_t y, int32_t z);

// 通用 compute 纹理 pass（3×SRV t0/t1/t2 + 1×UAV u0 + b0 的 8 个 int）。根签名与着色率图像那条
// **完全相同**，只是用途泛化成"读几张纹理、写一张纹理"；给一对不带 "ShadingRate" 的名字，免得读掩码的
// 代码写成 dispatchComputeShadingRate(..., mask, ...) 对不上号。
// image 必须带 usage 位 32（ALLOW_UNORDERED_ACCESS），否则返回 0。srv0/srv1/srv2 必须是带 CPU 描述符句柄的
// TextureView，用不到的槽传同一张有效视图（着色器不读即可）—— 三张都必须有，这是固定根签名。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineTexture3(const void* dxil,
                                                                     int32_t dxilSize);
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchComputeTexture3(
    mcdr_command_ctx* ctx, mcdr_object* pipeline, mcdr_object* srv0, mcdr_object* srv1,
    mcdr_object* srv2, mcdr_object* image, const int32_t* constants, int32_t constantCount,
    int32_t x, int32_t y, int32_t z);

// 把图像挂到列表的 SRI 槽位（image 传 NULL 摘掉）。内部把图像从 COMMON 转到
// SHADING_RATE_SOURCE；返回 1=已绑定 0=列表不是 ID3D12GraphicsCommandList5 -1=不是纹理
// -2=没有打开的命令列表。
MCDR_API int32_t MCDR_CALL mcdr_dx12SetShadingRateImage(mcdr_command_ctx* ctx, mcdr_object* image);

MCDR_API int32_t MCDR_CALL mcdr_dx12MultiDrawIndexedPacked(mcdr_command_ctx* ctx,
    const int32_t* drawParameters, int32_t instanceCount, int32_t firstInstance, int32_t drawCount);
MCDR_API int32_t MCDR_CALL mcdr_dx12MultiDrawIndexedSeparate(mcdr_command_ctx* ctx,
    const int32_t* firstIndices, const int32_t* indexCounts, const int32_t* baseVertices,
    int32_t instanceCount, int32_t firstInstance, int32_t drawCount);
MCDR_API int32_t MCDR_CALL mcdr_dx12MultiDrawPacked(mcdr_command_ctx* ctx,
    const int32_t* drawParameters, int32_t instanceCount, int32_t firstInstance, int32_t drawCount);
MCDR_API int32_t MCDR_CALL mcdr_dx12MultiDrawSeparate(mcdr_command_ctx* ctx,
    const int32_t* firstVertices, const int32_t* vertexCounts, int32_t drawCount);
MCDR_API int32_t MCDR_CALL mcdr_dx12MultiDrawBound(mcdr_command_ctx* ctx, int32_t indexed,
    const int32_t* drawParameters, int32_t instanceCount, int32_t firstInstance, int32_t drawCount,
    const mcdr_binding_slot* sharedBindings, int32_t bindingCount, const int32_t* overrideIndices,
    int32_t overrideCount, const mcdr_binding_slot* drawBindings);

MCDR_API void* MCDR_CALL mcdr_dx12CreateWorkGraph(
    uint32_t schemaVersion, const void* dxil, int32_t dxilSize, const char* programName,
    const char* entryName, uint32_t recordSize, const uint32_t* bindingTriples,
    uint32_t bindingCount, uint64_t backingBudget);
MCDR_API void MCDR_CALL mcdr_dx12DestroyWorkGraph(void* program);
#define MCDR_VANILLA_FRAME_SCHEMA_VERSION 2u
typedef struct mcdr_frame_identity {
    uint64_t worldEpoch;
    uint64_t realFrameId;
    uint64_t configVersion;
    uint64_t resourceVersion;
} mcdr_frame_identity;

typedef struct mcdr_prepared_binding {
    mcdr_object* object;
    uint32_t firstSubresource;
    uint32_t subresourceCount;
} mcdr_prepared_binding;

#include "mcdr_terrain_tile_adapter.h"

#define MCDR_PRESENT_RECEIPT_SCHEMA_VERSION 1u
enum {
    MCDR_DISPLAY_SETTLE_EMPTY = 0,
    MCDR_DISPLAY_SETTLE_COMPLETE = 1,
    MCDR_DISPLAY_SETTLE_TIMEOUT = -1,
    MCDR_DISPLAY_SETTLE_SUBMISSION_UNKNOWN = -2,
    MCDR_DISPLAY_SETTLE_INVALID_REQUEST = -3
};
enum {
    MCDR_SUBMISSION_NONE = 0,
    MCDR_SUBMISSION_QUEUED = 1,
    MCDR_SUBMISSION_SUBMITTED = 2,
    MCDR_SUBMISSION_FAILED = 3,
    MCDR_SUBMISSION_UNKNOWN = 4
};
enum {
    MCDR_PRESENT_PENDING = 0,
    MCDR_PRESENT_SUCCEEDED = 1,
    MCDR_PRESENT_OCCLUDED = 2,
    MCDR_PRESENT_MODE_CHANGED = 3,
    MCDR_PRESENT_FAILED = 4,
    MCDR_PRESENT_NOT_CALLED = 5,
    MCDR_PRESENT_CANCELLED = 6,
    MCDR_PRESENT_UNKNOWN = 7
};
enum {
    MCDR_PRESENT_RETIREMENT_PENDING = 0,
    MCDR_PRESENT_RETIREMENT_SIGNALLED = 1,
    MCDR_PRESENT_RETIREMENT_NOT_REQUIRED = 2,
    MCDR_PRESENT_RETIREMENT_UNKNOWN = 3
};
enum {
    MCDR_PRESENT_OIT_HISTORY_PUBLISHED = 1u,
    MCDR_PRESENT_FG_CAMERA_PUBLISHED = 2u,
    MCDR_PRESENT_DXGI_STATUS_VALID = 4u
};
typedef struct mcdr_present_receipt_v1 {
    uint32_t schemaVersion, structSize;
    uint64_t serial;
    mcdr_frame_identity identity;
    uint64_t providerGeneration, historyGeneration;
    uint64_t commandFenceValue, submitQueueFenceValue, presentQueueFenceValue;
    uint32_t submissionState, presentOutcome;
    int32_t dxgiStatus;
    uint32_t flags, outputFrameCount;
    uint32_t retirementState;
} mcdr_present_receipt_v1;
MCDR_API int32_t MCDR_CALL mcdr_dx12SettleDisplayFrameV1(
    mcdr_command_ctx* context, uint64_t timeoutMs, mcdr_present_receipt_v1* receipt);
typedef struct mcdr_raster_camera {
    double origin[3];
    double jitterX;
    double jitterY;
    float projection[16];
    float rasterProjection[16];
    float viewRotation[16];
    float nearPlane;
    float farPlane;
    float verticalFovRadians;
    uint32_t flags;
    float yaw;
    float pitch;
} mcdr_raster_camera;
typedef struct mcdr_frame_settings {
    uint32_t antiAliasing;
    uint32_t reconstructionBackend;
    uint32_t renderScalePercent;
    uint32_t jitterEnabled;
    uint32_t jitterSamples;
    float providerSharpness;
    uint32_t providerMasks;
    uint32_t dlssQuality;
    uint32_t dlssPreset;
    uint32_t frameGenerationBackend;
    uint32_t frameGenerationMode;
    uint32_t frameGenerationFrames;
    float taaBlend;
    float taaSharpness;
} mcdr_frame_settings;
typedef struct mcdr_vanilla_frame_input {
    uint32_t schemaVersion;
    uint32_t structSize;
    mcdr_frame_identity identity;
    uint64_t historyGeneration;
    uint64_t frameTimeNanos;
    uint64_t jitterPhaseIndex;
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t flags;
    uint32_t colorEncoding;
    uint32_t depthConvention;
    uint32_t motionConvention;
    float frameDeltaMs;
    float preExposure;
    float motionScaleX;
    float motionScaleY;
    float viewSpaceToMeters;
    uint32_t reserved;
    uint64_t previousRealFrameId;
    mcdr_frame_settings settings;
    mcdr_raster_camera current;
    mcdr_raster_camera previous;
} mcdr_vanilla_frame_input;
#define MCDR_OIT_SCHEMA_VERSION 1u
#define MCDR_OIT_CAPTURE_CERTIFIED_RGB_V1 1u
#define MCDR_OIT_CURRENT_MATERIAL_VALID 1u
#define MCDR_OIT_GEOMETRY_PREVIOUS_VALID 2u
#define MCDR_OIT_MATERIAL_PREVIOUS_VALID 4u
#define MCDR_OIT_COVERAGE_VALID 8u
#define MCDR_OIT_STATUS_OVERFLOW 1u
#define MCDR_OIT_STATUS_UNKNOWN_RANGE 2u
#define MCDR_OIT_STATUS_INVALID_MATERIAL 4u
#define MCDR_OIT_STATUS_CORRUPT_CHAIN 8u
#define MCDR_OIT_STATUS_INVALID_CURRENT 16u
#define MCDR_OIT_STATUS_EMPTY 32u
#define MCDR_OIT_STATUS_NO_PREVIOUS 64u
#define MCDR_OIT_STATUS_HISTORY_REJECTED 128u
#define MCDR_OIT_STATUS_HISTORY_USED 256u
#define MCDR_OIT_MAX_RECORDS_PER_PIXEL 32u
#define MCDR_OIT_RESULT_METADATA_CAPACITY_EXCEEDED -7
#define MCDR_OIT_TOTAL_BUDGET_LIMIT 536870912ull
#define MCDR_OIT_SOURCE_TERRAIN 0u
#define MCDR_OIT_SOURCE_FEATURE 1u
#define MCDR_OIT_SOURCE_BORDER 2u
#define MCDR_OIT_SOURCE_WEATHER 3u
#define MCDR_OIT_SOURCE_CLOUD 4u
#define MCDR_OIT_SOURCE_CUSTOM_WEATHER 5u
#define MCDR_OIT_SOURCE_CUSTOM_CLOUD 6u
#define MCDR_OIT_SOURCE_CUSTOM_TRANSLUCENT 7u
#define MCDR_OIT_RESULT_SUCCESS 1
#define MCDR_OIT_RESULT_INVALID_REQUEST (-1)
#define MCDR_OIT_RESULT_UNSUPPORTED (-2)
#define MCDR_OIT_RESULT_BUDGET_EXCEEDED (-3)
#define MCDR_OIT_RESULT_BUSY (-4)
#define MCDR_OIT_RESULT_STALE (-5)
#define MCDR_OIT_RESULT_SUBMISSION_UNKNOWN (-6)
typedef struct mcdr_oit_capture_pipeline_v1 {
    uint32_t schemaVersion, structSize, shaderAbiVersion, flags;
    uint64_t shaderContractHash, programGeneration;
} mcdr_oit_capture_pipeline_v1;
typedef struct mcdr_oit_begin_v1 {
    uint32_t schemaVersion, structSize;
    mcdr_frame_identity identity;
    uint64_t historyGeneration, previousRealFrameId, programGeneration;
    uint64_t totalBudgetBytes;
    uint32_t maxRecords, maxRecordsPerPixel;
    uint32_t width, height, colorEncoding, flags;
    float preExposure;
    uint32_t reserved;
    uint32_t maxSourceRows, maxPrimitiveRows, maxPreviousRows, reservedMetadata;
    mcdr_prepared_binding opaqueColor, opaqueDepth, opaqueMotion;
} mcdr_oit_begin_v1;
typedef struct mcdr_oit_begin_v2 {
    mcdr_oit_begin_v1 base;
    uint32_t maxTerrainSectionRows, maxTerrainPrimitives;
    float previousViewProjection[16];
    double previousOrigin[3];
    uint32_t previousCameraValid, reserved;
} mcdr_oit_begin_v2;
typedef struct mcdr_oit_source_v1 {
    uint64_t producerId, producerGeneration, shaderGeneration;
    uint32_t producerKind, validity;
} mcdr_oit_source_v1;
typedef struct mcdr_oit_primitive_v1 {
    uint64_t surfaceId, geometryGeneration, materialId, materialGeneration, textureGeneration;
    uint32_t sourceIndex, previousIndex, flags, reserved;
} mcdr_oit_primitive_v1;
typedef struct mcdr_oit_previous_triangle_v1 {
    float clip[12], materialUv[6];
    uint32_t flags, reserved;
} mcdr_oit_previous_triangle_v1;
typedef struct mcdr_oit_metadata_v1 {
    uint32_t schemaVersion, structSize;
    uint32_t sourceCount, primitiveCount, previousCount, sectionCount;
    const mcdr_oit_source_v1* sources;
    const mcdr_oit_primitive_v1* primitives;
    const mcdr_oit_previous_triangle_v1* previous;
    const uint32_t* sectionPrimitiveBases;
} mcdr_oit_metadata_v1;
typedef struct mcdr_oit_draw_v1 {
    uint32_t schemaVersion, structSize;
    uint32_t sourceIndex, primitiveBase, primitiveCount, flags;
    uint64_t producerScopeSerial;
    uint32_t primitivesPerInstance, instanceCount, firstInstance, reserved;
} mcdr_oit_draw_v1;
typedef struct mcdr_oit_terrain_section_v2 {
    uint64_t surfaceId, geometryGeneration;
    uint64_t materialId, materialGeneration, textureGeneration;
    uint32_t primitiveBase, primitiveCount;
    int32_t sectionX, sectionY, sectionZ;
    uint32_t flags;
    uint32_t vertexBase, vertexCount;
} mcdr_oit_terrain_section_v2;
typedef struct mcdr_oit_terrain_metadata_v2 {
    uint32_t schemaVersion, structSize;
    mcdr_oit_source_v1 source;
    uint32_t sectionCount, primitiveCount;
    const mcdr_oit_terrain_section_v2* sections;
    mcdr_object* vertexBuffer;
    uint64_t vertexOffset, vertexBytes;
} mcdr_oit_terrain_metadata_v2;
typedef struct mcdr_oit_terrain_draw_v2 {
    uint32_t schemaVersion, structSize;
    uint64_t producerScopeSerial;
} mcdr_oit_terrain_draw_v2;
#define MCDR_OIT_RESOLVE_UNRECORDED_COVERAGE_UNAVAILABLE 1u
typedef struct mcdr_oit_resolve_v3 {
    uint32_t schemaVersion, structSize;
    mcdr_frame_identity identity;
    void* frame;
    mcdr_prepared_binding currentTransparency, unrecordedCoverage, correctedTransparency, statusOutput;
    uint32_t flags, reserved;
} mcdr_oit_resolve_v3;
// Resolve V4 adds the sealed terrain tile packets. `terrainBatches` points to a contiguous array of
// mcdr::terrain_tile::BatchV3 payloads owned by the caller for the duration of this call; native copies
// and validates every payload before recording GPU work and never retains this pointer.
typedef struct mcdr_oit_resolve_v4 {
    mcdr_oit_resolve_v3 base;
    const void* terrainBatches;
    uint32_t terrainBatchCount;
    uint32_t reservedV4;
} mcdr_oit_resolve_v4;
MCDR_API int32_t MCDR_CALL mcdr_dx12OitShaderPreludeV1(char* destination, uint32_t capacity, uint32_t* required, uint32_t* shaderAbiVersion, uint64_t* contractHash);
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateGraphicsCapturePipelineV1(const mcdr_pipeline_desc* descriptor, uint32_t pushBytes, const mcdr_oit_capture_pipeline_v1* capture);
MCDR_API uint64_t MCDR_CALL mcdr_dx12CreateMeshCaptureRootV1(const mcdr_pipeline_binding_desc* bindings, int32_t count, uint32_t meshLayout, uint32_t pushBytes, const mcdr_oit_capture_pipeline_v1* capture);
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateMeshCapturePipelineV1(const mcdr_mesh_pipeline_desc* descriptor, uint64_t root, uint32_t pushBytes, const mcdr_oit_capture_pipeline_v1* capture);
// Terrain tile route (schema 1): creates a mesh-visible root extension in register space 3 and its
// DispatchMesh indirect signature. The route is attached to the capture pipeline separately so the
// vanilla draw pipeline remains unchanged. It is only valid for the matching capture hash and
// generation; all handles are opaque and must be destroyed explicitly.
MCDR_API uint64_t MCDR_CALL mcdr_dx12CreateTerrainTileRootV1(
    const mcdr_pipeline_binding_desc* bindings, int32_t count, uint32_t pushBytes,
    const mcdr_oit_capture_pipeline_v1* capture);
MCDR_API void* MCDR_CALL mcdr_dx12CreateTerrainTileRouteV1(
    uint64_t rootSignature, const mcdr_oit_capture_pipeline_v1* capture);
MCDR_API int32_t MCDR_CALL mcdr_dx12BindTerrainTileRouteV1(
    mcdr_pipeline* capturePipeline, void* route);
MCDR_API int32_t MCDR_CALL mcdr_dx12BindTerrainTilePipelineV1(
    void* route, mcdr_pipeline* tilePipeline);
MCDR_API int32_t MCDR_CALL mcdr_dx12BindTerrainTileEnvelopeV1(
    void* route, const void* dxil, uint32_t bytes);
MCDR_API int32_t MCDR_CALL mcdr_dx12BindTerrainTileStageV2(
    mcdr_pipeline* source, void* route, const mcdr_terrain_tile_stage_v2* contract);
MCDR_API int32_t MCDR_CALL mcdr_dx12BindTerrainTileStageV3(
    mcdr_pipeline* source, void* route, const mcdr_terrain_tile_stage_v2* contract, const uint32_t laneRegisters[5]);
MCDR_API int32_t MCDR_CALL mcdr_dx12BindTerrainOpaqueSeedV1(
    mcdr_pipeline* source, void* route, uint64_t programGeneration, uint32_t quadsPerMeshlet);
MCDR_API void MCDR_CALL mcdr_dx12DestroyTerrainTileRouteV1(void* route);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitBeginFrameV1(mcdr_command_ctx* context, const mcdr_oit_begin_v1* input, void** frame);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitBeginFrameV2(mcdr_command_ctx* context, const mcdr_oit_begin_v2* input, void** frame);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitPublishMetadataV1(mcdr_command_ctx* context, void* frame, const mcdr_oit_metadata_v1* input, void** metadata);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitPublishTerrainMetadataV2(mcdr_command_ctx* context, void* frame, const mcdr_oit_terrain_metadata_v2* input, void** metadata);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitBindDrawV1(mcdr_command_ctx* context, void* frame, void* metadata, const mcdr_oit_draw_v1* input);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitBindTerrainDrawV2(mcdr_command_ctx* context, void* frame, void* metadata, const mcdr_oit_terrain_draw_v2* input);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitUnbindDrawV1(mcdr_command_ctx* context, void* frame);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitSealFrameV1(mcdr_command_ctx* context, void* frame);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitAbortFrameV1(mcdr_command_ctx* context, void* frame);
MCDR_API int32_t MCDR_CALL mcdr_dx12OitResolveRecordsV3(mcdr_command_ctx* context, const mcdr_oit_resolve_v3* input);
#define MCDR_DISPLAY_FRAME_SCHEMA_VERSION 2u
typedef struct mcdr_display_layer_input {
    uint32_t structSize;
    uint32_t kind;
    uint32_t flags;
    uint32_t composition;
    uint32_t width;
    uint32_t height;
    mcdr_prepared_binding depth;
    mcdr_prepared_binding motion;
    mcdr_prepared_binding coverage;
    mcdr_raster_camera current;
    mcdr_raster_camera previous;
} mcdr_display_layer_input;
typedef struct mcdr_display_post_input {
    uint32_t structSize;
    uint32_t kind;
    uint32_t flags;
    uint32_t reserved;
    float current[16];
    float previous[16];
} mcdr_display_post_input;
typedef struct mcdr_display_frame_input {
    uint32_t schemaVersion;
    uint32_t structSize;
    mcdr_frame_identity identity;
    uint64_t historyGeneration;
    uint64_t previousRealFrameId;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t resetReasons;
    float frameDeltaMs;
    float viewSpaceToMeters;
    uint32_t frameGenerationBackend;
    uint32_t frameGenerationMode;
    uint32_t frameGenerationFrames;
    uint32_t colorEncoding;
    mcdr_prepared_binding scene;
    mcdr_prepared_binding ui;
    mcdr_raster_camera current;
    mcdr_raster_camera previous;
    uint32_t layerCount;
    uint32_t layerStride;
    const mcdr_display_layer_input* layers;
    uint32_t postCount;
    uint32_t postStride;
    const mcdr_display_post_input* posts;
} mcdr_display_frame_input;
typedef struct mcdr_vanilla_resource_use {
    void* object;
    uint64_t version;
    uint32_t firstSubresource;
    uint32_t subresourceCount;
    uint32_t access;
    uint32_t requiredState;
    uint32_t finalState;
    uint32_t externalState;
} mcdr_vanilla_resource_use;
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaBeginFrame(mcdr_command_ctx* context, const mcdr_vanilla_frame_input* input);
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaRegisterWorldDepthV1(mcdr_command_ctx* context, mcdr_object* depth);
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaArmWorldDepthSeedV1(
    mcdr_command_ctx* context, uint32_t maxEvents, uint64_t totalBudgetBytes);
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaSealWorldDepthSeedV1(mcdr_command_ctx* context);
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaWorldReady(mcdr_command_ctx* context, const mcdr_frame_identity* input);
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaGuiReady(mcdr_command_ctx* context, const mcdr_frame_identity* input);
#define MCDR_PREPARED_FRAME_SCHEMA_VERSION 2u
typedef struct mcdr_prepared_frame {
    uint32_t schemaVersion;
    uint32_t structSize;
    mcdr_frame_identity identity;
    uint64_t historyGeneration;
    uint32_t availability;
    uint32_t required;
    uint32_t consumer;
    uint32_t motionCoverage;
    uint32_t motionGuideCoverage;
    uint32_t resetReasons;
    mcdr_prepared_binding color;
    mcdr_prepared_binding depth;
    mcdr_prepared_binding motion;
    mcdr_prepared_binding exposure;
    mcdr_prepared_binding reactive;
    mcdr_prepared_binding composition;
    mcdr_prepared_binding responsive;
} mcdr_prepared_frame;
typedef struct mcdr_reconstruction_request {
    uint32_t schemaVersion;
    uint32_t structSize;
    mcdr_frame_identity identity;
    uint32_t consumer;
    uint32_t flags;
    mcdr_prepared_binding output;
} mcdr_reconstruction_request;
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaPublishPreparedFrame(mcdr_command_ctx* context, const mcdr_prepared_frame* input);
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaEvaluateReconstruction(mcdr_command_ctx* context, const mcdr_reconstruction_request* input);
MCDR_API int32_t MCDR_CALL mcdr_dx12BeginDisplayFrame(mcdr_command_ctx* context);
MCDR_API int32_t MCDR_CALL mcdr_dx12AbortDisplayFrame(mcdr_command_ctx* context);
MCDR_API int32_t MCDR_CALL mcdr_dx12PrepareDisplayFrame(mcdr_command_ctx* context,
    const mcdr_display_frame_input* input);
MCDR_API int32_t MCDR_CALL mcdr_dx12SubmitDisplayFrame(mcdr_command_ctx* context, const mcdr_display_frame_input* input);
MCDR_API int32_t MCDR_CALL mcdr_dx12PrewarmDisplayFrame(mcdr_command_ctx* context, uint64_t generation);
enum {
    MCDR_FRAME_PROGRAMS_SUCCESS = 1,
    MCDR_FRAME_PROGRAMS_INVALID_REQUEST = 0,
    MCDR_FRAME_PROGRAMS_COMPILATION_FAILED = -1,
    MCDR_FRAME_PROGRAMS_BUSY = -2,
    MCDR_FRAME_PROGRAMS_STALE = -3,
    MCDR_FRAME_PROGRAMS_INVALID_CONTEXT = -4,
    MCDR_FRAME_PROGRAMS_SUBMISSION_UNKNOWN = -5
};
// 每个 context 只允许一个控制调用方；prepare 与 commit 之间不得录制。Prepare 失败时清空输出且不动已生效
// 程序；Commit 成功时消费 candidate、失败时保留它。Abort 接受 null，每个非 null candidate 必须恰好被消费
// 一次；Abort 不访问 context，context 销毁后仍可用。
MCDR_API int32_t MCDR_CALL mcdr_dx12PrepareFramePrograms(mcdr_command_ctx* context,
    uint64_t generation, uint32_t flags, void** candidate);
MCDR_API int32_t MCDR_CALL mcdr_dx12CommitFramePrograms(mcdr_command_ctx* context, void* candidate);
MCDR_API void MCDR_CALL mcdr_dx12AbortFramePrograms(void* candidate);
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaResetFrame(mcdr_command_ctx* context);
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaBeginRasterPass(mcdr_command_ctx* context, void** token);
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaBeginDeclaredPass(
    mcdr_command_ctx* context, uint32_t pass, const mcdr_vanilla_resource_use* uses,
    uint32_t useCount, uint32_t graphicsEffects, void** token);
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaFinishDeclaredPass(
    mcdr_command_ctx* context, void* token);
MCDR_API int32_t MCDR_CALL mcdr_dx12PrewarmFrameprep(
    mcdr_command_ctx* context, uint64_t generation, uint32_t flags);
MCDR_API int32_t MCDR_CALL mcdr_dx12ProduceReactiveMask(
    mcdr_command_ctx* context, mcdr_object* source0, mcdr_object* source1, mcdr_object* output,
    int32_t width, int32_t height, uint64_t worldEpoch, uint64_t realFrameId,
    uint64_t sourceVersion, uint64_t viewportVersion);
MCDR_API int32_t MCDR_CALL mcdr_dx12ProduceProviderMasks(
    mcdr_command_ctx* context, mcdr_object* source0, mcdr_object* source1, mcdr_object* output,
    mcdr_object* compositionOutput, int32_t width, int32_t height, uint32_t outputFlags,
    uint64_t worldEpoch, uint64_t realFrameId, uint64_t sourceVersion, uint64_t viewportVersion);
enum {
    MCDR_PROVIDER_MASK_SCHEMA_VERSION = 2,
    MCDR_PROVIDER_MASK_FRAME_INPUTS = 1,
    MCDR_PROVIDER_MASK_OIT_INPUTS = 2,
    MCDR_PROVIDER_MASK_HISTORY_COMPATIBLE = 4
};
typedef struct mcdr_provider_mask_input {
    uint32_t schemaVersion;
    uint32_t structSize;
    mcdr_frame_identity identity;
    uint64_t historyGeneration;
    uint64_t previousRealFrameId;
    mcdr_object* color;
    mcdr_object* depth;
    mcdr_object* motion;
    mcdr_object* source0;
    mcdr_object* source1;
    mcdr_object* output;
    mcdr_object* compositionOutput;
    uint32_t width;
    uint32_t height;
    uint32_t outputFlags;
    uint32_t inputFlags;
    float historyJitterX;
    float historyJitterY;
    float currentToPrevious[16];
} mcdr_provider_mask_input;
#define MCDR_WORLD_OUTPUT_SCHEMA_VERSION 3u
#define MCDR_WORLD_SOURCE_COMPLETE_WORLD 0u
#define MCDR_WORLD_SOURCE_OPAQUE_BEFORE_COMPOSITE 1u
typedef struct mcdr_world_output_input {
    uint32_t schemaVersion;
    uint32_t structSize;
    mcdr_frame_identity identity;
    uint32_t motionCoverage;
    uint32_t motionGuideCoverage;
    mcdr_prepared_binding color;
    mcdr_prepared_binding depth;
    mcdr_prepared_binding motion;
    mcdr_prepared_binding exposure;
    mcdr_prepared_binding reconstructionOutput;
    mcdr_prepared_binding scene;
    mcdr_provider_mask_input masks;
    float dlssPostSharpness;
    uint32_t reserved;
    uint32_t sourceStage;
    uint32_t reservedStage;
} mcdr_world_output_input;
MCDR_API int32_t MCDR_CALL mcdr_dx12VanillaPrepareWorldOutput(mcdr_command_ctx* context, const mcdr_world_output_input* input);
MCDR_API int32_t MCDR_CALL mcdr_dx12ProduceProviderMasksV2(
    mcdr_command_ctx* context, const mcdr_provider_mask_input* input);
MCDR_API int32_t MCDR_CALL mcdr_dx12ProduceVrs(
    mcdr_command_ctx* context, mcdr_object* depth, mcdr_object* motion, mcdr_object* previous,
    mcdr_object* output, int32_t width, int32_t height, uint32_t tileSize, uint32_t edgePermille,
    uint32_t motionPixels, uint32_t flags, uint64_t worldEpoch, uint64_t realFrameId,
    uint64_t sourceVersion, uint64_t viewportVersion, mcdr_object* candidates,
    const float* previousToCurrent, const float* currentToPrevious);
MCDR_API int32_t MCDR_CALL mcdr_dx12ProduceExposure(
    mcdr_command_ctx* context, mcdr_object* color, mcdr_object* output,
    int32_t width, int32_t height, uint32_t colorEncoding, float preExposure);
MCDR_API void MCDR_CALL mcdr_dx12ResetFrameprep(mcdr_command_ctx* context);
MCDR_API void* MCDR_CALL mcdr_dx12CreateTerrainWorkGraph(const void* nodes, int32_t nodesBytes,
    const void* initialize, int32_t initializeBytes, const void* finalize, int32_t finalizeBytes,
    const uint32_t* uniformRegisters, int32_t uniformCount, uint64_t budget);
MCDR_API void MCDR_CALL mcdr_dx12DestroyTerrainWorkGraph(void* program);
MCDR_API void* MCDR_CALL mcdr_dx12PrepareTerrainWorkGraph(void* context, void* program,
    void* const* buffers, const uint64_t* offsets, const uint64_t* lengths,
    int32_t bindingCount, uint32_t sectionCount, uint32_t meshletCapacity);
MCDR_API int32_t MCDR_CALL mcdr_dx12DrawTerrainWorkGraph(void* context, void* batch,
    void* vertexBuffer, uint64_t vertexOffset);
MCDR_API int32_t MCDR_CALL mcdr_dx12DrawTerrainWorkGraphV2(void* context, void* batch,
    void* vertexBuffer, uint64_t vertexOffset, uint64_t vertexBytes);
MCDR_API void MCDR_CALL mcdr_dx12DestroyTerrainBatch(void* batch);
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchWorkGraph(
    mcdr_command_ctx* ctx, void* program, const void* records, uint32_t recordCount,
    uint32_t recordStride, mcdr_object* const* buffers, const uint64_t* offsets,
    const uint64_t* lengths, uint32_t bindingCount);
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchWorkGraphGpuBuffers(
    mcdr_command_ctx* ctx, void* program, uint32_t inputSchema, mcdr_object* inputBuffer,
    uint64_t inputOffset, uint64_t inputBytes, mcdr_object* const* buffers,
    const uint64_t* offsets, const uint64_t* lengths, uint32_t bindingCount);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MCDR_ABI_H
