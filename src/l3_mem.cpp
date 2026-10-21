#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "l3_mem_internal.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace l3mem {
namespace {
#ifdef __linux__
static_assert(sizeof(void*) == 8 && sizeof(off_t) >= 8,
              "l3mem requires a 64-bit process and file offsets");

bool physical_address(int fd, std::size_t page_size, std::uintptr_t va,
                      std::uint64_t& pa) noexcept {
    const auto index = va / page_size;
    if (index > static_cast<std::uint64_t>(
                    std::numeric_limits<off_t>::max()) / sizeof(std::uint64_t)) {
        errno = EOVERFLOW;
        return false;
    }
    std::uint64_t entry = 0;
    ssize_t n;
    do {
        n = pread(fd, &entry, sizeof(entry),
                  static_cast<off_t>(index * sizeof(entry)));
    } while (n < 0 && errno == EINTR);
    if (n != static_cast<ssize_t>(sizeof(entry))) {
        if (n >= 0) errno = EIO;
        return false;
    }
    if (!(entry & (UINT64_C(1) << 63)) || (entry & (UINT64_C(1) << 62))) {
        errno = EFAULT;
        return false;
    }
    const auto pfn = entry & ((UINT64_C(1) << 55) - 1);
    if (!pfn) {
        errno = EACCES;
        return false;
    }
    pa = pfn * page_size + va % page_size;
    return true;
}

L3Buffer rollback(void* base, int fd, int error, const char* stage) noexcept {
    if (fd >= 0) close(fd);
    if (base && munmap(base, kMappingSize) != 0) {
        std::fprintf(stderr,
                     "l3mem: rollback munmap failed: base=%p bytes=%zu errno=%d; "
                     "manual cleanup/process exit required\n",
                     base, kMappingSize, errno);
    }
    std::fprintf(stderr, "l3mem: %s failed: %s (errno=%d); no fallback\n",
                 stage, std::strerror(error), error);
    errno = error;
    return {};
}
#endif
} // namespace

L3Buffer malloc_l3() noexcept {
#ifdef __linux__
    const long system_page_size = sysconf(_SC_PAGESIZE);
    if (system_page_size <= 0 ||
        kHugePageSize % static_cast<std::size_t>(system_page_size)) {
        errno = EINVAL;
        return {};
    }
    const auto page_size = static_cast<std::size_t>(system_page_size);
    const int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    // Same mapping as the source program; inherit the caller's NUMA policy.
    void* base = mmap(nullptr, kMappingSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (30 << 26),
                      -1, 0);
    if (base == MAP_FAILED) return rollback(nullptr, fd, errno, "mmap");
    if (!base) {
        // nullptr is reserved as the public API's failure value.
        if (munmap(base, kMappingSize))
            std::fprintf(stderr, "l3mem: zero-address rollback failed\n");
        return rollback(nullptr, fd, ENOMEM, "zero-address mapping");
    }
    if (reinterpret_cast<std::uintptr_t>(base) % kHugePageSize)
        return rollback(base, fd, EINVAL, "virtual alignment");

    std::fprintf(stderr, "l3mem: memset full 32GiB; base=%p\n", base);
    // Exclude initialization from measurements. Resource faults can SIGBUS.
    std::memset(base, 0, kMappingSize);
    std::array<detail::Page, kHugePageCount> pages{};
    for (std::size_t h = 0; h < kHugePageCount; ++h) {
        const auto va = reinterpret_cast<std::uintptr_t>(base) + h * kHugePageSize;
        std::uint64_t pa;
        if (!physical_address(fd, page_size, va, pa))
            return rollback(base, fd, errno, "pagemap (CAP_SYS_ADMIN required)");
        if (pa % kHugePageSize)
            return rollback(base, fd, EINVAL, "physical alignment");
        for (std::size_t off = page_size; off < kHugePageSize; off += page_size) {
            std::uint64_t current;
            if (!physical_address(fd, page_size, va + off, current))
                return rollback(base, fd, errno, "pagemap continuity");
            if (current != pa + off)
                return rollback(base, fd, EINVAL, "physical continuity");
        }
        pages[h] = {va, pa, h};
    }
    close(fd);
    if (!detail::sort_pages(pages))
        return rollback(base, -1, EINVAL, "physical page sorting");

    L3Buffer result{};
    result.base_address = base; // Not sorted page 0's virtual address.
    detail::fill_regions(pages, result);
    for (std::size_t h = 0; h < kHugePageCount; ++h) {
        std::fprintf(stderr,
                     "sorted_page=%zu original_page=%zu huge_va=%p "
                     "huge_pa=0x%016" PRIx64 "\n",
                     h, pages[h].original_index,
                     reinterpret_cast<void*>(pages[h].virtual_base),
                     pages[h].physical_base);
    }
    for (std::size_t i = 0; i < kRegionCount; ++i) {
        const auto& spec = detail::kFixedRegions[i];
        std::fprintf(stderr,
                     "region=%zu sorted_page=%zu offset=0x%08" PRIx64
                     " region_va=%p region_pa=0x%016" PRIx64 " size=%zu\n",
                     i, spec.page, spec.offset, result.regions[i].address,
                     pages[spec.page].physical_base + spec.offset, kRegionSize);
    }
    std::fprintf(stderr,
                 "l3mem ready: base=%p reserved=%zu selected=%zu regions=%zu "
                 "layout=seed35-fixed-v1 numa=inherited\n",
                 base, kMappingSize, kBufferSize, kRegionCount);
    return result;
#else
    errno = ENOTSUP;
    return {};
#endif
}

bool free_l3(void* base_address) noexcept {
    if (!base_address) return true;
#ifdef __linux__
    // Alignment is NOT ownership validation. Caller must supply a live base.
    if (reinterpret_cast<std::uintptr_t>(base_address) % kHugePageSize) {
        errno = EINVAL;
        return false;
    }
    return munmap(base_address, kMappingSize) == 0;
#else
    errno = ENOTSUP;
    return false;
#endif
}
} // namespace l3mem
