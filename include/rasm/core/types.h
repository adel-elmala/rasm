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
    constexpr int MAX_FRAMES_IN_FLIGHT = 2;
    constexpr int MAX_DESCRIPTOR_TYPES = 1;
    constexpr uint32_t MAX_BINDLESS_TEXTURES = 1024;

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
        STORAGE,
        DEVICE_ADDRESS,
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
        R8G8B8A8_SRGB,
        B8G8R8A8_SRGB,
        R8G8B8_UNORM,
        R16G16B16A16_SFLOAT,
        R32G32B32_SFLOAT,
        R32G32B32A32_SFLOAT,
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
    struct RenderTargetTag;
    struct DescriptorSetLayoutTag;
    struct DescriptorPoolTag;
    struct DescriptorSetTag;

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
    using RenderTargetHandle   = Handle<RenderTargetTag>;
    using DescriptorSetLayoutHandle = Handle<DescriptorSetLayoutTag>;
    using DescriptorPoolHandle = Handle<DescriptorPoolTag>;
    using DescriptorSetHandle = Handle<DescriptorSetTag>;

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
        uint64_t scene                  = 1;
        uint64_t entity                 = 1;
        uint64_t mesh                   = 1;
        uint64_t texture                = 1;
        uint64_t material               = 1;
        uint64_t buffer                 = 1;
        uint64_t pipeline               = 1;
        uint64_t shader                 = 1;
        uint64_t commandPool            = 1;
        uint64_t commandBuffer          = 1;
        uint64_t semaphore              = 1;
        uint64_t fence                  = 1;
        uint64_t swapchain              = 1;
        uint64_t renderTarget           = 1;
        uint64_t descriptorSetLayout    = 1;
        uint64_t descriptorPool         = 1;
        uint64_t descriptorSet          = 1;
    };

    struct FrameResources
    {
        CommandPoolHandle   commandPool;
        CommandBufferHandle commandBuffer;
        SemaphoreHandle     readyToDrawSemaphore;
        FenceHandle         inFlightFence;
        BufferHandle        shaderDataBuffer;
        TextureHandle       depthTexture;
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


    enum class ResourceType
    {
        TEXTURE,
        BUFFER,
        SHADER,
        SAMPLER,
        GRAPHICS_PIPELINE,
        COMPUTE_PIPELINE,
        RENDER_TARGET,
        BINDLESS_DESCRIPTOR_SET_LAYOUT,
        DESCRIPTOR_POOL,
        DESCRIPTOR_SET,
        // Add more types as needed
    };

    struct DescriptorPoolSizeDesc
    {
        uint32_t descriptorCount;
        ResourceType descriptorType;
    };


    struct ResourceDesc
    {
        std::string name;
        ResourceType type;

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
                BufferUsage     usage;
                uint64_t        size;
                union {
                    struct {
                        uint64_t        indexCount;
                        uint64_t        vertexCount;
                        uint64_t        offset;
                    } vertexIndexBuffer;

                    struct {
                        uint64_t        vertexCount;
                        uint64_t        offset;
                    } vertexBuffer;

                    struct {
                        uint64_t        indexCount;
                        uint64_t        offset;
                    } indexBuffer;

                    struct {
                        uint64_t        offset;
                    } uniformBuffer;

                    struct {
                        uint64_t        offset;
                    } storageBuffer;

                    struct {
                        uint64_t        address;
                    } deviceAddressBuffer;
                };
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
                ShaderHandle                vertexShader;
                ShaderHandle                fragmentShader;
                VertexInputLayout           vertexInputLayout;
                DescriptorSetLayoutHandle   descriptorSetLayout;
                PrimitiveTopology           topology;
                Format                      colorAttachmentFormat;
                Format                      depthStencilAttachmentFormat;
            } pipeline;

            struct
            {
                uint32_t width;
                uint32_t height;
                Format colorFormat;
                Format depthFormat;
            } renderTarget;

            struct 
            {
                ShaderType stage;
                uint32_t count;
                ResourceType type;
            } bindlessDescriptorSetLayout;

            struct
            {
                DescriptorPoolSizeDesc descriptorType[MAX_DESCRIPTOR_TYPES];
                uint32_t maxSets;
            } descriptorPool;

            struct
            {
                DescriptorPoolHandle pool;
                DescriptorSetLayoutHandle layout;
            } descriptorSet;
        };

        bool operator==(const ResourceDesc& other) const
        {
            if (type != other.type || name != other.name)
                return false;

            switch (type)
            {
            case ResourceType::TEXTURE:
                return texture.width == other.texture.width &&
                       texture.height == other.texture.height &&
                       texture.format == other.texture.format &&
                       texture.usage == other.texture.usage;
            case ResourceType::BUFFER:
                return buffer.size == other.buffer.size &&
                       buffer.usage == other.buffer.usage &&
                       buffer.vertexIndexBuffer.indexCount == other.buffer.vertexIndexBuffer.indexCount &&
                       buffer.vertexIndexBuffer.vertexCount == other.buffer.vertexIndexBuffer.vertexCount &&
                       buffer.vertexIndexBuffer.offset == other.buffer.vertexIndexBuffer.offset;
            case ResourceType::SHADER:
                return shader.shaderType == other.shader.shaderType &&
                       shader.sourceSize == other.shader.sourceSize &&
                       shader.source == other.shader.source;
            case ResourceType::GRAPHICS_PIPELINE:
            case ResourceType::COMPUTE_PIPELINE:
                // For pipelines, you would compare the relevant fields (e.g., shader handles)
                return pipeline.vertexShader == other.pipeline.vertexShader &&
                       pipeline.fragmentShader == other.pipeline.fragmentShader &&
                       pipeline.topology == other.pipeline.topology;
            case ResourceType::RENDER_TARGET:
                return renderTarget.width == other.renderTarget.width &&
                       renderTarget.height == other.renderTarget.height &&
                       renderTarget.colorFormat == other.renderTarget.colorFormat &&
                       renderTarget.depthFormat == other.renderTarget.depthFormat;
            case ResourceType::BINDLESS_DESCRIPTOR_SET_LAYOUT:
                return bindlessDescriptorSetLayout.stage == other.bindlessDescriptorSetLayout.stage &&
                       bindlessDescriptorSetLayout.count == other.bindlessDescriptorSetLayout.count &&
                       bindlessDescriptorSetLayout.type == other.bindlessDescriptorSetLayout.type;
            case ResourceType::DESCRIPTOR_POOL:
            {
                auto equal = false;
                for (uint32_t i = 0; i < MAX_DESCRIPTOR_TYPES; ++i)
                {
                    if (descriptorPool.descriptorType[i].descriptorType != other.descriptorPool.descriptorType[i].descriptorType ||
                        descriptorPool.descriptorType[i].descriptorCount != other.descriptorPool.descriptorType[i].descriptorCount)
                    {
                        equal = false;
                        break;
                    }
                    equal = true;
                }
                return equal && descriptorPool.maxSets == other.descriptorPool.maxSets;
            }
            case ResourceType::DESCRIPTOR_SET:
                return descriptorSet.pool == other.descriptorSet.pool &&
                       descriptorSet.layout == other.descriptorSet.layout;
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
            case ResourceType::TEXTURE:
                return std::hash<std::string>()(desc.name) ^
                       std::hash<uint32_t>()(desc.texture.width) ^
                       std::hash<uint32_t>()(desc.texture.height) ^
                       std::hash<uint32_t>()(static_cast<uint32_t>(desc.texture.format)) ^
                       std::hash<uint32_t>()(static_cast<uint32_t>(desc.texture.usage));
            case ResourceType::BUFFER:
                return std::hash<std::string>()(desc.name) ^
                       std::hash<uint64_t>()(desc.buffer.size) ^
                       std::hash<uint64_t>()(static_cast<uint64_t>(desc.buffer.usage)) ^
                       std::hash<uint64_t>()(desc.buffer.vertexIndexBuffer.indexCount) ^
                       std::hash<uint64_t>()(desc.buffer.vertexIndexBuffer.vertexCount) ^
                       std::hash<uint64_t>()(desc.buffer.vertexIndexBuffer.offset);
            case ResourceType::SHADER:
                return std::hash<std::string>()(desc.name) ^
                       std::hash<uint64_t>()(desc.shader.sourceSize) ^
                       std::hash<uint32_t>()(static_cast<uint32_t>(desc.shader.shaderType));
            case ResourceType::GRAPHICS_PIPELINE:
            case ResourceType::COMPUTE_PIPELINE:
                return std::hash<std::string>()(desc.name) ^
                       std::hash<uint64_t>()(static_cast<uint64_t>(desc.pipeline.vertexShader.index)) ^
                       std::hash<uint64_t>()(static_cast<uint64_t>(desc.pipeline.fragmentShader.index)) ^
                       std::hash<uint32_t>()(static_cast<uint32_t>(desc.pipeline.topology));
            case ResourceType::RENDER_TARGET:
                return std::hash<uint32_t>()(desc.renderTarget.width) ^
                       std::hash<uint32_t>()(desc.renderTarget.height) ^
                       std::hash<uint32_t>()(static_cast<uint32_t>(desc.renderTarget.colorFormat)) ^
                       std::hash<uint32_t>()(static_cast<uint32_t>(desc.renderTarget.depthFormat));
            case ResourceType::BINDLESS_DESCRIPTOR_SET_LAYOUT:
                return std::hash<uint32_t>()(static_cast<uint32_t>(desc.bindlessDescriptorSetLayout.stage)) ^
                       std::hash<uint32_t>()(desc.bindlessDescriptorSetLayout.count) ^
                       std::hash<uint32_t>()(static_cast<uint32_t>(desc.bindlessDescriptorSetLayout.type));
            case ResourceType::DESCRIPTOR_POOL:
            {
                auto hash = std::hash<uint32_t>()(desc.descriptorPool.maxSets);
                for (uint32_t i = 0; i < MAX_DESCRIPTOR_TYPES; ++i)
                {
                    hash ^= std::hash<uint32_t>()(static_cast<uint32_t>(desc.descriptorPool.descriptorType[i].descriptorType)) ^
                            std::hash<uint32_t>()(desc.descriptorPool.descriptorType[i].descriptorCount);
                }
                return hash;
            }
            case ResourceType::DESCRIPTOR_SET:
                return std::hash<uint64_t>()(static_cast<uint64_t>(desc.descriptorSet.pool.index)) ^
                       std::hash<uint64_t>()(static_cast<uint64_t>(desc.descriptorSet.layout.index));
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

    struct RenderTarget
    {
        TextureHandle colorAttachment[rasm::MAX_FRAMES_IN_FLIGHT];
        TextureHandle depthAttachment[rasm::MAX_FRAMES_IN_FLIGHT];
        uint32_t width;
        uint32_t height;
        Format colorFormat;
        Format depthFormat;
    };

}
// clang-format on
