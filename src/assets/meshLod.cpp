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
        // meshopt_generateVertexRemapMulti compares at most 16 streams: the vertex, its color, its skin and one per
        // morph target.
        constexpr size_t MAX_WELD_STREAMS = 16;

        // Simplification only collapses edges between triangles that share vertices, and OBJ import gives every
        // corner its own vertex, so identical vertices are merged first. A vertex is identical only with the same
        // color, the same joints and weights and the same morph deltas too, when the mesh has them. Returns false when
        // the mesh has more morph targets than the streams can compare, and is not welded.
        bool weldVertices(ObjRaw &raw)
        {
            const size_t vertexCount = raw.vertices.size();
            const size_t morphTargets = raw.morphs.empty() ? 0 : raw.morphTargetCount;
            if (3 + morphTargets > MAX_WELD_STREAMS)
                return false;
            meshopt_Stream streams[MAX_WELD_STREAMS] = {{raw.vertices.data(), sizeof(Vertex), sizeof(Vertex)}};
            size_t streamCount = 1;
            if (!raw.colors.empty())
                streams[streamCount++] = {raw.colors.data(), sizeof(uint32_t), sizeof(uint32_t)};
            if (!raw.skin.empty())
                streams[streamCount++] = {raw.skin.data(), sizeof(SkinVertex), sizeof(SkinVertex)};
            for (size_t t = 0; t < morphTargets; ++t)
                streams[streamCount++] = {raw.morphs.data() + t * vertexCount, sizeof(MorphDelta), sizeof(MorphDelta)};
            std::vector<uint32_t> remap(vertexCount);
            const size_t uniqueCount = meshopt_generateVertexRemapMulti(remap.data(), raw.indices.data(), raw.indices.size(), vertexCount, streams, streamCount);
            if (uniqueCount == vertexCount)
                return true;

            std::vector<Vertex> vertices(uniqueCount);
            meshopt_remapVertexBuffer(vertices.data(), raw.vertices.data(), raw.vertices.size(), sizeof(Vertex), remap.data());
            meshopt_remapIndexBuffer(raw.indices.data(), raw.indices.data(), raw.indices.size(), remap.data());
            raw.vertices = std::move(vertices);
            if (!raw.colors.empty())
            {
                std::vector<uint32_t> colors(uniqueCount);
                meshopt_remapVertexBuffer(colors.data(), raw.colors.data(), raw.colors.size(), sizeof(uint32_t), remap.data());
                raw.colors = std::move(colors);
            }
            if (!raw.skin.empty())
            {
                std::vector<SkinVertex> skin(uniqueCount);
                meshopt_remapVertexBuffer(skin.data(), raw.skin.data(), raw.skin.size(), sizeof(SkinVertex), remap.data());
                raw.skin = std::move(skin);
            }
            if (morphTargets > 0)
            {
                std::vector<MorphDelta> morphs(morphTargets * uniqueCount);
                for (size_t t = 0; t < morphTargets; ++t)
                    meshopt_remapVertexBuffer(morphs.data() + t * uniqueCount, raw.morphs.data() + t * vertexCount, vertexCount, sizeof(MorphDelta), remap.data());
                raw.morphs = std::move(morphs);
            }
            return true;
        }
    }

    // Each level has about half the triangles of the one before and reuses the mesh's vertices, so the levels
    // cost only their indices. A level is simplified from the one before; its error adds up the errors of the
    // steps, which bounds its distance from LOD 0. Open borders stay as they are: sub-meshes that meet along one
    // pick their LODs on their own, and the seam must not crack. The levels are simplified in the base shape, without
    // the weights of a skin or the deltas of morph targets (D29).
    void buildMeshLods(ObjRaw &raw)
    {
        ZoneScoped;

        raw.lods.clear();
        if (raw.indices.size() < MIN_LOD_TRIANGLES * 3)
            return;
        if (!weldVertices(raw))
            return;

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
            // Sparse: a coarse level uses few of the vertices, so the work follows its indices, not the vertex count.
            const unsigned int options = meshopt_SimplifyLockBorder | meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute;
            const size_t count = meshopt_simplify(lod.indices.data(), previous->data(), previous->size(), &raw.vertices[0].pos.x, raw.vertices.size(), sizeof(Vertex),
                                                  target, std::numeric_limits<float>::max(), options, &stepError);
            // A level that removes under a tenth of the triangles is not worth drawing instead of the one before.
            if (count == 0 || count * 10 > previous->size() * 9)
                break;

            lod.indices.resize(count);
            lod.indices.shrink_to_fit();
            meshopt_optimizeVertexCache(lod.indices.data(), lod.indices.data(), count, raw.vertices.size());
            error += stepError;
            lod.error = error;
            raw.lods.push_back(std::move(lod));
            previous = &raw.lods.back().indices;
        }
    }
}
