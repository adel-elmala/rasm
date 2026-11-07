#include "renderer/gpuTable.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <vector>

using namespace rasm;

TEST_CASE("mergeRows: no rows give no ranges")
{
    std::vector<uint32_t> rows;
    std::vector<RowRange> ranges = {{1, 1}};
    mergeRows(rows, ranges);
    CHECK(ranges.empty());
}

TEST_CASE("mergeRows: one row is one range")
{
    std::vector<uint32_t> rows = {7};
    std::vector<RowRange> ranges;
    mergeRows(rows, ranges);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].first == 7);
    CHECK(ranges[0].count == 1);
}

TEST_CASE("mergeRows: unsorted neighbours merge into one range")
{
    std::vector<uint32_t> rows = {5, 3, 4, 6};
    std::vector<RowRange> ranges;
    mergeRows(rows, ranges);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].first == 3);
    CHECK(ranges[0].count == 4);
}

TEST_CASE("mergeRows: gaps split ranges")
{
    std::vector<uint32_t> rows = {10, 0, 1, 12, 11, 20};
    std::vector<RowRange> ranges;
    mergeRows(rows, ranges);
    REQUIRE(ranges.size() == 3);
    CHECK(ranges[0].first == 0);
    CHECK(ranges[0].count == 2);
    CHECK(ranges[1].first == 10);
    CHECK(ranges[1].count == 3);
    CHECK(ranges[2].first == 20);
    CHECK(ranges[2].count == 1);
}

TEST_CASE("mergeRows: duplicates count once")
{
    std::vector<uint32_t> rows = {2, 2, 3, 3, 3, 9, 9};
    std::vector<RowRange> ranges;
    mergeRows(rows, ranges);
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0].first == 2);
    CHECK(ranges[0].count == 2);
    CHECK(ranges[1].first == 9);
    CHECK(ranges[1].count == 1);
}
