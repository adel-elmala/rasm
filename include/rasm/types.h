// clang-format off
#pragma once

#include "rasm/handle.h"

#include "glm/glm.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rasm
{
    constexpr uint32_t  MAX_FRAMES_IN_FLIGHT    = 2;
    constexpr uint32_t  MAX_DESCRIPTOR_TYPES    = 1;
    constexpr uint32_t  MAX_BINDLESS_TEXTURES   = 1024;
    constexpr uint32_t  PUSH_CONSTANTS_SIZE     = 16;
    // Sentinel written by the CPU when a material slot has no texture bound; the shader must
    // never sample the bindless array with this index.
    constexpr uint32_t  INVALID_TEXTURE_INDEX   = 0xFFFFFFFF;

    enum class Backend
    {
        VULKAN,
        DX12,
        METAL,
        WEBGPU
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
        D32_SFLOAT,
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
        COMPUTE,
        ALL             // every stage, for descriptor set layouts
    };

    enum class PrimitiveTopology
    {
        TRIANGLE_LIST,
        LINE_LIST,
        POINT_LIST,
        TRIANGLE_STRIP,
        LINE_STRIP,
    };

    enum class CullMode
    {
        NONE,
        FRONT,
        BACK,
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
        std::string appName                 = {};
        int         windowWidth             = 0;
        int         windowHeight            = 0;
        bool        enableValidation        = false;
        bool        enableSyncValidation    = false;
        bool        enableGpuValidation     = false;
        Backend     preferredBackend        = Backend::VULKAN;
        std::string assetRoot               = {};
        std::string shaderDir               = {};
        uint32_t    geometryPoolVertices    = 1u << 21; // 64 B each: 128 MiB
        uint32_t    geometryPoolIndices     = 1u << 24; // 4 B each: 64 MiB
        uint32_t    geometryPoolSubMeshes   = 1u << 14; // 120 B each
        uint32_t    maxInstances            = 1u << 16; // 108 B each, twice (one copy per frame slot)
        bool        showStats               = false;
        bool        occlusionCulling        = true;     // two-pass Hi-Z culling in drawScene; false keeps frustum culling only
        bool        meshLods                = true;
        float       lodErrorPixels          = 1.0f;
        int32_t     forcedLod               = -1;
    };

    struct FrameResources
    {
        CommandBufferHandle commandBuffer;
        TextureHandle       depthTexture;
        uint64_t            fenceValue      = 0; // frameFence value the slot's last submit signals
    };

    struct Swapchain
    {
        std::vector<TextureHandle>      imageHandles;
        Format                          imageFormat;
        uint32_t                        width   = 0; // swapchain extent in pixels
        uint32_t                        height  = 0;
    };

    struct TextureRaw
    {
        uint32_t        width   = 0;
        uint32_t        height  = 0;
        unsigned char*  data    = nullptr;
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
        glm::vec4 clearColor;
    };

} // namespace rasm
// clang-format on
