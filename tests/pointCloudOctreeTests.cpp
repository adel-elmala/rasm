#include "rasm/pointCloudOctree.h"
#include "assets/meshBuild.h"
#include "renderer/uberMaterialTypes.h"

#include "doctest/doctest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    // A rolling ground 400 m across with a few walls, 6 000 km from the origin, about a point a 20 cm.
    std::vector<glm::dvec3> makeGround(uint32_t count)
    {
        std::vector<glm::dvec3> points;
        points.reserve(count);
        uint64_t state = 12345;
        const auto next = [&state]()
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<double>(state >> 11) / static_cast<double>(1ull << 53);
        };
        const glm::dvec3 origin(6.0e6, 1.0e5, -2.0e5);
        for (uint32_t i = 0; i < count; ++i)
        {
            const double x = 400.0 * next();
            const double z = 400.0 * next();
            double y = 5.0 * std::sin(x * 0.02) * std::cos(z * 0.015);
            if (i % 10 == 0)
                y += 12.0 * next();
            points.push_back(origin + glm::dvec3(x, y, z));
        }
        return points;
    }

    float headerSpacing(const rasm::MeshBuild &build)
    {
        rasm::gfx::PointChunkHeader header{};
        std::memcpy(&header, build.data->raw.points.data(), sizeof(header));
        return header.spacing;
    }
}

TEST_CASE("a chunk's spacing is measured from its points, or taken from its desc")
{
    std::vector<glm::vec3> grid;
    for (int z = 0; z < 100; ++z)
    {
        for (int x = 0; x < 100; ++x)
            grid.emplace_back(0.5f * static_cast<float>(x), 0.0f, 0.5f * static_cast<float>(z));
    }
    const rasm::MeshBuild measured = rasm::buildPointCloudChunk({.positions = grid.data(), .count = static_cast<uint32_t>(grid.size())});
    REQUIRE(measured.isValid());
    // A grid of step h measures 1.4 h.
    CHECK(headerSpacing(measured) > 0.5f);
    CHECK(headerSpacing(measured) < 0.9f);
    const rasm::MeshBuild given = rasm::buildPointCloudChunk({.positions = grid.data(), .count = static_cast<uint32_t>(grid.size()), .spacing = 0.25f});
    CHECK(headerSpacing(given) == doctest::Approx(0.25f));
    const glm::vec3 one(1.0f);
    CHECK(headerSpacing(rasm::buildPointCloudChunk({.positions = &one, .count = 1})) == 0.0f);
}

TEST_CASE("an octree refuses no points, a bad grid and points that are not finite")
{
    rasm::PointCloudOctree octree;
    const std::vector<glm::dvec3> points = makeGround(100);
    CHECK_FALSE(rasm::buildPointCloudOctree({.positions = nullptr, .count = 4}, octree));
    CHECK_FALSE(rasm::buildPointCloudOctree({.positions = points.data(), .count = 100, .cells = 100}, octree));
    CHECK_FALSE(rasm::buildPointCloudOctree({.positions = points.data(), .count = 100, .maxNodePoints = rasm::MAX_POINT_CHUNK_POINTS + 1}, octree));
    std::vector<glm::dvec3> bad = points;
    bad[7].y = std::nan("");
    CHECK_FALSE(rasm::buildPointCloudOctree({.positions = bad.data(), .count = 100}, octree));
    CHECK(octree.nodes.empty());
    REQUIRE(rasm::buildPointCloudOctree({.positions = points.data(), .count = 100}, octree));
    CHECK(octree.nodes.size() == 1);
    CHECK(octree.nodes[0].geometricError == 0.0f);
}

TEST_CASE("an octree holds every point once, each in its node's cube, the nodes nested and finer down")
{
    const uint32_t count = 600000;
    const std::vector<glm::dvec3> points = makeGround(count);
    std::vector<float> scalars(count);
    std::vector<uint32_t> colors(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        scalars[i] = static_cast<float>(i);
        colors[i] = i * 2654435761u;
    }
    rasm::PointCloudOctree octree;
    REQUIRE(rasm::buildPointCloudOctree({.positions = points.data(), .colors = colors.data(), .scalars = scalars.data(), .count = count, .maxNodePoints = 20000}, octree));
    REQUIRE(octree.positions.size() == count);
    REQUIRE(octree.colors.size() == count);
    REQUIRE(octree.scalars.size() == count);
    REQUIRE(octree.nodes.size() > 8);

    std::vector<uint32_t> seen(count, 0);
    uint32_t inner = 0;
    for (uint32_t n = 0; n < octree.nodes.size(); ++n)
    {
        const rasm::PointCloudNode &node = octree.nodes[n];
        CHECK(node.pointCount > 0);
        CHECK(node.pointCount <= rasm::MAX_POINT_CHUNK_POINTS);
        bool leaf = true;
        for (uint32_t octant = 0; octant < 8; ++octant)
        {
            const uint32_t child = node.children[octant];
            if (child == rasm::POINT_NODE_NONE)
                continue;
            leaf = false;
            REQUIRE(child > n);
            const rasm::PointCloudNode &record = octree.nodes[child];
            CHECK(record.parent == n);
            CHECK(record.spacing <= node.spacing);
            CHECK(record.level == node.level + 1);
            CHECK(record.halfSize == doctest::Approx(0.5 * node.halfSize));
            // The child's cube is the octant's, and its sphere is inside its parent's.
            const glm::dvec3 side = record.center - node.center;
            CHECK((side.x > 0.0) == ((octant & 1u) != 0));
            CHECK((side.y > 0.0) == ((octant & 2u) != 0));
            CHECK((side.z > 0.0) == ((octant & 4u) != 0));
            CHECK(glm::length(record.boundsCenter - node.boundsCenter) + record.boundsRadius <= node.boundsRadius * (1.0 + 1e-9) + 1e-9);
        }
        if (leaf)
            CHECK(node.geometricError == 0.0f);
        else
        {
            ++inner;
            CHECK(node.pointCount <= 20000);
            CHECK(node.geometricError == node.spacing);
            CHECK(node.spacing > 0.0f);
        }
        for (uint32_t i = node.firstPoint; i < node.firstPoint + node.pointCount; ++i)
        {
            const uint32_t index = octree.indices[i];
            REQUIRE(index < count);
            ++seen[index];
            const glm::dvec3 world = node.center + glm::dvec3(octree.positions[i]);
            CHECK(glm::length(world - points[index]) < 1e-3);
            const glm::dvec3 offset = glm::abs(points[index] - node.center);
            CHECK(std::max(std::max(offset.x, offset.y), offset.z) <= node.halfSize * (1.0 + 1e-6));
            CHECK(glm::length(points[index] - node.boundsCenter) <= node.boundsRadius * (1.0 + 1e-9) + 1e-6);
            CHECK(octree.scalars[i] == scalars[index]);
            CHECK(octree.colors[i] == colors[index]);
        }
    }
    CHECK(inner > 0);
    CHECK(std::all_of(seen.begin(), seen.end(), [](uint32_t n)
                      { return n == 1; }));
    // Each level is about twice as fine as the one above.
    const rasm::PointCloudNode &root = octree.nodes[0];
    for (uint32_t octant = 0; octant < 8; ++octant)
    {
        const uint32_t child = root.children[octant];
        if (child != rasm::POINT_NODE_NONE && octree.nodes[child].geometricError > 0.0f)
        {
            CHECK(octree.nodes[child].spacing < 0.8f * root.spacing);
            CHECK(octree.nodes[child].spacing > 0.3f * root.spacing);
        }
    }

    const rasm::PointCloudChunkDesc chunk = rasm::getPointCloudNodeChunk(octree, 1);
    CHECK(chunk.positions == octree.positions.data() + octree.nodes[1].firstPoint);
    CHECK(chunk.count == octree.nodes[1].pointCount);
    CHECK(chunk.spacing == octree.nodes[1].spacing);
    CHECK(chunk.scalars != nullptr);
    CHECK(rasm::buildPointCloudChunk(chunk).isValid());
    CHECK(rasm::getPointCloudNodeChunk(octree, static_cast<uint32_t>(octree.nodes.size())).count == 0);
}
