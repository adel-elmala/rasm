#include "rasm/terrain.h"
#include "scene/terrain.h"

#include "doctest/doctest.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
    constexpr uint32_t CELLS = 8;
    constexpr uint32_t WIDTH = CELLS + 3;
    constexpr double STEP = 1.0 / 16.0; // metres per R16_UNORM step: a power of two

    double groundHeight(double x, double z)
    {
        return 120.0 * std::sin(x / 37.0) * std::cos(z / 53.0) + 0.37 * x - 0.21 * z;
    }

    // A tile's samples, as a server makes them: heights put on the world's grid of STEP first, then stored from the
    // tile's offset, a multiple of STEP. The border reaches one cell past each edge.
    struct TileSamples
    {
        std::vector<uint16_t> values;
        rasm::TerrainMaterial terrain;
    };

    TileSamples makeTile(double x0, double z0, double size)
    {
        const double cell = size / CELLS;
        std::vector<int64_t> steps(WIDTH * WIDTH);
        int64_t lowest = INT64_MAX;
        for (uint32_t j = 0; j < WIDTH; ++j)
        {
            for (uint32_t i = 0; i < WIDTH; ++i)
            {
                const auto step = static_cast<int64_t>(std::llround(groundHeight(x0 + (static_cast<double>(i) - 1.0) * cell, z0 + (static_cast<double>(j) - 1.0) * cell) / STEP));
                steps[j * WIDTH + i] = step;
                lowest = std::min(lowest, step);
            }
        }
        TileSamples tile;
        tile.values.resize(WIDTH * WIDTH);
        int64_t highest = lowest;
        for (size_t k = 0; k < steps.size(); ++k)
        {
            tile.values[k] = static_cast<uint16_t>(steps[k] - lowest);
            highest = std::max(highest, steps[k]);
        }
        tile.terrain = {.heightScale = static_cast<float>(STEP),
                        .heightOffset = static_cast<float>(static_cast<double>(lowest) * STEP),
                        .minHeight = static_cast<float>(static_cast<double>(lowest) * STEP),
                        .maxHeight = static_cast<float>(static_cast<double>(highest) * STEP)};
        return tile;
    }

    rasm::TerrainHeights heightsOf(const TileSamples &tile)
    {
        return {.unorm16 = tile.values.data(), .width = WIDTH};
    }
}

TEST_CASE("the terrain grid is the unit box, its cells face up and its skirt faces out")
{
    std::vector<rasm::Vertex> vertices;
    std::vector<uint32_t> indices;
    rasm::buildTerrainGrid(CELLS, vertices, indices);
    REQUIRE(vertices.size() == (CELLS + 1) * (CELLS + 1) + 4 * (CELLS + 1));
    REQUIRE(indices.size() == CELLS * CELLS * 6 + 4 * CELLS * 6);
    glm::vec3 low(1e9f);
    glm::vec3 high(-1e9f);
    for (const rasm::Vertex &vertex : vertices)
    {
        low = glm::min(low, vertex.pos);
        high = glm::max(high, vertex.pos);
        CHECK(vertex.uv[1].x == doctest::Approx(vertex.uv[0].x * CELLS));
        CHECK(vertex.uv[2].x == static_cast<float>(CELLS));
    }
    CHECK(low == glm::vec3(-0.5f, 0.0f, -0.5f));
    CHECK(high == glm::vec3(0.5f, 1.0f, 0.5f));
    for (size_t t = 0; t < indices.size(); t += 3)
    {
        const glm::vec3 a = vertices[indices[t]].pos;
        const glm::vec3 b = vertices[indices[t + 1]].pos;
        const glm::vec3 c = vertices[indices[t + 2]].pos;
        const glm::vec3 normal = glm::cross(b - a, c - a);
        const glm::vec3 middle = (a + b + c) / 3.0f;
        if (t < CELLS * CELLS * 6)
            CHECK(normal.y > 0.0f);
        else
            CHECK(glm::dot(normal, glm::vec3(middle.x, 0.0f, middle.z)) > 0.0f);
    }
}

TEST_CASE("a terrain height follows the drawn triangles")
{
    // A plane is drawn as itself, whatever the split.
    std::vector<float> plane(WIDTH * WIDTH);
    for (uint32_t j = 0; j < WIDTH; ++j)
        for (uint32_t i = 0; i < WIDTH; ++i)
            plane[j * WIDTH + i] = 2.0f * static_cast<float>(i) + 3.0f * static_cast<float>(j);
    const rasm::TerrainHeights heights{.floats = plane.data(), .width = WIDTH};
    const rasm::TerrainMaterial terrain{.heightScale = 0.5f, .heightOffset = 10.0f};
    for (double v = 0.0; v <= 1.0; v += 0.0625)
        for (double u = 0.0; u <= 1.0; u += 0.0625)
            CHECK(rasm::sampleTerrainHeight(heights, terrain, u, v) == doctest::Approx(10.0 + 0.5 * (2.0 * (u * CELLS + 1.0) + 3.0 * (v * CELLS + 1.0))));

    // A bump at one point: inside a cell the height is the triangle's, split from (0, 1) to (1, 0).
    std::vector<float> bump(WIDTH * WIDTH, 0.0f);
    bump[2 * WIDTH + 2] = 4.0f; // grid point (1, 1)
    const rasm::TerrainHeights bumped{.floats = bump.data(), .width = WIDTH};
    const rasm::TerrainMaterial plain{};
    const double cell = 1.0 / CELLS;
    CHECK(rasm::sampleTerrainHeight(bumped, plain, cell, cell) == 4.0);
    // Cell (0, 0): (1, 1) is its far corner, on the second triangle; its first triangle does not reach it.
    CHECK(rasm::sampleTerrainHeight(bumped, plain, 0.25 * cell, 0.25 * cell) == 0.0);
    CHECK(rasm::sampleTerrainHeight(bumped, plain, 0.75 * cell, 0.75 * cell) == doctest::Approx(2.0));
    // Cell (1, 0): (1, 1) is its (0, 1) corner, on both triangles' diagonal.
    CHECK(rasm::sampleTerrainHeight(bumped, plain, 1.5 * cell, 0.5 * cell) == doctest::Approx(2.0));
}

TEST_CASE("neighbouring terrain tiles have bit-equal heights on their shared edge")
{
    const double size = 256.0;
    const TileSamples west = makeTile(0.0, 0.0, size);
    const TileSamples east = makeTile(size, 0.0, size);
    const TileSamples south = makeTile(0.0, size, size);
    // Different offsets, the same heights.
    REQUIRE(west.terrain.heightOffset != east.terrain.heightOffset);
    for (uint32_t k = 0; k <= CELLS; ++k)
    {
        const double along = static_cast<double>(k) / CELLS;
        const double a = rasm::sampleTerrainHeight(heightsOf(west), west.terrain, 1.0, along);
        const double b = rasm::sampleTerrainHeight(heightsOf(east), east.terrain, 0.0, along);
        CHECK(a == b);
        const double c = rasm::sampleTerrainHeight(heightsOf(west), west.terrain, along, 1.0);
        const double d = rasm::sampleTerrainHeight(heightsOf(south), south.terrain, along, 0.0);
        CHECK(c == d);
    }
    // The border samples the shader takes the edge normals from are the neighbour's samples next to the edge.
    for (uint32_t j = 0; j < WIDTH; ++j)
    {
        const float westBorder = west.terrain.heightOffset + west.terrain.heightScale * static_cast<float>(west.values[j * WIDTH + WIDTH - 1]);
        const float eastInner = east.terrain.heightOffset + east.terrain.heightScale * static_cast<float>(east.values[j * WIDTH + 2]);
        CHECK(westBorder == eastInner);
    }
}

TEST_CASE("a terrain tile's transform holds its surface and skirt, and getTerrainPosition is the drawn point")
{
    const TileSamples samples = makeTile(1000.0, -2000.0, 512.0);
    const rasm::TerrainTileDesc tile{.center = glm::dvec3(4.0e6, 3.0e6, 3.0e6),
                                     .orientation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f))),
                                     .size = 512.0,
                                     .terrain = {.heightScale = samples.terrain.heightScale,
                                                 .heightOffset = samples.terrain.heightOffset,
                                                 .minHeight = samples.terrain.minHeight,
                                                 .maxHeight = samples.terrain.maxHeight,
                                                 .skirt = 7.0f,
                                                 .sphereRadius = 6.371e6}};
    const rasm::Transform transform = rasm::getTerrainTransform(tile);
    const glm::dmat4 world = transform.toMatrix();
    const double corner = 0.5 * tile.size;
    const double floor = tile.terrain.minHeight - tile.terrain.skirt - (2.0 * corner * corner) / (2.0 * tile.terrain.sphereRadius);
    CHECK(transform.scale.x == doctest::Approx(tile.size));
    CHECK(transform.scale.y == doctest::Approx(tile.terrain.maxHeight - floor));
    for (double v = 0.0; v <= 1.0; v += 0.125)
    {
        for (double u = 0.0; u <= 1.0; u += 0.125)
        {
            // The shader's point: the height in the box, put in the world by the entity's matrix.
            const double x = (u - 0.5) * tile.size;
            const double z = (v - 0.5) * tile.size;
            const double height = rasm::sampleTerrainHeight(heightsOf(samples), tile.terrain, u, v) - (x * x + z * z) / (2.0 * tile.terrain.sphereRadius);
            const glm::dvec3 drawn = glm::dvec3(world * glm::dvec4(u - 0.5, (height - floor) / transform.scale.y, v - 0.5, 1.0));
            CHECK(glm::length(drawn - rasm::getTerrainPosition(tile, heightsOf(samples), u, v)) < 1e-3);
            // Inside the box, the skirt under it too.
            CHECK((height - tile.terrain.skirt - floor) / transform.scale.y >= -1e-6);
            CHECK((height - floor) / transform.scale.y <= 1.0 + 1e-6);
        }
    }
}

TEST_CASE("the curvature's parabola stays within d^4 / 8R^3 of the sphere")
{
    const double radius = 6.371e6;
    for (const double d : {1.0e3, 1.0e4, 1.0e5})
    {
        const double sphere = radius - std::sqrt(radius * radius - d * d);
        const double parabola = d * d / (2.0 * radius);
        CHECK(sphere - parabola == doctest::Approx(std::pow(d, 4.0) / (8.0 * std::pow(radius, 3.0))).epsilon(0.01));
    }
    // 100 km out: about 5 cm.
    CHECK(std::pow(1.0e5, 4.0) / (8.0 * std::pow(radius, 3.0)) == doctest::Approx(0.048).epsilon(0.01));
}
