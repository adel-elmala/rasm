#include "renderer/bloom.h"

#include "doctest/doctest.h"

#include <cstdint>

namespace
{
    uint32_t levelCount(glm::uvec2 sourceSize)
    {
        glm::uvec2 sizes[rasm::BLOOM_LEVELS];
        for (uint32_t i = 0; i < rasm::BLOOM_LEVELS; ++i)
            sizes[i] = rasm::getBloomLevelSize(sourceSize, i + 1);
        return rasm::getBloomLevelCount(sizes, rasm::BLOOM_LEVELS);
    }
}

TEST_CASE("bloom levels halve the source and never reach zero")
{
    CHECK(rasm::getBloomLevelSize({1920, 1080}, 1) == glm::uvec2(960, 540));
    CHECK(rasm::getBloomLevelSize({1920, 1080}, 6) == glm::uvec2(30, 16));
    CHECK(rasm::getBloomLevelSize({256, 144}, 6) == glm::uvec2(4, 2));
    CHECK(rasm::getBloomLevelSize({1, 1}, 1) == glm::uvec2(1, 1));
    CHECK(rasm::getBloomLevelSize({100, 3}, 3) == glm::uvec2(12, 1));
}

TEST_CASE("the bloom chain stops at the last level of at least 2x2")
{
    CHECK(levelCount({1920, 1080}) == 6);
    // A small render-to-texture view still has every level.
    CHECK(levelCount({256, 144}) == 6);
    // 50x15, 25x7, 12x3, then 6x1.
    CHECK(levelCount({100, 30}) == 3);
    // A window made very small keeps level 1, which only copies the image down.
    CHECK(levelCount({3, 3}) == 1);
    CHECK(levelCount({1, 1}) == 1);
}

TEST_CASE("the level count reads the levels' sizes as they are")
{
    // A swapchain-scaled chain rounds each level from the window, so a level may be a pixel off the halved size.
    const glm::uvec2 sizes[rasm::BLOOM_LEVELS] = {{64, 36}, {32, 18}, {16, 9}, {8, 5}, {4, 2}, {2, 1}};
    CHECK(rasm::getBloomLevelCount(sizes, rasm::BLOOM_LEVELS) == 5);
    CHECK(rasm::getBloomLevelCount(sizes, 0) == 0);
}
