// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/mesh.h"
#include "rasm/types.h"

#include <cstddef>
#include <cstdint>

namespace rasm
{

    class RenderContext;

    class GeometryPool
    {
    public:
        bool                    create(RenderContext& ctx, const EngineConfig& config);
        bool                    fits(size_t vertexCount, size_t indexCount, size_t subMeshCount) const;
        bool                    upload(RenderContext& ctx, const ObjRaw& raw, MeshGeometry& geometry);
        BufferHandle            vertexBuffer() const;
        BufferHandle            indexBuffer() const;
        BufferHandle            tableBuffer() const;
        uint32_t                vertexCount() const;
        uint32_t                indexCount() const;

    private:
        BufferHandle            vertices                = {};
        BufferHandle            indices                 = {};
        BufferHandle            table                   = {};
        uint32_t                vertexCapacity          = 0;
        uint32_t                indexCapacity           = 0;
        uint32_t                subMeshCapacity         = 0;
        uint32_t                usedVertices            = 0;
        uint32_t                usedIndices             = 0;
        uint32_t                usedSubMeshes           = 0;
    };

}
// clang-format on
