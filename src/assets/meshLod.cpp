#include "assets/meshLod.h"

#include "meshoptimizer.h"
#include "tracy/Tracy.hpp"

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rasm
{
    namespace
    {
        // Below this many triangles a coarser level saves too little to be worth its indices.
        constexpr size_t MIN_LOD_TRIANGLES = 1024;
        constexpr size_t MIN_LAST_LOD_TRIANGLES = 64;

        // Simplification only collapses edges between triangles that share vertices, and OBJ import gives every
        // corner its own vertex, so identical vertices are merged first.
        void weldVertices(ObjRaw &raw)
        {
            std::vector<uint32_t> remap(raw.vertices.size());
            const size_t uniqueCount = meshopt_generateVertexRemap(remap.data(), raw.indices.data(), raw.indices.size(), raw.vertices.data(), raw.vertices.size(), sizeof(Vertex));
            if (uniqueCount == raw.vertices.size())
                return;

            std::vector<Vertex> vertices(uniqueCount);
            meshopt_remapVertexBuffer(vertices.data(), raw.vertices.data(), raw.vertices.size(), sizeof(Vertex), remap.data());
            meshopt_remapIndexBuffer(raw.indices.data(), raw.indices.data(), raw.indices.size(), remap.data());
            raw.vertices = std::move(vertices);
        }
    }

    // Each level has about half the triangles of the one before and reuses the mesh's vertices, so the levels
    // cost only their indices. A level is simplified from the one before; its error adds up the errors of the
    // steps, which bounds its distance from LOD 0.
    void buildMeshLods(ObjRaw &raw)
    {
        ZoneScoped;

        raw.lods.clear();
        if (raw.indices.size() < MIN_LOD_TRIANGLES * 3)
            return;
        weldVertices(raw);

        // previous points into raw.lods, so it must not reallocate.
        raw.lods.reserve(MAX_MESH_LODS - 1);
        const std::vector<uint32_t> *previous = &raw.indices;
        float error = 0.0f;
        while (raw.lods.size() + 1 < MAX_MESH_LODS)
        {
            const size_t target = previous->size() / 6 * 3;
            if (target < MIN_LAST_LOD_TRIANGLES * 3)
                break;

            MeshLod lod;
            lod.indices.resize(previous->size());
            float stepError = 0.0f;
            const size_t count = meshopt_simplify(lod.indices.data(), previous->data(), previous->size(), &raw.vertices[0].pos.x, raw.vertices.size(), sizeof(Vertex),
                                                  target, std::numeric_limits<float>::max(), meshopt_SimplifyErrorAbsolute, &stepError);
            // A level that removes under a tenth of the triangles is not worth drawing instead of the one before.
            if (count == 0 || count * 10 > previous->size() * 9)
                break;

            lod.indices.resize(count);
            lod.indices.shrink_to_fit();
            error += stepError;
            lod.error = error;
            raw.lods.push_back(std::move(lod));
            previous = &raw.lods.back().indices;
        }
    }
}
