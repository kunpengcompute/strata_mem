#pragma once

#include "../src/l3_mem.h"
#include <cstdint>

namespace workload {
inline constexpr std::size_t kStride = 64;

// Access order only. Allocator placement has no random generator.
inline std::uint64_t next_random(std::uint64_t& state) noexcept {
    auto z = (state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

inline std::uint64_t bounded_random(std::uint64_t& state,
                                    std::uint64_t bound) noexcept {
    const auto threshold = (UINT64_C(0) - bound) % bound;
    std::uint64_t value;
    do { value = next_random(state); } while (value < threshold);
    return value % bound;
}

inline void read_region(const l3mem::L3Region& region) noexcept {
    auto* p = static_cast<const volatile std::uint8_t*>(region.address);
    for (std::size_t off = 0; off < region.size; off += kStride)
        (void)p[off];
}
} // namespace workload
