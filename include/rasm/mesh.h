// clang-format off
#pragma once

#include "glm/glm.hpp"

#include "rasm/animation.h"
#include "rasm/material.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rasm
{

    constexpr uint32_t MAX_VERTEX_UV_SETS      = 3;
    constexpr uint32_t MAX_MESH_LODS           = 8;
    constexpr uint32_t MAX_MORPH_TARGETS       = 256;      // per primitive: the Skin pass keeps a group's weights in shared memory

    struct Vertex
    {
        glm::vec3 pos;
        glm::vec3 normal;
        glm::vec2 uv[MAX_VERTEX_UV_SETS];
        glm::vec4 tangent;      // glTF TANGENT: xyz tangent, w = bitangent sign (±1). w = 0 means none; the shader reconstructs one.
    };

    uint32_t    packVertexColor(const glm::vec4& linearColor);

    // A vertex's JOINTS_0 / WEIGHTS_0: its 4 largest influences, the weights as UNORM16 that add up to 65535 exactly.
    struct SkinVertex
    {
        uint16_t    joints[4];      // in its skin's joint list
        uint16_t    weights[4];
    };

    // A morph target's change to one vertex: the position as floats, the normal and the tangent's xyz as half floats,
    // two per uint, the first in the low half (normal x y, normal z tangent x, tangent y z).
    struct MorphDelta
    {
        glm::vec3   position;
        uint32_t    normalTangent[3];
    };

    enum class MeshState
    {
        LOADING,
        READY,
        FAILED,
    };

    struct MeshGeometry
    {
        uint32_t            firstVertex      = 0;
        uint32_t            vertexCount      = 0;
        uint32_t            firstIndex       = 0;
        uint32_t            indexCount       = 0;
        uint32_t            lodIndexCount    = 0;
        uint32_t            firstColor       = 0;
        uint32_t            colorCount       = 0;
        uint32_t            firstSkin        = 0;
        uint32_t            skinCount        = 0;
        uint32_t            firstMorph       = 0;
        uint32_t            morphCount       = 0;        // MorphDeltas: vertexCount * morphTargetCount
        uint32_t            morphTargetCount = 0;
        uint32_t            firstMeshlet     = 0;
        uint32_t            meshletCount     = 0;        // of every LOD
        uint32_t            maxLodMeshlets   = 0;        // the most of one LOD: an instance's meshlet visibility bits
        uint32_t            firstMeshletData = 0;
        uint32_t            meshletDataCount = 0;        // 0 without mesh shaders
        uint32_t            firstPoint       = 0;        // a point cloud chunk's place in the point pool, in 8 B units
        uint32_t            pointUnits       = 0;
        uint32_t            pointCount       = 0;        // 0: not a point cloud chunk
        uint32_t            glyphCount       = 0;
        uint32_t            geometryIndex    = 0;
        bool                allocated        = false;    // it holds ranges of the geometry pool, freed with the mesh
        glm::vec3           boundsCenter     = glm::vec3(0.0f);
        float               boundsRadius     = 0.0f;
    };

    // A label of a mesh (addVectorLabels): its anchor in the mesh's space, and the box around its text on screen in
    // ems from the anchor (+y up), which collision places it by.
    struct MeshLabel
    {
        glm::vec3           anchor          = glm::vec3(0.0f);
        glm::vec2           center          = glm::vec2(0.0f);
        glm::vec2           halfSize        = glm::vec2(0.0f);
        float               priority        = 0.0f;
        uint32_t            featureId       = 0;
    };

    struct SubMesh
    {
        MeshGeometry        geometry;
        glm::mat4           transform;
        MaterialHandle      material;
        int32_t             animatedNode    = -1;
    };

    struct Mesh
    {
        MeshState                   state       = MeshState::LOADING;
        std::vector<SubMesh>        subMeshes;
        std::vector<AnimationClip>  animations;
        std::vector<std::string>    morphTargetNames;   // one per weight of an entity (setMorphWeights)
        std::vector<MaterialHandle> ownedMaterials;     // made by its glTF import: destroyed with it
        std::vector<TextureHandle>  ownedTextures;
        std::vector<MeshLabel>      labels;             // its labels, each a slot of every entity drawn with it
        std::vector<uint16_t>       pointOrder;         // a point cloud chunk's: each drawn point's index in its PointCloudChunkDesc
        MeshHandle                  glyphShape;
        uint32_t                    glyphChunks = 0;
        uint32_t                    users       = 0;    // entities that use it
    };

    // What buildMesh makes besides the vertices: LODs by mesh simplification, and meshlets for a sub-mesh of 1024
    // triangles or more. Leave both off for geometry the shader moves (vector features, terrain).
    struct MeshBuildOptions
    {
        bool                        lods        = true;
        bool                        meshlets    = true;
    };

    // A mesh's CPU data, checked and built by buildMesh on any thread: Renderer::createMesh(MeshBuild) only uploads it.
    class MeshBuild
    {
    public:
                                    MeshBuild();
                                    ~MeshBuild();
                                    MeshBuild(MeshBuild&& other) noexcept;
        MeshBuild&                  operator=(MeshBuild&& other) noexcept;
                                    MeshBuild(const MeshBuild&) = delete;
        MeshBuild&                  operator=(const MeshBuild&) = delete;
        // False when buildMesh refused the data (it logged why), or after createMesh took it.
        bool                        isValid() const;

        struct Data;
        std::unique_ptr<Data>       data;
    };

    // Checks the data (colors empty or one per vertex, whole triangles, indices in range) and builds what the options
    // ask for. Thread-safe: it touches no renderer, so an application builds its tiles' meshes on its own threads.
    MeshBuild                       buildMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices, std::vector<uint32_t> colors = {}, const MeshBuildOptions& options = {});

    struct MeshSize
    {
        uint64_t verticesByteSize;
        uint64_t indicesByteSize;
    };

}
// clang-format on
