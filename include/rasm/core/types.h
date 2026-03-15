#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <variant>

#include "tiny_gltf.h"

#include "rasm/gfx/types.h"

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
    struct BufferTag;
    struct TextureTag;
    struct MaterialTag;
    struct PipelineTag;
    struct ShaderTag;
    struct SceneTag;
    struct EntityTag;
    struct WindowTag;

    using MeshHandle = Handle<MeshTag>;
    using BufferHandle = Handle<BufferTag>;
    using TextureHandle = Handle<TextureTag>;
    using MaterialHandle = Handle<MaterialTag>;
    using PipelineHandle = Handle<PipelineTag>;
    using ShaderHandle = Handle<ShaderTag>;
    using SceneHandle = Handle<SceneTag>;
    using EntityHandle = Handle<EntityTag>;
    using WindowHandle = Handle<WindowTag>;

    struct EngineConfig
    {
        std::string appName{};
        int windowWidth = 0;
        int windowHeight = 0;
        bool enableValidation = false;
        Backend preferredBackend = Backend::Vulkan;
    };

    struct HandleCounters
    {
        uint64_t scene = 1;
        uint64_t entity = 1;
        uint64_t mesh = 1;
        uint64_t texture = 1;
        uint64_t material = 1;
        uint64_t buffer = 1;
        uint64_t pipeline = 1;
        uint64_t shader = 1;
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