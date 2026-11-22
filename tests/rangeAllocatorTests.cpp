#include "core/rangeAllocator.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <vector>

using rasm::RangeAllocator;

TEST_CASE("ranges are taken first fit from the start")
{
    RangeAllocator ranges;
    ranges.reset(100);
    uint64_t a = 1;
    uint64_t b = 1;
    CHECK(ranges.allocate(30, a));
    CHECK(ranges.allocate(20, b));
    CHECK(a == 0);
    CHECK(b == 30);
    CHECK(ranges.used() == 50);
    CHECK(ranges.largestFree() == 50);
    CHECK(ranges.end() == 50);
}

TEST_CASE("a freed range is reused, and joins its free neighbours")
{
    RangeAllocator ranges;
    ranges.reset(100);
    uint64_t a = 0;
    uint64_t b = 0;
    uint64_t c = 0;
    ranges.allocate(10, a);
    ranges.allocate(10, b);
    ranges.allocate(10, c);
    ranges.free(b, 10);
    CHECK(ranges.freeRangeCount() == 2);
    uint64_t d = 0;
    CHECK(ranges.allocate(5, d));
    CHECK(d == b);
    ranges.free(d, 5);
    ranges.free(a, 10);
    // a and b join: one range from 0 to 20, and the end.
    CHECK(ranges.freeRangeCount() == 2);
    CHECK(ranges.largestFree() == 70);
    ranges.free(c, 10);
    CHECK(ranges.freeRangeCount() == 1);
    CHECK(ranges.largestFree() == 100);
    CHECK(ranges.used() == 0);
    CHECK(ranges.end() == 0);
}

TEST_CASE("a request larger than every free range fails, even when the sum is enough")
{
    RangeAllocator ranges;
    ranges.reset(30);
    uint64_t a = 0;
    uint64_t b = 0;
    uint64_t c = 0;
    ranges.allocate(10, a);
    ranges.allocate(10, b);
    ranges.allocate(10, c);
    ranges.free(a, 10);
    ranges.free(c, 10);
    uint64_t d = 0;
    CHECK_FALSE(ranges.allocate(15, d));
    CHECK(ranges.used() == 10);
    CHECK(ranges.largestFree() == 10);
    CHECK(ranges.allocate(10, d));
}

TEST_CASE("size 0 takes nothing")
{
    RangeAllocator ranges;
    ranges.reset(4);
    uint64_t a = 7;
    CHECK(ranges.allocate(0, a));
    CHECK(a == 0);
    CHECK(ranges.used() == 0);
    ranges.free(0, 0);
    CHECK(ranges.largestFree() == 4);
}

TEST_CASE("growing adds a free range at the end, joined with a free end")
{
    RangeAllocator ranges;
    ranges.reset(10);
    uint64_t a = 0;
    CHECK(ranges.allocate(6, a));
    ranges.grow(20);
    CHECK(ranges.capacity() == 20);
    CHECK(ranges.used() == 6);
    CHECK(ranges.largestFree() == 14);
    CHECK(ranges.freeRangeCount() == 1);
    uint64_t b = 0;
    CHECK(ranges.allocate(14, b));
    CHECK(b == 6);
    ranges.grow(20);
    CHECK(ranges.capacity() == 20);
}

TEST_CASE("streaming: allocating and freeing in a ring keeps the pool from growing")
{
    RangeAllocator ranges;
    ranges.reset(1000);
    std::vector<uint64_t> offsets;
    std::vector<uint64_t> sizes;
    for (uint32_t round = 0; round < 5000; ++round)
    {
        const uint64_t size = 10 + (round * 37) % 40;
        uint64_t offset = 0;
        if (offsets.size() == 16)
        {
            ranges.free(offsets.front(), sizes.front());
            offsets.erase(offsets.begin());
            sizes.erase(sizes.begin());
        }
        REQUIRE(ranges.allocate(size, offset));
        offsets.push_back(offset);
        sizes.push_back(size);
    }
    uint64_t live = 0;
    for (uint64_t size : sizes)
        live += size;
    CHECK(ranges.used() == live);
    for (size_t i = 0; i < offsets.size(); ++i)
        ranges.free(offsets[i], sizes[i]);
    CHECK(ranges.used() == 0);
    CHECK(ranges.freeRangeCount() == 1);
}
