// clang-format off
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <variant>

#include "tiny_gltf.h"

namespace rasm
{
    enum class Backend
    {
        VULKAN,
        DX12,
        METAL
    };

    enum class BufferUsage
    {
        VERTEX,
        INDEX,
        UNIFORM,
        STORAGE
    };

    enum class TextureUsage
    {
        TRANSFER_SRC,
        TRANSFER_DST,
        SAMPLED,
        STORAGE,
        COLOR_ATTACHMENT,
        DEPTH_STENCIL_ATTACHMENT,
        TRANSIENT_ATTACHMENT,
        INPUT_ATTACHMENT,
    };

    enum class TextureFormat
    {
        RGBA8,
        RGBA16F,
        DEPTH24STENCIL8
    };

    enum class ShaderType
    {
        VERTEX,
        FRAGMENT,
        COMPUTE
    };

    const static std::unordered_set<std::string> supportedMeshExtensions = {"obj", "gltf", "glb"};
    const static std::unordered_set<std::string> supportedTextureExtensions = {"png", "jpg", "jpeg"};

    template <typename Tag>
    struct Handle
    {
        uint64_t index = 0;
        uint64_t generation = 0;

        [[nodiscard]] bool isValid() const { return generation != 0; }
        bool operator==(const Handle& other) const { return index == other.index && generation == other.generation; }
    };

    struct HandleHash
    {
        template <typename Tag>
        std::size_t operator()(const Handle<Tag>& handle) const
        {
            return std::hash<uint64_t>()(handle.index) ^ (std::hash<uint64_t>()(handle.generation) << 1);
        }
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
    struct ResourceTag;

    using MeshHandle = Handle<MeshTag>;
    using BufferHandle = Handle<BufferTag>;
    using TextureHandle = Handle<TextureTag>;
    using MaterialHandle = Handle<MaterialTag>;
    using PipelineHandle = Handle<PipelineTag>;
    using ShaderHandle = Handle<ShaderTag>;
    using SceneHandle = Handle<SceneTag>;
    using EntityHandle = Handle<EntityTag>;
    using WindowHandle = Handle<WindowTag>;
    using ResourceHandle = Handle<ResourceTag>;

    struct EngineConfig
    {
        std::string appName{};
        int windowWidth = 0;
        int windowHeight = 0;
        bool enableValidation = false;
        Backend preferredBackend = Backend::VULKAN;
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

    struct ResourceDesc
    {
        std::string name;
        enum class Type
        {
            TEXTURE,
            BUFFER,
            SHADER,
            // Add more types as needed
        } type;

        union {
            // Texture-specific data
            struct
            {
                uint32_t width;
                uint32_t height;
                TextureFormat format;
                TextureUsage usage;
            } texture;

            // Buffer-specific data
            struct
            {
                uint64_t size;
                uint64_t stride;
                BufferUsage usage;
            } buffer;

            // Shader-specific data
            struct
            {
                ShaderType shaderType;
                const char *source;
            } shader;
        };
    };

}
// clang-format on