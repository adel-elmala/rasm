#include "renderer/taa.h"

#include "doctest/doctest.h"

#include "glm/gtc/quaternion.hpp"

#include <cmath>
#include <cstdint>

TEST_CASE("the TAA jitter is Halton (2, 3) around the pixel's center, repeating every 8 frames")
{
    // Halton index 1: 1/2 and 1/3; index 2: 1/4 and 2/3; index 8: 1/16 and 8/9.
    CHECK(rasm::getTaaJitter(0).x == doctest::Approx(0.0f));
    CHECK(rasm::getTaaJitter(0).y == doctest::Approx(1.0f / 3.0f - 0.5f));
    CHECK(rasm::getTaaJitter(1).x == doctest::Approx(-0.25f));
    CHECK(rasm::getTaaJitter(1).y == doctest::Approx(2.0f / 3.0f - 0.5f));
    CHECK(rasm::getTaaJitter(7).x == doctest::Approx(1.0f / 16.0f - 0.5f));
    CHECK(rasm::getTaaJitter(7).y == doctest::Approx(8.0f / 9.0f - 0.5f));
    for (uint32_t frame = 0; frame < rasm::TAA_JITTER_PHASES; ++frame)
    {
        const glm::vec2 jitter = rasm::getTaaJitter(frame);
        CHECK(jitter == rasm::getTaaJitter(frame + rasm::TAA_JITTER_PHASES));
        CHECK(std::abs(jitter.x) < 0.5f);
        CHECK(std::abs(jitter.y) < 0.5f);
    }
}

TEST_CASE("the jitter's 8 offsets average near the pixel's center")
{
    glm::vec2 sum(0.0f);
    for (uint32_t frame = 0; frame < rasm::TAA_JITTER_PHASES; ++frame)
        sum += rasm::getTaaJitter(frame);
    CHECK(std::abs(sum.x / 8.0f) < 0.07f);
    CHECK(std::abs(sum.y / 8.0f) < 0.07f);
}

TEST_CASE("a camera jump is a turn above 30 degrees or a move above the jump distance")
{
    const glm::quat still(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::vec3 origin(0.0f);
    CHECK_FALSE(rasm::isCameraJump(origin, still, origin, still, 10.0f));
    CHECK_FALSE(rasm::isCameraJump(origin, still, glm::vec3(0.0f, 0.0f, 9.0f), still, 10.0f));
    CHECK(rasm::isCameraJump(origin, still, glm::vec3(0.0f, 0.0f, 11.0f), still, 10.0f));
    CHECK_FALSE(rasm::isCameraJump(origin, still, origin, glm::angleAxis(glm::radians(25.0f), glm::vec3(0.0f, 1.0f, 0.0f)), 10.0f));
    CHECK(rasm::isCameraJump(origin, still, origin, glm::angleAxis(glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f)), 10.0f));
    // q and -q are the same turn.
    CHECK_FALSE(rasm::isCameraJump(origin, still, origin, -still, 10.0f));
}
