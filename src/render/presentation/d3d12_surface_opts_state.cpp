#include <atomic>
#include <cstdint>

#include "mcdr_surface_opts_state.h"

namespace {

std::atomic<int32_t> gAllowTearing{1};
std::atomic<int32_t> gForcePresentChild{1};

}

extern "C" {

void mcdr_surface_opts_set_allow_tearing(int32_t allow) {
    gAllowTearing.store(allow != 0 ? 1 : 0, std::memory_order_relaxed);
}

int32_t mcdr_surface_opts_allow_tearing(void) {
    return gAllowTearing.load(std::memory_order_relaxed);
}

void mcdr_surface_opts_set_force_present_child(int32_t force) {
    gForcePresentChild.store(force != 0 ? 1 : 0, std::memory_order_relaxed);
}

int32_t mcdr_surface_opts_force_present_child(void) {
    return gForcePresentChild.load(std::memory_order_relaxed);
}

}
