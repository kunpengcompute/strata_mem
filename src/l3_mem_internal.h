#pragma once

#include "l3_mem.h"
#include <cstdint>

// Internal helpers, not public API.
namespace l3mem::detail {
struct RegionSpec {
    std::size_t page;
    std::uint64_t offset;
};

// Exact region order from supplied mode=random seed=35 log, not regenerated.
inline constexpr std::array<RegionSpec, kRegionCount> kFixedRegions{{
    {0, 0x05b00000ULL}, // region 0
    {1, 0x23200000ULL}, // region 1
    {2, 0x32f00000ULL}, // region 2
    {3, 0x11500000ULL}, // region 3
    {4, 0x34400000ULL}, // region 4
    {5, 0x36700000ULL}, // region 5
    {6, 0x15300000ULL}, // region 6
    {7, 0x3b500000ULL}, // region 7
    {8, 0x03a00000ULL}, // region 8
    {9, 0x21300000ULL}, // region 9
    {10, 0x38500000ULL}, // region 10
    {11, 0x2eb00000ULL}, // region 11
    {12, 0x09600000ULL}, // region 12
    {13, 0x28c00000ULL}, // region 13
    {14, 0x2b500000ULL}, // region 14
    {15, 0x1b200000ULL}, // region 15
    {16, 0x2d300000ULL}, // region 16
    {17, 0x24100000ULL}, // region 17
    {18, 0x11c00000ULL}, // region 18
    {19, 0x24000000ULL}, // region 19
    {20, 0x1eb00000ULL}, // region 20
    {21, 0x2ad00000ULL}, // region 21
    {22, 0x14f00000ULL}, // region 22
    {23, 0x09500000ULL}, // region 23
    {24, 0x2b400000ULL}, // region 24
    {25, 0x04a00000ULL}, // region 25
    {26, 0x13f00000ULL}, // region 26
    {27, 0x26400000ULL}, // region 27
    {28, 0x36e00000ULL}, // region 28
    {29, 0x2a200000ULL}, // region 29
    {30, 0x0d400000ULL}, // region 30
    {31, 0x34f00000ULL}, // region 31
    {0, 0x36c00000ULL}, // region 32
    {1, 0x25500000ULL}, // region 33
    {2, 0x27400000ULL}, // region 34
    {3, 0x02100000ULL}, // region 35
    {4, 0x07000000ULL}, // region 36
    {5, 0x2e300000ULL}, // region 37
    {6, 0x00500000ULL}, // region 38
    {7, 0x29200000ULL}, // region 39
    {8, 0x37100000ULL}, // region 40
    {9, 0x08a00000ULL}, // region 41
    {10, 0x08200000ULL}, // region 42
    {11, 0x1e400000ULL}, // region 43
    {12, 0x2e500000ULL}, // region 44
    {13, 0x0e200000ULL}, // region 45
    {14, 0x27600000ULL}, // region 46
    {15, 0x16800000ULL}, // region 47
    {16, 0x25d00000ULL}, // region 48
    {17, 0x31300000ULL}, // region 49
    {18, 0x02e00000ULL}, // region 50
    {19, 0x0a700000ULL}, // region 51
    {20, 0x23000000ULL}, // region 52
    {21, 0x2ba00000ULL}, // region 53
    {22, 0x17600000ULL}, // region 54
    {23, 0x12e00000ULL}, // region 55
    {24, 0x2f500000ULL}, // region 56
    {25, 0x29d00000ULL}, // region 57
    {26, 0x19f00000ULL}, // region 58
    {27, 0x22900000ULL}, // region 59
    {28, 0x3f500000ULL}, // region 60
    {29, 0x2a300000ULL}, // region 61
    {30, 0x02700000ULL}, // region 62
    {31, 0x2bd00000ULL}, // region 63
    {0, 0x07700000ULL}, // region 64
    {1, 0x1ab00000ULL}, // region 65
    {2, 0x0bf00000ULL}, // region 66
    {3, 0x34600000ULL}, // region 67
    {4, 0x37400000ULL}, // region 68
    {5, 0x03600000ULL}, // region 69
}};

struct Page {
    std::uintptr_t virtual_base;
    std::uint64_t physical_base;
    std::size_t original_index;
};

constexpr bool valid_specs(
    const std::array<RegionSpec, kRegionCount>& specs) noexcept {
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto& s = specs[i];
        if (s.page >= kHugePageCount || s.offset % kRegionSize ||
            s.offset > kHugePageSize - kRegionSize)
            return false;
        for (std::size_t j = 0; j < i; ++j) {
            if (s.page == specs[j].page &&
                s.offset < specs[j].offset + kRegionSize &&
                specs[j].offset < s.offset + kRegionSize)
                return false;
        }
    }
    return true;
}
static_assert(valid_specs(kFixedRegions), "Invalid fixed region table");

inline bool sort_pages(std::array<Page, kHugePageCount>& pages) noexcept {
    // Fixed small table; no heap allocation or throwing operations.
    for (std::size_t i = 1; i < pages.size(); ++i) {
        auto page = pages[i];
        auto j = i;
        while (j && pages[j - 1].physical_base > page.physical_base) {
            pages[j] = pages[j - 1];
            --j;
        }
        pages[j] = page;
    }
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (!pages[i].virtual_base ||
            pages[i].virtual_base % kHugePageSize ||
            pages[i].physical_base % kHugePageSize ||
            (i && pages[i].physical_base == pages[i - 1].physical_base))
            return false;
    }
    return true;
}

inline void fill_regions(const std::array<Page, kHugePageCount>& pages,
                         L3Buffer& result) noexcept {
    for (std::size_t i = 0; i < kRegionCount; ++i) {
        const auto& spec = kFixedRegions[i];
        result.regions[i] = {
            reinterpret_cast<void*>(pages[spec.page].virtual_base + spec.offset),
            kRegionSize
        };
    }
}
} // namespace l3mem::detail
