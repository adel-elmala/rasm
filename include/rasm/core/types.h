// clang-format off
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <variant>

#include "tiny_gltf.h"
#include "glm/glm.hpp"


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
        VERTEXINDEX,
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
        PRESENT_SRC,
        UNKNOWN,
    };

    enum class Format
    {
        R8G8B8A8_UNORM,
        R16G16B16A16_SFLOAT,
        R32G32B32_SFLOAT,
        D24_UNORM_S8_UINT,
        R32G32_SFLOAT,
        U16_UINT,
        U32_UINT,
        UNKNOWN
    };

    enum class ShaderType
    {
        VERTEX,
        FRAGMENT,
        COMPUTE
    };

    enum class PrimitiveTopology
    {
        TRIANGLE_LIST,
        LINE_LIST,
        POINT_LIST,
        TRIANGLE_STRIP,
        LINE_STRIP,
    };

    const static std::unordered_set<std::string> supportedMeshExtensions    = {"obj", "gltf", "glb"};
    const static std::unordered_set<std::string> supportedTextureExtensions = {"png", "jpg", "jpeg"};

    template <typename Tag>
    struct Handle
    {
        uint64_t index      = 0;
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
    struct CommandPoolTag;
    struct CommandBufferTag;
    struct SemaphoreTag;
    struct FenceTag;
    struct SwapchainTag;
    struct ShaderTag;
    struct SceneTag;
    struct EntityTag;
    struct WindowTag;
    struct ResourceTag;

    using MeshHandle          = Handle<MeshTag>;
    using BufferHandle        = Handle<BufferTag>;
    using TextureHandle       = Handle<TextureTag>;
    using MaterialHandle      = Handle<MaterialTag>;
    using PipelineHandle      = Handle<PipelineTag>;
    using CommandPoolHandle   = Handle<CommandPoolTag>;
    using CommandBufferHandle = Handle<CommandBufferTag>;
    using SemaphoreHandle     = Handle<SemaphoreTag>;
    using FenceHandle         = Handle<FenceTag>;
    using SwapchainHandle     = Handle<SwapchainTag>;
    using ShaderHandle        = Handle<ShaderTag>;
    using SceneHandle         = Handle<SceneTag>;
    using EntityHandle        = Handle<EntityTag>;
    using WindowHandle        = Handle<WindowTag>;
    using ResourceHandle      = Handle<ResourceTag>;

    struct EngineConfig
    {
        std::string appName             = {};
        int         windowWidth         = 0;
        int         windowHeight        = 0;
        bool        enableValidation    = false;
        Backend     preferredBackend    = Backend::VULKAN;
    };

    struct HandleCounters
    {
        uint64_t scene          = 1;
        uint64_t entity         = 1;
        uint64_t mesh           = 1;
        uint64_t texture        = 1;
        uint64_t material       = 1;
        uint64_t buffer         = 1;
        uint64_t pipeline       = 1;
        uint64_t shader         = 1;
        uint64_t commandPool    = 1;
        uint64_t commandBuffer  = 1;
        uint64_t semaphore      = 1;
        uint64_t fence          = 1;
        uint64_t swapchain      = 1;
    };

    struct FrameResources
    {
        CommandPoolHandle   commandPool;
        CommandBufferHandle commandBuffer;
        SemaphoreHandle     readyToDrawSemaphore;
        FenceHandle         inFlightFence;
        BufferHandle        shaderDataBuffer;
    };

    struct Swapchain
    {
        std::vector<TextureHandle>      imageHandles;
        std::vector<SemaphoreHandle>    readyToPresentSemaphores;
        Format                          imageFormat;
    };

    struct TextureRaw
    {
        uint32_t        width;
        uint32_t        height;
        uint32_t        channels;
        unsigned char*  data;
    };

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

    struct MeshRaw
    {
        enum class MeshType
        {
            GLTF,
            OBJ
        };
        MeshType type;
        std::variant<tinygltf::Model, ObjRaw> data;
    };

    struct VertexAttributeDescription
    {
        uint32_t    binding;
        uint32_t    location;
        Format      format;
        uint32_t    size;
        uint32_t    offset;
        bool        used = false;
    };

    constexpr uint32_t MAX_VERTEX_ATTRIBUTES = 4;

    struct VertexInputLayout
    {
        VertexAttributeDescription  attributes[MAX_VERTEX_ATTRIBUTES];
        uint32_t                    binding;
        uint32_t                    stride;
        bool                        perInstance;
    };

    struct ResourceDesc
    {
        std::string name;
        enum class Type
        {
            TEXTURE,
            BUFFER,
            SHADER,
            GRAPHICS_PIPELINE,
            COMPUTE_PIPELINE
            // Add more types as needed
        } type;

        union {
            // Texture-specific data
            struct
            {
                uint32_t        width;
                uint32_t        height;
                Format          format;
                TextureUsage    usage;
            } texture;

            // Buffer-specific data
            struct
            {
                uint64_t        size;
                uint64_t        stride;
                uint64_t        offset;
                BufferUsage     usage;
            } buffer;

            // Shader-specific data
            struct
            {
                ShaderType      shaderType;
                uint64_t        sourceSize;
                const char *    source;
            } shader;

            // Pipeline-specific data
            struct
            {
                ShaderHandle      vertexShader;
                ShaderHandle      fragmentShader;
                VertexInputLayout vertexInputLayout;
                PrimitiveTopology topology;
                Format            colorAttachmentFormat;
                Format            depthStencilAttachmentFormat;
            } pipeline;
        };

        bool operator==(const ResourceDesc& other) const
        {
            if (type != other.type || name != other.name)
                return false;

            switch (type)
            {
            case Type::TEXTURE:
                return texture.width == other.texture.width &&
                       texture.height == other.texture.height &&
                       texture.format == other.texture.format &&
                       texture.usage == other.texture.usage;
            case Type::BUFFER:
                return buffer.size == other.buffer.size &&
                       buffer.stride == other.buffer.stride &&
                       buffer.usage == other.buffer.usage;
            case Type::SHADER:
                return shader.shaderType == other.shader.shaderType &&
                       shader.sourceSize == other.shader.sourceSize &&
                       shader.source == other.shader.source;
            case Type::GRAPHICS_PIPELINE:
            case Type::COMPUTE_PIPELINE:
                // For pipelines, you would compare the relevant fields (e.g., shader handles)
                return pipeline.vertexShader == other.pipeline.vertexShader &&
                       pipeline.fragmentShader == other.pipeline.fragmentShader &&
                       pipeline.topology == other.pipeline.topology;
            default:
                return false;
            }
        }

        ResourceDesc() {}
        ~ResourceDesc() {}
    };

    struct ResourceDescHash
    {
        std::size_t operator()(const ResourceDesc& desc) const
        {
            switch (desc.type)
            {
            case ResourceDesc::Type::TEXTURE:
                return std::hash<std::string>()(desc.name) ^
                       std::hash<uint32_t>()(desc.texture.width) ^
                       std::hash<uint32_t>()(desc.texture.height) ^
                       std::hash<uint32_t>()(static_cast<uint32_t>(desc.texture.format)) ^
                       std::hash<uint32_t>()(static_cast<uint32_t>(desc.texture.usage));
            case ResourceDesc::Type::BUFFER:
                return std::hash<std::string>()(desc.name) ^
                       std::hash<uint64_t>()(desc.buffer.size) ^
                       std::hash<uint64_t>()(desc.buffer.stride) ^
                       std::hash<uint64_t>()(static_cast<uint64_t>(desc.buffer.usage));
            case ResourceDesc::Type::SHADER:
                return std::hash<std::string>()(desc.name) ^
                       std::hash<uint64_t>()(desc.shader.sourceSize) ^
                       std::hash<uint32_t>()(static_cast<uint32_t>(desc.shader.shaderType));
            case ResourceDesc::Type::GRAPHICS_PIPELINE:
            case ResourceDesc::Type::COMPUTE_PIPELINE:
                return std::hash<std::string>()(desc.name) ^
                       std::hash<uint64_t>()(static_cast<uint64_t>(desc.pipeline.vertexShader.index)) ^
                       std::hash<uint64_t>()(static_cast<uint64_t>(desc.pipeline.fragmentShader.index)) ^
                       std::hash<uint32_t>()(static_cast<uint32_t>(desc.pipeline.topology));
            default:
                return 0;
            }
        }
    };

    struct CompiledScene
    {
        std::unordered_map<MaterialHandle, std::unordered_set<EntityHandle, HandleHash>, HandleHash> materialToMeshes;
        std::unordered_map<MaterialHandle, PipelineHandle, HandleHash> materialToPipeline;
        std::unordered_map<EntityHandle, BufferHandle, HandleHash> meshData;
    };

}
// clang-format on
