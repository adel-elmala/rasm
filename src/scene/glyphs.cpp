#include "rasm/glyphs.h"
#include "assets/meshBuild.h"
#include "renderer/uberMaterialTypes.h"

#include "glm/gtc/packing.hpp"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <vector>

namespace rasm
{
    namespace
    {
        constexpr float PI = 3.14159265358979323846f;
        // An arrow's shaft and head, along +Y from 0 to 1.
        constexpr float ARROW_SHAFT_RADIUS = 0.05f;
        constexpr float ARROW_HEAD_RADIUS = 0.13f;
        constexpr float ARROW_HEAD_START = 0.68f;

        // An arrow's rings, along +Y: its bottom cap, its shaft, the back of its head and its head's side, whose normal leans up
        // by the slope; the tip is a vertex per segment, with that segment's normal.
        struct ArrowVertices
        {
            uint32_t bottom;
            uint32_t shaftLow;
            uint32_t shaftHigh;
            uint32_t headBack;
            uint32_t base;
            uint32_t tips;
        };

        static_assert(gfx::GLYPH_BATCHES_PER_CHUNK * gfx::GLYPH_BATCH_GLYPHS == MAX_GLYPH_CHUNK_GLYPHS, "a glyph chunk's glyphs must fill its batches");
    }

    namespace
    {
        bool isFinite(const glm::vec3 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        uint32_t packSnorm2(float a, float b)
        {
            return glm::packSnorm1x16(a) | static_cast<uint32_t>(glm::packSnorm1x16(b)) << 16;
        }

        uint32_t packHalf2(float a, float b)
        {
            return glm::packHalf1x16(a) | static_cast<uint32_t>(glm::packHalf1x16(b)) << 16;
        }

        void pushVertex(std::vector<Vertex> &vertices, const glm::vec3 &position, const glm::vec3 &normal)
        {
            vertices.push_back({.pos = position, .normal = normal, .uv = {}, .tangent = glm::vec4(0.0f)});
        }

        // A ring of segments vertices at height y, each with the normal that leans normalOut outward and normalY up.
        uint32_t pushRing(std::vector<Vertex> &vertices, uint32_t segments, float y, float radius, float normalY, float normalOut)
        {
            const auto first = static_cast<uint32_t>(vertices.size());
            for (uint32_t i = 0; i < segments; ++i)
            {
                const float angle = 2.0f * PI * static_cast<float>(i) / static_cast<float>(segments);
                const glm::vec3 out(std::cos(angle), 0.0f, -std::sin(angle));
                pushVertex(vertices, glm::vec3(0.0f, y, 0.0f) + out * radius, glm::normalize(out * normalOut + glm::vec3(0.0f, normalY, 0.0f)));
            }
            return first;
        }

        // The side between two rings of `segments` vertices, every step-th of them, counter-clockwise seen from outside.
        void pushBand(std::vector<uint32_t> &indices, uint32_t segments, uint32_t step, uint32_t lower, uint32_t upper)
        {
            for (uint32_t i = 0; i < segments; i += step)
            {
                const uint32_t j = (i + step) % segments;
                indices.insert(indices.end(), {lower + i, lower + j, upper + i, upper + i, lower + j, upper + j});
            }
        }

        // A flat disc facing -Y (or +Y): a fan from its centre vertex, through every step-th vertex of the ring after it.
        void pushFan(std::vector<uint32_t> &indices, uint32_t segments, uint32_t step, uint32_t centre, bool up)
        {
            const uint32_t ring = centre + 1;
            for (uint32_t i = 0; i < segments; i += step)
            {
                const uint32_t j = (i + step) % segments;
                if (up)
                    indices.insert(indices.end(), {centre, ring + i, ring + j});
                else
                    indices.insert(indices.end(), {centre, ring + j, ring + i});
            }
        }

        uint32_t pushDisc(std::vector<Vertex> &vertices, uint32_t segments, float y, float radius, bool up)
        {
            const auto centre = static_cast<uint32_t>(vertices.size());
            const float normalY = up ? 1.0f : -1.0f;
            pushVertex(vertices, glm::vec3(0.0f, y, 0.0f), glm::vec3(0.0f, normalY, 0.0f));
            pushRing(vertices, segments, y, radius, normalY, 0.0f);
            return centre;
        }

        ArrowVertices pushArrowVertices(uint32_t segments, std::vector<Vertex> &vertices)
        {
            const float height = 1.0f - ARROW_HEAD_START;
            const glm::vec2 slope = glm::normalize(glm::vec2(height, ARROW_HEAD_RADIUS));
            ArrowVertices arrow{};
            arrow.bottom = pushDisc(vertices, segments, 0.0f, ARROW_SHAFT_RADIUS, false);
            arrow.shaftLow = pushRing(vertices, segments, 0.0f, ARROW_SHAFT_RADIUS, 0.0f, 1.0f);
            arrow.shaftHigh = pushRing(vertices, segments, ARROW_HEAD_START, ARROW_SHAFT_RADIUS, 0.0f, 1.0f);
            arrow.headBack = pushDisc(vertices, segments, ARROW_HEAD_START, ARROW_HEAD_RADIUS, false);
            arrow.base = pushRing(vertices, segments, ARROW_HEAD_START, ARROW_HEAD_RADIUS, slope.y, slope.x);
            arrow.tips = pushRing(vertices, segments, 1.0f, 0.0f, slope.y, slope.x);
            return arrow;
        }

        void pushArrowIndices(const ArrowVertices &arrow, uint32_t segments, uint32_t step, bool caps, std::vector<uint32_t> &indices)
        {
            if (caps)
                pushFan(indices, segments, step, arrow.bottom, false);
            pushBand(indices, segments, step, arrow.shaftLow, arrow.shaftHigh);
            if (caps)
                pushFan(indices, segments, step, arrow.headBack, false);
            for (uint32_t i = 0; i < segments; i += step)
                indices.insert(indices.end(), {arrow.base + i, arrow.base + (i + step) % segments, arrow.tips + i});
        }

        // A sphere of rings x segments quads (a triangle at each pole): rings = segments / 2. A coarser LOD takes every
        // step-th ring and segment.
        void pushSphereVertices(uint32_t segments, std::vector<Vertex> &vertices)
        {
            const uint32_t rings = segments / 2;
            for (uint32_t r = 0; r <= rings; ++r)
            {
                const float polar = PI * static_cast<float>(r) / static_cast<float>(rings);
                const float y = std::cos(polar);
                const float across = std::sin(polar);
                for (uint32_t i = 0; i <= segments; ++i)
                {
                    const float angle = 2.0f * PI * static_cast<float>(i) / static_cast<float>(segments);
                    const glm::vec3 normal(across * std::cos(angle), y, -across * std::sin(angle));
                    pushVertex(vertices, 0.5f * normal, normal);
                }
            }
        }

        void pushSphereIndices(uint32_t segments, uint32_t step, std::vector<uint32_t> &indices)
        {
            const uint32_t rings = segments / 2;
            const uint32_t stride = segments + 1;
            for (uint32_t r = 0; r < rings; r += step)
            {
                for (uint32_t i = 0; i < segments; i += step)
                {
                    const uint32_t a = r * stride + i;
                    const uint32_t b = (r + step) * stride + i;
                    if (r > 0)
                        indices.insert(indices.end(), {a, b, a + step});
                    if (r + step < rings)
                        indices.insert(indices.end(), {a + step, b, b + step});
                }
            }
        }

        void buildCube(std::vector<Vertex> &vertices, std::vector<uint32_t> &indices)
        {
            for (uint32_t face = 0; face < 6; ++face)
            {
                const uint32_t axis = face / 2;
                const float sign = face % 2 == 0 ? 1.0f : -1.0f;
                glm::vec3 normal(0.0f);
                normal[static_cast<int>(axis)] = sign;
                glm::vec3 u(0.0f);
                u[static_cast<int>((axis + 1) % 3)] = 1.0f;
                const glm::vec3 v = glm::cross(normal, u);
                const auto first = static_cast<uint32_t>(vertices.size());
                for (uint32_t c = 0; c < 4; ++c)
                {
                    const float su = (c == 1 || c == 2) ? 0.5f : -0.5f;
                    const float sv = c >= 2 ? 0.5f : -0.5f;
                    pushVertex(vertices, 0.5f * normal + su * u + sv * v, normal);
                }
                indices.insert(indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
            }
        }
    }

    // 16 B a glyph: its position and its value; then the orientations and the scales, each a stream of 8 B a glyph, only
    // when given. The header's sphere holds the glyphs' positions; culling grows it by the shape's reach times maxScale.
    MeshBuild buildGlyphChunk(const GlyphChunkDesc &desc)
    {
        if (desc.positions == nullptr || desc.count == 0 || desc.count > MAX_GLYPH_CHUNK_GLYPHS)
        {
            spdlog::error("buildGlyphChunk: a chunk holds 1 to {} glyphs; this one has {}{}.", MAX_GLYPH_CHUNK_GLYPHS, desc.count, desc.positions == nullptr ? " and no positions" : "");
            return {};
        }
        if (!desc.shape.isValid())
        {
            spdlog::error("buildGlyphChunk: the chunk has no shape (GlyphChunkDesc::shape).");
            return {};
        }
        if (desc.colors != nullptr && desc.scalars != nullptr)
        {
            spdlog::error("buildGlyphChunk: a chunk holds colors or scalars, not both.");
            return {};
        }
        const uint32_t count = desc.count;
        float maxScale = 1.0f;
        for (uint32_t i = 0; i < count; ++i)
        {
            const bool finite = isFinite(desc.positions[i]) && (desc.scalars == nullptr || std::isfinite(desc.scalars[i])) &&
                                (desc.orientations == nullptr || (std::isfinite(desc.orientations[i].x) && std::isfinite(desc.orientations[i].y) && std::isfinite(desc.orientations[i].z) &&
                                                                  std::isfinite(desc.orientations[i].w) && glm::length(desc.orientations[i]) > 1e-6f)) &&
                                (desc.scales == nullptr || (isFinite(desc.scales[i]) && desc.scales[i].x > 0.0f && desc.scales[i].y > 0.0f && desc.scales[i].z > 0.0f));
            if (!finite)
            {
                spdlog::error("buildGlyphChunk: glyph {} is not finite, or its orientation is zero or a scale not above 0.", i);
                return {};
            }
            if (desc.scales != nullptr)
            {
                const glm::vec3 s = glm::abs(desc.scales[i]);
                maxScale = std::max(i == 0 ? 0.0f : maxScale, std::max(s.x, std::max(s.y, s.z)));
            }
        }
        if (desc.scales != nullptr && maxScale > 65504.0f)
        {
            spdlog::error("buildGlyphChunk: a scale of {} does not fit a half float (65504 at most).", maxScale);
            return {};
        }

        const bool oriented = desc.orientations != nullptr;
        const bool scaled = desc.scales != nullptr;
        glm::vec3 boxMin = desc.positions[0];
        glm::vec3 boxMax = desc.positions[0];
        for (uint32_t i = 0; i < count; ++i)
        {
            boxMin = glm::min(boxMin, desc.positions[i]);
            boxMax = glm::max(boxMax, desc.positions[i]);
        }
        const glm::vec3 centre = 0.5f * (boxMin + boxMax);
        float radius = 0.0f;
        for (uint32_t i = 0; i < count; ++i)
            radius = std::max(radius, glm::length(desc.positions[i] - centre));
        const uint32_t words = 2 * gfx::GLYPH_CHUNK_HEADER_UNITS + 4 * count + (oriented ? 2 * count : 0) + (scaled ? 2 * count : 0);
        const gfx::GlyphChunkHeader header{.glyphCount = count,
                                           .flags = (desc.colors != nullptr ? gfx::GLYPH_CHUNK_COLORS : 0u) | (desc.scalars != nullptr ? gfx::GLYPH_CHUNK_SCALARS : 0u) |
                                                    (oriented ? gfx::GLYPH_CHUNK_ORIENTED : 0u) | (scaled ? gfx::GLYPH_CHUNK_SCALED : 0u),
                                           .maxScale = maxScale,
                                           .padding = 0,
                                           .boundsCenter = centre,
                                           .boundsRadius = radius};

        MeshBuild build;
        build.data = std::make_unique<MeshBuild::Data>();
        std::vector<uint32_t> &data = build.data->raw.points;
        data.assign(words, 0u);
        std::memcpy(data.data(), &header, sizeof(header));
        uint32_t *glyphs = data.data() + 2 * gfx::GLYPH_CHUNK_HEADER_UNITS;
        uint32_t *orientations = glyphs + 4 * count;
        uint32_t *scales = orientations + (oriented ? 2 * count : 0);
        for (uint32_t i = 0; i < count; ++i)
        {
            const glm::vec3 &p = desc.positions[i];
            std::memcpy(glyphs + 4 * i, &p, sizeof(p));
            glyphs[4 * i + 3] = desc.colors != nullptr ? desc.colors[i] : (desc.scalars != nullptr ? std::bit_cast<uint32_t>(desc.scalars[i]) : 0xFFFFFFFFu);
            if (oriented)
            {
                const glm::quat q = glm::normalize(desc.orientations[i]);
                orientations[2 * i] = packSnorm2(q.x, q.y);
                orientations[2 * i + 1] = packSnorm2(q.z, q.w);
            }
            if (scaled)
            {
                scales[2 * i] = packHalf2(desc.scales[i].x, desc.scales[i].y);
                scales[2 * i + 1] = packHalf2(desc.scales[i].z, 0.0f);
            }
        }
        build.data->raw.glyphCount = count;
        build.data->glyphShape = desc.shape;
        return build;
    }

    // Its LODs halve the segments around while they stay a whole number of at least 3 (an arrow) or 4 (a sphere, whose
    // rings halve with them): every LOD's vertices are the finest one's. A LOD's error is how far its polygon falls inside
    // the circle, at the widest ring: r (1 - cos(pi / segments)). An arrow has one more, the last without its caps. The
    // cube has one LOD.
    MeshBuild buildGlyphShape(GlyphShape shape, uint32_t segments)
    {
        segments = std::clamp(segments, shape == GlyphShape::SPHERE ? 4u : 3u, 64u);
        segments += shape == GlyphShape::SPHERE ? segments % 2 : 0;
        const uint32_t smallest = shape == GlyphShape::SPHERE ? 4u : 3u;
        const float radius = shape == GlyphShape::SPHERE ? 0.5f : ARROW_HEAD_RADIUS;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<MeshLod> lods;
        if (shape == GlyphShape::CUBE)
        {
            buildCube(vertices, indices);
        }
        else
        {
            const ArrowVertices arrow = shape == GlyphShape::ARROW ? pushArrowVertices(segments, vertices) : ArrowVertices{};
            if (shape == GlyphShape::SPHERE)
                pushSphereVertices(segments, vertices);
            for (uint32_t step = 1; segments % step == 0 && segments / step >= smallest && lods.size() < MAX_MESH_LODS && (shape != GlyphShape::SPHERE || (segments / 2) % step == 0); step *= 2)
            {
                MeshLod lod;
                if (shape == GlyphShape::ARROW)
                    pushArrowIndices(arrow, segments, step, true, lod.indices);
                else
                    pushSphereIndices(segments, step, lod.indices);
                lod.error = step == 1 ? 0.0f : radius * (1.0f - std::cos(PI / static_cast<float>(segments / step)));
                lods.push_back(std::move(lod));
            }
            // An arrow's last LOD leaves out its two caps, a third of its vertices: a hole as wide as the head, its error,
            // under a pixel where it is drawn.
            if (shape == GlyphShape::ARROW && lods.size() < MAX_MESH_LODS)
            {
                const uint32_t step = 1u << (static_cast<uint32_t>(lods.size()) - 1);
                MeshLod lod;
                pushArrowIndices(arrow, segments, step, false, lod.indices);
                lod.error = std::max(ARROW_HEAD_RADIUS, lods.back().error);
                lods.push_back(std::move(lod));
            }
            indices = std::move(lods.front().indices);
            lods.erase(lods.begin());
        }
        MeshBuild build = buildMesh(std::move(vertices), std::move(indices), {}, {.lods = false, .meshlets = false});
        if (build.isValid())
            build.data->raw.lods = std::move(lods);
        return build;
    }

    // The turn by twice the angle from +Y to the half-way direction, about their cross product: exact also near -Y,
    // where 1 + dot(+Y, direction) loses its digits.
    glm::quat getGlyphOrientation(const glm::vec3 &direction)
    {
        const float length = glm::length(direction);
        if (!(length > 0.0f) || !std::isfinite(length))
            return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const glm::vec3 half = up + direction / length;
        const float halfLength = glm::length(half);
        // Straight down: half a turn around X.
        if (halfLength < 1e-6f)
            return glm::quat(0.0f, 1.0f, 0.0f, 0.0f);
        const glm::vec3 h = half / halfLength;
        const glm::vec3 axis = glm::cross(up, h);
        return glm::quat(glm::dot(up, h), axis.x, axis.y, axis.z);
    }
}
