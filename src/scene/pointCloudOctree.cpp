#include "rasm/pointCloudOctree.h"
#include "scene/pointCloudInternal.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace rasm
{
    namespace
    {
        struct SortedPoint
        {
            uint64_t code = 0;
            uint32_t index = 0;
        };

        struct NodeBox
        {
            glm::dvec3 low = glm::dvec3(0.0);
            glm::dvec3 high = glm::dvec3(0.0);
            bool empty = true;
        };

        struct Builder
        {
            const PointCloudOctreeDesc &desc;
            PointCloudOctree &octree;
            glm::dvec3 cubeLow = glm::dvec3(0.0);
            double cubeSize = 0.0;
            std::vector<SortedPoint> points;
            std::vector<SortedPoint> rest;
            uint64_t dropped = 0;
        };
    }

    namespace
    {
        uint32_t getOctant(uint64_t code, uint32_t level)
        {
            return static_cast<uint32_t>(code >> (3 * (MORTON_LEVELS - level - 1))) & 7u;
        }

        void growBox(NodeBox &box, const glm::dvec3 &low, const glm::dvec3 &high)
        {
            box.low = box.empty ? low : glm::min(box.low, low);
            box.high = box.empty ? high : glm::max(box.high, high);
            box.empty = false;
        }

        // The points of [begin, end), the cube's points the ancestors left, in Morton order; the node is made already.
        NodeBox buildNode(Builder &builder, uint32_t node, uint32_t level, uint32_t begin, uint32_t end)
        {
            const PointCloudOctreeDesc &desc = builder.desc;
            PointCloudOctree &octree = builder.octree;
            std::vector<SortedPoint> &points = builder.points;
            std::vector<SortedPoint> &rest = builder.rest;
            const uint32_t count = end - begin;
            const uint32_t cellsLog = static_cast<uint32_t>(std::countr_zero(desc.cells));
            std::vector<SortedPoint> own;
            uint32_t childBegin[8] = {};
            uint32_t childEnd[8] = {};
            bool leaf = count <= desc.maxNodePoints || level >= MORTON_LEVELS;
            if (leaf)
            {
                const uint32_t kept = std::min(count, MAX_POINT_CHUNK_POINTS);
                builder.dropped += count - kept;
                own.assign(points.begin() + begin, points.begin() + begin + kept);
            }
            else
            {
                // The grid: cells of level + log2(cells), coarser while it would keep more than maxNodePoints.
                uint32_t cellLevel = std::min(level + cellsLog, MORTON_LEVELS);
                for (; cellLevel > level + 1; --cellLevel)
                {
                    const uint32_t shift = 3 * (MORTON_LEVELS - cellLevel);
                    uint32_t runs = 1;
                    for (uint32_t i = begin + 1; i < end && runs <= desc.maxNodePoints; ++i)
                        runs += (points[i].code >> shift) != (points[i - 1].code >> shift) ? 1u : 0u;
                    if (runs <= desc.maxNodePoints)
                        break;
                }
                // Of each cell the point nearest its centre, so the sample is even; the others stay, in order.
                const uint32_t shift = 3 * (MORTON_LEVELS - cellLevel);
                const uint32_t cellUnits = 1u << (MORTON_LEVELS - cellLevel);
                const auto half = static_cast<int64_t>(cellUnits / 2);
                rest.clear();
                for (uint32_t run = begin; run < end;)
                {
                    uint32_t runEnd = run + 1;
                    while (runEnd < end && (points[runEnd].code >> shift) == (points[run].code >> shift))
                        ++runEnd;
                    uint32_t best = run;
                    int64_t bestDistance = INT64_MAX;
                    for (uint32_t i = run; i < runEnd; ++i)
                    {
                        const uint64_t code = points[i].code;
                        const int64_t dx = static_cast<int64_t>(compactMortonBits(code) & (cellUnits - 1)) - half;
                        const int64_t dy = static_cast<int64_t>(compactMortonBits(code >> 1) & (cellUnits - 1)) - half;
                        const int64_t dz = static_cast<int64_t>(compactMortonBits(code >> 2) & (cellUnits - 1)) - half;
                        const int64_t distance = dx * dx + dy * dy + dz * dz;
                        if (distance < bestDistance)
                        {
                            bestDistance = distance;
                            best = i;
                        }
                    }
                    for (uint32_t i = run; i < runEnd; ++i)
                    {
                        if (i == best)
                            own.push_back(points[i]);
                        else
                            rest.push_back(points[i]);
                    }
                    run = runEnd;
                }
                std::copy(rest.begin(), rest.end(), points.begin() + begin);
                const uint32_t restEnd = begin + static_cast<uint32_t>(rest.size());
                for (uint32_t i = begin; i < restEnd;)
                {
                    const uint32_t octant = getOctant(points[i].code, level);
                    uint32_t j = i + 1;
                    while (j < restEnd && getOctant(points[j].code, level) == octant)
                        ++j;
                    childBegin[octant] = i;
                    childEnd[octant] = j;
                    i = j;
                }
            }

            // Its points, as offsets from its cube's centre.
            NodeBox box;
            {
                PointCloudNode &record = octree.nodes[node];
                record.firstPoint = static_cast<uint32_t>(octree.positions.size());
                record.pointCount = static_cast<uint32_t>(own.size());
                const glm::dvec3 center = record.center;
                for (const SortedPoint &point : own)
                {
                    const glm::dvec3 &position = desc.positions[point.index];
                    growBox(box, position, position);
                    octree.positions.push_back(glm::vec3(position - center));
                    if (desc.colors != nullptr)
                        octree.colors.push_back(desc.colors[point.index]);
                    if (desc.scalars != nullptr)
                        octree.scalars.push_back(desc.scalars[point.index]);
                    octree.indices.push_back(point.index);
                }
                // A few points at the edge of the data measure far apart, but they are no surface to cover: a node is
                // never sparser than its parent, so its adaptive points never grow past its parent's. Fewer than 2 points
                // measure nothing (0): such a node takes its parent's spacing, or its grid's cell.
                record.spacing = measurePointSpacing(octree.positions.data() + record.firstPoint, record.pointCount);
                const float parentSpacing = record.parent != POINT_NODE_NONE ? octree.nodes[record.parent].spacing : 0.0f;
                if (!(record.spacing > 0.0f))
                    record.spacing = parentSpacing > 0.0f ? parentSpacing : static_cast<float>(2.0 * record.halfSize / static_cast<double>(desc.cells));
                else if (parentSpacing > 0.0f)
                    record.spacing = std::min(record.spacing, parentSpacing);
            }
            own.clear();
            own.shrink_to_fit();

            bool children = false;
            for (uint32_t octant = 0; octant < 8 && !leaf; ++octant)
            {
                if (childEnd[octant] == childBegin[octant])
                    continue;
                const auto child = static_cast<uint32_t>(octree.nodes.size());
                PointCloudNode record;
                const PointCloudNode &parent = octree.nodes[node];
                record.halfSize = 0.5 * parent.halfSize;
                record.center = parent.center + glm::dvec3((octant & 1u) != 0 ? record.halfSize : -record.halfSize, (octant & 2u) != 0 ? record.halfSize : -record.halfSize,
                                                           (octant & 4u) != 0 ? record.halfSize : -record.halfSize);
                record.parent = node;
                record.level = level + 1;
                octree.nodes.push_back(record);
                octree.nodes[node].children[octant] = child;
                children = true;
                const NodeBox childBox = buildNode(builder, child, level + 1, childBegin[octant], childEnd[octant]);
                if (!childBox.empty)
                    growBox(box, childBox.low, childBox.high);
            }

            // The sphere around the box, grown to hold each child's.
            PointCloudNode &record = octree.nodes[node];
            record.geometricError = children ? record.spacing : 0.0f;
            record.boundsCenter = box.empty ? record.center : 0.5 * (box.low + box.high);
            record.boundsRadius = box.empty ? 0.0 : 0.5 * glm::length(box.high - box.low);
            for (uint32_t octant = 0; octant < 8; ++octant)
            {
                const uint32_t child = record.children[octant];
                if (child != POINT_NODE_NONE)
                    record.boundsRadius = std::max(record.boundsRadius, glm::length(octree.nodes[child].boundsCenter - record.boundsCenter) + octree.nodes[child].boundsRadius);
            }
            return box;
        }
    }

    // Potree's octree (Schütz, Ohrhallinger and Wimmer 2020): the points sorted along a Morton curve of the cube around
    // them, so a node's points are a run of it and each grid cell of a node a run within that; each node keeps a point
    // of each cell its points take, and passes the others down to its children.
    bool buildPointCloudOctree(const PointCloudOctreeDesc &desc, PointCloudOctree &octree)
    {
        ZoneScoped;
        octree = {};
        if (desc.positions == nullptr || desc.count == 0)
        {
            spdlog::error("buildPointCloudOctree: no points.");
            return false;
        }
        if (desc.cells < 2 || !std::has_single_bit(desc.cells) || desc.maxNodePoints == 0 || desc.maxNodePoints > MAX_POINT_CHUNK_POINTS)
        {
            spdlog::error("buildPointCloudOctree: cells must be a power of two of 2 or more ({}), and maxNodePoints 1 to {} ({}).", desc.cells, MAX_POINT_CHUNK_POINTS, desc.maxNodePoints);
            return false;
        }
        glm::dvec3 low = desc.positions[0];
        glm::dvec3 high = desc.positions[0];
        for (uint32_t i = 0; i < desc.count; ++i)
        {
            const glm::dvec3 &p = desc.positions[i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || (desc.scalars != nullptr && !std::isfinite(desc.scalars[i])))
            {
                spdlog::error("buildPointCloudOctree: point {} is not finite.", i);
                return false;
            }
            low = glm::min(low, p);
            high = glm::max(high, p);
        }

        Builder builder{.desc = desc, .octree = octree};
        // A cube a little larger than the box, so the far side's points fall inside its last cell.
        const double extent = std::max(std::max(high.x - low.x, high.y - low.y), high.z - low.z);
        builder.cubeSize = std::max(extent * (1.0 + 1e-6), 1e-6);
        builder.cubeLow = 0.5 * (low + high) - glm::dvec3(0.5 * builder.cubeSize);
        const double scale = static_cast<double>(1u << MORTON_LEVELS) / builder.cubeSize;
        const double last = static_cast<double>((1u << MORTON_LEVELS) - 1);
        builder.points.resize(desc.count);
        for (uint32_t i = 0; i < desc.count; ++i)
        {
            const glm::dvec3 cell = glm::clamp(glm::floor((desc.positions[i] - builder.cubeLow) * scale), glm::dvec3(0.0), glm::dvec3(last));
            builder.points[i] = {.code = spreadMortonBits(static_cast<uint32_t>(cell.x)) | spreadMortonBits(static_cast<uint32_t>(cell.y)) << 1 |
                                         spreadMortonBits(static_cast<uint32_t>(cell.z)) << 2,
                                 .index = i};
        }
        std::sort(builder.points.begin(), builder.points.end(), [](const SortedPoint &a, const SortedPoint &b)
                  { return a.code != b.code ? a.code < b.code : a.index < b.index; });

        octree.positions.reserve(desc.count);
        octree.indices.reserve(desc.count);
        if (desc.colors != nullptr)
            octree.colors.reserve(desc.count);
        if (desc.scalars != nullptr)
            octree.scalars.reserve(desc.count);
        PointCloudNode root;
        root.halfSize = 0.5 * builder.cubeSize;
        root.center = builder.cubeLow + glm::dvec3(root.halfSize);
        octree.nodes.push_back(root);
        buildNode(builder, 0, 0, 0, desc.count);
        if (builder.dropped > 0)
            spdlog::warn("buildPointCloudOctree: {} points at the same place as {} others were left out: a node holds at most {}.", builder.dropped, MAX_POINT_CHUNK_POINTS, MAX_POINT_CHUNK_POINTS);
        return true;
    }

    PointCloudChunkDesc getPointCloudNodeChunk(const PointCloudOctree &octree, uint32_t node)
    {
        if (node >= octree.nodes.size())
        {
            spdlog::error("getPointCloudNodeChunk: node {} of {}.", node, octree.nodes.size());
            return {};
        }
        const PointCloudNode &record = octree.nodes[node];
        return {.positions = octree.positions.data() + record.firstPoint,
                .colors = octree.colors.empty() ? nullptr : octree.colors.data() + record.firstPoint,
                .scalars = octree.scalars.empty() ? nullptr : octree.scalars.data() + record.firstPoint,
                .count = record.pointCount,
                .spacing = record.spacing};
    }
}
