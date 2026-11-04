// clang-format off
#pragma once

#include "rhi/device.h"
#include "rhi/vulkan/vulkanTypes.h"

#include "VkBootstrap.h"

#include <memory>
#include <string>

namespace rasm::rhi
{
    class VulkanQueue final : public Queue
    {
    public:
                                    VulkanQueue(VkQueue queue, uint32_t familyIndex);
        bool                        submit(const SubmitDesc& desc) override;
        VkQueue                     getVkQueue() const;
        uint32_t                    getFamilyIndex() const;

    private:
        VkQueue                     queue                   = VK_NULL_HANDLE;
        uint32_t                    familyIndex             = 0;
    };

    class VulkanDevice final : public Device
    {
    public:
                                    VulkanDevice() = default;
                                    ~VulkanDevice() override;
        bool                        initialize(const DeviceDesc& desc);
        Buffer*                     createBuffer(const BufferDesc& desc) override;
        Texture*                    createTexture(const TextureDesc& desc) override;
        Sampler*                    createSampler(const SamplerDesc& desc) override;
        Shader*                     createShader(const ShaderDesc& desc) override;
        Pipeline*                   createGraphicsPipeline(const PipelineDesc& desc, Shader* vertexShader, Shader* fragmentShader, DescriptorSetLayout* layout) override;
        DescriptorSetLayout*        createBindlessDescriptorSetLayout(const BindlessDescriptorSetLayoutDesc& desc) override;
        DescriptorPool*             createDescriptorPool(const DescriptorPoolDesc& desc) override;
        DescriptorSet*              allocateDescriptorSet(const std::string& name, DescriptorPool* pool, DescriptorSetLayout* layout) override;
        CommandBuffer*              createCommandBuffer(QueueType queueType, const std::string& name) override;
        Fence*                      createFence(uint64_t initialValue, const std::string& name) override;
        Swapchain*                  createSwapchain(const SwapchainDesc& desc) override;
        void                        destroyBuffer(Buffer* buffer) override;
        void                        destroyTexture(Texture* texture) override;
        void                        destroySampler(Sampler* sampler) override;
        void                        destroyShader(Shader* shader) override;
        void                        destroyPipeline(Pipeline* pipeline) override;
        void                        destroyDescriptorSetLayout(DescriptorSetLayout* layout) override;
        void                        destroyDescriptorPool(DescriptorPool* pool) override;
        void                        destroyDescriptorSet(DescriptorSet* descriptorSet) override;
        void                        destroyCommandBuffer(CommandBuffer* commandBuffer) override;
        void                        destroyFence(Fence* fence) override;
        void                        destroySwapchain(Swapchain* swapchain) override;
        Queue*                      getQueue(QueueType queueType) override;
        bool                        fillBuffer(Buffer* buffer, const void* data, size_t size, size_t offset) override;
        bool                        fillTexture(Texture* texture, const void* data) override;
        void                        updateBindlessTexture(DescriptorSet* descriptorSet, uint32_t slot, Texture* texture, Sampler* sampler) override;
        bool                        waitForFence(Fence* fence, uint64_t value, uint64_t timeout) override;
        VRAMStats                   queryVRAMStats() override;
        void                        waitIdle() override;
        VkDevice                    getVkDevice() const;
        const vkb::Device&          getVkbDevice() const;
        VkPhysicalDevice            getVkPhysicalDevice() const;
        VkSurfaceKHR                getSurface() const;
        VulkanQueue*                getGraphicsQueue() const;
        void                        setObjectName(VkObjectType type, uint64_t handle, const std::string& name);

    private:
        VmaAllocator                    allocator           = nullptr;
        vkb::Instance                   instance            = {};
        vkb::PhysicalDevice             physicalDevice      = {};
        vkb::Device                     device              = {};
        vkb::DispatchTable              dispatchTable       = {};
        std::unique_ptr<VulkanQueue>    graphicsQueue       = {};
        VkSurfaceKHR                    surface             = VK_NULL_HANDLE;
        bool                            debugUtilsEnabled   = false;
    };

    std::unique_ptr<Device>         createVulkanDevice(const DeviceDesc& desc);
}
// clang-format on
