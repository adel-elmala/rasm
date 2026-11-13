#include "rhi/memoryAliasing.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <vector>

namespace
{
    constexpr uint32_t DEVICE_LOCAL = 0b0011;
    constexpr uint32_t OTHER_TYPES = 0b0100;

    rasm::rhi::AliasRequest request(uint64_t size, uint32_t firstPass, uint32_t lastPass, uint64_t alignment = 256, uint32_t memoryTypeBits = DEVICE_LOCAL)
    {
        return {.size = size, .alignment = alignment, .memoryTypeBits = memoryTypeBits, .firstPass = firstPass, .lastPass = lastPass};
    }

    bool rangesOverlap(const rasm::rhi::AliasPlacement &a, uint64_t sizeA, const rasm::rhi::AliasPlacement &b, uint64_t sizeB)
    {
        return a.heap == b.heap && a.offset < b.offset + sizeB && b.offset < a.offset + sizeA;
    }
}

TEST_CASE("textures whose lifetimes do not overlap share memory")
{
    const rasm::rhi::AliasRequest requests[] = {request(1024, 0, 1), request(1024, 2, 3)};
    rasm::rhi::AliasPlacement placements[2] = {};
    std::vector<rasm::rhi::AliasHeap> heaps;

    rasm::rhi::packAliased(requests, 2, placements, heaps);

    REQUIRE(heaps.size() == 1);
    CHECK(heaps[0].size == 1024);
    CHECK(placements[0].offset == 0);
    CHECK(placements[1].offset == 0);
}

TEST_CASE("textures used by the same pass get separate memory")
{
    // The second starts in the pass where the first ends: that pass reads one and draws the other.
    const rasm::rhi::AliasRequest requests[] = {request(1024, 0, 1), request(1024, 1, 2)};
    rasm::rhi::AliasPlacement placements[2] = {};
    std::vector<rasm::rhi::AliasHeap> heaps;

    rasm::rhi::packAliased(requests, 2, placements, heaps);

    REQUIRE(heaps.size() == 1);
    CHECK(heaps[0].size == 2048);
    CHECK_FALSE(rangesOverlap(placements[0], 1024, placements[1], 1024));
}

TEST_CASE("a small texture fills a gap that a later one left free")
{
    // A and B live together, C only after A: C goes where A was, below B.
    const rasm::rhi::AliasRequest requests[] = {request(4096, 0, 1), request(2048, 0, 3), request(1024, 2, 3)};
    rasm::rhi::AliasPlacement placements[3] = {};
    std::vector<rasm::rhi::AliasHeap> heaps;

    rasm::rhi::packAliased(requests, 3, placements, heaps);

    REQUIRE(heaps.size() == 1);
    CHECK(heaps[0].size == 4096 + 2048);
    CHECK(placements[0].offset == 0);
    CHECK(placements[1].offset == 4096);
    CHECK(placements[2].offset == 0);
}

TEST_CASE("offsets keep each texture's alignment")
{
    const rasm::rhi::AliasRequest requests[] = {request(1000, 0, 0, 256), request(500, 0, 0, 4096)};
    rasm::rhi::AliasPlacement placements[2] = {};
    std::vector<rasm::rhi::AliasHeap> heaps;

    rasm::rhi::packAliased(requests, 2, placements, heaps);

    REQUIRE(heaps.size() == 1);
    CHECK(placements[0].offset == 0);
    CHECK(placements[1].offset == 4096);
    CHECK(heaps[0].alignment == 4096);
    CHECK(heaps[0].size == 4096 + 500);
}

TEST_CASE("textures with other memory types go to another heap")
{
    const rasm::rhi::AliasRequest requests[] = {request(1024, 0, 0), request(1024, 1, 1, 256, OTHER_TYPES)};
    rasm::rhi::AliasPlacement placements[2] = {};
    std::vector<rasm::rhi::AliasHeap> heaps;

    rasm::rhi::packAliased(requests, 2, placements, heaps);

    REQUIRE(heaps.size() == 2);
    CHECK(placements[0].heap != placements[1].heap);
    CHECK(heaps[placements[1].heap].memoryTypeBits == OTHER_TYPES);
}

TEST_CASE("no two overlapping lifetimes share a byte")
{
    std::vector<rasm::rhi::AliasRequest> requests;
    requests.reserve(12);
    for (uint32_t i = 0; i < 12; ++i)
        requests.push_back(request(uint64_t{1024} * (1 + (i * 7) % 5), i % 4, i % 4 + i % 3));
    std::vector<rasm::rhi::AliasPlacement> placements(requests.size());
    std::vector<rasm::rhi::AliasHeap> heaps;

    rasm::rhi::packAliased(requests.data(), static_cast<uint32_t>(requests.size()), placements.data(), heaps);

    for (size_t a = 0; a < requests.size(); ++a)
    {
        CHECK(placements[a].offset + requests[a].size <= heaps[placements[a].heap].size);
        for (size_t b = a + 1; b < requests.size(); ++b)
        {
            const bool together = requests[a].firstPass <= requests[b].lastPass && requests[b].firstPass <= requests[a].lastPass;
            if (together)
                CHECK_FALSE(rangesOverlap(placements[a], requests[a].size, placements[b], requests[b].size));
        }
    }
}
