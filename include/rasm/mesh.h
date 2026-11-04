// clang-format off
#pragma once

#include "glm/glm.hpp"

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

    struct Vertex
    {
        glm::vec3 pos;
        glm::vec3 normal;
        glm::vec2 uv[MAX_VERTEX_UV_SETS];
        glm::vec4 tangent;      // glTF TANGENT: xyz tangent, w = bitangent sign (±1). w = 0 means none; the shader reconstructs one.
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
        int                 materialIndex;
    };

    struct GLTFRaw
    {
        std::vector<SubGLTFRaw>             subGltfRaws;
        std::shared_ptr<tinygltf::Model>    model;
        bool                                loaded          = false;
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
    };

    struct SubMesh
    {
        MeshGeometry        geometry;
        glm::mat4           transform;
        MaterialHandle      material;
    };

    struct Mesh
    {
        MeshState               state       = MeshState::LOADING;
        std::vector<SubMesh>    subMeshes;
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
