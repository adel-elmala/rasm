// clang-format off
#pragma once

#include "rasm/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
        TRANSIENT_ATTACHMENT,
        INPUT_ATTACHMENT,
        PRESENT_SRC,
        UNKNOWN,
    };

    enum class BufferUsage
    {
        INDEX,
        INDIRECT,
        UNIFORM,
        STORAGE,
        DEVICE_ADDRESS,
    };

    enum class DescriptorType
    {
        UNIFORM_BUFFER,
        SAMPLER,
        COMBINED_IMAGE_SAMPLER,
    };

    struct BufferDesc
    {
        std::string                 name;
        BufferUsage                 usage                           = BufferUsage::STORAGE;
        uint64_t                    size                            = 0;

        bool                        operator==(const BufferDesc&) const = default;
    };

    struct TextureDesc
    {
        std::string                 name;
        uint32_t                    width                           = 0;
        uint32_t                    height                          = 0;
        Format                      format                          = Format::UNKNOWN;
        TextureUsage                usage                           = TextureUsage::SAMPLED;

        bool                        operator==(const TextureDesc&) const = default;
    };

    struct ShaderDesc
    {
        std::string                 name;
        ShaderType                  type                            = ShaderType::VERTEX;
        std::vector<uint8_t>        code;

        bool                        operator==(const ShaderDesc&) const = default;
    };

    struct PipelineDesc
    {
        std::string                 name;
        ShaderHandle                vertexShader;
        ShaderHandle                fragmentShader;
        DescriptorSetLayoutHandle   descriptorSetLayout;
        PrimitiveTopology           topology                        = PrimitiveTopology::TRIANGLE_LIST;
        Format                      colorAttachmentFormat           = Format::UNKNOWN;
        Format                      depthStencilAttachmentFormat    = Format::UNKNOWN;

        bool                        operator==(const PipelineDesc&) const = default;
    };

    struct RenderTargetDesc
    {
        std::string                 name;
        uint32_t                    width                           = 0;
        uint32_t                    height                          = 0;
        Format                      colorFormat                     = Format::UNKNOWN;
        Format                      depthFormat                     = Format::UNKNOWN;
        glm::vec4                   clearColor                      = glm::vec4(0.0f); // linear; every pass that writes the target starts from this

        bool                        operator==(const RenderTargetDesc&) const = default;
    };

    struct BindlessDescriptorSetLayoutDesc
    {
        std::string                 name;
        ShaderType                  stage                           = ShaderType::FRAGMENT;
        uint32_t                    count                           = 0;
        DescriptorType              type                            = DescriptorType::COMBINED_IMAGE_SAMPLER;

        bool                        operator==(const BindlessDescriptorSetLayoutDesc&) const = default;
    };

    struct DescriptorPoolSize
    {
        DescriptorType              type                            = DescriptorType::COMBINED_IMAGE_SAMPLER;
        uint32_t                    count                           = 0;

        bool                        operator==(const DescriptorPoolSize&) const = default;
    };

    struct DescriptorPoolDesc
    {
        std::string                 name;
        DescriptorPoolSize          sizes[MAX_DESCRIPTOR_TYPES]     = {};
        uint32_t                    maxSets                         = 0;

        bool                        operator==(const DescriptorPoolDesc&) const = default;
    };

    struct ShaderDescHash
    {
        std::size_t                 operator()(const ShaderDesc& desc) const;
    };

    struct PipelineDescHash
    {
        std::size_t                 operator()(const PipelineDesc& desc) const;
    };

    struct BindlessDescriptorSetLayoutDescHash
    {
        std::size_t                 operator()(const BindlessDescriptorSetLayoutDesc& desc) const;
    };
}
// clang-format on
