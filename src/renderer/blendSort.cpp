#include "renderer/blendSort.h"

namespace rasm
{
    namespace
    {
        uint32_t getLog2(uint32_t powerOfTwo)
        {
            uint32_t log = 0;
            while ((1u << log) < powerOfTwo)
                ++log;
            return log;
        }

        // The argument of shaders/passes/sort.slang: log2 of the merge size in bits 8 to 15, of the step in 0 to 7.
        uint32_t getSortArgument(uint32_t k, uint32_t j)
        {
            return getLog2(k) << 8 | getLog2(j);
        }

        void addDispatch(BlendSortDispatch *dispatches, uint32_t maxDispatches, uint32_t &count, BlendSortDispatch dispatch)
        {
            if (count < maxDispatches)
                dispatches[count] = dispatch;
            ++count;
        }
    }

    // A power of two, so the bitonic network is whole, and at least one block, which the keys dispatch sorts alone.
    uint32_t getBlendSortCapacity(uint32_t drawCount)
    {
        uint32_t capacity = BLEND_SORT_BLOCK;
        while (capacity < drawCount && capacity <= UINT32_MAX / 2)
            capacity *= 2;
        return capacity;
    }

    // Up to one block, the keys dispatch sorts everything and the write follows. Each merge of size k past a block
    // takes a dispatch per step of distance a block or more, then one for the steps below a block, done in shared
    // memory. Returns the dispatch count, which may be more than maxDispatches were written.
    uint32_t getBlendSortDispatches(uint32_t capacity, BlendSortDispatch *dispatches, uint32_t maxDispatches)
    {
        const uint32_t blocks = capacity / BLEND_SORT_BLOCK;
        uint32_t count = 0;
        addDispatch(dispatches, maxDispatches, count, {.step = BlendSortStep::KEYS, .groups = blocks});
        for (uint64_t k = 2 * BLEND_SORT_BLOCK; k <= capacity; k *= 2)
        {
            for (uint64_t j = k / 2; j >= BLEND_SORT_BLOCK; j /= 2)
            {
                addDispatch(dispatches, maxDispatches, count,
                            {.step = BlendSortStep::MERGE, .argument = getSortArgument(static_cast<uint32_t>(k), static_cast<uint32_t>(j)), .groups = capacity / 2 / BLEND_SORT_GROUP_SIZE});
            }
            addDispatch(dispatches, maxDispatches, count,
                        {.step = BlendSortStep::LOCAL_MERGE, .argument = getSortArgument(static_cast<uint32_t>(k), BLEND_SORT_BLOCK / 2), .groups = blocks});
        }
        addDispatch(dispatches, maxDispatches, count, {.step = BlendSortStep::WRITE, .groups = capacity / BLEND_SORT_GROUP_SIZE});
        return count;
    }
}
