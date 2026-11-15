// 光追可见性分层的跨 TU 状态（纯 C，无 D3D12 依赖）。
#include "mcdr_rt_state.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>

#include "mcdr_rt_tracker.h"

namespace {
std::atomic<int32_t> gRtMode{0};
std::atomic<int32_t> gGbufferReady{0};
std::atomic<int32_t> gGbufferWidth{0};
std::atomic<int32_t> gGbufferHeight{0};
}  // namespace

extern "C" void mcdr_rt_state_set_mode(int32_t mode) {
    gRtMode.store(mode, std::memory_order_relaxed);
}

extern "C" int32_t mcdr_rt_state_mode(void) {
    return gRtMode.load(std::memory_order_relaxed);
}

extern "C" void mcdr_rt_state_set_gbuffer_ready(int32_t ready) {
    gGbufferReady.store(ready, std::memory_order_relaxed);
}

extern "C" int32_t mcdr_rt_state_gbuffer_ready(void) {
    return gGbufferReady.load(std::memory_order_relaxed);
}

extern "C" void mcdr_rt_state_set_gbuffer_size(int32_t width, int32_t height) {
    gGbufferWidth.store(width, std::memory_order_relaxed);
    gGbufferHeight.store(height, std::memory_order_relaxed);
}

extern "C" int32_t mcdr_rt_state_gbuffer_width(void) {
    return gGbufferWidth.load(std::memory_order_relaxed);
}

extern "C" int32_t mcdr_rt_state_gbuffer_height(void) {
    return gGbufferHeight.load(std::memory_order_relaxed);
}

// ---- S0-D1 shadow resource-state tracker -------------------------------------------
// Records what each resource was last transitioned to and compares against the
// StateBefore the caller declared. Never changes what is submitted.
namespace {

std::mutex gTrackMutex;
std::unordered_map<uint64_t, uint32_t> gTrackStates;
mcdr_rt_track_stats gTrackStats{};
char gTrackNote[64]{};

// D3D12_RESOURCE_STATE_COMMON：可以作为任意读状态的起点（隐式提升，不需要 barrier）。
constexpr uint32_t kStateCommon = 0u;
// D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES：整资源过渡，是逐切片之外的另一种键。
constexpr uint32_t kAllSubresources = 0xFFFFFFFFu;
constexpr size_t kTrackCapacity = 16384;

uint64_t trackKey(void* resource, uint32_t subresource) {
    return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(resource)) << 16) |
           (subresource & 0xFFFFull);
}

void setNote(uint32_t declared, uint32_t tracked) {
    std::snprintf(gTrackNote, sizeof(gTrackNote), "decl=0x%X track=0x%X",
                  static_cast<unsigned>(declared), static_cast<unsigned>(tracked));
}

}  // namespace

extern "C" void mcdr_rt_track_transition(void* resource, uint32_t declaredBefore, uint32_t after,
                                         uint32_t subresource) {
    if (!resource) return;
    std::lock_guard<std::mutex> lk(gTrackMutex);
    ++gTrackStats.transitions;
    // 逐切片的键优先；只有"整资源"那条被记过时，逐切片查询退回它（后者范围更大）。
    uint64_t key = trackKey(resource, subresource);
    auto it = gTrackStates.find(key);
    if (it == gTrackStates.end() && subresource != kAllSubresources) {
        key = trackKey(resource, kAllSubresources);
        it = gTrackStates.find(key);
    }
    if (it == gTrackStates.end()) {
        if (gTrackStates.size() < kTrackCapacity) {
            gTrackStates.emplace(trackKey(resource, subresource), after);
            ++gTrackStats.tracked;
        }
        ++gTrackStats.firstUse;
        return;
    }
    const uint32_t tracked = it->second;
    if (tracked == kStateCommon) {
        // 隐式提升：从 COMMON 出发不需要 barrier，D3D12 明确规定。
        ++gTrackStats.promotions;
    } else if (tracked != declaredBefore) {
        if ((declaredBefore & tracked) == tracked) {
            // 调用方用组合读掩码声明 before（例如 0xAC3），单状态只是它的子集。
            ++gTrackStats.widened;
        } else {
            ++gTrackStats.mismatches;
            setNote(declaredBefore, tracked);
        }
    }
    it->second = after;
}

extern "C" void mcdr_rt_track_uav(void* resource) {
    if (!resource) return;
    std::lock_guard<std::mutex> lk(gTrackMutex);
    ++gTrackStats.uavBarriers;
}

extern "C" void mcdr_rt_track_query(mcdr_rt_track_stats* out) {
    if (!out) return;
    std::lock_guard<std::mutex> lk(gTrackMutex);
    *out = gTrackStats;
    out->tracked = static_cast<int32_t>(gTrackStates.size());
    std::snprintf(out->note, sizeof(out->note), "%s", gTrackNote);
}

extern "C" void mcdr_rt_track_reset(void) {
    std::lock_guard<std::mutex> lk(gTrackMutex);
    gTrackStates.clear();
    gTrackStats = mcdr_rt_track_stats{};
    gTrackNote[0] = '\0';
}