// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/mesh.h"
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
        BufferHandle            tableBuffer() const;
        GeometryPoolStats       stats() const;

    private:
        BufferHandle            vertices                = {};
        BufferHandle            indices                 = {};
        BufferHandle            colors                  = {};
        BufferHandle            skins                   = {};
        BufferHandle            morphs                  = {};
        BufferHandle            meshlets                = {};
        BufferHandle            meshletData             = {};       // with mesh shaders only
        BufferHandle            table                   = {};
        RangeAllocator          vertexRanges;
        RangeAllocator          indexRanges;
        RangeAllocator          colorRanges;
        RangeAllocator          skinRanges;
        RangeAllocator          morphRanges;
        RangeAllocator          meshletRanges;
        RangeAllocator          meshletDataRanges;
        RangeAllocator          subMeshRows;
        bool                    meshletsFullLogged      = false;
        bool                    meshletDataFullLogged   = false;
    };

}
// clang-format on
