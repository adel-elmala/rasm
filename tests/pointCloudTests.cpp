#include "rasm/pointCloud.h"
#include "assets/meshBuild.h"
#include "renderer/uberMaterialTypes.h"

#include "doctest/doctest.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
    // A chunk's point k as the shader reads it: its box position and its scalar, from the words the build made.
    struct DecodedPoint
    {
        glm::vec3 position;
        float scalar;
        uint32_t color;
    };

    rasm::gfx::PointChunkHeader getHeader(const rasm::MeshBuild &build)
    {
        rasm::gfx::PointChunkHeader header{};
        std::memcpy(&header, build.data->raw.points.data(), sizeof(header));
        return header;
    }

    DecodedPoint decode(const rasm::MeshBuild &build, uint32_t k)
    {
        const rasm::gfx::PointChunkHeader header = getHeader(build);
        const std::vector<uint32_t> &words = build.data->raw.points;
        const uint32_t a = words[rasm::gfx::POINT_CHUNK_HEADER_WORDS + 2 * k];
        const uint32_t b = words[rasm::gfx::POINT_CHUNK_HEADER_WORDS + 2 * k + 1];
        const glm::vec3 steps(static_cast<float>(a & 0xFFFF), static_cast<float>(a >> 16), static_cast<float>(b & 0xFFFF));
        return {.position = header.boxMin + steps * header.boxStep,
                .scalar = header.scalarMin + static_cast<float>(b >> 16) * header.scalarStep,
                .color = (header.flags & rasm::gfx::POINT_CHUNK_COLORS) != 0 ? words[header.colorOffset + k] : 0u};
    }

    // 256 x 256 points on a plane, a metre apart: the largest chunk.
    std::vector<glm::vec3> makeGrid()
    {
        std::vector<glm::vec3> points;
        for (int z = 0; z < 256; ++z)
        {
            for (int x = 0; x < 256; ++x)
                points.emplace_back(static_cast<float>(x), 0.0f, static_cast<float>(z));
        }
        return points;
    }
}

TEST_CASE("a point cloud chunk refuses no points, too many points and points that are not finite")
{
    const std::vector<glm::vec3> grid = makeGrid();
    CHECK_FALSE(rasm::buildPointCloudChunk({.positions = grid.data(), .count = 0}).isValid());
    CHECK_FALSE(rasm::buildPointCloudChunk({.positions = nullptr, .count = 4}).isValid());
    std::vector<glm::vec3> tooMany(rasm::MAX_POINT_CHUNK_POINTS + 1, glm::vec3(1.0f));
    CHECK_FALSE(rasm::buildPointCloudChunk({.positions = tooMany.data(), .count = static_cast<uint32_t>(tooMany.size())}).isValid());
    std::vector<glm::vec3> nan = {glm::vec3(0.0f), glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f)};
    CHECK_FALSE(rasm::buildPointCloudChunk({.positions = nan.data(), .count = 2}).isValid());
    const float scalars[2] = {1.0f, std::numeric_limits<float>::infinity()};
    CHECK_FALSE(rasm::buildPointCloudChunk({.positions = grid.data(), .scalars = scalars, .count = 2}).isValid());
}

TEST_CASE("a point cloud chunk keeps every point, in 16-bit steps of its box, with its scalar and color")
{
    std::vector<glm::vec3> positions;
    std::vector<float> scalars;
    std::vector<uint32_t> colors;
    for (uint32_t i = 0; i < 1000; ++i)
    {
        const float t = static_cast<float>(i);
        positions.emplace_back(std::sin(t) * 40.0f, std::cos(t * 0.7f) * 3.0f, t * 0.05f - 20.0f);
        scalars.push_back(t * 0.25f - 30.0f);
        colors.push_back(i * 2654435761u);
    }
    const rasm::MeshBuild build = rasm::buildPointCloudChunk({.positions = positions.data(), .colors = colors.data(), .scalars = scalars.data(), .count = 1000});
    REQUIRE(build.isValid());
    const rasm::gfx::PointChunkHeader header = getHeader(build);
    CHECK(header.pointCount == 1000);
    CHECK(header.flags == (rasm::gfx::POINT_CHUNK_COLORS | rasm::gfx::POINT_CHUNK_SCALARS));
    CHECK(header.colorOffset == rasm::gfx::POINT_CHUNK_HEADER_WORDS + 2000);
    // 8 B a point, 4 more for its color, the header, and an even number of words (the pool's 8 B units).
    CHECK(build.data->raw.points.size() == rasm::gfx::POINT_CHUNK_HEADER_WORDS + 3000);
    CHECK(build.data->raw.points.size() % 2 == 0);
    CHECK(build.data->raw.pointCount == 1000);

    const std::vector<uint16_t> &order = build.data->pointOrder;
    REQUIRE(order.size() == 1000);
    std::vector<bool> seen(1000, false);
    for (uint32_t k = 0; k < 1000; ++k)
    {
        const uint32_t i = order[k];
        REQUIRE(i < 1000);
        CHECK_FALSE(seen[i]);
        seen[i] = true;
        const DecodedPoint point = decode(build, k);
        // Half a step of the box, and of the scalars' range, at most.
        CHECK(std::abs(point.position.x - positions[i].x) <= 0.5f * header.boxStep.x + 1e-5f);
        CHECK(std::abs(point.position.y - positions[i].y) <= 0.5f * header.boxStep.y + 1e-5f);
        CHECK(std::abs(point.position.z - positions[i].z) <= 0.5f * header.boxStep.z + 1e-5f);
        CHECK(std::abs(point.scalar - scalars[i]) <= 0.5f * header.scalarStep + 1e-4f);
        CHECK(point.color == colors[i]);
    }
}

TEST_CASE("every prefix of a point cloud chunk's order is an even sample of it")
{
    const std::vector<glm::vec3> grid = makeGrid();
    const rasm::MeshBuild build = rasm::buildPointCloudChunk({.positions = grid.data(), .count = static_cast<uint32_t>(grid.size())});
    REQUIRE(build.isValid());
    CHECK(getHeader(build).flags == 0);
    CHECK(build.data->raw.points.size() == rasm::gfx::POINT_CHUNK_HEADER_WORDS + 2 * grid.size());
    // The first 4^k points put one point in each cell of a 2^k x 2^k split of the grid.
    for (uint32_t level = 1; level <= 6; ++level)
    {
        const uint32_t side = 1u << level;
        const uint32_t cell = 256 / side;
        std::vector<uint32_t> counts(side * side, 0);
        for (uint32_t k = 0; k < side * side; ++k)
        {
            const glm::vec3 &p = grid[build.data->pointOrder[k]];
            ++counts[static_cast<uint32_t>(p.z) / cell * side + static_cast<uint32_t>(p.x) / cell];
        }
        uint32_t once = 0;
        for (uint32_t count : counts)
            once += count == 1 ? 1 : 0;
        CHECK(once == side * side);
    }
}

TEST_CASE("a point cloud chunk of one point, or of points in a plane, has steps of 0 on its flat axes")
{
    const glm::vec3 one(3.0f, -2.0f, 7.0f);
    const rasm::MeshBuild single = rasm::buildPointCloudChunk({.positions = &one, .count = 1});
    REQUIRE(single.isValid());
    CHECK(getHeader(single).boxStep == glm::vec3(0.0f));
    CHECK(decode(single, 0).position == one);
}
