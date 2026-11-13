// clang-format off
#pragma once

#include "rasm/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rasm
{
    struct VRAMStats
    {
        uint64_t                    vramBudget                      = 0;    // DEVICE_LOCAL, bytes
        uint64_t                    vramUsage                       = 0;
        uint64_t                    hostBudget                      = 0;    // HOST_VISIBLE, bytes
        uint64_t                    hostUsage                       = 0;
    };

    struct Viewport
    {
        float                       x                               = 0.0f; // pixels, from the top-left corner
        float                       y                               = 0.0f;
        float                       width                           = 0.0f;
        float                       height                          = 0.0f;
        float                       minDepth                        = 0.0f;
        float                       maxDepth                        = 1.0f;
    };

    enum class TextureUsage
    {
        TRANSFER_SRC,
        TRANSFER_DST,
        SAMPLED,
        STORAGE,
        COLOR_ATTACHMENT,
        DEPTH_STENCIL_ATTACHMENT,
        SAMPLED_COLOR_ATTACHMENT,
        SAMPLED_DEPTH_ATTACHMENT,
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

    // Flags: a barrier can wait for, or be waited on by, several stages at once.
    enum class PipelineStage : uint32_t
    {
        COMPUTE                     = 1u << 0,
        GRAPHICS                    = 1u << 1,
        HOST                        = 1u << 2,
    };

    constexpr PipelineStage         operator|(PipelineStage a, PipelineStage b)     { return static_cast<PipelineStage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }
    constexpr bool                  hasStage(PipelineStage stages, PipelineStage stage) { return (static_cast<uint32_t>(stages) & static_cast<uint32_t>(stage)) != 0; }

    enum class DescriptorType
    {
        UNIFORM_BUFFER,
        SAMPLER,
        COMBINED_IMAGE_SAMPLER,
    };

    enum class SamplerType
    {
        LINEAR_REPEAT,
        LINEAR_MIRRORED_REPEAT,
        LINEAR_CLAMP,
        NEAREST_REPEAT,
        NEAREST_MIRRORED_REPEAT,
        NEAREST_CLAMP,
        COUNT,
    };

    struct BufferDesc
    {
        std::string                 name;
        BufferUsage                 usage                           = BufferUsage::STORAGE;
        uint64_t                    size                            = 0;
        bool                        hostVisible                     = false;
        bool                        hostReadable                    = false;    // host-visible, in cached memory, for RenderContext::readBuffer

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
        std::string                 entryPoint;
        std::vector<uint8_t>        code;

        bool                        operator==(const ShaderDesc&) const = default;
    };

    struct PipelineDesc
    {
        std::string                 name;
        ShaderHandle                vertexShader;
        ShaderHandle                fragmentShader;
        ShaderHandle                computeShader;
        DescriptorSetLayoutHandle   descriptorSetLayout;
        PrimitiveTopology           topology                                      = PrimitiveTopology::TRIANGLE_LIST;
        Format                      colorAttachmentFormats[MAX_COLOR_ATTACHMENTS] = {};
        Format                      depthStencilAttachmentFormat                  = Format::UNKNOWN;

        bool                        operator==(const PipelineDesc&) const = default;
    };

    struct RenderTargetDesc
    {
        std::string                 name;
        uint32_t                    width                                         = 0;
        uint32_t                    height                                        = 0;
        float                       swapchainScale                                = 0.0f;
        Format                      colorFormats[MAX_COLOR_ATTACHMENTS]           = {};
        Format                      depthFormat                                   = Format::UNKNOWN;
        glm::vec4                   clearColor                                    = glm::vec4(0.0f); // linear; every pass that writes the target starts from this
        bool                        history                                       = false;

        bool                        operator==(const RenderTargetDesc&) const = default;
    };

    uint32_t                        colorAttachmentCount(const Format* formats);

    struct AttachmentOps
    {
        bool                        loadColor                       = false;
        bool                        loadDepth                       = false;
        bool                        storeDepth                      = false;
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
