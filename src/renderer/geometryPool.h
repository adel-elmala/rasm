// clang-format off
#pragma once

#include "rasm/handle.h"
#include "assets/meshData.h"
#include "rasm/types.h"
#include "core/rangeAllocator.h"

#include <cstddef>
#include <cstdint>

namespace rasm
{

    class RenderContext;

    struct GeometryPoolStats
    {
        uint64_t                vertices                = 0;
        uint64_t                indices                 = 0;
        uint64_t                subMeshes               = 0;
        uint64_t                meshlets                = 0;
        uint64_t                points                  = 0;
        uint64_t                largestFreeVertices     = 0;
        uint64_t                largestFreeIndices      = 0;
    };

    // Every mesh's geometry in a few engine-wide buffers. Each stream hands out ranges with a RangeAllocator, so a destroyed
    // mesh's ranges and table rows are reused (Renderer::destroyMesh frees them once the GPU is done with them).
    class GeometryPool
    {
    public:
        bool                    create(RenderContext& ctx, const RendererConfig& config);
        bool                    check(const ObjRaw& raw) const;
        bool                    allocate(const ObjRaw& raw, MeshGeometry& geometry);
        bool                    write(RenderContext& ctx, const ObjRaw& raw, const MeshGeometry& geometry);
        void                    free(const MeshGeometry& geometry);
        void                    logFull(size_t subMeshCount) const;
        BufferHandle            vertexBuffer() const;
        BufferHandle            indexBuffer() const;
        BufferHandle            colorBuffer() const;
        BufferHandle            skinBuffer() const;
        BufferHandle            morphBuffer() const;
        BufferHandle            meshletBuffer() const;
        BufferHandle            meshletDataBuffer() const;
        BufferHandle            pointBuffer() const;
        BufferHandle            tableBuffer() const;
        GeometryPoolStats       stats() const;

    private:
        bool                    allocatePoints(const ObjRaw& raw, MeshGeometry& geometry);
        bool                    writePoints(RenderContext& ctx, const ObjRaw& raw, const MeshGeometry& geometry);
        BufferHandle            vertices                = {};
        BufferHandle            indices                 = {};
        BufferHandle            colors                  = {};
        BufferHandle            skins                   = {};
        BufferHandle            morphs                  = {};
        BufferHandle            meshlets                = {};
        BufferHandle            meshletData             = {};       // with mesh shaders only
        BufferHandle            points                  = {};       // point cloud chunks, in 8 B units
        BufferHandle            table                   = {};
        RangeAllocator          vertexRanges;
        RangeAllocator          indexRanges;
        RangeAllocator          colorRanges;
        RangeAllocator          skinRanges;
        RangeAllocator          morphRanges;
        RangeAllocator          meshletRanges;
        RangeAllocator          meshletDataRanges;
        RangeAllocator          pointRanges;
        RangeAllocator          subMeshRows;
        uint32_t                pointTriangles          = 0;        // the index range every chunk draws: 0, 1, 2, ..., a triangle a point
        bool                    pointTrianglesAllocated = false;
        bool                    pointTrianglesWritten   = false;
        bool                    meshletsFullLogged      = false;
        bool                    meshletDataFullLogged   = false;
    };

}
// clang-format on
