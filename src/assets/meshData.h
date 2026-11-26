// clang-format off
#pragma once

#include "rasm/animation.h"
#include "rasm/mesh.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tinygltf { class Model; }

namespace rasm
{

    // A mesh's data on the CPU, before it goes into the geometry pool: what createMesh builds from the application's
    // arrays and the AssetLoader's imports produce. Private: applications pass plain arrays to createMesh.
    struct MeshLod
    {
        std::vector<uint32_t>   indices;
        float                   error       = 0.0f;
    };

    struct Meshlet
    {
        glm::vec3               center;
        float                   radius;
        glm::vec3               coneAxis;
        float                   coneCutoff;
        uint32_t                firstIndex;                 // in its LOD's indices
        uint32_t                indexCount;
        uint32_t                firstData;                  // in ObjRaw::meshletData: its vertices, then its triangles
        uint32_t                vertexCount;
    };

    struct ObjRaw
    {
        std::vector<Vertex>     vertices;
        std::vector<uint32_t>   indices;
        std::vector<MeshLod>    lods;
        std::vector<Meshlet>    meshlets;                   // LOD 0's, then each coarser LOD's
        std::vector<uint32_t>   lodMeshletCounts;           // per LOD, empty without meshlets
        std::vector<uint32_t>   meshletData;                // per meshlet: its vertices (the sub-mesh's), then its triangles (3 local indices, a byte each)
        std::vector<uint32_t>   colors;
        std::vector<SkinVertex> skin;
        std::vector<MorphDelta> morphs;                     // target-major: target t's delta of vertex v at t * vertices.size() + v
        uint32_t                morphTargetCount    = 0;
        std::vector<uint32_t>   points;                     // a point cloud or glyph chunk's words in the point pool
        uint32_t                pointCount          = 0;
        uint32_t                glyphCount          = 0;
    };

    // What a glTF or OBJ import gives the AssetLoader, which uploads it through the renderer.
    struct SubGLTFRaw
    {
        ObjRaw                  objRaw;
        glm::mat4               transform;
        int                     materialIndex;
        int                     node            = -1;       // the glTF node
        int                     animatedNode    = -1;       // in GLTFRaw::animation.nodes, -1 when no clip moves it
        int                     skin            = -1;       // the glTF skin of a skinned primitive
        uint32_t                firstJoint      = 0;        // in GLTFRaw::animation.joints
        uint32_t                jointCount      = 0;        // 0: not skinned
        std::vector<glm::vec4>  jointSpheres;               // per joint of its skin: the sphere around the vertices it moves (radius < 0: none)
        int32_t                 firstWeight     = -1;       // its node's first weight in GLTFRaw::animation.morphWeights, -1 without morph targets
    };

    struct GLTFRaw
    {
        std::vector<SubGLTFRaw>                                         subGltfRaws;
        std::shared_ptr<tinygltf::Model>                                model;
        std::unordered_map<uint64_t, Format>                            ktxFormats;     // KHR_texture_basisu: getKtxSlotKey(image, slot) → format
        std::unordered_map<uint64_t, std::shared_ptr<const TextureRaw>> ktxImages;      // getKtxImageKey(image, format) → the transcoded levels
        std::unordered_map<uint64_t, std::shared_ptr<const TextureRaw>> imageLevels;    // PNG, JPEG: getImageLevelsKey(image, slot) → levels with mips
        AnimationRaw                                                    animation;
        bool                                                            loaded          = false;
    };

    // Materials and textures created while importing one glTF model, so primitives that share a material
    // (and materials that share an image) create it once. Textures are keyed by image, sampler and format.
    struct GltfImportCache
    {
        std::unordered_map<int, MaterialHandle>                 materials;
        std::unordered_map<uint64_t, TextureHandle>             imageTextures;
        const GLTFRaw*                                          raw             = nullptr;
        std::unordered_set<const TextureRaw*>                   countedLevels;              // levels whose load budget bytes moved to a texture
        uint64_t                                                movedBytes      = 0;
    };

}
// clang-format on
