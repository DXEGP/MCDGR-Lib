// abi_framegen.cpp — frame-generation C ABI adapter, module mcdr.abi.common: validate, fill the POD
// table, forward to the registry in src\advanced\d3d12_framegen.cpp (resource args are JVM handles).
// 见 docs\advanced\framegen.md

module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

import mcdr.d3d12.device;
// mcdr.error 是叶子模块，不 import 就取不到 mcdr::Error（import 不传递）。
import mcdr.error;

using namespace d3d12_mc;

extern "C" {
void MCDR_CALL mcdr_dx12FgSetConstants(const mcdr_fg_constants* constants)
{
    fgSetFrameConstants(constants);
}

// Records the wanted backend only; the rebuild happens in the next frame's fgEnsureProvider.
int32_t MCDR_CALL mcdr_dx12FgPick(int32_t backend) {
    return fgPickBackend(backend);
}

// `available` is the verdict; the return value only means the table was filled.
int32_t MCDR_CALL mcdr_dx12FgQueryCaps(int32_t backend, mcdr_fg_caps* out) {
    if (out == nullptr) {
        return -1;
    }
    // 0..3 之外一律 -1：非法 id 若填成 "unknown backend" 再返回成功，调用方会当成表有效。
    if (backend < MCDR_FG_BACKEND_NONE || backend > MCDR_FG_BACKEND_XESSFG) {
        return -1;
    }
    fgFillCaps(backend, *out);
    return 1;
}

// Records the wanted mode only: the real slDLSSGSetOptions goes out on the render thread
// before the next present, so a cross-thread call would land on an undefined frame.
int32_t MCDR_CALL mcdr_dx12FgSetMode(const mcdr_fg_dispatch* desc) {
    if (desc == nullptr) {
        return -1;
    }
    return fgSetMode(*desc);
}

// Frames actually presented last time (1 = no interpolation).
int32_t MCDR_CALL mcdr_dx12FgPresented(void) {
    return fgPresented();
}

// One-line status text via writeString: (NULL,0) probes size+1, a short buffer is not an error.
int32_t MCDR_CALL mcdr_dx12FgStatus(char* note, int32_t noteCap) {
    if (noteCap < 0 || (noteCap > 0 && note == nullptr)) {
        return -1;
    }
    const std::string text = fgStatusText();
    if (note == nullptr || noteCap == 0) {
        // 只问长度：与 mcdrLastError 的约定一致，不写缓冲。
        return static_cast<int32_t>(text.size());
    }
    return mcdr::abi::writeString(text, note, noteCap);
}

// Packs wanted / chain / backend-alive / rebuild-count into one int32 (bit layout in
// include\mcdr_abi.h): a rollback clears the wanted mode while the JVM's options file still says "on".
int32_t MCDR_CALL mcdr_dx12FgState(void) {
    return fgStatePacked();
}

// UI texture: the GUI composition target (a JVM texture handle).
void MCDR_CALL mcdr_dx12FgSetUiTexture(void* object) { fgSetUiTexture(object); }

// Hudless-present switch and "who composites the UI" query; semantics live in mcdr_abi.h.
void MCDR_CALL mcdr_dx12FgSetUiComposition(int32_t enabled) { fgSetUiComposition(enabled); }
int32_t MCDR_CALL mcdr_dx12FgUiCompositionReady(void) { return fgUiCompositionReady(); }

// Internal (JVM never calls it): the DLSS path's per-frame tag bridge. src\advanced\d3d12_dlss.cpp is
// a plain TU that cannot name module types, so only PODs travel here and both mangled names agree.
// 见 docs\c_abi\README.md §3.2
void MCDR_CALL mcdr_fgTagDlssInputs(void* cmdList, void* color, int32_t colorState, void* depth,
                                    int32_t depthState, void* motionVectors, int32_t motionVectorState) {
    FgFrameInputs in{};
    in.cmdList = static_cast<ID3D12GraphicsCommandList*>(cmdList);
    in.pColorOutput = color;
    in.colorState = static_cast<D3D12_RESOURCE_STATES>(colorState);
    in.pDepth = depth;
    in.depthState = static_cast<D3D12_RESOURCE_STATES>(depthState);
    in.pMotionVectors = motionVectors;
    in.motionVectorState = static_cast<D3D12_RESOURCE_STATES>(motionVectorState);
    fgTagFrameInputs(in);
}
}  // extern "C"
