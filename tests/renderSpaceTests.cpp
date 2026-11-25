#include "renderer/renderSpace.h"

#include "doctest/doctest.h"
#include "glm/gtc/matrix_transform.hpp"

#include <algorithm>
#include <cmath>

namespace
{
    // What toRenderSpace does on the GPU, in floats: each part on its own, the high parts first.
    glm::vec3 subtractOnGpu(const rasm::SplitPosition &a, const rasm::SplitPosition &b)
    {
        const glm::vec3 high = a.high - b.high;
        const glm::vec3 low = a.low - b.low;
        return high + low;
    }

    // The farthest that points up to 20 m from a camera `distance` metres from the world's origin land from where they
    // should, over 256 cameras and points.
    double getRenderError(double distance)
    {
        double error = 0.0;
        uint32_t seed = 12345;
        const auto next = [&seed]()
        {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24) * 2.0 - 1.0;
        };
        for (int i = 0; i < 256; ++i)
        {
            const glm::dvec3 camera = glm::normalize(glm::dvec3(next(), next(), next())) * distance;
            const glm::dvec3 offset = glm::dvec3(next(), next(), next()) * 20.0;
            const glm::vec3 render = subtractOnGpu(rasm::splitPosition(camera + offset), rasm::splitPosition(camera));
            error = std::max(error, glm::length(glm::dvec3(render) - offset));
        }
        return error;
    }

    double getFloatError(double distance, const glm::dvec3 &offset)
    {
        const glm::dvec3 camera = glm::normalize(glm::dvec3(0.66, 0.03, 0.75)) * distance;
        const glm::vec3 render = glm::vec3(camera + offset) - glm::vec3(camera);
        return glm::length(glm::dvec3(render) - offset);
    }
}

TEST_CASE("A split position keeps the double's precision")
{
    for (double distance : {1.0, 1e3, 6.4e6, 1e8})
    {
        const glm::dvec3 position = glm::dvec3(1.0, -0.7, 0.3) * distance + glm::dvec3(0.123456789);
        const rasm::SplitPosition split = rasm::splitPosition(position);
        const glm::dvec3 back = glm::dvec3(split.high) + glm::dvec3(split.low);
        CHECK(glm::length(back - position) <= 1e-14 * distance + 1e-15);
    }
}

// Two floats hold about 48 bits: the error is about 2^-48 of the distance from the world's origin, plus the float
// rounding of the result (about 2 micrometres at 20 m). On the Earth (6.4e6 m) the split adds nothing to that; at 1e10 m
// (70 times the Earth's distance from the Sun) it still adds under 0.1 mm; by 1e12 m it adds millimetres, and such a
// world would need a double origin of its own per region.
TEST_CASE("Render space is precise near the camera, and where it stops being exact")
{
    const double floatRounding = 4e-6;
    for (double distance : {0.0, 6.4e6, 1e8, 1e9, 1e10, 1e12})
    {
        const double error = getRenderError(distance);
        MESSAGE("camera " << distance << " m from the origin: points up to 20 m away land within " << error << " m");
        CHECK(error <= std::ldexp(distance, -47) + floatRounding);
    }
    CHECK(getRenderError(6.4e6) < 1e-5);
    CHECK(getRenderError(1e10) < 1e-4);
    CHECK(getRenderError(1e12) > 1e-4);
    // A float alone is off by up to half a step: 0.25 m on the Earth.
    CHECK(getFloatError(6.4e6, glm::dvec3(3.123456, -1.5, 12.25)) > 0.05);
}

TEST_CASE("Instance rows keep the translation as two floats, and the motion as a small difference")
{
    const glm::dvec3 position(4197000.25, 168000.5, 4786000.75);
    const glm::dmat4 world = glm::translate(glm::dmat4(1.0), position) * glm::dmat4(glm::rotate(glm::mat4(1.0f), 0.4f, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::dmat4 previous = glm::translate(glm::dmat4(1.0), glm::dvec3(-0.02, 0.0, 0.01)) * world;
    glm::vec4 rows[3];
    glm::vec3 low;
    rasm::writeWorldRows(world, rows, low);
    glm::vec4 previousRows[3];
    rasm::writePreviousRows(previous, world, previousRows);
    for (int row = 0; row < 3; ++row)
    {
        CHECK(static_cast<double>(rows[row].w) + static_cast<double>(low[row]) == doctest::Approx(position[row]).epsilon(1e-15));
        CHECK(rows[row].x == doctest::Approx(world[0][row]));
        CHECK(previousRows[row].w == doctest::Approx(previous[3][row] - world[3][row]).epsilon(1e-6));
    }
}

TEST_CASE("A view moved into render space has the camera at the origin")
{
    const glm::dvec3 camera(4197000.123, 168000.456, 4786000.789);
    const glm::dmat4 view = glm::lookAt(camera, camera + glm::dvec3(0.3, -0.2, -1.0), glm::dvec3(0.0, 1.0, 0.0));
    const glm::mat4 render = rasm::toRenderMatrix(view, camera);
    CHECK(glm::length(glm::vec3(render[3])) < 1e-6f);
    // A point 2 m ahead is where the double view puts it.
    const glm::dvec3 point = camera + glm::dvec3(0.3, -0.2, -1.0) * 2.0;
    const glm::vec3 expected = glm::vec3(view * glm::dvec4(point, 1.0));
    const glm::vec3 drawn = glm::vec3(render * glm::vec4(rasm::toRenderPosition(point, camera), 1.0f));
    CHECK(glm::length(drawn - expected) < 1e-5f);
}

TEST_CASE("A plane moved to another origin keeps the points it holds")
{
    const glm::dvec3 from(4197000.0, 168000.0, 4786000.0);
    const glm::dvec3 to = from + glm::dvec3(12.5, -3.0, 40.25);
    const glm::vec4 plane(glm::normalize(glm::vec3(0.2f, 0.9f, -0.3f)), 1.5f);
    const glm::dvec3 world = from + glm::dvec3(4.0, -2.0, 7.0);
    const float before = glm::dot(glm::vec3(plane), rasm::toRenderPosition(world, from)) + plane.w;
    const glm::vec4 moved = rasm::moveRenderPlane(plane, from, to);
    const float after = glm::dot(glm::vec3(moved), rasm::toRenderPosition(world, to)) + moved.w;
    CHECK(after == doctest::Approx(before).epsilon(1e-5));
    CHECK(rasm::toWorldPosition(rasm::toRenderPosition(world, to), to).x == doctest::Approx(world.x).epsilon(1e-15));
}
