#include "rhi/memoryAliasing.h"

#include <algorithm>
#include <numeric>

namespace rasm::rhi
{
    namespace
    {
        uint64_t alignUp(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        // Both ends are inclusive: a pass that reads one texture and writes another uses both at once.
        bool lifetimesOverlap(const AliasRequest &a, const AliasRequest &b)
        {
            return a.firstPass <= b.lastPass && b.firstPass <= a.lastPass;
        }
    }

    // Greedy first fit, biggest first: each request goes at the lowest offset that no request with an overlapping
    // lifetime uses. Requests share a heap only with the same memory types.
    void packAliased(const AliasRequest *requests, uint32_t count, AliasPlacement *placements, std::vector<AliasHeap> &heaps)
    {
        heaps.clear();
        std::vector<uint32_t> order(count);
        std::iota(order.begin(), order.end(), 0u);
        std::stable_sort(order.begin(), order.end(), [requests](uint32_t a, uint32_t b)
                         { return requests[a].size > requests[b].size; });

        std::vector<uint32_t> placed;
        std::vector<uint32_t> blocking;
        for (uint32_t index : order)
        {
            const AliasRequest &request = requests[index];
            uint32_t heap = 0;
            while (heap < heaps.size() && heaps[heap].memoryTypeBits != request.memoryTypeBits)
                ++heap;
            if (heap == heaps.size())
                heaps.push_back({.memoryTypeBits = request.memoryTypeBits});

            blocking.clear();
            for (uint32_t other : placed)
            {
                if (placements[other].heap == heap && lifetimesOverlap(request, requests[other]))
                    blocking.push_back(other);
            }
            std::sort(blocking.begin(), blocking.end(), [placements](uint32_t a, uint32_t b)
                      { return placements[a].offset < placements[b].offset; });

            uint64_t offset = 0;
            for (uint32_t other : blocking)
            {
                if (alignUp(offset, request.alignment) + request.size <= placements[other].offset)
                    break;
                offset = std::max(offset, placements[other].offset + requests[other].size);
            }
            offset = alignUp(offset, request.alignment);

            placements[index] = {.heap = heap, .offset = offset};
            heaps[heap].size = std::max(heaps[heap].size, offset + request.size);
            heaps[heap].alignment = std::max(heaps[heap].alignment, request.alignment);
            placed.push_back(index);
        }
    }
}
