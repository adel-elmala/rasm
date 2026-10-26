// clang-format off
#pragma once

#include <cstdint>
#include <memory>

namespace rasm
{

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
    struct CameraTag;
    struct WindowTag;
    struct ResourceTag;
    struct RenderTargetTag;
    struct LightTag;
    struct DescriptorSetLayoutTag;
    struct DescriptorPoolTag;
    struct DescriptorSetTag;

    using MeshHandle                = Handle<MeshTag>;
    using BufferHandle              = Handle<BufferTag>;
    using TextureHandle             = Handle<TextureTag>;
    using MaterialHandle            = Handle<MaterialTag>;
    using PipelineHandle            = Handle<PipelineTag>;
    using CommandPoolHandle         = Handle<CommandPoolTag>;
    using CommandBufferHandle       = Handle<CommandBufferTag>;
    using SemaphoreHandle           = Handle<SemaphoreTag>;
    using FenceHandle               = Handle<FenceTag>;
    using SwapchainHandle           = Handle<SwapchainTag>;
    using ShaderHandle              = Handle<ShaderTag>;
    using SceneHandle               = Handle<SceneTag>;
    using EntityHandle              = Handle<EntityTag>;
    using CameraHandle              = Handle<CameraTag>;
    using LightHandle               = Handle<LightTag>;
    using WindowHandle              = Handle<WindowTag>;
    using ResourceHandle            = Handle<ResourceTag>;
    using RenderTargetHandle        = Handle<RenderTargetTag>;
    using DescriptorSetLayoutHandle = Handle<DescriptorSetLayoutTag>;
    using DescriptorPoolHandle      = Handle<DescriptorPoolTag>;
    using DescriptorSetHandle       = Handle<DescriptorSetTag>;


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
        uint64_t camera                 = 1;
        uint64_t light                  = 1;
    };

    class HandleManager
    {
    public:
        HandleManager() = default;
        ~HandleManager() = default;


        MeshHandle                  getNextMeshHandle();
        ShaderHandle                getNextShaderHandle();
        BufferHandle                getNextBufferHandle();
        TextureHandle               getNextTextureHandle();
        PipelineHandle              getNextPipelineHandle();
        CommandPoolHandle           getNextCommandPoolHandle();
        CommandBufferHandle         getNextCommandBufferHandle();
        SemaphoreHandle             getNextSemaphoreHandle();
        FenceHandle                 getNextFenceHandle();
        SwapchainHandle             getNextSwapchainHandle();
        RenderTargetHandle          getNextRenderTargetHandle();
        DescriptorSetLayoutHandle   getNextDescriptorSetLayoutHandle();
        DescriptorPoolHandle        getNextDescriptorPoolHandle();
        DescriptorSetHandle         getNextDescriptorSetHandle();
        MaterialHandle              getNextMaterialHandle();
        CameraHandle                getNextCameraHandle();
        EntityHandle                getNextEntityHandle();
        LightHandle                 getNextLightHandle();
        SceneHandle                 getNextSceneHandle();

        private:
            HandleCounters nextHandle = {};

    };

} // namespace rasm

// clang-format on