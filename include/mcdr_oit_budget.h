#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
namespace mcdr::oit_budget {
inline constexpr uint64_t limit = 512ull * 1024 * 1024;
inline std::atomic<uint64_t> committedBytes{0};
struct Reservation {
    uint64_t bytes;
    ~Reservation() { committedBytes.fetch_sub(bytes, std::memory_order_acq_rel); }
};
inline std::shared_ptr<Reservation> reserve(uint64_t bytes, uint64_t requestedLimit = limit) {
    const auto maximum = requestedLimit < limit ? requestedLimit : limit;
    if (!bytes || bytes > maximum) return {};
    auto observed = committedBytes.load(std::memory_order_acquire);
    do {
        if (observed > maximum - bytes) return {};
    } while (!committedBytes.compare_exchange_weak(observed, observed + bytes, std::memory_order_acq_rel));
    try { return std::make_shared<Reservation>(bytes); }
    catch (...) { committedBytes.fetch_sub(bytes, std::memory_order_acq_rel); throw; }
}
}
