#include "core/rangeAllocator.h"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace rasm
{
    void RangeAllocator::reset(uint64_t capacity)
    {
        freeRanges.clear();
        total = capacity;
        inUse = 0;
        if (capacity > 0)
            freeRanges[0] = capacity;
    }

    void RangeAllocator::grow(uint64_t capacity)
    {
        if (capacity <= total)
            return;
        const uint64_t added = capacity - total;
        const uint64_t oldTotal = total;
        total = capacity;
        inUse += added; // free() takes it off again
        free(oldTotal, added);
    }

    bool RangeAllocator::allocate(uint64_t size, uint64_t &offset)
    {
        offset = 0;
        if (size == 0)
            return true;
        for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it)
        {
            if (it->second < size)
                continue;
            offset = it->first;
            const uint64_t rest = it->second - size;
            freeRanges.erase(it);
            if (rest > 0)
                freeRanges[offset + size] = rest;
            inUse += size;
            return true;
        }
        return false;
    }

    void RangeAllocator::free(uint64_t offset, uint64_t size)
    {
        if (size == 0)
            return;
        assert(offset + size <= total && inUse >= size);
        inUse -= size;
        auto next = freeRanges.lower_bound(offset);
        // Joins the range before when it ends here, and the one after when it starts at the end.
        if (next != freeRanges.begin())
        {
            auto previous = std::prev(next);
            assert(previous->first + previous->second <= offset);
            if (previous->first + previous->second == offset)
            {
                offset = previous->first;
                size += previous->second;
                freeRanges.erase(previous);
            }
        }
        if (next != freeRanges.end())
        {
            assert(offset + size <= next->first);
            if (offset + size == next->first)
            {
                size += next->second;
                freeRanges.erase(next);
            }
        }
        freeRanges[offset] = size;
    }

    uint64_t RangeAllocator::capacity() const
    {
        return total;
    }

    uint64_t RangeAllocator::used() const
    {
        return inUse;
    }

    uint64_t RangeAllocator::largestFree() const
    {
        uint64_t largest = 0;
        for (const auto &[offset, size] : freeRanges)
            largest = std::max(largest, size);
        return largest;
    }

    uint64_t RangeAllocator::end() const
    {
        if (inUse == 0)
            return 0;
        if (freeRanges.empty())
            return total;
        const auto last = std::prev(freeRanges.end());
        return last->first + last->second == total ? last->first : total;
    }

    uint32_t RangeAllocator::freeRangeCount() const
    {
        return static_cast<uint32_t>(freeRanges.size());
    }
}
