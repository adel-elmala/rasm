// clang-format off
#pragma once

#include "glm/glm.hpp"

#include "tiny_gltf.h"
#include "rasm/core/material.h"

#include <variant>


namespace rasm
{

    constexpr uint32_t MAX_MESHES = 8;

    struct Vertex
    {
        glm::vec3 pos;
        glm::vec3 normal;
        glm::vec2 uv;
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

}
// clang-format on