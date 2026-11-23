#include "assets/meshlets.h"

#include "meshoptimizer.h"
#include "tracy/Tracy.hpp"

#include <vector>

namespace rasm
{
    namespace
    {
        // meshopt_buildMeshlets weighs small bounds against narrow normal cones; 0.25 is its suggested balance for cone
        // culling.
        constexpr float MESHLET_CONE_WEIGHT = 0.25f;

        // Splits one LOD's triangles into meshlets and writes the LOD's indices again in meshlet order, so each meshlet is
        // one range of them: the same triangles, which a whole draw of the LOD still draws. Returns the meshlet count.
        uint32_t buildLodMeshlets(std::vector<uint32_t> &indices, const std::vector<Vertex> &vertices, std::vector<Meshlet> &out)
        {
            const size_t maxMeshlets = meshopt_buildMeshletsBound(indices.size(), MESHLET_MAX_VERTICES, MESHLET_MAX_TRIANGLES);
            std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
            std::vector<unsigned int> meshletVertices(indices.size());
            std::vector<unsigned char> meshletTriangles(indices.size());
            const size_t count = meshopt_buildMeshlets(meshlets.data(), meshletVertices.data(), meshletTriangles.data(), indices.data(), indices.size(), &vertices[0].pos.x,
                                                       vertices.size(), sizeof(Vertex), MESHLET_MAX_VERTICES, MESHLET_MAX_TRIANGLES, MESHLET_CONE_WEIGHT);

            std::vector<uint32_t> ordered;
            ordered.reserve(indices.size());
            for (size_t i = 0; i < count; ++i)
            {
                const meshopt_Meshlet &meshlet = meshlets[i];
                unsigned int *localVertices = meshletVertices.data() + meshlet.vertex_offset;
                unsigned char *localTriangles = meshletTriangles.data() + meshlet.triangle_offset;
                meshopt_optimizeMeshlet(localVertices, localTriangles, meshlet.triangle_count, meshlet.vertex_count);
                const meshopt_Bounds bounds = meshopt_computeMeshletBounds(localVertices, localTriangles, meshlet.triangle_count, &vertices[0].pos.x, vertices.size(), sizeof(Vertex));

                const auto first = static_cast<uint32_t>(ordered.size());
                for (size_t k = 0; k < static_cast<size_t>(meshlet.triangle_count) * 3; ++k)
                    ordered.push_back(localVertices[localTriangles[k]]);
                out.push_back({.center = glm::vec3(bounds.center[0], bounds.center[1], bounds.center[2]),
                               .radius = bounds.radius,
                               .coneAxis = glm::vec3(bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]),
                               .coneCutoff = bounds.cone_cutoff,
                               .firstIndex = first,
                               .indexCount = meshlet.triangle_count * 3});
            }
            indices = std::move(ordered);
            return static_cast<uint32_t>(count);
        }
    }

    // Every LOD of a big sub-mesh gets meshlets of its own, after the LODs are built, on the same worker job. A skinned or
    // morphed sub-mesh gets none: its vertices move every frame, so bounds made here would not hold.
    void buildMeshlets(ObjRaw &raw)
    {
        ZoneScoped;

        raw.meshlets.clear();
        raw.lodMeshletCounts.clear();
        if (!raw.skin.empty() || !raw.morphs.empty() || raw.vertices.empty() || raw.indices.size() < static_cast<size_t>(MIN_MESHLET_TRIANGLES) * 3)
            return;

        raw.lodMeshletCounts.push_back(buildLodMeshlets(raw.indices, raw.vertices, raw.meshlets));
        for (MeshLod &lod : raw.lods)
            raw.lodMeshletCounts.push_back(lod.indices.empty() ? 0u : buildLodMeshlets(lod.indices, raw.vertices, raw.meshlets));
    }
}
