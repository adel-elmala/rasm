#include "assets/meshBuild.h"
#include "rasm/assetLoader.h"
#include "rasm/text.h"
#include "rasm/vector.h"

#include "doctest/doctest.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace
{
    constexpr float KIND_GLYPH = 4.0f;

    // A font of three glyphs, 'A' and 'B' half an em wide and '?' the fallback, with a kerning pair A then B.
    rasm::FontAtlas makeFont()
    {
        rasm::FontAtlas font;
        font.width = 4;
        font.height = 4;
        font.pixels.assign(16, 0);
        font.ascent = 0.8f;
        font.descent = 0.2f;
        font.lineHeight = 1.2f;
        const glm::vec4 uv(0.0f, 0.0f, 1.0f, 1.0f);
        font.glyphs = {{.codepoint = '?', .advance = 0.5f, .offset = glm::vec2(0.0f), .size = glm::vec2(0.4f, 0.7f), .uv = uv},
                       {.codepoint = 'A', .advance = 0.5f, .offset = glm::vec2(0.0f), .size = glm::vec2(0.4f, 0.7f), .uv = uv},
                       {.codepoint = 'B', .advance = 0.5f, .offset = glm::vec2(0.0f), .size = glm::vec2(0.4f, 0.7f), .uv = uv},
                       {.codepoint = ' ', .advance = 0.25f}};
        std::sort(font.glyphs.begin(), font.glyphs.end(), [](const rasm::GlyphMetrics &a, const rasm::GlyphMetrics &b)
                  { return a.codepoint < b.codepoint; });
        font.kerning = {{.left = 'A', .right = 'B', .advance = -0.1f}};
        return font;
    }

    std::vector<uint8_t> readFontFile()
    {
        std::ifstream file(RASM_TEST_FONT, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    uint32_t glyphQuads(const rasm::VectorGeometry &geometry)
    {
        uint32_t quads = 0;
        for (size_t v = 0; v < geometry.vertices.size(); v += 4)
            quads += geometry.vertices[v].uv[2].x == KIND_GLYPH ? 1 : 0;
        return quads;
    }
}

TEST_CASE("decodeUtf8: ASCII and multi-byte codepoints")
{
    std::vector<uint32_t> codepoints;
    // "Aé€😀": one, two, three and four bytes.
    CHECK(rasm::decodeUtf8("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80", codepoints) == 4);
    CHECK(codepoints == std::vector<uint32_t>{0x41, 0xE9, 0x20AC, 0x1F600});
    // It appends.
    CHECK(rasm::decodeUtf8("B", codepoints) == 1);
    CHECK(codepoints.back() == 0x42);
    CHECK(rasm::decodeUtf8(nullptr, codepoints) == 0);
}

TEST_CASE("decodeUtf8: a malformed sequence becomes U+FFFD and decoding goes on")
{
    std::vector<uint32_t> codepoints;
    // A stray continuation byte, an overlong '/', a surrogate, a lead byte cut short.
    rasm::decodeUtf8("\x80"
                     "a\xC0\xAF"
                     "b\xED\xA0\x80"
                     "c\xE2\x82",
                     codepoints);
    CHECK(codepoints.front() == 0xFFFD);
    CHECK(codepoints[1] == 'a');
    CHECK(std::find(codepoints.begin(), codepoints.end(), 'b') != codepoints.end());
    CHECK(std::find(codepoints.begin(), codepoints.end(), 'c') != codepoints.end());
    CHECK(std::find(codepoints.begin(), codepoints.end(), 0xD800u) == codepoints.end());
    CHECK(std::count(codepoints.begin(), codepoints.end(), 0xFFFDu) >= 4);
}

TEST_CASE("findGlyph and getKerning")
{
    const rasm::FontAtlas font = makeFont();
    REQUIRE(rasm::findGlyph(font, 'B') != nullptr);
    CHECK(rasm::findGlyph(font, 'B')->codepoint == 'B');
    // A codepoint the atlas lacks draws the fallback.
    REQUIRE(rasm::findGlyph(font, 'Z') != nullptr);
    CHECK(rasm::findGlyph(font, 'Z')->codepoint == '?');
    CHECK(rasm::getKerning(font, 'A', 'B') == doctest::Approx(-0.1f));
    CHECK(rasm::getKerning(font, 'B', 'A') == 0.0f);

    rasm::FontAtlas noFallback = font;
    noFallback.fallback = 'X';
    CHECK(rasm::findGlyph(noFallback, 'Z') == nullptr);
}

TEST_CASE("addVectorLabels: the box, the halo's quads first, and the label's index")
{
    const rasm::FontAtlas font = makeFont();
    rasm::VectorGeometry geometry;
    const glm::dvec3 origin(1000.0, 0.0, -2000.0);
    const rasm::LabelDesc labels[2] = {{.position = origin + glm::dvec3(5.0, 1.0, 0.0), .text = "AB", .priority = 3.0f, .featureId = 7},
                                       {.position = origin, .text = "A A", .anchor = rasm::LabelAnchor::LEFT, .offset = glm::vec2(0.5f, 0.0f)}};
    rasm::addVectorLabels(geometry, origin, font, labels, 2);
    REQUIRE(geometry.labels.size() == 2);

    // "AB": 0.5 + 0.5 ems less the kerning, centred on the anchor.
    const rasm::MeshLabel &first = geometry.labels[0];
    CHECK(first.anchor == glm::vec3(5.0f, 1.0f, 0.0f));
    CHECK(first.halfSize.x == doctest::Approx(0.45f));
    CHECK(first.halfSize.y == doctest::Approx(0.5f));
    CHECK(first.center.x == doctest::Approx(0.0f));
    CHECK(first.center.y == doctest::Approx(0.0f));
    CHECK(first.priority == 3.0f);
    CHECK(first.featureId == 7);
    // "A A" right of its anchor: its half width, the gap of a quarter em, the offset.
    const rasm::MeshLabel &second = geometry.labels[1];
    CHECK(second.halfSize.x == doctest::Approx(0.625f));
    CHECK(second.center.x == doctest::Approx(0.625f + 0.25f + 0.5f));

    // Each glyph with a shape is a quad twice: the halo's quads, then the fill's, label by label.
    CHECK(glyphQuads(geometry) == 2 * 2 + 2 * 2);
    CHECK(geometry.indices.size() == 8 * 6);
    for (uint32_t v = 0; v < 8; ++v)
    {
        CHECK(geometry.vertices[v].uv[2].y == 1.0f);
        CHECK(std::bit_cast<uint32_t>(geometry.vertices[v].tangent.x) == 0);
        CHECK(std::bit_cast<uint32_t>(geometry.vertices[v].tangent.w) == 7);
    }
    CHECK(geometry.vertices[8].uv[2].y == 0.0f);
    CHECK(std::bit_cast<uint32_t>(geometry.vertices[16].tangent.x) == 1);
    // The kerning moves B: the second glyph's left edge is 0.4 ems right of the first's.
    CHECK(geometry.vertices[4].uv[0].x - geometry.vertices[0].uv[0].x == doctest::Approx(0.4f));
    for (const rasm::Vertex &vertex : geometry.vertices)
    {
        CHECK(std::abs(vertex.uv[0].x) <= rasm::MAX_LABEL_REACH);
        CHECK(std::abs(vertex.uv[0].y) <= rasm::MAX_LABEL_REACH);
    }
}

TEST_CASE("addVectorLabels: lines, empty text, and the labels through buildVectorMesh")
{
    const rasm::FontAtlas font = makeFont();
    rasm::VectorGeometry geometry;
    const rasm::LabelDesc labels[2] = {{.text = ""}, {.text = "AB\nA", .anchor = rasm::LabelAnchor::BOTTOM}};
    rasm::addVectorLabels(geometry, glm::dvec3(0.0), font, labels, 2);
    // The empty one is left out.
    REQUIRE(geometry.labels.size() == 1);
    const rasm::MeshLabel &label = geometry.labels[0];
    CHECK(label.halfSize.y == doctest::Approx(0.5f * (1.0f + 1.2f)));
    // Above its anchor: the box's bottom a quarter em over it.
    CHECK(label.center.y - label.halfSize.y == doctest::Approx(0.25f));
    // The second line's A is centred under the first: its pen starts at -0.25 ems.
    CHECK(geometry.vertices[8].uv[0].x == doctest::Approx(-0.25f));

    rasm::MeshBuild build = rasm::buildVectorMesh(std::move(geometry));
    REQUIRE(build.isValid());
    CHECK(build.data->labels.size() == 1);
}

TEST_CASE("buildFontAtlas: Roboto's distance atlas, metrics and kerning")
{
    const std::vector<uint8_t> file = readFontFile();
    REQUIRE(!file.empty());
    rasm::FontAtlas atlas;
    REQUIRE(rasm::buildFontAtlas(file.data(), file.size(), {}, atlas));
    CHECK(atlas.width == 512);
    CHECK(atlas.pixels.size() == static_cast<size_t>(atlas.width) * atlas.height);
    CHECK(atlas.distanceRange == 16.0f);
    CHECK(atlas.ascent > 0.5f);
    CHECK(atlas.descent > 0.0f);
    CHECK(atlas.lineHeight >= atlas.ascent + atlas.descent);
    CHECK(std::is_sorted(atlas.glyphs.begin(), atlas.glyphs.end(), [](const rasm::GlyphMetrics &a, const rasm::GlyphMetrics &b)
                         { return a.codepoint < b.codepoint; }));
    // ASCII and Latin-1, with the fallback.
    CHECK(atlas.glyphs.size() >= 180);
    const rasm::GlyphMetrics *e = rasm::findGlyph(atlas, 0xE9);
    REQUIRE(e != nullptr);
    CHECK(e->codepoint == 0xE9);
    const rasm::GlyphMetrics *space = rasm::findGlyph(atlas, ' ');
    REQUIRE(space != nullptr);
    CHECK(space->advance > 0.0f);

    // The middle of the I's stem is inside (over 0.5), the corner of its quad outside.
    const rasm::GlyphMetrics *i = rasm::findGlyph(atlas, 'I');
    REQUIRE(i != nullptr);
    CHECK(i->uv.x >= 0.0f);
    CHECK(i->uv.w <= 1.0f);
    const auto texel = [&](float u, float v)
    { return atlas.pixels[static_cast<size_t>(v * static_cast<float>(atlas.height)) * atlas.width + static_cast<size_t>(u * static_cast<float>(atlas.width))]; };
    CHECK(texel(0.5f * (i->uv.x + i->uv.z), 0.5f * (i->uv.y + i->uv.w)) > 128);
    CHECK(texel(i->uv.x, i->uv.y) < 64);

    // Roboto kerns A then V closer.
    CHECK(rasm::getKerning(atlas, 'A', 'V') < 0.0f);
    CHECK(std::is_sorted(atlas.kerning.begin(), atlas.kerning.end(), [](const rasm::KerningPair &a, const rasm::KerningPair &b)
                         { return a.left < b.left || (a.left == b.left && a.right < b.right); }));

    // Only the codepoints asked for, and the fallback.
    const uint32_t digits[3] = {'1', '2', '3'};
    rasm::FontAtlas small;
    REQUIRE(rasm::buildFontAtlas(file.data(), file.size(), {.codepoints = digits, .codepointCount = 3}, small));
    CHECK(small.glyphs.size() == 4);
    CHECK(rasm::findGlyph(small, 'A')->codepoint == '?');
}

TEST_CASE("buildFontAtlas: refuses what is no font and bad options")
{
    const uint8_t junk[64] = {};
    rasm::FontAtlas atlas;
    CHECK_FALSE(rasm::buildFontAtlas(junk, sizeof(junk), {}, atlas));
    CHECK_FALSE(rasm::buildFontAtlas(nullptr, 0, {}, atlas));
    const std::vector<uint8_t> file = readFontFile();
    REQUIRE(!file.empty());
    CHECK_FALSE(rasm::buildFontAtlas(file.data(), file.size(), {.emSize = 4.0f}, atlas));
    CHECK_FALSE(rasm::buildFontAtlas(file.data(), file.size(), {.distanceRange = 0.0f}, atlas));
}

TEST_CASE("addVectorLabels: control characters are not drawn, and a priority that is not finite sorts last")
{
    const rasm::FontAtlas font = makeFont();
    rasm::VectorGeometry geometry;
    const rasm::LabelDesc labels[1] = {{.text = "A\r\tB", .priority = std::numeric_limits<float>::quiet_NaN(), .offset = glm::vec2(std::numeric_limits<float>::infinity(), 0.0f)}};
    rasm::addVectorLabels(geometry, glm::dvec3(0.0), font, labels, 1);
    REQUIRE(geometry.labels.size() == 1);
    // A and B, kerned, with nothing between them: no fallback glyph for the carriage return and the tab.
    CHECK(glyphQuads(geometry) == 2 * 2);
    CHECK(geometry.labels[0].halfSize.x == doctest::Approx(0.45f));
    CHECK(std::isfinite(geometry.labels[0].priority));
    CHECK(geometry.labels[0].priority < -1e30f);
    CHECK(geometry.labels[0].center.x == doctest::Approx(0.0f));
}

TEST_CASE("buildFontAtlas: refuses an atlas past 4096 texels and options out of range")
{
    const std::vector<uint8_t> file = readFontFile();
    REQUIRE(!file.empty());
    rasm::FontAtlas atlas;
    CHECK_FALSE(rasm::buildFontAtlas(file.data(), file.size(), {.distanceRange = 2000.0f}, atlas));
    CHECK_FALSE(rasm::buildFontAtlas(file.data(), file.size(), {.emSize = 300.0f}, atlas));
    // Large glyphs grow the atlas's width instead of overflowing it.
    REQUIRE(rasm::buildFontAtlas(file.data(), file.size(), {.emSize = 256.0f, .distanceRange = 32.0f}, atlas));
    CHECK(atlas.width <= 4096);
    CHECK(atlas.height <= 4096);
    CHECK(atlas.pixels.size() == static_cast<size_t>(atlas.width) * atlas.height);
}
