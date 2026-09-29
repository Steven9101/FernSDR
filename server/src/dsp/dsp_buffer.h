// Storage for the large arrays the band transforms sweep on every block.
//
// A million-point transform passes over megabytes in several streams at a
// time. On 4 KiB pages each stream needs a new TLB entry every 4 KiB; on
// 2 MiB pages a 2^20-point transform took 19 to 25% less time than with the
// same arrays on 4 KiB ones. So from 1 MiB on, an array is aligned to 2 MiB
// and, on Linux, offered to transparent huge pages. That is only advice:
// with huge pages disabled nothing changes but the alignment. Smaller arrays
// use ordinary allocations, 64-byte aligned for vector loads.
#pragma once
#include <cstddef>
#include <new>
#include <vector>
#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace fernsdr {

template <class T>
struct LargePageAllocator {
    using value_type = T;

    static constexpr size_t kHugePage = size_t{2} << 20;
    static constexpr size_t kFrom = size_t{1} << 20;
    static constexpr size_t kSmallAlignment = 64;

    LargePageAllocator() noexcept = default;
    template <class U>
    LargePageAllocator(const LargePageAllocator<U>&) noexcept {}

    T* allocate(size_t count) {
        const size_t bytes = count * sizeof(T);
        if (bytes < kFrom) return static_cast<T*>(::operator new(bytes, std::align_val_t(kSmallAlignment)));
        const size_t rounded = (bytes + kHugePage - 1) & ~(kHugePage - 1);
        void* p = ::operator new(rounded, std::align_val_t(kHugePage));
#if defined(__linux__) && defined(MADV_HUGEPAGE)
        madvise(p, rounded, MADV_HUGEPAGE);
#endif
        return static_cast<T*>(p);
    }

    void deallocate(T* p, size_t count) noexcept {
        if (count * sizeof(T) < kFrom) ::operator delete(p, std::align_val_t(kSmallAlignment));
        else ::operator delete(p, std::align_val_t(kHugePage));
    }

    template <class U>
    bool operator==(const LargePageAllocator<U>&) const noexcept { return true; }
    template <class U>
    bool operator!=(const LargePageAllocator<U>&) const noexcept { return false; }
};

template <class T>
using DspVector = std::vector<T, LargePageAllocator<T>>;

}  // namespace fernsdr
