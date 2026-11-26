#include "renderer/geometryPool.h"
#include "rasm/render/renderContext.h"
#include "renderer/uberMaterialTypes.h"
#include "rasm/pointCloud.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace rasm
{
    namespace
    {
        // A sphere around the bounding box: not the tightest, but one pass and good enough to cull with.
        void computeBounds(const std::vector<Vertex> &vertices, MeshGeometry &geometry)
        {
            if (vertices.empty())
                return;

            glm::vec3 minPos = vertices[0].pos;
            glm::vec3 maxPos = vertices[0].pos;
            for (const auto &vertex : vertices)
            {
                minPos = glm::min(minPos, vertex.pos);
                maxPos = glm::max(maxPos, vertex.pos);
            }

            geometry.boundsCenter = (minPos + maxPos) * 0.5f;
            float radius2 = 0.0f;
            for (const auto &vertex : vertices)
            {
                const glm::vec3 d = vertex.pos - geometry.boundsCenter;
                radius2 = std::max(radius2, glm::dot(d, d));
            }
            geometry.boundsRadius = glm::sqrt(radius2);
        }

        // The LODs the table row holds: LOD 0 and at most MAX_MESH_LODS - 1 coarser ones.
        size_t getLodCount(const ObjRaw &raw)
        {
            return std::min<size_t>(raw.lods.size() + 1, MAX_MESH_LODS);
        }

        // The meshlets of the LODs the row holds; raw.lodMeshletCounts is empty without meshlets.
        uint32_t getLodMeshletCount(const ObjRaw &raw, size_t lod)
        {
            return lod < raw.lodMeshletCounts.size() ? raw.lodMeshletCounts[lod] : 0u;
        }
    }

    // The mega vertex buffer is written with Vertex and read in the shader as gfx::VertexFull
    // (scalar layout); the two must not drift apart.
    static_assert(sizeof(Vertex) == sizeof(gfx::VertexFull), "Vertex must match the shader's VertexFull");
    static_assert(offsetof(Vertex, uv) == offsetof(gfx::VertexFull, uv), "Vertex must match the shader's VertexFull");
    static_assert(offsetof(Vertex, tangent) == offsetof(gfx::VertexFull, tangent), "Vertex must match the shader's VertexFull");
    static_assert(sizeof(SkinVertex) == sizeof(gfx::SkinVertex), "SkinVertex must match the shader's SkinVertex");
    static_assert(sizeof(MorphDelta) == sizeof(gfx::MorphDelta), "MorphDelta must match the shader's MorphDelta");

    // Every mesh lives in these buffers, so its CPU copy can go after upload (P6). Their size is fixed for the
    // engine's lifetime, so apps size them through RendererConfig. A vertex stays one 64-byte cache line: an attribute
    // that only some meshes have (vertex colors, a skinned vertex's joints and weights, morph targets) is a stream of its
    // own, and a sub-mesh without it costs nothing there. The sub-mesh's row says where its part of the stream starts,
    // or -1. The morph stream is target-major: the Skin pass reads one target for 64 neighbouring vertices at once, and
    // a target whose weight is 0 is not read at all.
    bool GeometryPool::create(RenderContext &ctx, const RendererConfig &config)
    {
        if (config.geometryPoolVertices == 0 || config.geometryPoolIndices == 0 || config.geometryPoolColors == 0 || config.geometryPoolSkins == 0 ||
            config.geometryPoolMorphs == 0 || config.geometryPoolSubMeshes == 0 || config.geometryPoolMeshlets == 0)
        {
            spdlog::error("The geometry pool needs room for at least one vertex, one index, one color, one skin, one morph delta, one meshlet and one sub-mesh.");
            return false;
        }

        vertexRanges.reset(config.geometryPoolVertices);
        indexRanges.reset(config.geometryPoolIndices);
        colorRanges.reset(config.geometryPoolColors);
        skinRanges.reset(config.geometryPoolSkins);
        morphRanges.reset(config.geometryPoolMorphs);
        meshletRanges.reset(config.geometryPoolMeshlets);
        // The mesh shaders read each meshlet's own vertices and triangles; without them only the index ranges are needed.
        meshletDataRanges.reset(ctx.hasMeshShaders() ? config.geometryPoolMeshletData : 0);
        pointRanges.reset(config.geometryPoolPoints);
        subMeshRows.reset(config.geometryPoolSubMeshes);
        vertices = ctx.createBuffer({.name = "Geometry Vertex Pool", .usage = BufferUsage::STORAGE, .size = static_cast<uint64_t>(config.geometryPoolVertices) * sizeof(Vertex), .staged = true});
        indices = ctx.createBuffer({.name = "Geometry Index Pool", .usage = BufferUsage::INDEX, .size = static_cast<uint64_t>(config.geometryPoolIndices) * sizeof(uint32_t), .staged = true});
        colors = ctx.createBuffer({.name = "Geometry Color Pool", .usage = BufferUsage::STORAGE, .size = static_cast<uint64_t>(config.geometryPoolColors) * sizeof(uint32_t), .staged = true});
        skins = ctx.createBuffer({.name = "Geometry Skin Pool", .usage = BufferUsage::STORAGE, .size = static_cast<uint64_t>(config.geometryPoolSkins) * sizeof(SkinVertex), .staged = true});
        morphs = ctx.createBuffer({.name = "Geometry Morph Pool", .usage = BufferUsage::STORAGE, .size = static_cast<uint64_t>(config.geometryPoolMorphs) * sizeof(MorphDelta), .staged = true});
        meshlets = ctx.createBuffer({.name = "Geometry Meshlet Pool", .usage = BufferUsage::STORAGE, .size = static_cast<uint64_t>(config.geometryPoolMeshlets) * sizeof(gfx::GeometryMeshlet), .staged = true});
        if (meshletDataRanges.capacity() > 0)
        {
            meshletData = ctx.createBuffer({.name = "Geometry Meshlet Data Pool", .usage = BufferUsage::STORAGE, .size = meshletDataRanges.capacity() * sizeof(uint32_t), .staged = true});
            if (!meshletData.isValid())
            {
                spdlog::error("Failed to create the meshlet data pool.");
                return false;
            }
        }
        table = ctx.createBuffer({.name = "Geometry Table", .usage = BufferUsage::STORAGE, .size = static_cast<uint64_t>(config.geometryPoolSubMeshes) * sizeof(gfx::GeometryData)});
        if (!vertices.isValid() || !indices.isValid() || !colors.isValid() || !skins.isValid() || !morphs.isValid() || !meshlets.isValid() || !table.isValid())
        {
            spdlog::error("Failed to create the geometry pool.");
            return false;
        }
        return true;
    }

    // The streams of a sub-mesh that does not add up are refused before any range is taken.
    bool GeometryPool::check(const ObjRaw &raw) const
    {
        if (raw.pointCount > 0 && (!raw.vertices.empty() || raw.points.size() % 2 != 0 || raw.points.size() < gfx::POINT_CHUNK_HEADER_WORDS + 2 * static_cast<size_t>(raw.pointCount)))
        {
            spdlog::error("A point cloud chunk of {} points has {} words and {} vertices; buildPointCloudChunk makes them.", raw.pointCount, raw.points.size(), raw.vertices.size());
            return false;
        }
        if (!raw.colors.empty() && raw.colors.size() != raw.vertices.size())
        {
            spdlog::error("A sub-mesh has {} colors for {} vertices.", raw.colors.size(), raw.vertices.size());
            return false;
        }
        if (!raw.skin.empty() && raw.skin.size() != raw.vertices.size())
        {
            spdlog::error("A sub-mesh has {} skins for {} vertices.", raw.skin.size(), raw.vertices.size());
            return false;
        }
        if (raw.morphs.size() != static_cast<size_t>(raw.morphTargetCount) * raw.vertices.size())
        {
            spdlog::error("A sub-mesh has {} morph deltas for {} targets of {} vertices.", raw.morphs.size(), raw.morphTargetCount, raw.vertices.size());
            return false;
        }
        size_t meshletCount = 0;
        for (uint32_t count : raw.lodMeshletCounts)
            meshletCount += count;
        if (!raw.lodMeshletCounts.empty() && (raw.lodMeshletCounts.size() != raw.lods.size() + 1 || meshletCount != raw.meshlets.size()))
        {
            spdlog::error("A sub-mesh has {} meshlets in {} LODs, and its LOD counts do not add up to them.", raw.meshlets.size(), raw.lods.size() + 1);
            return false;
        }
        return true;
    }

    // Takes a range of each stream the sub-mesh uses and a row of the table: all of them or none, so a mesh that does not
    // fit takes nothing. Returns false only when a stream has no free range that large. A range is free when no frame uses
    // it: Renderer::destroyMesh frees them after the GPU is done.
    bool GeometryPool::allocate(const ObjRaw &raw, MeshGeometry &geometry)
    {
        if (raw.pointCount > 0)
            return allocatePoints(raw, geometry);
        const size_t lodCount = getLodCount(raw);
        size_t lodIndexCount = 0;
        for (size_t i = 1; i < lodCount; ++i)
            lodIndexCount += raw.lods[i - 1].indices.size();
        uint32_t meshletCount = 0;
        uint32_t maxLodMeshlets = 0;
        for (size_t i = 0; i < lodCount; ++i)
        {
            meshletCount += getLodMeshletCount(raw, i);
            maxLodMeshlets = std::max(maxLodMeshlets, getLodMeshletCount(raw, i));
        }
        geometry = {.vertexCount = static_cast<uint32_t>(raw.vertices.size()),
                    .indexCount = static_cast<uint32_t>(raw.indices.size()),
                    .lodIndexCount = static_cast<uint32_t>(lodIndexCount),
                    .colorCount = static_cast<uint32_t>(raw.colors.size()),
                    .skinCount = static_cast<uint32_t>(raw.skin.size()),
                    .morphCount = static_cast<uint32_t>(raw.morphs.size()),
                    .morphTargetCount = raw.morphs.empty() ? 0 : raw.morphTargetCount,
                    .meshletCount = meshletCount,
                    .maxLodMeshlets = maxLodMeshlets};
        uint64_t firstVertex = 0;
        uint64_t firstIndex = 0;
        uint64_t firstColor = 0;
        uint64_t firstSkin = 0;
        uint64_t firstMorph = 0;
        uint64_t firstMeshlet = 0;
        uint64_t row = 0;
        const bool vertex = vertexRanges.allocate(geometry.vertexCount, firstVertex);
        const bool index = vertex && indexRanges.allocate(static_cast<uint64_t>(geometry.indexCount) + geometry.lodIndexCount, firstIndex);
        const bool color = index && colorRanges.allocate(geometry.colorCount, firstColor);
        const bool skin = color && skinRanges.allocate(geometry.skinCount, firstSkin);
        const bool morph = skin && morphRanges.allocate(geometry.morphCount, firstMorph);
        // Meshlets are optional: a sub-mesh whose meshlets do not fit is drawn whole, with the same triangles.
        if (morph && geometry.meshletCount > 0 && !meshletRanges.allocate(geometry.meshletCount, firstMeshlet))
        {
            if (!meshletsFullLogged)
                spdlog::warn("The meshlet pool is full ({}/{} meshlets, RendererConfig::geometryPoolMeshlets): a sub-mesh of {} meshlets is drawn whole.", meshletRanges.used(),
                             meshletRanges.capacity(), geometry.meshletCount);
            meshletsFullLogged = true;
            geometry.meshletCount = 0;
            geometry.maxLodMeshlets = 0;
        }
        uint64_t firstMeshletData = 0;
        const auto meshletDataCount = static_cast<uint32_t>(meshletDataRanges.capacity() > 0 && geometry.meshletCount > 0 ? raw.meshletData.size() : 0);
        if (morph && meshletDataCount > 0 && !meshletDataRanges.allocate(meshletDataCount, firstMeshletData))
        {
            if (!meshletDataFullLogged)
                spdlog::warn("The meshlet data pool is full ({}/{} words, RendererConfig::geometryPoolMeshletData): a sub-mesh of {} meshlets is drawn whole.", meshletDataRanges.used(),
                             meshletDataRanges.capacity(), geometry.meshletCount);
            meshletDataFullLogged = true;
            meshletRanges.free(firstMeshlet, geometry.meshletCount);
            geometry.meshletCount = 0;
            geometry.maxLodMeshlets = 0;
        }
        else
            geometry.meshletDataCount = meshletDataCount;
        const bool tableRow = morph && subMeshRows.allocate(1, row);
        if (!tableRow)
        {
            if (morph)
            {
                meshletRanges.free(firstMeshlet, geometry.meshletCount);
                meshletDataRanges.free(firstMeshletData, geometry.meshletDataCount);
            }
            if (vertex)
                vertexRanges.free(firstVertex, geometry.vertexCount);
            if (index)
                indexRanges.free(firstIndex, static_cast<uint64_t>(geometry.indexCount) + geometry.lodIndexCount);
            if (color)
                colorRanges.free(firstColor, geometry.colorCount);
            if (skin)
                skinRanges.free(firstSkin, geometry.skinCount);
            if (morph)
                morphRanges.free(firstMorph, geometry.morphCount);
            return false;
        }
        geometry.firstVertex = static_cast<uint32_t>(firstVertex);
        geometry.firstIndex = static_cast<uint32_t>(firstIndex);
        geometry.firstColor = static_cast<uint32_t>(firstColor);
        geometry.firstSkin = static_cast<uint32_t>(firstSkin);
        geometry.firstMorph = static_cast<uint32_t>(firstMorph);
        geometry.firstMeshlet = static_cast<uint32_t>(firstMeshlet);
        geometry.firstMeshletData = static_cast<uint32_t>(firstMeshletData);
        geometry.geometryIndex = static_cast<uint32_t>(row);
        geometry.allocated = true;
        computeBounds(raw.vertices, geometry);
        return true;
    }

    // A point cloud chunk takes a range of the point pool and a row; its indices are the triangles every chunk shares, taken
    // from the index pool with the first chunk and kept. Its sphere is the box's.
    bool GeometryPool::allocatePoints(const ObjRaw &raw, MeshGeometry &geometry)
    {
        if (!pointTrianglesAllocated)
        {
            uint64_t first = 0;
            if (!indexRanges.allocate(gfx::POINT_INDICES * static_cast<uint64_t>(MAX_POINT_CHUNK_POINTS), first))
                return false;
            pointTriangles = static_cast<uint32_t>(first);
            pointTrianglesAllocated = true;
        }
        const auto units = static_cast<uint32_t>(raw.points.size() / 2);
        uint64_t firstPoint = 0;
        uint64_t row = 0;
        if (!pointRanges.allocate(units, firstPoint))
            return false;
        if (!subMeshRows.allocate(1, row))
        {
            pointRanges.free(firstPoint, units);
            return false;
        }
        gfx::PointChunkHeader header{};
        std::memcpy(&header, raw.points.data(), sizeof(header));
        const glm::vec3 size = header.boxStep * 65535.0f;
        geometry = {.firstIndex = pointTriangles,
                    .indexCount = gfx::POINT_INDICES * raw.pointCount,
                    .firstPoint = static_cast<uint32_t>(firstPoint),
                    .pointUnits = units,
                    .pointCount = raw.pointCount,
                    .geometryIndex = static_cast<uint32_t>(row),
                    .allocated = true,
                    .boundsCenter = header.boxMin + size * 0.5f,
                    .boundsRadius = glm::length(size) * 0.5f};
        return true;
    }

    void GeometryPool::free(const MeshGeometry &geometry)
    {
        if (!geometry.allocated)
            return;
        if (geometry.pointCount > 0)
        {
            pointRanges.free(geometry.firstPoint, geometry.pointUnits);
            subMeshRows.free(geometry.geometryIndex, 1);
            return;
        }
        vertexRanges.free(geometry.firstVertex, geometry.vertexCount);
        indexRanges.free(geometry.firstIndex, static_cast<uint64_t>(geometry.indexCount) + geometry.lodIndexCount);
        colorRanges.free(geometry.firstColor, geometry.colorCount);
        skinRanges.free(geometry.firstSkin, geometry.skinCount);
        morphRanges.free(geometry.firstMorph, geometry.morphCount);
        meshletRanges.free(geometry.firstMeshlet, geometry.meshletCount);
        meshletDataRanges.free(geometry.firstMeshletData, geometry.meshletDataCount);
        subMeshRows.free(geometry.geometryIndex, 1);
    }

    void GeometryPool::logFull(size_t subMeshCount) const
    {
        spdlog::error("The geometry pool is full: a mesh of {} sub-meshes does not fit. In use: {}/{} vertices, {}/{} indices, {}/{} colors, {}/{} skins, {}/{} "
                      "morph deltas, {}/{} meshlets, {}/{} point units, {}/{} sub-meshes; the largest free ranges are {} vertices and {} indices. RendererConfig::geometryPoolVertices, "
                      "geometryPoolIndices, geometryPoolColors, geometryPoolSkins, geometryPoolMorphs, geometryPoolMeshlets, geometryPoolPoints and geometryPoolSubMeshes set its size.",
                      subMeshCount, vertexRanges.used(), vertexRanges.capacity(), indexRanges.used(), indexRanges.capacity(), colorRanges.used(), colorRanges.capacity(),
                      skinRanges.used(), skinRanges.capacity(), morphRanges.used(), morphRanges.capacity(), meshletRanges.used(), meshletRanges.capacity(), pointRanges.used(), pointRanges.capacity(), subMeshRows.used(),
                      subMeshRows.capacity(), vertexRanges.largestFree(), indexRanges.largestFree());
    }

    // Writes the sub-mesh's streams and its table row: the vertices, then LOD 0's indices followed by the coarser LODs'.
    // No frame reads the ranges or the row before this (a reused one only after the GPU finished every frame that read
    // its last mesh), so one copy of the table is enough.
    bool GeometryPool::write(RenderContext &ctx, const ObjRaw &raw, const MeshGeometry &geometry)
    {
        if (geometry.pointCount > 0)
            return writePoints(ctx, raw, geometry);
        const size_t lodCount = getLodCount(raw);
        gfx::GeometryData row{.vertexOffset = static_cast<int32_t>(geometry.firstVertex),
                              .boundsCenter = geometry.boundsCenter,
                              .boundsRadius = geometry.boundsRadius,
                              .lodCount = static_cast<uint32_t>(lodCount),
                              .colorOffset = geometry.colorCount > 0 ? static_cast<int32_t>(geometry.firstColor) : -1,
                              .skinOffset = geometry.skinCount > 0 ? static_cast<int32_t>(geometry.firstSkin) : -1,
                              .morphOffset = geometry.morphCount > 0 ? static_cast<int32_t>(geometry.firstMorph) : -1,
                              .morphTargetCount = geometry.morphTargetCount};
        // A sub-mesh whose meshlets did not fit the pool (allocate) has none.
        const bool hasMeshlets = geometry.meshletCount > 0;
        row.lods[0] = {.firstIndex = geometry.firstIndex, .indexCount = geometry.indexCount, .error = 0.0f, .firstMeshlet = geometry.firstMeshlet, .meshletCount = hasMeshlets ? getLodMeshletCount(raw, 0) : 0u};
        uint32_t nextIndex = geometry.firstIndex + geometry.indexCount;
        uint32_t nextMeshlet = geometry.firstMeshlet + row.lods[0].meshletCount;
        for (size_t i = 1; i < lodCount; ++i)
        {
            const MeshLod &lod = raw.lods[i - 1];
            row.lods[i] = {.firstIndex = nextIndex, .indexCount = static_cast<uint32_t>(lod.indices.size()), .error = lod.error, .firstMeshlet = nextMeshlet, .meshletCount = hasMeshlets ? getLodMeshletCount(raw, i) : 0u};
            nextIndex += row.lods[i].indexCount;
            nextMeshlet += row.lods[i].meshletCount;
        }

        // A meshlet's indices are a range of its LOD's, which start at the LOD's place in the index pool.
        if (geometry.meshletCount > 0)
        {
            std::vector<gfx::GeometryMeshlet> meshletRows;
            meshletRows.reserve(geometry.meshletCount);
            size_t source = 0;
            for (size_t i = 0; i < lodCount; ++i)
            {
                for (uint32_t m = 0; m < row.lods[i].meshletCount; ++m)
                {
                    const Meshlet &meshlet = raw.meshlets[source++];
                    meshletRows.push_back({.center = meshlet.center,
                                           .radius = meshlet.radius,
                                           .coneAxis = meshlet.coneAxis,
                                           .coneCutoff = meshlet.coneCutoff,
                                           .firstIndex = row.lods[i].firstIndex + meshlet.firstIndex,
                                           .indexCount = meshlet.indexCount,
                                           .firstData = geometry.firstMeshletData + meshlet.firstData,
                                           .vertexCount = meshlet.vertexCount});
                }
            }
            if (!ctx.fillBuffer(meshlets, meshletRows.data(), meshletRows.size() * sizeof(gfx::GeometryMeshlet), static_cast<size_t>(geometry.firstMeshlet) * sizeof(gfx::GeometryMeshlet)))
                return false;
            if (geometry.meshletDataCount > 0 &&
                !ctx.fillBuffer(meshletData, raw.meshletData.data(), static_cast<size_t>(geometry.meshletDataCount) * sizeof(uint32_t), static_cast<size_t>(geometry.firstMeshletData) * sizeof(uint32_t)))
                return false;
        }

        if (!raw.vertices.empty() && !ctx.fillBuffer(vertices, raw.vertices.data(), raw.vertices.size() * sizeof(Vertex), static_cast<size_t>(geometry.firstVertex) * sizeof(Vertex)))
            return false;
        if (!raw.colors.empty() && !ctx.fillBuffer(colors, raw.colors.data(), raw.colors.size() * sizeof(uint32_t), static_cast<size_t>(geometry.firstColor) * sizeof(uint32_t)))
            return false;
        if (!raw.skin.empty() && !ctx.fillBuffer(skins, raw.skin.data(), raw.skin.size() * sizeof(SkinVertex), static_cast<size_t>(geometry.firstSkin) * sizeof(SkinVertex)))
            return false;
        if (!raw.morphs.empty() &&
            !ctx.fillBuffer(morphs, raw.morphs.data(), raw.morphs.size() * sizeof(MorphDelta), static_cast<size_t>(geometry.firstMorph) * sizeof(MorphDelta)))
            return false;
        if (!raw.indices.empty() && !ctx.fillBuffer(indices, raw.indices.data(), raw.indices.size() * sizeof(uint32_t), static_cast<size_t>(geometry.firstIndex) * sizeof(uint32_t)))
            return false;
        for (size_t i = 1; i < lodCount; ++i)
        {
            const std::vector<uint32_t> &lodIndices = raw.lods[i - 1].indices;
            if (!lodIndices.empty() && !ctx.fillBuffer(indices, lodIndices.data(), lodIndices.size() * sizeof(uint32_t), static_cast<size_t>(row.lods[i].firstIndex) * sizeof(uint32_t)))
                return false;
        }
        return ctx.fillBuffer(table, &row, sizeof(row), static_cast<size_t>(geometry.geometryIndex) * sizeof(row));
    }

    // The shared triangles (0, 1, 2, ...) go in with the first chunk. The chunk's row has one LOD, the whole chunk, which culling cuts to a
    // prefix (cull.slang), and its vertexOffset is the chunk's first unit in the point pool.
    bool GeometryPool::writePoints(RenderContext &ctx, const ObjRaw &raw, const MeshGeometry &geometry)
    {
        // The point pool is made with the first chunk, so a renderer that draws no point cloud has none.
        if (!points.isValid())
        {
            points = ctx.createBuffer({.name = "Geometry Point Pool", .usage = BufferUsage::STORAGE, .size = pointRanges.capacity() * 2 * sizeof(uint32_t), .staged = true});
            if (!points.isValid())
            {
                spdlog::error("Failed to create the point pool of {} MiB (RendererConfig::geometryPoolPoints).", pointRanges.capacity() * 2 * sizeof(uint32_t) >> 20);
                return false;
            }
        }
        if (!pointTrianglesWritten)
        {
            std::vector<uint32_t> triangles(gfx::POINT_INDICES * static_cast<size_t>(MAX_POINT_CHUNK_POINTS));
            for (size_t i = 0; i < triangles.size(); ++i)
                triangles[i] = static_cast<uint32_t>(i);
            if (!ctx.fillBuffer(indices, triangles.data(), triangles.size() * sizeof(uint32_t), static_cast<size_t>(pointTriangles) * sizeof(uint32_t)))
                return false;
            pointTrianglesWritten = true;
        }
        if (!ctx.fillBuffer(points, raw.points.data(), raw.points.size() * sizeof(uint32_t), static_cast<size_t>(geometry.firstPoint) * 2 * sizeof(uint32_t)))
            return false;
        gfx::GeometryData row{.vertexOffset = static_cast<int32_t>(geometry.firstPoint),
                              .boundsCenter = geometry.boundsCenter,
                              .boundsRadius = geometry.boundsRadius,
                              .lodCount = 1,
                              .colorOffset = -1,
                              .skinOffset = -1,
                              .morphOffset = -1,
                              .morphTargetCount = 0};
        row.lods[0] = {.firstIndex = geometry.firstIndex, .indexCount = geometry.indexCount, .error = 0.0f, .firstMeshlet = 0, .meshletCount = 0};
        return ctx.fillBuffer(table, &row, sizeof(row), static_cast<size_t>(geometry.geometryIndex) * sizeof(row));
    }

    BufferHandle GeometryPool::pointBuffer() const
    {
        return points;
    }

    BufferHandle GeometryPool::vertexBuffer() const
    {
        return vertices;
    }

    BufferHandle GeometryPool::indexBuffer() const
    {
        return indices;
    }

    BufferHandle GeometryPool::colorBuffer() const
    {
        return colors;
    }

    BufferHandle GeometryPool::skinBuffer() const
    {
        return skins;
    }

    BufferHandle GeometryPool::morphBuffer() const
    {
        return morphs;
    }

    BufferHandle GeometryPool::meshletBuffer() const
    {
        return meshlets;
    }

    BufferHandle GeometryPool::meshletDataBuffer() const
    {
        return meshletData;
    }

    BufferHandle GeometryPool::tableBuffer() const
    {
        return table;
    }

    GeometryPoolStats GeometryPool::stats() const
    {
        return {.vertices = vertexRanges.used(),
                .indices = indexRanges.used(),
                .subMeshes = subMeshRows.used(),
                .meshlets = meshletRanges.used(),
                .points = pointRanges.used(),
                .largestFreeVertices = vertexRanges.largestFree(),
                .largestFreeIndices = indexRanges.largestFree()};
    }
}
