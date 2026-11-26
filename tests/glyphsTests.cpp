#include "rasm/glyphs.h"
#include "rasm/vector.h"
#include "assets/meshBuild.h"
#include "renderer/glyphCull.h"
#include "renderer/uberMaterialTypes.h"

#include "doctest/doctest.h"
#include "glm/gtc/packing.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    // A shape handle for builds without a renderer: buildGlyphChunk only checks that one is given.
    constexpr rasm::MeshHandle SHAPE{1, 1};

    rasm::gfx::GlyphChunkHeader getHeader(const rasm::MeshBuild &build)
    {
        rasm::gfx::GlyphChunkHeader header{};
        std::memcpy(&header, build.data->raw.points.data(), sizeof(header));
        return header;
    }

    // A glyph as lib/glyphs.slang's loadGlyph reads it.
    struct DecodedGlyph
    {
        glm::vec3 position;
        uint32_t value;
        glm::vec4 orientation;
        glm::vec3 scale;
    };

    float unpackSnorm(uint32_t bits)
    {
        return glm::unpackSnorm1x16(static_cast<uint16_t>(bits & 0xFFFF));
    }

    DecodedGlyph decode(const rasm::MeshBuild &build, uint32_t glyph)
    {
        const rasm::gfx::GlyphChunkHeader header = getHeader(build);
        const uint32_t *words = build.data->raw.points.data() + 2 * rasm::gfx::GLYPH_CHUNK_HEADER_UNITS;
        DecodedGlyph decoded{.position = glm::vec3(0.0f), .value = words[4 * glyph + 3], .orientation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), .scale = glm::vec3(1.0f)};
        std::memcpy(&decoded.position, words + 4 * glyph, sizeof(glm::vec3));
        const uint32_t *next = words + 4 * header.glyphCount;
        if ((header.flags & rasm::gfx::GLYPH_CHUNK_ORIENTED) != 0)
        {
            decoded.orientation = glm::vec4(unpackSnorm(next[2 * glyph]), unpackSnorm(next[2 * glyph] >> 16), unpackSnorm(next[2 * glyph + 1]), unpackSnorm(next[2 * glyph + 1] >> 16));
            next += 2 * header.glyphCount;
        }
        if ((header.flags & rasm::gfx::GLYPH_CHUNK_SCALED) != 0)
            decoded.scale = glm::vec3(glm::unpackHalf1x16(static_cast<uint16_t>(next[2 * glyph] & 0xFFFF)), glm::unpackHalf1x16(static_cast<uint16_t>(next[2 * glyph] >> 16)),
                                      glm::unpackHalf1x16(static_cast<uint16_t>(next[2 * glyph + 1] & 0xFFFF)));
        return decoded;
    }
}

TEST_CASE("a glyph chunk keeps each glyph's position, value, orientation and scale, and its sphere holds the positions")
{
    const glm::vec3 positions[3] = {{0.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}, {0.0f, 3.0f, -2.0f}};
    const glm::quat orientations[3] = {glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::angleAxis(1.0f, glm::vec3(0.0f, 0.0f, 1.0f)), rasm::getGlyphOrientation(glm::vec3(1.0f, 1.0f, 0.0f))};
    const glm::vec3 scales[3] = {{1.0f, 1.0f, 1.0f}, {0.5f, 2.0f, 0.5f}, {0.001f, 300.0f, 7.25f}};
    const float scalars[3] = {-1.5f, 0.0f, 1e6f};
    const rasm::MeshBuild build = rasm::buildGlyphChunk({.shape = SHAPE, .positions = positions, .orientations = orientations, .scales = scales, .scalars = scalars, .count = 3});
    REQUIRE(build.isValid());
    const rasm::gfx::GlyphChunkHeader header = getHeader(build);
    CHECK(header.glyphCount == 3);
    CHECK(header.flags == (rasm::gfx::GLYPH_CHUNK_SCALARS | rasm::gfx::GLYPH_CHUNK_ORIENTED | rasm::gfx::GLYPH_CHUNK_SCALED));
    CHECK(header.maxScale == 300.0f);
    // 32 B of header, then 16 + 8 + 8 B a glyph.
    CHECK(build.data->raw.points.size() * sizeof(uint32_t) == 32 + 3 * 32);
    CHECK(build.data->raw.glyphCount == 3);
    CHECK(build.data->glyphShape == SHAPE);
    for (uint32_t i = 0; i < 3; ++i)
    {
        const DecodedGlyph glyph = decode(build, i);
        CHECK(glyph.position == positions[i]);
        CHECK(std::bit_cast<float>(glyph.value) == scalars[i]);
        const glm::quat q = glm::normalize(orientations[i]);
        // The quaternion's sign does not matter; snorm16 keeps it to 1/32767.
        const float dot = std::abs(glyph.orientation.x * q.x + glyph.orientation.y * q.y + glyph.orientation.z * q.z + glyph.orientation.w * q.w);
        CHECK(dot == doctest::Approx(1.0f).epsilon(1e-4));
        for (int axis = 0; axis < 3; ++axis)
            CHECK(glyph.scale[axis] == doctest::Approx(scales[i][axis]).epsilon(1e-3));
        CHECK(glm::length(positions[i] - header.boundsCenter) <= header.boundsRadius + 1e-5f);
    }
}

TEST_CASE("a glyph chunk without orientations and scales takes 16 B a glyph, and its colors stay as given")
{
    const glm::vec3 positions[2] = {{1.0f, 2.0f, 3.0f}, {-1.0f, 0.0f, 0.5f}};
    const uint32_t colors[2] = {0xFF0000FFu, 0x80FF8000u};
    const rasm::MeshBuild build = rasm::buildGlyphChunk({.shape = SHAPE, .positions = positions, .colors = colors, .count = 2});
    REQUIRE(build.isValid());
    CHECK(getHeader(build).flags == rasm::gfx::GLYPH_CHUNK_COLORS);
    CHECK(getHeader(build).maxScale == 1.0f);
    CHECK(build.data->raw.points.size() * sizeof(uint32_t) == 32 + 2 * 16);
    CHECK(decode(build, 1).value == colors[1]);
    CHECK(decode(build, 1).scale == glm::vec3(1.0f));
    // Without colors or scalars each glyph is white.
    const rasm::MeshBuild white = rasm::buildGlyphChunk({.shape = SHAPE, .positions = positions, .count = 2});
    REQUIRE(white.isValid());
    CHECK(decode(white, 0).value == 0xFFFFFFFFu);
}

TEST_CASE("buildGlyphChunk refuses a chunk it cannot draw")
{
    const glm::vec3 positions[2] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    const uint32_t colors[2] = {};
    const float scalars[2] = {};
    CHECK_FALSE(rasm::buildGlyphChunk({.shape = SHAPE, .positions = positions, .count = 0}).isValid());
    CHECK_FALSE(rasm::buildGlyphChunk({.shape = SHAPE, .positions = nullptr, .count = 2}).isValid());
    CHECK_FALSE(rasm::buildGlyphChunk({.positions = positions, .count = 2}).isValid());
    CHECK_FALSE(rasm::buildGlyphChunk({.shape = SHAPE, .positions = positions, .colors = colors, .scalars = scalars, .count = 2}).isValid());
    const glm::vec3 notFinite[2] = {{0.0f, 0.0f, 0.0f}, {std::nanf(""), 0.0f, 0.0f}};
    CHECK_FALSE(rasm::buildGlyphChunk({.shape = SHAPE, .positions = notFinite, .count = 2}).isValid());
    const glm::quat zero[2] = {glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::quat(0.0f, 0.0f, 0.0f, 0.0f)};
    CHECK_FALSE(rasm::buildGlyphChunk({.shape = SHAPE, .positions = positions, .orientations = zero, .count = 2}).isValid());
    const glm::vec3 mirrored[2] = {{1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f}};
    CHECK_FALSE(rasm::buildGlyphChunk({.shape = SHAPE, .positions = positions, .scales = mirrored, .count = 2}).isValid());
    const glm::vec3 flat[2] = {{1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 1.0f}};
    CHECK_FALSE(rasm::buildGlyphChunk({.shape = SHAPE, .positions = positions, .scales = flat, .count = 2}).isValid());
    const glm::vec3 huge[2] = {{1.0f, 1.0f, 1.0f}, {1e5f, 1.0f, 1.0f}};
    CHECK_FALSE(rasm::buildGlyphChunk({.shape = SHAPE, .positions = positions, .scales = huge, .count = 2}).isValid());
    std::vector<glm::vec3> many(rasm::MAX_GLYPH_CHUNK_GLYPHS + 1, glm::vec3(0.0f));
    CHECK_FALSE(rasm::buildGlyphChunk({.shape = SHAPE, .positions = many.data(), .count = static_cast<uint32_t>(many.size())}).isValid());
    CHECK(rasm::buildGlyphChunk({.shape = SHAPE, .positions = many.data(), .count = rasm::MAX_GLYPH_CHUNK_GLYPHS}).isValid());
}

TEST_CASE("getGlyphOrientation turns +Y to the direction, also straight down")
{
    const glm::vec3 directions[] = {{0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {3.0f, 0.0f, 0.0f}, {-1.0f, 2.0f, 0.5f}, {1e-3f, -1.0f, 0.0f}};
    for (const glm::vec3 &direction : directions)
    {
        const glm::vec3 turned = rasm::getGlyphOrientation(direction) * glm::vec3(0.0f, 1.0f, 0.0f);
        CHECK(glm::length(turned - glm::normalize(direction)) < 1e-4f);
    }
    CHECK(rasm::getGlyphOrientation(glm::vec3(0.0f)) == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
}

TEST_CASE("the glyph shapes have LODs that share their vertices, coarser and with a larger error each")
{
    for (const rasm::GlyphShape shape : {rasm::GlyphShape::ARROW, rasm::GlyphShape::SPHERE})
    {
        const rasm::MeshBuild build = rasm::buildGlyphShape(shape, 16);
        REQUIRE(build.isValid());
        const rasm::ObjRaw &raw = build.data->raw;
        // 16, 8 and 4 segments around; an arrow's last LOD also leaves out its caps.
        CHECK(raw.lods.size() == (shape == rasm::GlyphShape::ARROW ? 3u : 2u));
        size_t previous = raw.indices.size();
        float error = 0.0f;
        for (const rasm::MeshLod &lod : raw.lods)
        {
            CHECK(lod.indices.size() % 3 == 0);
            CHECK(lod.indices.size() < previous);
            CHECK(lod.error > error);
            for (uint32_t index : lod.indices)
                CHECK(index < raw.vertices.size());
            previous = lod.indices.size();
            error = lod.error;
        }
    }
    // The arrow runs from the origin to +Y 1, with its head 0.13 across.
    const rasm::MeshBuild arrow = rasm::buildGlyphShape(rasm::GlyphShape::ARROW, 16);
    float top = 0.0f;
    float bottom = 1.0f;
    for (const rasm::Vertex &vertex : arrow.data->raw.vertices)
    {
        top = std::max(top, vertex.pos.y);
        bottom = std::min(bottom, vertex.pos.y);
    }
    CHECK(top == 1.0f);
    CHECK(bottom == 0.0f);
    // Odd segments: an arrow of 12 halves to 6 and 3, then its last LOD without caps.
    CHECK(rasm::buildGlyphShape(rasm::GlyphShape::ARROW, 12).data->raw.lods.size() == 3);
    CHECK(rasm::buildGlyphShape(rasm::GlyphShape::CUBE).data->raw.indices.size() == 36);
    CHECK(rasm::buildGlyphShape(rasm::GlyphShape::CUBE).data->raw.lods.empty());
}

TEST_CASE("a glyph buffer's parts follow one another, a word per glyph of the batches for the slots and the list")
{
    const rasm::GlyphCullLayout layout = rasm::getGlyphCullLayout(3, 10);
    CHECK(layout.chunks == sizeof(rasm::gfx::GlyphCullHeader));
    CHECK(layout.batches == layout.chunks + 3 * sizeof(rasm::gfx::GlyphChunkSlot));
    CHECK(layout.commands == layout.batches + 10 * sizeof(uint32_t));
    CHECK(layout.commandCapacity == 3 * rasm::MAX_MESH_LODS);
    // The weighted chunks' commands follow the others' room, where lib/glyphs.slang puts them.
    CHECK(layout.weightedCommands == layout.commands + layout.commandCapacity * sizeof(rasm::gfx::DrawInfo));
    CHECK(layout.slots == layout.weightedCommands + layout.commandCapacity * sizeof(rasm::gfx::DrawInfo));
    CHECK(layout.list == layout.slots + 10 * rasm::gfx::GLYPH_BATCH_GLYPHS * sizeof(uint32_t));
    CHECK(layout.size == layout.list + 10 * rasm::gfx::GLYPH_BATCH_GLYPHS * sizeof(uint32_t));
    // The commands are read as DrawInfos: 4-byte aligned.
    CHECK(layout.commands % 4 == 0);
}

TEST_CASE("data lines carry a value per vertex, each its point's, and lines without values are white beside them")
{
    const glm::dvec3 origin(1e6, 0.0, -2e6);
    const glm::dvec3 points[3] = {origin + glm::dvec3(0.0, 0.0, 0.0), origin + glm::dvec3(10.0, 0.0, 0.0), origin + glm::dvec3(10.0, 0.0, 10.0)};
    const uint32_t size = 3;
    const float scalars[3] = {1.0f, 2.0f, 3.0f};
    rasm::VectorGeometry geometry;
    rasm::addVectorLines(geometry, origin, points, &size, 1, {.join = rasm::LineJoin::MITER}, {}, nullptr, nullptr, scalars);
    REQUIRE(geometry.colors.size() == geometry.vertices.size());
    for (size_t v = 0; v < geometry.vertices.size(); ++v)
    {
        const float value = std::bit_cast<float>(geometry.colors[v]);
        // Each vertex sits on one of the points, and holds that point's scalar.
        bool found = false;
        for (uint32_t p = 0; p < 3; ++p)
            found = found || (glm::vec3(points[p] - origin) == geometry.vertices[v].pos && value == scalars[p]);
        CHECK(found);
    }
    const size_t valued = geometry.vertices.size();
    rasm::addVectorLines(geometry, origin, points, &size, 1);
    REQUIRE(geometry.colors.size() == geometry.vertices.size());
    for (size_t v = valued; v < geometry.vertices.size(); ++v)
        CHECK(geometry.colors[v] == 0xFFFFFFFFu);

    // Lines without values first: the colors start white for them once a line brings values.
    rasm::VectorGeometry mixed;
    rasm::addVectorLines(mixed, origin, points, &size, 1);
    CHECK(mixed.colors.empty());
    const uint32_t colors[3] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u};
    rasm::addVectorLines(mixed, origin, points, &size, 1, {}, {}, nullptr, colors);
    REQUIRE(mixed.colors.size() == mixed.vertices.size());
    CHECK(mixed.colors.front() == 0xFFFFFFFFu);
    CHECK(mixed.colors.back() == colors[2]);
    const rasm::MeshBuild build = rasm::buildVectorMesh(std::move(mixed));
    REQUIRE(build.isValid());
    CHECK(build.data->raw.colors.size() == build.data->raw.vertices.size());
}
