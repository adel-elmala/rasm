// clang-format off
#pragma once

#include "glm/glm.hpp"

#include "tiny_gltf.h"

#include <variant>


namespace rasm
{

    constexpr uint32_t MAX_MESHES = 1024 * 1024;

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

    struct Mesh
    {
        enum class MeshType
        {
            GLTF,
            OBJ
        };
        MeshType type;
        std::variant<tinygltf::Model, ObjRaw> data; // TODO: convert to raw union
    };

}
// clang-format on