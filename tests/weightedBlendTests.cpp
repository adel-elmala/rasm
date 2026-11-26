#include "renderer/weightedBlend.h"

#include "doctest/doctest.h"

#include <cmath>
#include <cstdint>

using namespace rasm;

TEST_CASE("depth bits keep the order of the depths, also below 0, and come back")
{
    const float depths[] = {-1000.0f, -2.5f, -0.0001f, 0.0f, 0.0001f, 1.0f, 3.5f, 1.0e6f};
    for (uint32_t i = 0; i + 1 < sizeof(depths) / sizeof(depths[0]); ++i)
        CHECK(getDepthBits(depths[i]) < getDepthBits(depths[i + 1]));
    for (float depth : depths)
        CHECK(fromDepthBits(getDepthBits(depth)) == depth);
    // The cull's empty words, nearest 0xFFFFFFFF and farthest 0, lose to every depth's.
    CHECK(getDepthBits(1.0e30f) < 0xFFFFFFFFu);
    CHECK(getDepthBits(-1.0e30f) > 0u);
}

TEST_CASE("the weight span starts at the first measure, snapped to sqrt(2) steps with perspective")
{
    WeightedRange range;
    CHECK(getWeightedDepths(range).x == 0.0f);
    stepWeightedRange(range, 3.0f, 10.0f, false, 1.0f / 60.0f);
    REQUIRE(range.valid);
    const glm::vec2 depths = getWeightedDepths(range);
    CHECK(depths.x <= 3.0f);
    CHECK(depths.x > 3.0f / std::sqrt(2.0f));
    CHECK(depths.y >= 10.0f);
    CHECK(depths.y < 10.0f * std::sqrt(2.0f));
    // Nothing measured: the span stays.
    stepWeightedRange(range, 1.0f, 0.0f, false, 1.0f / 60.0f);
    CHECK(getWeightedDepths(range) == depths);
}

TEST_CASE("a draw that comes and goes near the span's ends leaves the weights as they are")
{
    WeightedRange range;
    stepWeightedRange(range, 4.0f, 16.0f, false, 1.0f / 60.0f);
    const glm::vec2 depths = getWeightedDepths(range);
    for (uint32_t frame = 0; frame < 120; ++frame)
    {
        const float far = frame % 2 == 0 ? 16.0f : 12.0f;
        const float near = frame % 3 == 0 ? 4.0f : 5.0f;
        stepWeightedRange(range, near, far, false, 1.0f / 60.0f);
    }
    CHECK(getWeightedDepths(range) == depths);
}

TEST_CASE("a far draw coming in moves the span out smoothly, and one going away brings it back")
{
    WeightedRange range;
    stepWeightedRange(range, 4.0f, 16.0f, false, 1.0f / 60.0f);
    const float far = getWeightedDepths(range).y;
    stepWeightedRange(range, 4.0f, 256.0f, false, 1.0f / 60.0f);
    const float first = getWeightedDepths(range).y;
    CHECK(first > far);
    CHECK(first < 64.0f);
    float previous = first;
    for (uint32_t frame = 0; frame < 120; ++frame)
    {
        stepWeightedRange(range, 4.0f, 256.0f, false, 1.0f / 60.0f);
        const float now = getWeightedDepths(range).y;
        CHECK(now >= previous);
        // A frame moves the log of the end by at most the step's share: no jump.
        CHECK(std::log2(now / previous) < 0.6f);
        previous = now;
    }
    CHECK(previous == doctest::Approx(256.0f).epsilon(0.01));
    for (uint32_t frame = 0; frame < 240; ++frame)
        stepWeightedRange(range, 4.0f, 16.0f, false, 1.0f / 60.0f);
    CHECK(getWeightedDepths(range).y == doctest::Approx(16.0f).epsilon(0.01));
}

TEST_CASE("an orthographic span is linear, may start below 0, and starts again when the projection changes")
{
    WeightedRange range;
    stepWeightedRange(range, -5.0f, 15.0f, true, 1.0f / 60.0f);
    REQUIRE(range.valid);
    CHECK(range.orthographic);
    const glm::vec2 depths = getWeightedDepths(range);
    CHECK(depths.x <= -5.0f);
    CHECK(depths.y >= 15.0f);
    CHECK(depths.y - depths.x < 25.0f);
    stepWeightedRange(range, -4.0f, 14.0f, true, 1.0f / 60.0f);
    CHECK(getWeightedDepths(range) == depths);
    stepWeightedRange(range, 2.0f, 8.0f, false, 1.0f / 60.0f);
    CHECK(!range.orthographic);
    CHECK(getWeightedDepths(range).x > 0.0f);
}
