// clang-format off
#pragma once

#include "glm/glm.hpp"

#include "tiny_gltf.h"
#include "rasm/material.h"

#include <variant>
#include <unordered_map>


namespace rasm
{

    constexpr uint32_t MAX_MESHES              = 8;
    constexpr uint32_t MAX_VERTEX_UV_SETS      = 3;

    struct Vertex
    {
        glm::vec3 pos;
        glm::vec3 normal;
        glm::vec2 uv[MAX_VERTEX_UV_SETS];
    };

    struct ObjRaw
    {
        std::vector<Vertex>     vertices;
        std::vector<uint32_t>   indices;
    };

    struct SubGLTFRaw
    {
        ObjRaw              objRaw;
        glm::mat4           transform;
        MaterialHandle      material;
    };

    struct GLTFRaw
    {
        tinygltf::Model model;
        std::vector<SubGLTFRaw> subGltfRaws;
    };

    struct Mesh
    {
        enum class MeshType
        {
            GLTF,
            OBJ
        };
        MeshType type;
        std::variant<GLTFRaw, ObjRaw> data;
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
        std::unordered_map<int, MaterialHandle>         materials;
        std::unordered_map<uint64_t, TextureHandle>     imageTextures;
    };

}
// clang-format on