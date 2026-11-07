#include "renderer/geometryPool.h"
#include "rasm/render/renderContext.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"

#include <algorithm>
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
    }

    // The mega vertex buffer is written with Vertex and read in the shader as gfx::VertexFull
    // (scalar layout); the two must not drift apart.
    static_assert(sizeof(Vertex) == sizeof(gfx::VertexFull), "Vertex must match the shader's VertexFull");
    static_assert(offsetof(Vertex, uv) == offsetof(gfx::VertexFull, uv), "Vertex must match the shader's VertexFull");
    static_assert(offsetof(Vertex, tangent) == offsetof(gfx::VertexFull, tangent), "Vertex must match the shader's VertexFull");

    // Every mesh lives in these buffers, so its CPU copy can go after upload (P6). Their size is fixed for the
    // engine's lifetime, so apps size them through EngineConfig.
    bool GeometryPool::create(RenderContext &ctx, const EngineConfig &config)
    {
        if (config.geometryPoolVertices == 0 || config.geometryPoolIndices == 0 || config.geometryPoolSubMeshes == 0)
        {
            spdlog::error("The geometry pool needs room for at least one vertex, one index and one sub-mesh.");
            return false;
        }

        vertexCapacity = config.geometryPoolVertices;
        indexCapacity = config.geometryPoolIndices;
        subMeshCapacity = config.geometryPoolSubMeshes;
        vertices = ctx.createBuffer({.name = "Geometry Vertex Pool", .usage = BufferUsage::STORAGE, .size = static_cast<uint64_t>(vertexCapacity) * sizeof(Vertex)});
        indices = ctx.createBuffer({.name = "Geometry Index Pool", .usage = BufferUsage::INDEX, .size = static_cast<uint64_t>(indexCapacity) * sizeof(uint32_t)});
        table = ctx.createBuffer({.name = "Geometry Table", .usage = BufferUsage::STORAGE, .size = static_cast<uint64_t>(subMeshCapacity) * sizeof(gfx::GeometryData)});
        if (!vertices.isValid() || !indices.isValid() || !table.isValid())
        {
            spdlog::error("Failed to create the geometry pool.");
            return false;
        }
        return true;
    }

    bool GeometryPool::fits(size_t vertexCount, size_t indexCount, size_t subMeshCount) const
    {
        if (vertexCount <= vertexCapacity - usedVertices && indexCount <= indexCapacity - usedIndices && subMeshCount <= subMeshCapacity - usedSubMeshes)
            return true;
        spdlog::error("The geometry pool is full: {} vertices, {} indices and {} sub-meshes do not fit ({}/{} vertices, {}/{} indices and {}/{} sub-meshes in use). "
                      "EngineConfig::geometryPoolVertices, geometryPoolIndices and geometryPoolSubMeshes set its size.",
                      vertexCount, indexCount, subMeshCount, usedVertices, vertexCapacity, usedIndices, indexCapacity, usedSubMeshes, subMeshCapacity);
        return false;
    }

    // Appends the sub-mesh and its table row. Nothing is ever freed: meshes are not destroyed yet (D4). No frame has
    // read the new row yet, so the table needs one copy, not one per frame slot.
    bool GeometryPool::upload(RenderContext &ctx, const ObjRaw &raw, MeshGeometry &geometry)
    {
        if (!fits(raw.vertices.size(), raw.indices.size(), 1))
            return false;

        geometry = {.firstVertex = usedVertices,
                    .vertexCount = static_cast<uint32_t>(raw.vertices.size()),
                    .firstIndex = usedIndices,
                    .indexCount = static_cast<uint32_t>(raw.indices.size()),
                    .geometryIndex = usedSubMeshes};
        computeBounds(raw.vertices, geometry);

        if (!raw.vertices.empty() && !ctx.fillBuffer(vertices, raw.vertices.data(), raw.vertices.size() * sizeof(Vertex), static_cast<size_t>(geometry.firstVertex) * sizeof(Vertex)))
            return false;
        if (!raw.indices.empty() && !ctx.fillBuffer(indices, raw.indices.data(), raw.indices.size() * sizeof(uint32_t), static_cast<size_t>(geometry.firstIndex) * sizeof(uint32_t)))
            return false;

        const gfx::GeometryData row{.firstIndex = geometry.firstIndex,
                                    .indexCount = geometry.indexCount,
                                    .vertexOffset = static_cast<int32_t>(geometry.firstVertex),
                                    .boundsCenter = geometry.boundsCenter,
                                    .boundsRadius = geometry.boundsRadius};
        if (!ctx.fillBuffer(table, &row, sizeof(row), static_cast<size_t>(usedSubMeshes) * sizeof(row)))
            return false;

        usedVertices += geometry.vertexCount;
        usedIndices += geometry.indexCount;
        ++usedSubMeshes;
        return true;
    }

    BufferHandle GeometryPool::vertexBuffer() const
    {
        return vertices;
    }

    BufferHandle GeometryPool::indexBuffer() const
    {
        return indices;
    }

    BufferHandle GeometryPool::tableBuffer() const
    {
        return table;
    }

    uint32_t GeometryPool::vertexCount() const
    {
        return usedVertices;
    }

    uint32_t GeometryPool::indexCount() const
    {
        return usedIndices;
    }
}
