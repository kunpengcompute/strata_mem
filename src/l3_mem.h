#pragma once

#include <array>
#include <cstddef>

namespace l3mem {

inline constexpr std::size_t kHugePageCount = 32;
inline constexpr std::size_t kHugePageSize = 1ULL << 30;
inline constexpr std::size_t kMappingSize = kHugePageCount * kHugePageSize;
inline constexpr std::size_t kRegionCount = 70;
inline constexpr std::size_t kRegionSize = 1ULL << 20;
inline constexpr std::size_t kBufferSize = kRegionCount * kRegionSize;

struct L3Region {
    void* address = nullptr;
    std::size_t size = 0;
};

// Non-owning descriptor: copies do NOT create new allocations/ownership.
// Exactly one owner must release base_address after all users have stopped.
struct L3Buffer {
    void* base_address = nullptr;
    std::array<L3Region, kRegionCount> regions{};

    [[nodiscard]] bool valid() const noexcept {
        return base_address != nullptr;
    }
};

// Linux 64-bit: one 32GiB anonymous HugeTLB mapping, inherited NUMA policy,
// full initialization, physical-page sorting and 70 fixed 1MiB regions.
// Returns {} and sets errno on failure. No random placement or fallback.
// Does not perform DMA registration or guarantee L3 residency.
[[nodiscard]] L3Buffer malloc_l3() noexcept;

// nullptr is a successful no-op. Otherwise the caller MUST supply an original,
// live base_address returned by malloc_l3(). No allocation registry is kept.
// All CPU/DMA accesses and device registrations must be finished beforehand.
// Success invalidates every associated pointer. Failure sets errno.
[[nodiscard]] bool free_l3(void* base_address) noexcept;

} // namespace l3mem
