module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include <exception>

module mcdr.abi.common;

import mcdr.render.display;

extern "C" int32_t MCDR_CALL mcdr_dx12PrewarmDisplayFrame(
    mcdr_command_ctx* context, uint64_t generation) {
    try {
        const int result = d3d12_mc::display::prewarm(context, generation);
        if (result <= 0) {
            mcdr::abi::setLastError("mcdr_dx12PrewarmDisplayFrame", d3d12_mc::display::lastFailureText());
        }
        return result;
    } catch (const std::exception& failure) {
        mcdr::abi::setLastError("mcdr_dx12PrewarmDisplayFrame", failure.what());
        return -1;
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12PrewarmDisplayFrame", "Display prewarm threw an unknown exception");
        return -1;
    }
}

extern "C" int32_t MCDR_CALL mcdr_dx12SubmitDisplayFrame(
    mcdr_command_ctx* context, const mcdr_display_frame_input* input) {
    try {
        const int result = d3d12_mc::display::submit(context, input, sizeof(mcdr_display_frame_input));
        if (result < 0) {
            mcdr::abi::setLastError("mcdr_dx12SubmitDisplayFrame", d3d12_mc::display::lastFailureText());
        }
        return result;
    } catch (const std::exception& failure) {
        mcdr::abi::setLastError("mcdr_dx12SubmitDisplayFrame", failure.what());
        return -1;
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12SubmitDisplayFrame", "Display submission threw an unknown exception");
        return -1;
    }
}

extern "C" int32_t MCDR_CALL mcdr_dx12PrepareDisplayFrame(
    mcdr_command_ctx* context, const mcdr_display_frame_input* input) {
    try {
        const int result = d3d12_mc::display::prepare(context, input, sizeof(mcdr_display_frame_input));
        if (result < 0) {
            mcdr::abi::setLastError("mcdr_dx12PrepareDisplayFrame", d3d12_mc::display::lastFailureText());
        }
        return result;
    } catch (const std::exception& failure) {
        mcdr::abi::setLastError("mcdr_dx12PrepareDisplayFrame", failure.what());
        return -1;
    } catch (...) {
        mcdr::abi::setLastError("mcdr_dx12PrepareDisplayFrame", "Display preparation threw an unknown exception");
        return -1;
    }
}
