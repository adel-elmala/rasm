// clang-format off
#pragma once

#include "rasm/handle.h"

#include "tiny_gltf.h"
#include "glm/glm.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <variant>

namespace rasm
{
    constexpr uint32_t  MAX_FRAMES_IN_FLIGHT    = 2;
    constexpr uint32_t  MAX_DESCRIPTOR_TYPES    = 1;
    constexpr uint32_t  MAX_BINDLESS_TEXTURES   = 1024;
    constexpr uint32_t  MAX_VERTEX_ATTRIBUTES   = 4;
    // Sentinel written by the CPU when a material slot has no texture bound; the shader must
    // never sample the bindless array with this index.
    constexpr uint32_t  INVALID_TEXTURE_INDEX   = 0xFFFFFFFF;

    const static std::unordered_set<std::string> supportedMeshExtensions    = {"obj", "gltf", "glb"};
    const static std::unordered_set<std::string> supportedTextureExtensions = {"png", "jpg", "jpeg"};

    enum class Backend
    {
        VULKAN,
        DX12,
        METAL
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
        D32_SFLOAT_S8_UINT,
        R32G32_SFLOAT,
        U16_UINT,
        U32_UINT,
        UNKNOWN
    };

    enum class ShaderType
    {
        VERTEX,
        FRAGMENT,
        VERTEX_FRAGMENT,
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

    // Result of acquire, present and swapchain recreation.
    enum class SwapchainStatus
    {
        OK,
        SUBOPTIMAL,     // the image was acquired/presented, but the swapchain should be recreated
        OUT_OF_DATE,    // the swapchain is unusable (or the surface is 0x0) until it is recreated
        FAILURE
    };

    struct EngineConfig
    {
        std::string appName             = {};
        int         windowWidth         = 0;
        int         windowHeight        = 0;
        bool        enableValidation    = false;
        Backend     preferredBackend    = Backend::VULKAN;
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
        uint32_t                        width   = 0; // swapchain extent in pixels
        uint32_t                        height  = 0;
    };

    struct TextureRaw
    {
        uint32_t        width;
        uint32_t        height;
        uint32_t        channels;
        unsigned char*  data;
    };

    struct VertexAttributeDescription
    {
        uint32_t                binding;
        uint32_t                location;
        rasm::Format            format;
        uint32_t                size;
        uint32_t                offset;
        bool                    used = false;

        bool operator==(const VertexAttributeDescription&) const = default;
    };

    struct VertexInputLayout
    {
        VertexAttributeDescription  attributes[MAX_VERTEX_ATTRIBUTES];
        uint32_t                    binding;
        uint32_t                    stride;
        bool                        perInstance;
        bool                        vertexPulling = false;

        bool operator==(const VertexInputLayout&) const = default;
    };

    struct RenderTargetAttachments
    {
        TextureHandle color;
        TextureHandle depth;
    };

    struct RenderTarget
    {
        RenderTargetAttachments attachments[rasm::MAX_FRAMES_IN_FLIGHT];
        uint32_t width;
        uint32_t height;
        Format colorFormat;
        Format depthFormat;
    };

} // namespace rasm
// clang-format on
