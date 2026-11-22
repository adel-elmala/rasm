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
    constexpr uint32_t  MAX_FRAMES_IN_FLIGHT        = 2;
    constexpr uint32_t  MAX_COLOR_ATTACHMENTS       = 4;
    constexpr uint32_t  MAX_DESCRIPTOR_TYPES        = 2;
    constexpr uint32_t  MAX_BINDLESS_TEXTURES       = 1024;
    // Binding 1 of the bindless set: storage images, each one mip of a texture, that compute shaders write. The slots
    // below RESERVED_STORAGE_IMAGES are the environment bake's; storage render targets take the others.
    constexpr uint32_t  MAX_BINDLESS_STORAGE_IMAGES = 1024;
    constexpr uint32_t  RESERVED_STORAGE_IMAGES     = 32;
    constexpr uint32_t  PUSH_CONSTANTS_SIZE         = 16;
    // Sentinel written by the CPU when a material slot has no texture bound; the shader must
    // never sample the bindless array with this index.
    constexpr uint32_t  INVALID_TEXTURE_INDEX       = 0xFFFFFFFF;

    enum class Backend
    {
        VULKAN,
        DX12,
        METAL,
        WEBGPU
    };

    // Who owns the window and the input. SDL: RASM opens a window and reads its events. HEADLESS: no window and no
    // events; each frame renders into an offscreen swapchain of windowWidth x windowHeight pixels, which
    // Engine::readFrame copies back.
    enum class PlatformType
    {
        SDL,
        HEADLESS,
    };

    enum class Format
    {
        UNKNOWN,
        R8G8B8A8_UNORM,
        R8G8B8A8_SRGB,
        B8G8R8A8_SRGB,
        B8G8R8A8_UNORM,         // the swapchain's encoded view, which the UI draws through (Swapchain::getEncodedFormat)
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
        BC4_UNORM,
        BC5_UNORM,
        BC7_UNORM,
        BC7_SRGB,
        ASTC_4x4_UNORM,
        ASTC_4x4_SRGB,
        ETC2_R8G8B8A8_UNORM,
        ETC2_R8G8B8A8_SRGB,
        EAC_R11_UNORM,
        EAC_R11G11_UNORM,
        R16G16_SFLOAT,          // the last value: VulkanDevice::FORMAT_COUNT
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

    enum class BlendMode
    {
        NONE,
        PREMULTIPLIED_ALPHA,
        ADDITIVE,
    };

    // Result of acquire, present and swapchain recreation.
    enum class SwapchainStatus
    {
        OK,
        SUBOPTIMAL,     // the image was acquired/presented, but the swapchain should be recreated
        OUT_OF_DATE,    // the swapchain is unusable (or the surface is 0x0) until it is recreated
        FAILURE
    };

    // How Engine::render smooths edges. MSAA_nX draws the scene with n samples per pixel and resolves them in a pass of
    // the graph (Engine::addMsaaResolvePass). TAA jitters the view and blends the frames (Engine::addTaaPass).
    enum class AntiAliasing
    {
        NONE,
        MSAA_2X,
        MSAA_4X,
        MSAA_8X,
        TAA,
    };

    enum class DebugView
    {
        NONE,
        NORMALS,
        UVS,
        OVERDRAW,
        LODS,
        CULLING,
        LIGHT_COUNT,
    };

    // What the RASM window's Buffers section shows of a frame resource (Engine::setInspection).
    enum class InspectKind
    {
        COLOR,              // a color of a render target: a motion color as a direction and length, any other one with exposure
        DEPTH,              // a render target's depth, as view depth
        HIZ,                // a scene view's Hi-Z level, as view depth
        LIGHT_CLUSTERS,     // a scene view's lights per cluster of a depth slice, or per screen tile, as heat
    };

    constexpr uint32_t  INSPECT_ALL_SAMPLES     = UINT32_MAX;   // Inspection::sample: the samples' average
    constexpr uint32_t  INSPECT_TILES           = 24;           // Inspection::level of LIGHT_CLUSTERS: the screen tiles' counts, after the 24 slices

    struct Inspection
    {
        std::string  target                  = {};       // the render target's RenderTargetDesc::name, for HIZ and LIGHT_CLUSTERS the view's; empty: nothing
        InspectKind  kind                    = InspectKind::COLOR;
        uint32_t     attachment              = 0;        // COLOR: which color
        uint32_t     face                    = 0;        // a cube target's face; HIZ, LIGHT_CLUSTERS: the view's face
        uint32_t     level                   = 0;        // HIZ: the level; LIGHT_CLUSTERS: the depth slice, or INSPECT_TILES
        uint32_t     sample                  = 0;        // a multisampled target's sample, or INSPECT_ALL_SAMPLES
        uint32_t     channel                 = 0;        // COLOR: 0 all, 1 to 4 red, green, blue or alpha alone as gray
        float        exposure                = 1.0f;     // COLOR: the value's multiplier before it is clamped to 0..1; motion: its gain
        float        depthRange              = 50.0f;    // DEPTH, HIZ: the view depth shown black, of a perspective view
        EntityHandle entity                  = {};       // the GPU tables' rows of this entity
    };

    struct EngineConfig
    {
        std::string  appName                 = {};
        PlatformType platform                = PlatformType::SDL;
        int          windowWidth             = 0;        // headless: the offscreen swapchain's size in pixels
        int          windowHeight            = 0;
        bool         enableValidation        = false;
        bool         enableSyncValidation    = false;
        bool         enableGpuValidation     = false;
        Backend      preferredBackend        = Backend::VULKAN;
        std::string  assetRoot               = {};
        std::string  shaderDir               = {};
        std::string  cacheDir                = "cache";  // compiled shaders and the pipeline cache, beside the executable (an absolute path as is); "": no disk caches
        uint32_t     geometryPoolVertices    = 1u << 21; // 64 B each: 128 MiB
        uint32_t     geometryPoolColors      = 1u << 20; // 4 B each: 4 MiB
        uint32_t     geometryPoolSkins       = 1u << 20; // a skinned vertex's joints and weights, 16 B each: 16 MiB
        uint32_t     geometryPoolMorphs      = 1u << 20; // a morph target's change to a vertex, 24 B each: 24 MiB
        uint32_t     geometryPoolIndices     = 1u << 24; // 4 B each: 64 MiB
        uint32_t     geometryPoolSubMeshes   = 1u << 14; // 136 B each
        uint32_t     maxInstances            = 1u << 16; // 160 B each, twice (one copy per frame slot)
        uint32_t     maxLights               = 1u << 14; // 56 B each, twice
        uint32_t     maxMaterials            = 1u << 12; // materials alive at once, 220 B each, twice
        uint32_t     maxAnimatedEntities     = 1u << 14; // entities that play a clip, 120 B each, twice
        uint32_t     animationPoolBytes      = 16u << 20; // the nodes, key times and values of every loaded animation
        uint32_t     maxMorphWeights         = 1u << 16; // the morph weights of every posed entity, 4 B each, four times
        uint32_t     loadThreads             = 0;        // workers that read and decode loads; 0: the hardware's threads minus one for the main thread (at least 1). A game with threads of its own sets fewer
        uint64_t     loadMemoryBudget        = 512ull << 20; // decoded loads in memory at once (in jobs, or waiting for upload); a load that would pass it waits in the queue
        bool         showStats               = false;
        float        fixedDeltaTime          = 0.0f;     // seconds per frame for the cameras, the UI and the stats' refresh; 0: the measured frame time
        bool         occlusionCulling        = true;     // two-pass Hi-Z culling in the scene passes; false keeps frustum culling only
        bool         meshLods                = true;
        float        lodErrorPixels          = 1.0f;
        int32_t      forcedLod               = -1;
        bool         asyncCompute            = false;
        bool         transferQueue           = true;     // loads copy their data on a queue family that only copies (the copy engine), where the GPU has one; else on the graphics queue
        bool         clusteredLighting       = true;     // false: every pixel shades every light of the scene
        bool         hostImageCopy           = true;     // where the device copies a format on the host into video memory at full speed; false: every texture comes from the staging ring
        bool         bloom                   = false;    // Engine::render adds the bloom passes (Camera::bloomStrength)
        bool         ui                      = false;    // Dear ImGui (CMake RASM_IMGUI): the RASM window on F1 and Engine::setUiCallback
        AntiAliasing antiAliasing            = AntiAliasing::NONE; // Engine::render's scene target: MSAA draws it with several samples per pixel
    };

    // What the engine-wide pools hold now: live objects, and the used part of each pool. A destroyed object's share comes
    // back once the GPU has finished the frames that may use it, so a streaming application stays flat.
    struct PoolStats
    {
        uint32_t     entities                = 0;
        uint32_t     meshes                  = 0;
        uint32_t     materials               = 0;
        uint32_t     lights                  = 0;
        uint32_t     textures                = 0;        // textures and render target attachments
        uint32_t     bindlessTextures        = 0;        // slots of binding 0 held by a texture
        uint64_t     vertices                = 0;        // of EngineConfig::geometryPoolVertices
        uint64_t     indices                 = 0;        // of geometryPoolIndices
        uint64_t     subMeshes               = 0;        // of geometryPoolSubMeshes
        uint64_t     instances               = 0;        // instance rows, of maxInstances
        uint64_t     animationPoolBytes      = 0;        // of animationPoolBytes
        uint32_t     releasesWaiting         = 0;        // destroyed objects the GPU may still use
    };

    // The load workers now. Decoded bytes are those of the loads that started and are not uploaded yet.
    struct LoadStats
    {
        uint32_t     workers                 = 0;
        uint32_t     queued                  = 0;        // jobs that wait for a worker or for the budget
        uint32_t     running                 = 0;
        uint64_t     decodedBytes            = 0;        // of EngineConfig::loadMemoryBudget
        uint64_t     peakDecodedBytes        = 0;        // since the engine started
    };

    struct FrameResources
    {
        CommandBufferHandle commandBuffer;
        uint64_t            fenceValue          = 0; // frameFence value the slot's last graphics submit signals
        uint64_t            computeFenceValue   = 0;
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
        uint32_t                    width       = 0;
        uint32_t                    height      = 0;
        Format                      format      = Format::UNKNOWN;  // the format of levels
        std::vector<unsigned char>  levels;                         // every level, one after another, mips made or from the file
        std::vector<uint64_t>       levelSizes;
    };

    struct EnvironmentRaw
    {
        uint32_t                width   = 0;
        uint32_t                height  = 0;
        std::vector<uint16_t>   pixels;         // RGBA16F, linear
    };

    struct RenderTargetAttachments
    {
        TextureHandle color[rasm::MAX_COLOR_ATTACHMENTS];
        TextureHandle depth;
    };

    struct RenderTarget
    {
        RenderTargetAttachments attachments[rasm::MAX_FRAMES_IN_FLIGHT];
        uint32_t width;
        uint32_t height;
        Format colorFormats[rasm::MAX_COLOR_ATTACHMENTS];
        uint32_t colorCount;
        Format depthFormat;
        glm::vec4 clearColor;
        bool history;
        uint32_t samples = 1;
        uint32_t storageSlots[rasm::MAX_FRAMES_IN_FLIGHT][rasm::MAX_COLOR_ATTACHMENTS] = {}; // binding 1 slot of each color of a storage target per frame slot, UINT32_MAX otherwise
    };

} // namespace rasm
// clang-format on
