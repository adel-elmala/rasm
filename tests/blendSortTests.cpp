#include "renderer/blendSort.h"

#include "doctest/doctest.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace
{
    struct Key
    {
        uint32_t depth;
        uint32_t draw;
    };

    bool isBefore(const Key &a, const Key &b)
    {
        return a.depth < b.depth || (a.depth == b.depth && a.draw < b.draw);
    }

    // The compare step of shaders/passes/sort.slang: elements i and i + j in order, ascending where i & k is 0.
    void mergeStep(std::vector<Key> &keys, uint32_t first, uint32_t end, uint32_t k, uint32_t j)
    {
        for (uint32_t i = first; i < end; ++i)
        {
            if ((i & j) != 0)
                continue;
            const bool ascending = (i & k) == 0;
            if (isBefore(keys[i + j], keys[i]) == ascending)
                std::swap(keys[i], keys[i + j]);
        }
    }

    // Runs the dispatches the way the shader entry points do, block by block where they work in shared memory.
    void runSort(std::vector<Key> &keys, const rasm::BlendSortDispatch *dispatches, uint32_t count)
    {
        const auto capacity = static_cast<uint32_t>(keys.size());
        for (uint32_t d = 0; d < count; ++d)
        {
            const rasm::BlendSortDispatch &dispatch = dispatches[d];
            const uint32_t k = 1u << (dispatch.argument >> 8 & 0xFF);
            const uint32_t j = 1u << (dispatch.argument & 0xFF);
            if (dispatch.step == rasm::BlendSortStep::KEYS)
            {
                CHECK(dispatch.groups == capacity / rasm::BLEND_SORT_BLOCK);
                for (uint32_t block = 0; block < dispatch.groups; ++block)
                {
                    const uint32_t first = block * rasm::BLEND_SORT_BLOCK;
                    for (uint32_t size = 2; size <= rasm::BLEND_SORT_BLOCK; size *= 2)
                        for (uint32_t step = size / 2; step > 0; step /= 2)
                            mergeStep(keys, first, first + rasm::BLEND_SORT_BLOCK, size, step);
                }
            }
            if (dispatch.step == rasm::BlendSortStep::MERGE)
            {
                CHECK(dispatch.groups * rasm::BLEND_SORT_GROUP_SIZE == capacity / 2);
                CHECK(j >= rasm::BLEND_SORT_BLOCK);
                mergeStep(keys, 0, capacity, k, j);
            }
            if (dispatch.step == rasm::BlendSortStep::LOCAL_MERGE)
            {
                CHECK(j == rasm::BLEND_SORT_BLOCK / 2);
                for (uint32_t block = 0; block < dispatch.groups; ++block)
                    for (uint32_t step = rasm::BLEND_SORT_BLOCK / 2; step > 0; step /= 2)
                        mergeStep(keys, block * rasm::BLEND_SORT_BLOCK, (block + 1) * rasm::BLEND_SORT_BLOCK, k, step);
            }
            if (dispatch.step == rasm::BlendSortStep::WRITE)
                CHECK(dispatch.groups * rasm::BLEND_SORT_GROUP_SIZE >= capacity);
        }
    }

    // count draws at random depths, padded with the largest key like sortKeysMain does.
    bool sortsDraws(uint32_t count)
    {
        const uint32_t capacity = rasm::getBlendSortCapacity(count);
        std::mt19937 random(count);
        std::vector<Key> keys(capacity, Key{0xFFFFFFFF, 0xFFFFFFFF});
        for (uint32_t i = 0; i < count; ++i)
            keys[i] = {static_cast<uint32_t>(random() % 64), i};

        rasm::BlendSortDispatch dispatches[rasm::MAX_BLEND_SORT_DISPATCHES];
        const uint32_t dispatchCount = rasm::getBlendSortDispatches(capacity, dispatches, rasm::MAX_BLEND_SORT_DISPATCHES);
        runSort(keys, dispatches, dispatchCount);
        return std::is_sorted(keys.begin(), keys.end(), isBefore) && keys[count == 0 ? 0 : count - 1].draw != 0xFFFFFFFF;
    }
}

TEST_CASE("the blend sort holds a power of two of at least one block")
{
    CHECK(rasm::getBlendSortCapacity(0) == 1024);
    CHECK(rasm::getBlendSortCapacity(1) == 1024);
    CHECK(rasm::getBlendSortCapacity(1024) == 1024);
    CHECK(rasm::getBlendSortCapacity(1025) == 2048);
    CHECK(rasm::getBlendSortCapacity(5000) == 8192);
}

TEST_CASE("up to one block the blend sort is the keys dispatch and the write")
{
    rasm::BlendSortDispatch dispatches[rasm::MAX_BLEND_SORT_DISPATCHES];
    REQUIRE(rasm::getBlendSortDispatches(1024, dispatches, rasm::MAX_BLEND_SORT_DISPATCHES) == 2);
    CHECK(dispatches[0].step == rasm::BlendSortStep::KEYS);
    CHECK(dispatches[0].groups == 1);
    CHECK(dispatches[1].step == rasm::BlendSortStep::WRITE);
    CHECK(dispatches[1].groups == 4);
}

TEST_CASE("each merge past one block takes a dispatch per wide step and one for the steps inside a block")
{
    rasm::BlendSortDispatch dispatches[rasm::MAX_BLEND_SORT_DISPATCHES];
    // keys, (k 2048: j 1024, local), write
    CHECK(rasm::getBlendSortDispatches(2048, dispatches, rasm::MAX_BLEND_SORT_DISPATCHES) == 4);
    // keys, (k 2048: j 1024, local), (k 4096: j 2048, j 1024, local), write
    CHECK(rasm::getBlendSortDispatches(4096, dispatches, rasm::MAX_BLEND_SORT_DISPATCHES) == 7);
    CHECK(dispatches[3].step == rasm::BlendSortStep::MERGE);
    CHECK(dispatches[3].argument == (12u << 8 | 11u));
    CHECK(dispatches[5].step == rasm::BlendSortStep::LOCAL_MERGE);
    CHECK(dispatches[5].argument == (12u << 8 | 9u));
    // The largest capacity still fits the dispatch array.
    CHECK(rasm::getBlendSortDispatches(1u << 31, dispatches, rasm::MAX_BLEND_SORT_DISPATCHES) <= rasm::MAX_BLEND_SORT_DISPATCHES);
}

TEST_CASE("the dispatches sort the draws back to front, in one block and across blocks")
{
    CHECK(sortsDraws(1));
    CHECK(sortsDraws(700));
    CHECK(sortsDraws(1024));
    CHECK(sortsDraws(1025));
    CHECK(sortsDraws(3000));
    CHECK(sortsDraws(8192));
}
