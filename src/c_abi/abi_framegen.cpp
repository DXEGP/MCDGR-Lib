module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include <exception>

module mcdr.abi.common;

import mcdr.d3d12.device;
import mcdr.render.vanilla;
import mcdr.render.display;
import mcdr.error;

using namespace d3d12_mc;

extern "C" {
int32_t MCDR_CALL mcdr_dx12BeginDisplayFrame(mcdr_command_ctx* context) {
    return context ? beginDisplayFrameRecording(context) : -1;
}

int32_t MCDR_CALL mcdr_dx12AbortDisplayFrame(mcdr_command_ctx* context) {
    try {
        const auto result = abortDisplayFrame(context);
        if (!result) {
            mcdr::abi::setLastError("mcdr_dx12AbortDisplayFrame", result.error().message);
            return -1;
        }
        if (display::retire(context) != 1) {
            mcdr::abi::setLastError("mcdr_dx12AbortDisplayFrame", display::lastFailureText());
            return -1;
        }
        vanilla::reset(context);
        return 1;
    } catch (const std::exception& failure) {
        mcdr::abi::setLastError("mcdr_dx12AbortDisplayFrame", failure.what());
        return -1;
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12AbortDisplayFrame", "Display cancellation threw an unknown exception");
        return -1;
    }
}

int32_t MCDR_CALL mcdr_dx12FgPick(int32_t backend) {
    return fgPickBackend(backend);
}

int32_t MCDR_CALL mcdr_dx12FgQueryCaps(int32_t backend, mcdr_fg_caps* out) {
    if (out == nullptr) {
        return -1;
    }
    if (backend < MCDR_FG_BACKEND_NONE || backend > MCDR_FG_BACKEND_XESSFG) {
        return -1;
    }
    fgFillCaps(backend, *out);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12FgSetMode(const mcdr_fg_dispatch* desc) {
    if (desc == nullptr) {
        return -1;
    }
    return fgSetMode(*desc);
}

int32_t MCDR_CALL mcdr_dx12FgPrepare(const mcdr_fg_dispatch* description, uint64_t* token) {
    if (description == nullptr || token == nullptr) {
        if (token != nullptr) *token = 0;
        mcdr::abi::setLastError("mcdr_dx12FgPrepare", "FG preparation requires a description and token output");
        return MCDR_FG_PREPARE_INVALID_ARGUMENT;
    }
    const int32_t result = fgPrepareMode(*description, token);
    if (result < 0) mcdr::abi::setLastError("mcdr_dx12FgPrepare", fgStatusText());
    return result;
}

int32_t MCDR_CALL mcdr_dx12CommitPreparedConfiguration(uint64_t srToken,
    uint64_t fgToken, int32_t antiAliasing) {
    return fgCommitPreparedConfiguration(fgToken, srToken, antiAliasing);
}

int32_t MCDR_CALL mcdr_dx12FgAbort(uint64_t token) {
    return fgAbortPreparedMode(token);
}

int32_t MCDR_CALL mcdr_dx12FgPresented(void) {
    return fgPresented();
}

int32_t MCDR_CALL mcdr_dx12FgStatus(char* note, int32_t noteCap) {
    if (noteCap < 0 || (noteCap > 0 && note == nullptr)) {
        return -1;
    }
    const std::string text = fgStatusText();
    if (note == nullptr || noteCap == 0) {
        return static_cast<int32_t>(text.size());
    }
    return mcdr::abi::writeString(text, note, noteCap);
}

int32_t MCDR_CALL mcdr_dx12FgState(void) {
    return fgStatePacked();
}

void MCDR_CALL mcdr_dx12FgSetUiTexture(void* object) { fgSetUiTexture(object); }

void MCDR_CALL mcdr_dx12FgSetUiComposition(int32_t enabled) { fgSetUiComposition(enabled); }
int32_t MCDR_CALL mcdr_dx12FgUiCompositionReady(void) { return fgUiCompositionReady(); }

}
