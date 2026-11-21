#include <atomic>
#include <cstdint>

#include "mcdr_hdr_state.h"

namespace {

std::atomic<int32_t> gHdrMode{0};
std::atomic<int32_t> gHdrActive{0};
std::atomic<int32_t> gHdrAppliedMode{-1};

}

extern "C" {

void mcdr_hdr_state_set_mode(int32_t mode) {
    if (mode < 0 || mode > 2) mode = 0;
    gHdrMode.store(mode, std::memory_order_relaxed);
}

int32_t mcdr_hdr_state_mode(void) { return gHdrMode.load(std::memory_order_relaxed); }

void mcdr_hdr_state_set_applied_mode(int32_t mode) {
    gHdrAppliedMode.store(mode, std::memory_order_relaxed);
}

int32_t mcdr_hdr_state_applied_mode(void) {
    return gHdrAppliedMode.load(std::memory_order_relaxed);
}

void mcdr_hdr_state_set_active(int32_t active) {
    gHdrActive.store(active != 0 ? 1 : 0, std::memory_order_relaxed);
}

int32_t mcdr_hdr_state_active(void) { return gHdrActive.load(std::memory_order_relaxed); }

}
