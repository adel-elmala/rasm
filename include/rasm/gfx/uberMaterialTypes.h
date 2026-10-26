// clang-format off
#pragma once

#include "glm/glm.hpp"

#include "rasm/core/light.h"
#include "rasm/core/mesh.h"

#include <cstdint>

namespace rasm::gfx {

    enum VertexFormat : uint32_t { Full, PosOnly };

    // Must match rasm::Vertex (static asserted in scene.cpp) and shaders/common/core.slang.
    struct VertexFull       { glm::vec3 pos; glm::vec3 normal; glm::vec2 uv[MAX_VERTEX_UV_SETS]; };
    struct VertexPosOnly    { glm::vec3 pos; };

    enum LightType : uint32_t { Directional , Point };

    struct Light
    {
        glm::vec3   position;
        float       intensity;
        glm::vec3   color;
        float       range;      // Only used for point lights
        glm::vec3   direction;  // Only used for directional lights
        LightType   type;
    };


    struct DrawInfo
    {
        uint32_t        indexCount;
        uint32_t        instanceCount;
        uint32_t        firstIndex;
        int32_t         vertexOffset;
        uint32_t        firstInstance;
    };

    struct MetallicRoughnessMaterial
    {
        glm::vec3       baseColorFactor;
        uint32_t        baseColorTextureIndex;
        uint32_t        baseColorTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        uint32_t        metallicRoughnessTextureIndex;
        uint32_t        metallicRoughtnessTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        float           metallicFactor;
        float           roughnessFactor;
    };

    struct NormalMaterial
    {
        uint32_t        normalTextureIndex;
        uint32_t        normalTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        float           scale;
    };

    struct OcclusionMaterial
    {
        uint32_t        occlusionTextureIndex;
        uint32_t        occlusionTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        float           strength;
    };

    struct EmissiveMaterial
    {
        uint32_t        emissiveTextureIndex;
        uint32_t        emissiveTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        glm::vec3       emissiveFactor;
    };

    struct Material
    {
        MetallicRoughnessMaterial   pbr;
        NormalMaterial              normal;
        OcclusionMaterial           occlusion;
        EmissiveMaterial            emissive;
    };

    struct Modeldata
    {
        glm::mat4       worldMatrix; 
        glm::mat3       normalMatrix; 
        Material        material;
        VertexFormat    vertFormat;
    };

    struct SceneData
    {
        glm::mat4        projection;
        glm::mat4        view;
        glm::vec3        cameraWorldPosition;
        uint32_t         numLights;
        Light            lights[MAX_LIGHTS];
        uint64_t         vertsPtr;
        uint64_t         drawInfoPtr;
        uint64_t         modeldataPtr;
    };

    // These structs are read by shaders/common/rasm.slang through buffer-device-address pointers,
    // compiled with GLSL scalar layout. Any drift here silently corrupts the GPU reads (a wrong
    // vertsPtr/modeldataPtr turns into a device lost on the first draw).
    static_assert(sizeof(Light) == 48, "Light must match the shader's scalar layout");
    static_assert(sizeof(VertexFull) == 48, "VertexFull must match the shader's scalar layout");
    static_assert(sizeof(MetallicRoughnessMaterial) == 36, "MetallicRoughnessMaterial must match the shader's scalar layout");
    static_assert(sizeof(Material) == 80, "Material must match the shader's scalar layout");
    static_assert(sizeof(Modeldata) == 184, "Modeldata must match the shader's scalar layout");
    static_assert(offsetof(Modeldata, normalMatrix) == 64, "Modeldata must match the shader's scalar layout");
    static_assert(offsetof(Modeldata, material) == 100, "Modeldata must match the shader's scalar layout");
    static_assert(sizeof(SceneData) == 552, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lights) == 144, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, vertsPtr) == 528, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, modeldataPtr) == 544, "SceneData must match the shader's scalar layout");


} // namespace rasm::gfx
// clang-format on