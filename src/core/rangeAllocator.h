// clang-format off
#pragma once

#include <cstdint>
#include <map>

namespace rasm
{
    // Ranges of [0, capacity): a sorted map of free ranges, first fit, and a freed range joins its free neighbours.
    // Offsets and sizes are in the caller's unit (vertices, rows, bytes).
    class RangeAllocator
    {
    public:
        void                            reset(uint64_t capacity);
        void                            grow(uint64_t capacity);                    // a larger capacity: the new end is free
        bool                            allocate(uint64_t size, uint64_t& offset);  // false when no free range is that large; size 0 takes nothing
        void                            free(uint64_t offset, uint64_t size);
        uint64_t                        capacity() const;
        uint64_t                        used() const;
        uint64_t                        largestFree() const;
        uint64_t                        end() const;                                // one past the highest offset in use, 0 when nothing is
        uint32_t                        freeRangeCount() const;

    private:
        std::map<uint64_t, uint64_t>    freeRanges;                                 // offset -> size
        uint64_t                        total               = 0;
        uint64_t                        inUse               = 0;
    };
}
// clang-format on
