// clang-format off
#pragma once

#include "rasm/core/types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace rasm
{

    enum class TextureUsage
    {
        TRANSFER_SRC,
        TRANSFER_DST,
        SAMPLED,
        STORAGE,
        COLOR_ATTACHMENT,
        DEPTH_STENCIL_ATTACHMENT,
        SAMPLED_COLOR_ATTACHMENT,
        SAMPLED_DEPTH_STENCIL_ATTACHMENT,
        TRANSIENT_ATTACHMENT,
        INPUT_ATTACHMENT,
        PRESENT_SRC,
        UNKNOWN,
    };

    enum class BufferUsage
    {
        VERTEX,
        INDEX,
        INDIRECT,
        VERTEXINDEX,
        UNIFORM,
        STORAGE,
        DEVICE_ADDRESS,
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
                uint64_t        deviceAddress;
                uint64_t        offset; // Offset within the buffer for index data if using a combined vertex/index buffer.
                union {
                    struct {
                        uint64_t        indexCount;
                        uint64_t        vertexCount;
                    } vertexIndexBuffer;

                    struct {
                        uint64_t        vertexCount;
                    } vertexBuffer;

                    struct {
                        uint64_t        indexCount;
                    } indexBuffer;
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
                       buffer.offset == other.buffer.offset &&
                       buffer.vertexIndexBuffer.indexCount == other.buffer.vertexIndexBuffer.indexCount &&
                       buffer.vertexIndexBuffer.vertexCount == other.buffer.vertexIndexBuffer.vertexCount;
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
                       std::hash<uint64_t>()(desc.buffer.offset) ^
                       std::hash<uint64_t>()(static_cast<uint64_t>(desc.buffer.usage)) ^
                       std::hash<uint64_t>()(desc.buffer.vertexIndexBuffer.indexCount) ^
                       std::hash<uint64_t>()(desc.buffer.vertexIndexBuffer.vertexCount);
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

} // namespace rasm

// clang-format on