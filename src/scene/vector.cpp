#include "rasm/vector.h"
#include "assets/meshBuild.h"

#include "mapbox/earcut.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace rasm
{
    namespace
    {
        // uv[2] of a vertex: its kind and how its distance to the edge is measured. Must match shaders/lib/vector.slang.
        enum class Kind : uint32_t
        {
            FILL = 1,
            LINE = 2,
            POINT = 3,
            GLYPH = 4,
            BAND = 5,
        };

        enum class Metric : uint32_t
        {
            ROUND = 0,
            BOX = 1,
            ACROSS = 2,
            FILL = 3,
        };

        // A feature's point in the world, with the ground's up there.
        struct GroundPoint
        {
            glm::dvec3 position;
            glm::dvec3 up;
        };

        using Ring2 = std::vector<std::array<double, 2>>;
        using Polygon2 = std::vector<glm::dvec2>;
    }

    namespace
    {
        constexpr double PI = 3.14159265358979323846;
        constexpr uint32_t LINE_FEED = 0x0A;
        // Round joins and caps: a vertex every 1/16 of a turn at most.
        constexpr double ROUND_STEP = PI / 8.0;

        glm::dvec3 normalizeOr(const glm::dvec3 &v, const glm::dvec3 &fallback)
        {
            const double length = glm::length(v);
            return length > 1e-12 ? v / length : fallback;
        }

        // A direction across the ground: any unit vector perpendicular to up.
        glm::dvec3 getGroundAxis(const glm::dvec3 &up)
        {
            const glm::dvec3 axis = std::abs(up.z) < 0.9 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(1.0, 0.0, 0.0);
            return normalizeOr(glm::cross(up, axis), glm::dvec3(1.0, 0.0, 0.0));
        }

        // The across direction the shader uses: cross(along, up).
        glm::dvec3 getAcross(const glm::dvec3 &along, const glm::dvec3 &up)
        {
            return normalizeOr(glm::cross(along, up), getGroundAxis(up));
        }

        void pushVertex(VectorGeometry &geometry, const glm::dvec3 &origin, const GroundPoint &point, const glm::dvec3 &along, const glm::dvec2 &offset, const glm::vec2 &extra, Kind kind,
                        Metric metric, uint32_t featureId)
        {
            geometry.vertices.push_back({.pos = glm::vec3(point.position - origin),
                                         .normal = glm::vec3(point.up),
                                         .uv = {glm::vec2(offset), extra, glm::vec2(static_cast<float>(kind), static_cast<float>(metric))},
                                         .tangent = glm::vec4(glm::vec3(along), std::bit_cast<float>(featureId))});
        }

        void pushTriangle(VectorGeometry &geometry, uint32_t a, uint32_t b, uint32_t c)
        {
            geometry.indices.insert(geometry.indices.end(), {a, b, c});
        }

        // ---- Draping: the tile's frame, its grid and the rule of sampleTerrainHeight. ----

        uint32_t getCells(const VectorPlacement &placement)
        {
            return placement.drapeHeights.width - 3;
        }

        bool isDraped(const VectorPlacement &placement)
        {
            const TerrainHeights &heights = placement.drapeHeights;
            return placement.drapeTile != nullptr && placement.drapeTile->size > 0.0 && heights.width >= 4 && (heights.unorm16 != nullptr || heights.floats != nullptr);
        }

        // A world point across the tile, in grid units: (0, 0) at its first corner, (cells, cells) at the other.
        glm::dvec2 toGrid(const VectorPlacement &placement, const glm::dvec3 &world)
        {
            const TerrainTileDesc &tile = *placement.drapeTile;
            const glm::dvec3 local = glm::inverse(glm::dquat(tile.orientation)) * (world - tile.center);
            const double cells = getCells(placement);
            return glm::clamp(glm::dvec2(local.x / tile.size + 0.5, local.z / tile.size + 0.5), 0.0, 1.0) * cells;
        }

        // The drawn ground at a grid point and the normal of the triangle under it, in the world.
        GroundPoint liftOnGrid(const VectorPlacement &placement, const glm::dvec2 &grid)
        {
            const TerrainTileDesc &tile = *placement.drapeTile;
            const TerrainHeights &heights = placement.drapeHeights;
            const uint32_t cells = getCells(placement);
            const double u = grid.x / cells;
            const double v = grid.y / cells;
            const uint32_t i = std::min(static_cast<uint32_t>(std::max(grid.x, 0.0)), cells - 1);
            const uint32_t j = std::min(static_cast<uint32_t>(std::max(grid.y, 0.0)), cells - 1);
            const double fx = grid.x - i;
            const double fz = grid.y - j;
            auto corner = [&](uint32_t x, uint32_t z)
            { return sampleTerrainHeight(heights, tile.terrain, static_cast<double>(x) / cells, static_cast<double>(z) / cells); };
            const double a = corner(i, j);
            const double b = corner(i, j + 1);
            const double c = corner(i + 1, j);
            const double d = corner(i + 1, j + 1);
            // The slopes of the triangle the point is on, in metres per metre, with the curvature's.
            const double cell = tile.size / cells;
            const double slopeX = (fx + fz <= 1.0 ? c - a : d - b) / cell - (tile.terrain.sphereRadius > 0.0 ? (u - 0.5) * tile.size / tile.terrain.sphereRadius : 0.0);
            const double slopeZ = (fx + fz <= 1.0 ? b - a : d - c) / cell - (tile.terrain.sphereRadius > 0.0 ? (v - 0.5) * tile.size / tile.terrain.sphereRadius : 0.0);
            const glm::dvec3 up = glm::dquat(tile.orientation) * glm::normalize(glm::dvec3(-slopeX, 1.0, -slopeZ));
            return {.position = getTerrainPosition(tile, heights, u, v), .up = up};
        }

        // The grid lines a segment crosses, x = k, z = k and the cells' diagonals x + z = k: between two of them it lies
        // on one triangle, so its straight line is on the drawn ground.
        void cutOnGrid(const glm::dvec2 &from, const glm::dvec2 &to, std::vector<double> &cuts)
        {
            cuts.clear();
            auto addLines = [&](double a, double b)
            {
                if (std::abs(b - a) < 1e-12)
                    return;
                const double low = std::min(a, b);
                const double high = std::max(a, b);
                for (double k = std::floor(low) + 1.0; k < high; k += 1.0)
                    cuts.push_back((k - a) / (b - a));
            };
            addLines(from.x, to.x);
            addLines(from.y, to.y);
            addLines(from.x + from.y, to.x + to.y);
            std::sort(cuts.begin(), cuts.end());
        }

        // A line's points on the ground: as given, or draped and cut on the tile's grid. Repeated points go.
        void placeLine(const VectorPlacement &placement, const glm::dvec3 *points, uint32_t count, std::vector<GroundPoint> &placed)
        {
            placed.clear();
            const glm::dvec3 up = normalizeOr(glm::dvec3(placement.up), glm::dvec3(0.0, 1.0, 0.0));
            auto add = [&](const GroundPoint &point)
            {
                if (placed.empty() || glm::length(point.position - placed.back().position) > 1e-9)
                    placed.push_back(point);
            };
            if (!isDraped(placement))
            {
                for (uint32_t i = 0; i < count; ++i)
                    add({.position = points[i], .up = up});
                return;
            }
            std::vector<double> cuts;
            for (uint32_t i = 0; i < count; ++i)
            {
                const glm::dvec2 grid = toGrid(placement, points[i]);
                if (i > 0)
                {
                    const glm::dvec2 previous = toGrid(placement, points[i - 1]);
                    cutOnGrid(previous, grid, cuts);
                    for (double t : cuts)
                        add(liftOnGrid(placement, glm::mix(previous, grid, t)));
                }
                add(liftOnGrid(placement, grid));
            }
        }

        // ---- Lines ----

        // The offsets of a join's or cap's rim, from angle a0 to a1 (radians, in the segment's frame: x along, y
        // across), as a fan around the point.
        void pushFan(VectorGeometry &geometry, const glm::dvec3 &origin, const GroundPoint &point, const glm::dvec3 &along, double a0, double a1, const glm::vec2 &extra, uint32_t featureId)
        {
            const auto steps = static_cast<uint32_t>(std::max(1.0, std::ceil(std::abs(a1 - a0) / ROUND_STEP)));
            const auto center = static_cast<uint32_t>(geometry.vertices.size());
            pushVertex(geometry, origin, point, along, glm::dvec2(0.0), extra, Kind::LINE, Metric::ROUND, featureId);
            for (uint32_t s = 0; s <= steps; ++s)
            {
                const double angle = a0 + (a1 - a0) * s / steps;
                pushVertex(geometry, origin, point, along, glm::dvec2(std::sin(angle), std::cos(angle)), extra, Kind::LINE, Metric::ROUND, featureId);
                if (s > 0)
                    pushTriangle(geometry, center, center + s, center + s + 1);
            }
        }

        // A join fills the gap on the outer side of the turn from alongA to alongB. Its vertices carry the turn (uv[1].y:
        // from the frame's outer normal to the other segment's, plus 8 times the outer side), so a line spread across the
        // screen turns the join to the screen's angles (shaders/lib/vector.slang, getScreenJoinOffset). A bevel is
        // measured round from the joint: its straight edge is a triangle's edge, inside the line's distance.
        void pushJoin(VectorGeometry &geometry, const glm::dvec3 &origin, const GroundPoint &point, const glm::dvec3 &alongA, const glm::dvec3 &alongB, const LineShape &shape, float distance,
                      uint32_t featureId)
        {
            const glm::dvec3 acrossA = getAcross(alongA, point.up);
            const glm::dvec3 acrossB = getAcross(alongB, point.up);
            // In A's frame, B's direction; the gap opens on the side B turns away from.
            const double turn = glm::dot(alongB, acrossA);
            if (std::abs(turn) < 1e-6 && glm::dot(alongA, alongB) > 0.0)
                return;
            const double side = turn > 0.0 ? -1.0 : 1.0;
            const glm::dvec2 outerB(glm::dot(side * acrossB, alongA), glm::dot(side * acrossB, acrossA));
            // The turn from A's outer normal (0, side) to B's, the short way round.
            const double a0 = std::atan2(0.0, side);
            double a1 = std::atan2(outerB.x, outerB.y);
            if (a1 - a0 > PI)
                a1 -= 2.0 * PI;
            if (a0 - a1 > PI)
                a1 += 2.0 * PI;
            const glm::vec2 inA(distance, static_cast<float>(a1 - a0 + 8.0 * side));
            const glm::vec2 inB(distance, static_cast<float>(a0 - a1 + 8.0 * side));
            if (shape.join == LineJoin::ROUND)
            {
                pushFan(geometry, origin, point, alongA, a0, a1, inA, featureId);
                return;
            }
            const glm::dvec3 bisector = normalizeOr(side * (acrossA + acrossB), side * acrossA);
            const double cosine = glm::dot(bisector, side * acrossA);
            const double miter = cosine > 1e-6 ? 1.0 / cosine : 1e9;
            const auto first = static_cast<uint32_t>(geometry.vertices.size());
            if (shape.join == LineJoin::MITER && miter <= std::clamp(static_cast<double>(shape.miterLimit), 1.0, static_cast<double>(MAX_VECTOR_MITER)))
            {
                // Two triangles, each in its own segment's frame, so the distance across is exact in both.
                const glm::dvec3 tip = bisector * miter;
                pushVertex(geometry, origin, point, alongA, glm::dvec2(0.0), inA, Kind::LINE, Metric::ACROSS, featureId);
                pushVertex(geometry, origin, point, alongA, glm::dvec2(0.0, side), inA, Kind::LINE, Metric::ACROSS, featureId);
                pushVertex(geometry, origin, point, alongA, glm::dvec2(glm::dot(tip, alongA), glm::dot(tip, acrossA)), inA, Kind::LINE, Metric::ACROSS, featureId);
                pushVertex(geometry, origin, point, alongB, glm::dvec2(0.0), inB, Kind::LINE, Metric::ACROSS, featureId);
                pushVertex(geometry, origin, point, alongB, glm::dvec2(glm::dot(tip, alongB), glm::dot(tip, acrossB)), inB, Kind::LINE, Metric::ACROSS, featureId);
                pushVertex(geometry, origin, point, alongB, glm::dvec2(0.0, side), inB, Kind::LINE, Metric::ACROSS, featureId);
                pushTriangle(geometry, first, first + 1, first + 2);
                pushTriangle(geometry, first + 3, first + 4, first + 5);
                return;
            }
            // Bevel: the two outer corners joined.
            pushVertex(geometry, origin, point, alongA, glm::dvec2(0.0), inA, Kind::LINE, Metric::ROUND, featureId);
            pushVertex(geometry, origin, point, alongA, glm::dvec2(0.0, side), inA, Kind::LINE, Metric::ROUND, featureId);
            pushVertex(geometry, origin, point, alongA, outerB, inA, Kind::LINE, Metric::ROUND, featureId);
            pushTriangle(geometry, first, first + 1, first + 2);
        }

        // A cap at an end, pointing along forward (the line's direction there, away from the line).
        void pushCap(VectorGeometry &geometry, const glm::dvec3 &origin, const GroundPoint &point, const glm::dvec3 &forward, LineCap cap, float distance, uint32_t featureId)
        {
            if (cap == LineCap::ROUND)
            {
                pushFan(geometry, origin, point, forward, 0.0, PI, glm::vec2(distance, 0.0f), featureId);
                return;
            }
            if (cap != LineCap::SQUARE)
                return;
            const auto first = static_cast<uint32_t>(geometry.vertices.size());
            const glm::vec2 extra(distance, 0.0f);
            pushVertex(geometry, origin, point, forward, glm::dvec2(0.0, -1.0), extra, Kind::LINE, Metric::BOX, featureId);
            pushVertex(geometry, origin, point, forward, glm::dvec2(0.0, 1.0), extra, Kind::LINE, Metric::BOX, featureId);
            pushVertex(geometry, origin, point, forward, glm::dvec2(1.0, -1.0), extra, Kind::LINE, Metric::BOX, featureId);
            pushVertex(geometry, origin, point, forward, glm::dvec2(1.0, 1.0), extra, Kind::LINE, Metric::BOX, featureId);
            pushTriangle(geometry, first, first + 1, first + 2);
            pushTriangle(geometry, first + 2, first + 1, first + 3);
        }

        void pushLine(VectorGeometry &geometry, const glm::dvec3 &origin, const std::vector<GroundPoint> &line, const LineShape &shape, uint32_t featureId)
        {
            double distance = 0.0;
            glm::dvec3 previousAlong(0.0);
            for (size_t i = 0; i + 1 < line.size(); ++i)
            {
                const GroundPoint &a = line[i];
                const GroundPoint &b = line[i + 1];
                const double length = glm::length(b.position - a.position);
                const glm::dvec3 along = (b.position - a.position) / length;
                if (i == 0)
                    pushCap(geometry, origin, a, -along, shape.cap, static_cast<float>(distance), featureId);
                else
                    pushJoin(geometry, origin, a, previousAlong, along, shape, static_cast<float>(distance), featureId);
                // The body: a quad from a to b, its offsets across only.
                const auto first = static_cast<uint32_t>(geometry.vertices.size());
                const glm::vec2 atA(static_cast<float>(distance), 0.0f);
                const glm::vec2 atB(static_cast<float>(distance + length), 0.0f);
                pushVertex(geometry, origin, a, along, glm::dvec2(0.0, -1.0), atA, Kind::LINE, Metric::ACROSS, featureId);
                pushVertex(geometry, origin, a, along, glm::dvec2(0.0, 1.0), atA, Kind::LINE, Metric::ACROSS, featureId);
                pushVertex(geometry, origin, b, along, glm::dvec2(0.0, -1.0), atB, Kind::LINE, Metric::ACROSS, featureId);
                pushVertex(geometry, origin, b, along, glm::dvec2(0.0, 1.0), atB, Kind::LINE, Metric::ACROSS, featureId);
                pushTriangle(geometry, first, first + 1, first + 2);
                pushTriangle(geometry, first + 2, first + 1, first + 3);
                distance += length;
                previousAlong = along;
            }
            if (line.size() >= 2)
                pushCap(geometry, origin, line.back(), previousAlong, shape.cap, static_cast<float>(distance), featureId);
        }

        // ---- Polygons ----

        // The part of a convex polygon where a·p - k is below (side -1) or above (side 1) zero.
        Polygon2 clipHalf(const Polygon2 &polygon, const glm::dvec2 &a, double k, double side)
        {
            Polygon2 result;
            for (size_t i = 0; i < polygon.size(); ++i)
            {
                const glm::dvec2 &p = polygon[i];
                const glm::dvec2 &q = polygon[(i + 1) % polygon.size()];
                const double fp = side * (glm::dot(a, p) - k);
                const double fq = side * (glm::dot(a, q) - k);
                if (fp >= 0.0)
                    result.push_back(p);
                if ((fp > 0.0 && fq < 0.0) || (fp < 0.0 && fq > 0.0))
                    result.push_back(p + (q - p) * (fp / (fp - fq)));
            }
            return result;
        }

        // A triangle in grid units cut by the grid lines and the cells' diagonals: each piece lies on one triangle of the
        // terrain, so it is flat on the drawn ground.
        void cutTriangle(const glm::dvec2 &a, const glm::dvec2 &b, const glm::dvec2 &c, std::vector<Polygon2> &pieces)
        {
            pieces.assign(1, Polygon2{a, b, c});
            const glm::dvec2 lines[3] = {glm::dvec2(1.0, 0.0), glm::dvec2(0.0, 1.0), glm::dvec2(1.0, 1.0)};
            std::vector<Polygon2> next;
            for (const glm::dvec2 &line : lines)
            {
                next.clear();
                for (const Polygon2 &piece : pieces)
                {
                    double low = 1e30;
                    double high = -1e30;
                    for (const glm::dvec2 &p : piece)
                    {
                        low = std::min(low, glm::dot(line, p));
                        high = std::max(high, glm::dot(line, p));
                    }
                    Polygon2 rest = piece;
                    for (double k = std::floor(low) + 1.0; k < high - 1e-9; k += 1.0)
                    {
                        Polygon2 below = clipHalf(rest, line, k, -1.0);
                        if (below.size() >= 3)
                            next.push_back(std::move(below));
                        rest = clipHalf(rest, line, k, 1.0);
                    }
                    if (rest.size() >= 3)
                        next.push_back(std::move(rest));
                }
                std::swap(pieces, next);
            }
        }

        // ---- Draped bands ----

        // A grid point on a lattice of 2^-20 cells: the pieces that meet at a point lift it to the same ground and up.
        glm::dvec2 snapToGrid(const glm::dvec2 &grid)
        {
            constexpr double STEPS = 1048576.0;
            return glm::round(grid * STEPS) / STEPS;
        }

        // A draped line shape.drapeWidth wide: built as on flat ground across the tile (metres along the grid's x and z),
        // then each triangle cut on the grid and each piece's corners lifted onto the drawn ground, so every piece lies
        // on one terrain triangle. A corner keeps its offset and distance (interpolated in its triangle) and carries the
        // built half width in uv[1].y; the shader only adds the anti-aliasing pixel, in the ground's plane.
        void pushDrapedLine(VectorGeometry &geometry, const glm::dvec3 &origin, const VectorPlacement &placement, const glm::dvec3 *points, uint32_t count, const LineShape &shape,
                            uint32_t featureId)
        {
            const TerrainTileDesc &tile = *placement.drapeTile;
            const double cell = tile.size / getCells(placement);
            const double half = 0.5 * static_cast<double>(shape.drapeWidth);
            const glm::dvec3 flatUp(0.0, 1.0, 0.0);
            std::vector<GroundPoint> flat;
            for (uint32_t i = 0; i < count; ++i)
            {
                const glm::dvec2 at = toGrid(placement, points[i]) * cell;
                const glm::dvec3 position(at.x, 0.0, at.y);
                if (flat.empty() || glm::length(position - flat.back().position) > 1e-9)
                    flat.push_back({.position = position, .up = flatUp});
            }
            if (flat.size() < 2)
                return;
            VectorGeometry band;
            pushLine(band, flat[0].position, flat, shape, featureId);

            const glm::dquat orientation(tile.orientation);
            std::vector<Polygon2> pieces;
            for (size_t t = 0; t + 2 < band.indices.size(); t += 3)
            {
                const Vertex *corners[3] = {&band.vertices[band.indices[t]], &band.vertices[band.indices[t + 1]], &band.vertices[band.indices[t + 2]]};
                // A triangle's corners share its frame: the direction its offsets are measured from.
                const glm::dvec3 along(glm::vec3(corners[0]->tangent));
                const glm::dvec3 across = getAcross(along, flatUp);
                glm::dvec2 at[3];
                for (uint32_t k = 0; k < 3; ++k)
                {
                    const glm::dvec2 offset(corners[k]->uv[0]);
                    const glm::dvec3 ground = flat[0].position + glm::dvec3(corners[k]->pos) + (offset.x * along + offset.y * across) * half;
                    at[k] = glm::dvec2(ground.x, ground.z) / cell;
                }
                const glm::dvec2 ab = at[1] - at[0];
                const glm::dvec2 ac = at[2] - at[0];
                const double area = ab.x * ac.y - ab.y * ac.x;
                if (std::abs(area) < 1e-14)
                    continue;
                const glm::dvec3 alongTile = orientation * along;
                const float metric = corners[0]->uv[2].y;
                cutTriangle(at[0], at[1], at[2], pieces);
                for (const Polygon2 &piece : pieces)
                {
                    const auto first = static_cast<uint32_t>(geometry.vertices.size());
                    for (const glm::dvec2 &p : piece)
                    {
                        // The corner's weights in its triangle.
                        const glm::dvec2 ap = p - at[0];
                        const double w1 = (ap.x * ac.y - ap.y * ac.x) / area;
                        const double w2 = (ab.x * ap.y - ab.y * ap.x) / area;
                        const double w0 = 1.0 - w1 - w2;
                        const glm::vec2 offset = static_cast<float>(w0) * corners[0]->uv[0] + static_cast<float>(w1) * corners[1]->uv[0] + static_cast<float>(w2) * corners[2]->uv[0];
                        const float distance = static_cast<float>(w0 * corners[0]->uv[1].x + w1 * corners[1]->uv[1].x + w2 * corners[2]->uv[1].x);
                        const GroundPoint ground = liftOnGrid(placement, snapToGrid(p));
                        const glm::dvec3 alongGround = normalizeOr(alongTile - ground.up * glm::dot(alongTile, ground.up), getGroundAxis(ground.up));
                        geometry.vertices.push_back({.pos = glm::vec3(ground.position - origin),
                                                     .normal = glm::vec3(ground.up),
                                                     .uv = {offset, glm::vec2(distance, static_cast<float>(half)), glm::vec2(static_cast<float>(Kind::BAND), metric)},
                                                     .tangent = glm::vec4(glm::vec3(alongGround), std::bit_cast<float>(featureId))});
                    }
                    for (uint32_t k = 1; k + 1 < piece.size(); ++k)
                        pushTriangle(geometry, first, first + k, first + k + 1);
                }
            }
        }

        void pushFillVertex(VectorGeometry &geometry, const glm::dvec3 &origin, const GroundPoint &point, uint32_t featureId)
        {
            pushVertex(geometry, origin, point, getGroundAxis(point.up), glm::dvec2(0.0), glm::vec2(0.0f), Kind::FILL, Metric::FILL, featureId);
        }

        // C0 and C1 control characters other than the line feed (a carriage return, a tab) are not drawn.
        bool isControl(uint32_t codepoint)
        {
            return codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0);
        }

        // A label's glyph quads, laid out line by line from its first baseline and moved by shift (ems). halo: the halo's
        // quads (uv[2].y = 1), which the label draws first, so no glyph's halo covers a neighbour's fill.
        void pushLabelGlyphs(VectorGeometry &geometry, const glm::dvec3 &origin, const GroundPoint &anchor, const FontAtlas &font, const std::vector<uint32_t> &codepoints,
                             const std::vector<float> &lineWidths, const glm::vec2 &shift, uint32_t labelIndex, uint32_t featureId, bool halo)
        {
            uint32_t line = 0;
            glm::vec2 pen(-0.5f * lineWidths[0], 0.0f);
            uint32_t previous = 0;
            for (const uint32_t codepoint : codepoints)
            {
                if (codepoint == LINE_FEED)
                {
                    ++line;
                    pen = glm::vec2(-0.5f * lineWidths[line], -static_cast<float>(line) * font.lineHeight);
                    previous = 0;
                    continue;
                }
                const GlyphMetrics *glyph = isControl(codepoint) ? nullptr : findGlyph(font, codepoint);
                if (glyph == nullptr)
                    continue;
                pen.x += previous != 0 ? getKerning(font, previous, glyph->codepoint) : 0.0f;
                previous = glyph->codepoint;
                if (glyph->size.x > 0.0f && glyph->size.y > 0.0f)
                {
                    // The quad's corners in ems from the anchor, and their atlas coordinates (v grows downward).
                    const glm::vec2 low = pen + glyph->offset + shift;
                    const glm::vec2 high = low + glyph->size;
                    const glm::vec2 corners[4] = {low, glm::vec2(high.x, low.y), glm::vec2(low.x, high.y), high};
                    const glm::vec2 uvs[4] = {{glyph->uv.x, glyph->uv.w}, {glyph->uv.z, glyph->uv.w}, {glyph->uv.x, glyph->uv.y}, {glyph->uv.z, glyph->uv.y}};
                    const auto first = static_cast<uint32_t>(geometry.vertices.size());
                    for (uint32_t c = 0; c < 4; ++c)
                        geometry.vertices.push_back({.pos = glm::vec3(anchor.position - origin),
                                                     .normal = glm::vec3(anchor.up),
                                                     .uv = {corners[c], uvs[c], glm::vec2(static_cast<float>(Kind::GLYPH), halo ? 1.0f : 0.0f)},
                                                     .tangent = glm::vec4(std::bit_cast<float>(labelIndex), 0.0f, 0.0f, std::bit_cast<float>(featureId))});
                    pushTriangle(geometry, first, first + 1, first + 2);
                    pushTriangle(geometry, first + 2, first + 1, first + 3);
                }
                pen.x += glyph->advance;
            }
        }
    }

    void addVectorLines(VectorGeometry &geometry, const glm::dvec3 &origin, const glm::dvec3 *points, const uint32_t *lineSizes, uint32_t lineCount, const LineShape &shape, const VectorPlacement &placement,
                        const uint32_t *featureIds)
    {
        std::vector<GroundPoint> line;
        size_t first = 0;
        for (uint32_t i = 0; i < lineCount; ++i)
        {
            const uint32_t featureId = featureIds != nullptr ? featureIds[i] : i;
            if (shape.drapeWidth > 0.0f && isDraped(placement))
            {
                pushDrapedLine(geometry, origin, placement, points + first, lineSizes[i], shape, featureId);
                first += lineSizes[i];
                continue;
            }
            placeLine(placement, points + first, lineSizes[i], line);
            first += lineSizes[i];
            if (line.size() >= 2)
                pushLine(geometry, origin, line, shape, featureId);
        }
    }

    void addVectorPolygons(VectorGeometry &geometry, const glm::dvec3 &origin, const glm::dvec3 *points, const uint32_t *ringSizes, const uint32_t *ringCounts, uint32_t polygonCount,
                           const VectorPlacement &placement, const uint32_t *featureIds)
    {
        const bool draped = isDraped(placement);
        const glm::dvec3 up = normalizeOr(glm::dvec3(placement.up), glm::dvec3(0.0, 1.0, 0.0));
        const glm::dvec3 axisX = getGroundAxis(up);
        const glm::dvec3 axisZ = glm::cross(axisX, up);
        std::vector<Ring2> rings;
        std::vector<const glm::dvec3 *> flat; // earcut's vertex order: the rings' points one after another
        std::vector<Polygon2> pieces;
        size_t point = 0;
        size_t ring = 0;
        for (uint32_t p = 0; p < polygonCount; ++p)
        {
            const uint32_t featureId = featureIds != nullptr ? featureIds[p] : p;
            rings.clear();
            flat.clear();
            for (uint32_t r = 0; r < ringCounts[p]; ++r, ++ring)
            {
                Ring2 &ring2 = rings.emplace_back();
                for (uint32_t k = 0; k < ringSizes[ring]; ++k, ++point)
                {
                    const glm::dvec3 &world = points[point];
                    // Draped: across the tile in grid units; else in the ground's plane through the origin.
                    const glm::dvec2 at = draped ? toGrid(placement, world) : glm::dvec2(glm::dot(world - origin, axisX), glm::dot(world - origin, axisZ));
                    ring2.push_back({at.x, at.y});
                    flat.push_back(&world);
                }
            }
            if (rings.empty() || rings[0].size() < 3)
                continue;
            const std::vector<uint32_t> triangles = mapbox::earcut<uint32_t>(rings);
            std::vector<glm::dvec2> flat2;
            for (const Ring2 &ring2 : rings)
                for (const std::array<double, 2> &at : ring2)
                    flat2.emplace_back(at[0], at[1]);
            if (!draped)
            {
                const auto first = static_cast<uint32_t>(geometry.vertices.size());
                for (const glm::dvec3 *world : flat)
                    pushFillVertex(geometry, origin, {.position = *world, .up = up}, featureId);
                for (size_t t = 0; t + 2 < triangles.size(); t += 3)
                    pushTriangle(geometry, first + triangles[t], first + triangles[t + 1], first + triangles[t + 2]);
                continue;
            }
            for (size_t t = 0; t + 2 < triangles.size(); t += 3)
            {
                cutTriangle(flat2[triangles[t]], flat2[triangles[t + 1]], flat2[triangles[t + 2]], pieces);
                for (const Polygon2 &piece : pieces)
                {
                    const auto first = static_cast<uint32_t>(geometry.vertices.size());
                    for (const glm::dvec2 &grid : piece)
                        pushFillVertex(geometry, origin, liftOnGrid(placement, grid), featureId);
                    for (uint32_t k = 1; k + 1 < piece.size(); ++k)
                        pushTriangle(geometry, first, first + k, first + k + 1);
                }
            }
        }
    }

    void addVectorPoints(VectorGeometry &geometry, const glm::dvec3 &origin, const glm::dvec3 *points, uint32_t pointCount, const VectorPlacement &placement, const uint32_t *featureIds,
                         const glm::vec4 *iconRects)
    {
        const glm::dvec3 up = normalizeOr(glm::dvec3(placement.up), glm::dvec3(0.0, 1.0, 0.0));
        const glm::dvec2 corners[4] = {{-1.0, -1.0}, {1.0, -1.0}, {-1.0, 1.0}, {1.0, 1.0}};
        for (uint32_t i = 0; i < pointCount; ++i)
        {
            const GroundPoint point = isDraped(placement) ? liftOnGrid(placement, toGrid(placement, points[i])) : GroundPoint{.position = points[i], .up = up};
            const glm::dvec3 along = getGroundAxis(point.up);
            const uint32_t featureId = featureIds != nullptr ? featureIds[i] : i;
            const auto first = static_cast<uint32_t>(geometry.vertices.size());
            for (const glm::dvec2 &corner : corners)
            {
                glm::vec2 icon(0.0f);
                if (iconRects != nullptr)
                {
                    const glm::vec4 &rect = iconRects[i];
                    icon = glm::vec2(corner.x < 0.0 ? rect.x : rect.z, corner.y < 0.0 ? rect.w : rect.y);
                }
                pushVertex(geometry, origin, point, along, corner, icon, Kind::POINT, iconRects != nullptr ? Metric::BOX : Metric::ROUND, featureId);
            }
            pushTriangle(geometry, first, first + 1, first + 2);
            pushTriangle(geometry, first + 2, first + 1, first + 3);
        }
    }

    void addVectorLabels(VectorGeometry &geometry, const glm::dvec3 &origin, const FontAtlas &font, const LabelDesc *labels, uint32_t labelCount, const VectorPlacement &placement)
    {
        // A label's gap from its anchor, in ems, when the text sits beside it.
        constexpr float GAP = 0.25f;
        const glm::dvec3 up = normalizeOr(glm::dvec3(placement.up), glm::dvec3(0.0, 1.0, 0.0));
        std::vector<uint32_t> codepoints;
        std::vector<float> lineWidths;
        for (uint32_t l = 0; l < labelCount; ++l)
        {
            const LabelDesc &label = labels[l];
            codepoints.clear();
            if (decodeUtf8(label.text, codepoints) == 0)
                continue;
            // The lines' widths, with kerning.
            lineWidths.assign(1, 0.0f);
            uint32_t previous = 0;
            for (uint32_t codepoint : codepoints)
            {
                if (codepoint == LINE_FEED)
                {
                    lineWidths.push_back(0.0f);
                    previous = 0;
                    continue;
                }
                const GlyphMetrics *glyph = isControl(codepoint) ? nullptr : findGlyph(font, codepoint);
                if (glyph == nullptr)
                    continue;
                lineWidths.back() += (previous != 0 ? getKerning(font, previous, glyph->codepoint) : 0.0f) + glyph->advance;
                previous = glyph->codepoint;
            }
            const float width = *std::max_element(lineWidths.begin(), lineWidths.end());
            const float height = font.ascent + font.descent + static_cast<float>(lineWidths.size() - 1) * font.lineHeight;
            // The box from the first baseline: x from -width / 2, y from the last line's descent to the first's ascent.
            glm::vec2 boxCenter(0.0f, font.ascent - 0.5f * height);
            glm::vec2 shift = -boxCenter;
            if (label.anchor == LabelAnchor::BOTTOM)
                shift.y += 0.5f * height + GAP;
            else if (label.anchor == LabelAnchor::TOP)
                shift.y -= 0.5f * height + GAP;
            else if (label.anchor == LabelAnchor::LEFT)
                shift.x += 0.5f * width + GAP;
            else if (label.anchor == LabelAnchor::RIGHT)
                shift.x -= 0.5f * width + GAP;
            // A priority or offset that is not finite (bad data) would break placement's sort: such a label sorts last.
            if (std::isfinite(label.offset.x) && std::isfinite(label.offset.y))
                shift += label.offset;
            const float priority = std::isfinite(label.priority) ? label.priority : -std::numeric_limits<float>::max();

            const GroundPoint anchor = isDraped(placement) ? liftOnGrid(placement, toGrid(placement, label.position)) : GroundPoint{.position = label.position, .up = up};
            const auto labelIndex = static_cast<uint32_t>(geometry.labels.size());
            geometry.labels.push_back({.anchor = glm::vec3(anchor.position - origin), .center = boxCenter + shift, .halfSize = 0.5f * glm::vec2(width, height), .priority = priority, .featureId = label.featureId});

            pushLabelGlyphs(geometry, origin, anchor, font, codepoints, lineWidths, shift, labelIndex, label.featureId, true);
            pushLabelGlyphs(geometry, origin, anchor, font, codepoints, lineWidths, shift, labelIndex, label.featureId, false);
        }
    }

    MeshBuild buildVectorMesh(VectorGeometry geometry)
    {
        std::vector<MeshLabel> labels = std::move(geometry.labels);
        MeshBuild build = buildMesh(std::move(geometry.vertices), std::move(geometry.indices), {}, {.lods = false, .meshlets = false});
        if (build.isValid())
            build.data->labels = std::move(labels);
        return build;
    }
}
