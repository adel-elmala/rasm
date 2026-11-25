#include "rasm/vector.h"

#include "doctest/doctest.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
    constexpr float KIND_LINE = 2.0f;
    constexpr float KIND_POINT = 3.0f;
    constexpr float METRIC_ROUND = 0.0f;
    constexpr float METRIC_ACROSS = 2.0f;

    double triangleArea(const rasm::VectorGeometry &geometry, size_t t)
    {
        const glm::vec3 a = geometry.vertices[geometry.indices[t]].pos;
        const glm::vec3 b = geometry.vertices[geometry.indices[t + 1]].pos;
        const glm::vec3 c = geometry.vertices[geometry.indices[t + 2]].pos;
        return 0.5 * glm::length(glm::cross(glm::dvec3(b - a), glm::dvec3(c - a)));
    }

    // A terrain tile of 4 cells over 40 m with some hills, its samples as floats.
    struct HillTile
    {
        std::vector<float> samples;
        rasm::TerrainTileDesc desc;
    };

    HillTile makeHills()
    {
        HillTile tile;
        constexpr uint32_t WIDTH = 7;
        for (uint32_t j = 0; j < WIDTH; ++j)
            for (uint32_t i = 0; i < WIDTH; ++i)
                tile.samples.push_back(static_cast<float>(3.0 * std::sin(1.3 * i) + 2.0 * std::cos(0.9 * j) + 0.5 * i * j));
        tile.desc = {.center = glm::dvec3(100.0, 0.0, -50.0), .size = 40.0, .terrain = {.minHeight = -10.0f, .maxHeight = 20.0f}};
        return tile;
    }
}

TEST_CASE("buildMesh checks the data on any thread and createMesh only takes valid builds")
{
    const std::vector<rasm::Vertex> vertices(3, rasm::Vertex{});
    CHECK(rasm::buildMesh(vertices, {0, 1, 2}).isValid());
    CHECK(rasm::buildMesh(vertices, {0, 1, 2}, {}, {.lods = false, .meshlets = false}).isValid());
    CHECK_FALSE(rasm::buildMesh(vertices, {0, 1}).isValid());
    CHECK_FALSE(rasm::buildMesh(vertices, {0, 1, 3}).isValid());
    CHECK_FALSE(rasm::buildMesh(vertices, {0, 1, 2}, {1u, 2u}).isValid());
    rasm::MeshBuild moved = rasm::buildMesh(vertices, {0, 1, 2});
    const rasm::MeshBuild taken = std::move(moved);
    CHECK(taken.isValid());
    CHECK_FALSE(moved.isValid()); // NOLINT(bugprone-use-after-move): a moved-from build is empty
}

TEST_CASE("a straight line is a quad of offsets across it, with its distance along it")
{
    rasm::VectorGeometry geometry;
    const glm::dvec3 origin(1000.0, 0.0, 1000.0);
    const glm::dvec3 points[2] = {{1000.0, 0.0, 1000.0}, {1010.0, 0.0, 1000.0}};
    const uint32_t size = 2;
    const uint32_t id = 77;
    rasm::addVectorLines(geometry, origin, points, &size, 1, {.cap = rasm::LineCap::BUTT}, {}, &id);
    REQUIRE(geometry.vertices.size() == 4);
    REQUIRE(geometry.indices.size() == 6);
    for (const rasm::Vertex &vertex : geometry.vertices)
    {
        CHECK(vertex.uv[2].x == KIND_LINE);
        CHECK(vertex.uv[2].y == METRIC_ACROSS);
        CHECK(std::abs(vertex.uv[0].y) == 1.0f);
        CHECK(vertex.uv[0].x == 0.0f);
        CHECK(vertex.tangent == glm::vec4(1.0f, 0.0f, 0.0f, 77.0f));
        CHECK(vertex.normal == glm::vec3(0.0f, 1.0f, 0.0f));
    }
    // Offsets from the origin, in floats; the distance along the line in metres.
    CHECK(geometry.vertices[0].pos == glm::vec3(0.0f));
    CHECK(geometry.vertices[2].pos == glm::vec3(10.0f, 0.0f, 0.0f));
    CHECK(geometry.vertices[0].uv[1].x == 0.0f);
    CHECK(geometry.vertices[3].uv[1].x == 10.0f);
}

TEST_CASE("caps and joins: round ones are fans of unit offsets, a miter reaches both edges exactly")
{
    const uint32_t three = 3;
    // A right-angle turn: a miter of sqrt(2) half widths, under the limit of 2.
    const glm::dvec3 corner[3] = {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {10.0, 0.0, 10.0}};
    rasm::VectorGeometry miter;
    rasm::addVectorLines(miter, glm::dvec3(0.0), corner, &three, 1, {.join = rasm::LineJoin::MITER, .cap = rasm::LineCap::BUTT});
    // Two quads and the join's two triangles of three vertices.
    REQUIRE(miter.vertices.size() == 4 + 6 + 4);
    bool tipFound = false;
    for (const rasm::Vertex &vertex : miter.vertices)
    {
        const glm::vec2 offset = vertex.uv[0];
        if (offset.x != 0.0f)
        {
            // The tip: one half width across in its triangle's own frame, sqrt(2) from the corner.
            CHECK(std::abs(offset.y) == doctest::Approx(1.0f));
            CHECK(glm::length(offset) == doctest::Approx(std::sqrt(2.0f)));
            tipFound = true;
        }
    }
    CHECK(tipFound);

    // A turn of 170 degrees is past the miter limit: a bevel, one triangle.
    const glm::dvec3 sharp[3] = {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {10.0 - 10.0 * std::cos(0.1745), 0.0, 10.0 * std::sin(0.1745)}};
    rasm::VectorGeometry beveled;
    rasm::addVectorLines(beveled, glm::dvec3(0.0), sharp, &three, 1, {.join = rasm::LineJoin::MITER, .cap = rasm::LineCap::BUTT});
    CHECK(beveled.vertices.size() == 4 + 3 + 4);

    rasm::VectorGeometry round;
    rasm::addVectorLines(round, glm::dvec3(0.0), corner, &three, 1, {.join = rasm::LineJoin::ROUND, .cap = rasm::LineCap::ROUND});
    uint32_t fanVertices = 0;
    for (const rasm::Vertex &vertex : round.vertices)
    {
        if (vertex.uv[2].y != METRIC_ROUND)
            continue;
        ++fanVertices;
        const float length = glm::length(vertex.uv[0]);
        CHECK((length == doctest::Approx(1.0f) || length == 0.0f));
    }
    // Two caps of a half turn (8 steps, 9 rim vertices and a centre) and a quarter-turn join (4 steps).
    CHECK(fanVertices == 10 + 10 + 6);
}

TEST_CASE("a join opens on the outer side of the turn")
{
    const uint32_t three = 3;
    // From +x, turning toward +z. The shader's across direction is cross(along, up) = +z for along +x: the line turns
    // toward its y = +1 side, so the gap opens on the other one, y = -1.
    const glm::dvec3 turn[3] = {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {10.0, 0.0, 10.0}};
    rasm::VectorGeometry geometry;
    rasm::addVectorLines(geometry, glm::dvec3(0.0), turn, &three, 1, {.join = rasm::LineJoin::BEVEL, .cap = rasm::LineCap::BUTT});
    REQUIRE(geometry.vertices.size() == 4 + 3 + 4);
    const rasm::Vertex &outer = geometry.vertices[5]; // the bevel's corner on the first segment
    CHECK(outer.uv[0] == glm::vec2(0.0f, -1.0f));
    const glm::vec3 across = glm::cross(glm::vec3(outer.tangent), outer.normal);
    CHECK(across.z == doctest::Approx(1.0f));
}

TEST_CASE("a polygon with a hole covers its outline's area less the hole's")
{
    const glm::dvec3 rings[8] = {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {10.0, 0.0, 8.0}, {0.0, 0.0, 8.0}, {2.0, 0.0, 2.0}, {2.0, 0.0, 5.0}, {6.0, 0.0, 5.0}, {6.0, 0.0, 2.0}};
    const uint32_t ringSizes[2] = {4, 4};
    const uint32_t ringCount = 2;
    rasm::VectorGeometry geometry;
    rasm::addVectorPolygons(geometry, glm::dvec3(5.0, 0.0, 4.0), rings, ringSizes, &ringCount, 1);
    double area = 0.0;
    for (size_t t = 0; t < geometry.indices.size(); t += 3)
        area += triangleArea(geometry, t);
    CHECK(area == doctest::Approx(80.0 - 12.0));
}

TEST_CASE("draped polygons and lines lie on the drawn terrain")
{
    const HillTile tile = makeHills();
    const rasm::TerrainHeights heights{.floats = tile.samples.data(), .width = 7};
    const rasm::VectorPlacement placement{.drapeTile = &tile.desc, .drapeHeights = heights};
    const glm::dvec3 origin = tile.desc.center;
    auto heightAt = [&](const glm::dvec3 &world)
    {
        const double u = (world.x - tile.desc.center.x) / tile.desc.size + 0.5;
        const double v = (world.z - tile.desc.center.z) / tile.desc.size + 0.5;
        return rasm::getTerrainPosition(tile.desc, heights, u, v).y;
    };

    // A triangle across several cells: cut into pieces that each lie on one terrain triangle, so every piece's middle
    // is on the ground too.
    const glm::dvec3 triangle[3] = {origin + glm::dvec3(-17.0, 0.0, -13.0), origin + glm::dvec3(15.0, 0.0, -9.0), origin + glm::dvec3(-3.0, 0.0, 16.0)};
    const uint32_t three = 3;
    const uint32_t one = 1;
    rasm::VectorGeometry polygon;
    rasm::addVectorPolygons(polygon, origin, triangle, &three, &one, 1, placement);
    REQUIRE(polygon.indices.size() > 3 * 8);
    double area = 0.0;
    for (size_t t = 0; t < polygon.indices.size(); t += 3)
    {
        glm::dvec3 middle(0.0);
        for (size_t k = 0; k < 3; ++k)
        {
            const glm::dvec3 world = origin + glm::dvec3(polygon.vertices[polygon.indices[t + k]].pos);
            CHECK(world.y == doctest::Approx(heightAt(world)).epsilon(1e-5));
            middle += world / 3.0;
        }
        CHECK(middle.y == doctest::Approx(heightAt(middle)).epsilon(1e-4));
        // Its area across the ground, seen from above.
        const glm::vec3 a = polygon.vertices[polygon.indices[t]].pos;
        const glm::vec3 b = polygon.vertices[polygon.indices[t + 1]].pos;
        const glm::vec3 c = polygon.vertices[polygon.indices[t + 2]].pos;
        area += 0.5 * std::abs((b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z));
    }
    CHECK(area == doctest::Approx(0.5 * std::abs(32.0 * 29.0 - 14.0 * 4.0)).epsilon(1e-4));

    // A line across the tile: its points are cut where it crosses the grid, so the middle of each piece is on the ground.
    const glm::dvec3 line[2] = {origin + glm::dvec3(-18.0, 0.0, 7.0), origin + glm::dvec3(18.0, 0.0, -11.0)};
    const uint32_t two = 2;
    rasm::VectorGeometry draped;
    rasm::addVectorLines(draped, origin, line, &two, 1, {.join = rasm::LineJoin::BEVEL, .cap = rasm::LineCap::BUTT}, placement);
    uint32_t pieces = 0;
    for (size_t v = 0; v + 3 < draped.vertices.size(); ++v)
    {
        const rasm::Vertex &a = draped.vertices[v];
        const rasm::Vertex &b = draped.vertices[v + 2];
        // A body quad: its first and third vertex are its two ends on the same side.
        if (a.uv[2].y != METRIC_ACROSS || a.uv[0] != glm::vec2(0.0f, -1.0f) || b.uv[0] != glm::vec2(0.0f, -1.0f) || a.pos == b.pos)
            continue;
        const glm::dvec3 middle = origin + 0.5 * (glm::dvec3(a.pos) + glm::dvec3(b.pos));
        CHECK(middle.y == doctest::Approx(heightAt(middle)).epsilon(1e-4));
        ++pieces;
    }
    CHECK(pieces >= 8);
}

TEST_CASE("points are quads of corner offsets, with an icon's corners when given")
{
    const glm::dvec3 points[2] = {{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}};
    const glm::vec4 icons[2] = {{0.0f, 0.0f, 0.5f, 0.5f}, {0.5f, 0.5f, 1.0f, 1.0f}};
    rasm::VectorGeometry geometry;
    rasm::addVectorPoints(geometry, glm::dvec3(0.0), points, 2, {}, nullptr, icons);
    REQUIRE(geometry.vertices.size() == 8);
    REQUIRE(geometry.indices.size() == 12);
    for (uint32_t i = 0; i < 8; ++i)
    {
        const rasm::Vertex &vertex = geometry.vertices[i];
        CHECK(vertex.uv[2].x == KIND_POINT);
        CHECK(std::abs(vertex.uv[0].x) == 1.0f);
        CHECK(std::abs(vertex.uv[0].y) == 1.0f);
        CHECK(vertex.tangent.w == static_cast<float>(i / 4));
    }
    // The second point's top-right corner: (u1, v0) of its rect.
    CHECK(geometry.vertices[7].uv[1] == glm::vec2(1.0f, 0.5f));
}
