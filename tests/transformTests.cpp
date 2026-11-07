#include "rasm/transform.h"

#include "doctest/doctest.h"

#include "glm/gtc/constants.hpp"

namespace
{
    bool isNear(const glm::vec3 &a, const glm::vec3 &b)
    {
        return glm::length(a - b) < 1e-5f;
    }
}

// glm::quat() is all zeros, which only looks like identity: q * v returns v, but q * anything stays zero (B22).
TEST_CASE("a default transform has an identity rotation")
{
    rasm::Transform transform;

    CHECK(transform.rotation.w == doctest::Approx(1.0f));
    CHECK(transform.rotation.x == doctest::Approx(0.0f));
    CHECK(transform.rotation.y == doctest::Approx(0.0f));
    CHECK(transform.rotation.z == doctest::Approx(0.0f));
}

TEST_CASE("rotate turns around the given axis")
{
    rasm::Transform transform;

    transform.rotate(glm::vec3(0.0f, 1.0f, 0.0f), glm::half_pi<float>());

    CHECK(isNear(transform.rotation * glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)));
}

TEST_CASE("rotate normalizes the axis")
{
    rasm::Transform unitAxis;
    rasm::Transform longAxis;

    unitAxis.rotate(glm::vec3(0.0f, 1.0f, 0.0f), 1.0f);
    longAxis.rotate(glm::vec3(0.0f, 5.0f, 0.0f), 1.0f);

    CHECK(isNear(unitAxis.rotation * glm::vec3(1.0f), longAxis.rotation * glm::vec3(1.0f)));
}

TEST_CASE("rotate composes in world space")
{
    rasm::Transform transform;

    transform.rotate(glm::vec3(0.0f, 1.0f, 0.0f), glm::half_pi<float>());
    transform.rotate(glm::vec3(1.0f, 0.0f, 0.0f), glm::half_pi<float>());

    CHECK(isNear(transform.rotation * glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
}

TEST_CASE("rotate ignores a zero axis")
{
    rasm::Transform transform;

    transform.rotate(glm::vec3(0.0f), 1.0f);

    CHECK(transform.rotation == glm::identity<glm::quat>());
}

TEST_CASE("toMatrix scales, then rotates, then translates")
{
    rasm::Transform transform;
    transform.position = glm::vec3(10.0f, 0.0f, 0.0f);
    transform.scale = glm::vec3(2.0f);
    transform.rotate(glm::vec3(0.0f, 0.0f, 1.0f), glm::half_pi<float>());

    // (1, 0, 0) scaled to (2, 0, 0), turned 90 degrees around Z to (0, 2, 0), then moved by (10, 0, 0).
    const glm::vec4 point = transform.toMatrix() * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    CHECK(isNear(glm::vec3(point), glm::vec3(10.0f, 2.0f, 0.0f)));
    CHECK(point.w == doctest::Approx(1.0f));
}
