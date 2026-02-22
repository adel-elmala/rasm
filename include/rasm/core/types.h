#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <variant>

#include "tiny_gltf.h"

namespace rasm
{

    const static std::unordered_set<std::string> supportedMeshExtensions = {"obj", "gltf", "glb"};
    const static std::unordered_set<std::string> supportedTextureExtensions = {"png", "jpg", "jpeg"};

    template <typename Tag>
    struct Handle
    {
        uint64_t index = 0;
        uint64_t generation = 0;

        [[nodiscard]] bool isValid() const { return generation != 0; }
    };

    struct MeshTag;
    struct TextureTag;
    struct MaterialTag;
    struct SceneTag;
    struct EntityTag;

    using MeshHandle = Handle<MeshTag>;
    using TextureHandle = Handle<TextureTag>;
    using MaterialHandle = Handle<MaterialTag>;
    using SceneHandle = Handle<SceneTag>;
    using EntityHandle = Handle<EntityTag>;

    struct EngineConfig
    {
        std::string appName{};
        int windowWidth = 0;
        int windowHeight = 0;
        bool enableValidation = false;
    };

    struct HandleCounters
    {
        uint64_t scene = 1;
        uint64_t entity = 1;
        uint64_t mesh = 1;
        uint64_t texture = 1;
        uint64_t material = 1;
    };

    struct TextureRaw
    {
        uint32_t width;
        uint32_t height;
        uint32_t channels;
        unsigned char *data;
    };

    
    struct MeshRaw
    {
        enum class MeshType
        {
            GLTF,
            OBJ
        };
        MeshType type;
        std::variant<tinygltf::Model, std::string> data;
    };

}