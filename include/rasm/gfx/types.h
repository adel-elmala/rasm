// clang-format off
#pragma once

#include "VkBootstrap.h"
#include "vk_mem_alloc.h"

#include "rasm/core/types.h"
namespace rasm::gfx
{
    struct BufferVKHandle
    {
        ResourceDesc desc;
        BufferHandle handle;
        VkBuffer buffer;
        VmaAllocation allocation;
        VmaAllocationInfo allocationInfo;
    };

    struct TextureVKHandle
    {
        ResourceDesc desc;
        TextureHandle handle;
        VkImage image;
        VkImageView view;
        VkSampler sampler;
        VmaAllocation allocation;
    };

    struct ShaderVKHandle
    {
        ResourceDesc desc;
        ShaderHandle handle;
        VkShaderModule module;
    };

    struct PipelineVKHandle
    {
        ResourceDesc desc;
        PipelineHandle handle;
        VkPipeline pipeline;
        VkPipelineLayout layout;
    };

    struct CommandPoolVKHandle
    {
        CommandPoolHandle handle;
        VkCommandPool commandPool;
    };

    struct CommandBufferVKHandle
    {
        CommandBufferHandle handle;
        VkCommandBuffer commandBuffer;
    };

    struct SemaphoreVKHandle
    {
        enum class Type
        {
            BINARY,
            TIMELINE
        } type;
        SemaphoreHandle handle;
        VkSemaphore semaphore;
    };

    struct FenceVKHandle
    {
        FenceHandle handle;
        VkFence fence;
    };

    struct SwapchainVKHandle
    {
        SwapchainHandle handle;
        vkb::Swapchain swapchain;
        std::vector<TextureVKHandle> images;
        Format imageFormat;
        uint32_t imageCount;
    };

    struct DescriptorSetLayoutVKHandle
    {
        ResourceDesc desc;
        DescriptorSetLayoutHandle handle;
        VkDescriptorSetLayout layout;
    };

    struct DescriptorPoolVKHandle
    {
        ResourceDesc desc;
        DescriptorPoolHandle handle;
        VkDescriptorPool pool;
    };

    struct DescriptorSetVKHandle
    {
        ResourceDesc desc;
        DescriptorSetHandle handle;
        DescriptorPoolVKHandle pool;
        DescriptorSetLayoutVKHandle layout;
        VkDescriptorSet set;
    };


    // -------------------------------------------
    // Graphics types for uber-material rendering
    // -------------------------------------------

    enum VertexFormat : uint32_t { Full, PosOnly };

    struct VertexFull       { glm::vec3 pos; glm::vec3 normal; glm::vec2 uv; };
    struct VertexPosOnly    { glm::vec3 pos; };

    struct DrawInfo
    {
        uint32_t        indexCount;
        uint32_t        instanceCount;
        uint32_t        firstIndex;
        int32_t         vertexOffset;
        uint32_t        firstInstance;
    };
    
    struct MetallicRoughnessMaterial
    {
        glm::vec3       baseColorFactor;
        uint32_t        baseColorTextureIndex;
        uint32_t        metallicRoughnessTextureIndex;
        uint32_t        metallicRoughtnessTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        float           metallicFactor;
        float           roughnessFactor;
    };

    struct NormalMaterial
    {
        uint32_t        normalTextureIndex;
        uint32_t        normalTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        float           scale;
    };

    struct OcclusionMaterial
    {
        uint32_t        occlusionTextureIndex;
        uint32_t        occlusionTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        float           strength;
    };

    struct EmissiveMaterial
    {
        uint32_t        emissiveTextureIndex;
        uint32_t        emissiveTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        glm::vec3       emissiveFactor;
    };

    struct Material
    {
        MetallicRoughnessMaterial   pbr;
        NormalMaterial              normal;
        OcclusionMaterial           occlusion;
        EmissiveMaterial            emissive;
    };

    struct Modeldata
    {
        glm::mat4       worldMatrix; 
        Material        material;
        VertexFormat    vertFormat;
    };

    struct SceneData
    {
        glm::mat4        projection;
        glm::mat4        view;
        uint64_t         vertsPtr;
        uint64_t         drawInfoPtr;
        uint64_t         modeldataPtr;
    };

}
// clang-format on
