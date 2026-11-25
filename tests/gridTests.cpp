#include "renderer/grid.h"

#include "doctest/doctest.h"
#include "glm/gtc/quaternion.hpp"

#include <cmath>
#include <cstdint>

namespace
{
    // Where the lines of a level fall relative to the camera, as the shader computes it: the fraction of a level-k cell
    // at a point q (world units, relative to the camera) along the plane's X axis.
    double getLevelFraction(const rasm::gfx::GridData &data, uint32_t level, double q)
    {
        uint64_t size = 1;
        for (uint32_t i = 0; i < level; ++i)
            size *= static_cast<uint64_t>(data.subdivisions);
        const double uv = (static_cast<double>(data.residueX % size) + data.fractionX + q / data.cellSize) / static_cast<double>(size);
        return uv - std::floor(uv);
    }
}

TEST_CASE("a grid's residues put every level's lines where doubles put them, far from the origin too")
{
    const rasm::Grid grid = {.enabled = true, .origin = glm::dvec3(12.5, 0.0, -3.0), .cellSize = 0.01};
    for (const glm::dvec3 camera : {glm::dvec3(1.234, 7.0, 5.678), glm::dvec3(4197000.123, 168000.0, 4786000.456), glm::dvec3(-6371000.789, 2.0, 0.25)})
    {
        const rasm::gfx::GridData data = rasm::makeGridData(grid, camera);
        CHECK(data.levelCount == 9);
        CHECK(data.period == 1000000000u);
        CHECK(data.height == doctest::Approx(camera.y));
        for (uint32_t level = 0; level <= 6; ++level)
        {
            const double size = 0.01 * std::pow(10.0, level);
            for (const double q : {0.0, 0.37, -2.5})
            {
                // The direct computation in doubles: the point's X on the plane, in cells of this level.
                const double cells = (camera.x - grid.origin.x + q) / size;
                const double expected = cells - std::floor(cells);
                const double fraction = getLevelFraction(data, level, q);
                const double error = std::min(std::abs(fraction - expected), 1.0 - std::abs(fraction - expected));
                CHECK(error < 1e-4);
            }
        }
    }
}

TEST_CASE("a grid's plane follows its orientation, and a disabled grid has no levels")
{
    const glm::quat front = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const rasm::gfx::GridData data = rasm::makeGridData({.enabled = true, .origin = glm::dvec3(0.0, 0.0, -3.0), .orientation = front}, glm::dvec3(0.0, 1.0, 20.0));
    CHECK(data.normal.z == doctest::Approx(1.0f));
    CHECK(data.axisZ.y == doctest::Approx(-1.0f));
    CHECK(data.height == doctest::Approx(23.0f));
    CHECK(data.originCells.y == doctest::Approx(-1.0f));
    CHECK(rasm::makeGridData({}, glm::dvec3(0.0)).levelCount == 0);
    CHECK(rasm::makeGridData({.enabled = true, .axes = false}, glm::dvec3(0.0)).xAxisColor.w == 0.0f);
}

TEST_CASE("a grid with a bad value is refused")
{
    CHECK(rasm::checkGrid({}));
    CHECK(!rasm::checkGrid({.cellSize = 0.0}));
    CHECK(!rasm::checkGrid({.cellSize = NAN}));
    CHECK(!rasm::checkGrid({.subdivisions = 1}));
    CHECK(!rasm::checkGrid({.subdivisions = 1001}));
    CHECK(!rasm::checkGrid({.extent = glm::vec2(-1.0f, 0.0f)}));
    CHECK(!rasm::checkGrid({.orientation = glm::quat(0.0f, 0.0f, 0.0f, 0.0f)}));
    CHECK(!rasm::checkGrid({.lineWidth = INFINITY}));
}
