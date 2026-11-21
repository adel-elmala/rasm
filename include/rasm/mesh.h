// clang-format off
#pragma once

#include "glm/glm.hpp"

#include "rasm/animation.h"
#include "rasm/material.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace tinygltf { class Model; }

namespace rasm
{

    constexpr uint32_t MAX_MESHES              = 8;
    constexpr uint32_t MAX_VERTEX_UV_SETS      = 3;
    constexpr uint32_t MAX_MESH_LODS           = 8;

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

    struct MeshLod
    {
        std::vector<uint32_t>   indices;
        float                   error       = 0.0f;
    };

    struct ObjRaw
    {
        std::vector<Vertex>     vertices;
        std::vector<uint32_t>   indices;
        std::vector<MeshLod>    lods;
        std::vector<uint32_t>   colors;
        std::vector<SkinVertex> skin;
    };

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
    };

    struct GLTFRaw
    {
        std::vector<SubGLTFRaw>                         subGltfRaws;
        std::shared_ptr<tinygltf::Model>                model;
        std::unordered_map<uint64_t, Format>            ktxFormats;     // KHR_texture_basisu: getKtxSlotKey(image, slot) → format
        std::unordered_map<uint64_t, TextureRaw>        ktxImages;      // getKtxImageKey(image, format) → the transcoded levels
        AnimationRaw                                    animation;
        bool                                            loaded          = false;
    };

    enum class MeshState
    {
        LOADING,
        READY,
        FAILED,
    };

    struct MeshGeometry
    {
        uint32_t            firstVertex     = 0;
        uint32_t            vertexCount     = 0;
        uint32_t            firstIndex      = 0;
        uint32_t            indexCount      = 0;
        uint32_t            lodIndexCount   = 0;
        uint32_t            firstColor      = 0;
        uint32_t            colorCount      = 0;
        uint32_t            firstSkin       = 0;
        uint32_t            skinCount       = 0;
        uint32_t            geometryIndex   = 0;
        glm::vec3           boundsCenter    = glm::vec3(0.0f);
        float               boundsRadius    = 0.0f;
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
    };

    struct MeshSize
    {
        uint64_t verticesByteSize;
        uint64_t indicesByteSize;
    };

    // Materials and textures created while importing one glTF model, so primitives that share a material
    // (and materials that share an image) create it once. Textures are keyed by (image index << 1 | sRGB).
    struct GltfImportCache
    {
        std::unordered_map<int, MaterialHandle>                 materials;
        std::unordered_map<uint64_t, TextureHandle>             imageTextures;
        const GLTFRaw*                                          raw             = nullptr;
    };

}
// clang-format on
