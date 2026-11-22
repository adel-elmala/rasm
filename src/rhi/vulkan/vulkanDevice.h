// clang-format off
#pragma once

#include "rhi/device.h"
#include "rhi/vulkan/vulkanTypes.h"
#include "rhi/vulkan/vulkanPipelineCache.h"
#include "rhi/vulkan/vulkanQueue.h"

#include "VkBootstrap.h"

#include <memory>
#include <string>

namespace rasm::rhi
{
    class VulkanDevice final : public Device
    {
    public:
                                    VulkanDevice() = default;
                                    ~VulkanDevice() override;
        bool                        initialize(const DeviceDesc& desc);
        Buffer*                     createBuffer(const BufferDesc& desc) override;
        Texture*                    createTexture(const TextureDesc& desc) override;
        MemoryRequirements          getTextureMemoryRequirements(const TextureDesc& desc) override;
        Heap*                       createHeap(uint64_t size, uint64_t alignment, uint32_t memoryTypeBits, const std::string& name) override;
        Texture*                    createPlacedTexture(const TextureDesc& desc, Heap* heap, uint64_t offset) override;
        Sampler*                    createSampler(const SamplerDesc& desc) override;
        Shader*                     createShader(const ShaderDesc& desc) override;
        Pipeline*                   createGraphicsPipeline(const PipelineDesc& desc, Shader* vertexShader, Shader* fragmentShader, DescriptorSetLayout* layout) override;
        Pipeline*                   createComputePipeline(const PipelineDesc& desc, Shader* computeShader, DescriptorSetLayout* layout) override;
        DescriptorSetLayout*        createBindlessDescriptorSetLayout(const BindlessDescriptorSetLayoutDesc& desc) override;
        DescriptorPool*             createDescriptorPool(const DescriptorPoolDesc& desc) override;
        DescriptorSet*              allocateDescriptorSet(const std::string& name, DescriptorPool* pool, DescriptorSetLayout* layout) override;
        CommandBuffer*              createCommandBuffer(QueueType queueType, const std::string& name) override;
        Fence*                      createFence(uint64_t initialValue, const std::string& name) override;
        Swapchain*                  createSwapchain(const SwapchainDesc& desc) override;
        QueryPool*                  createTimestampQueryPool(uint32_t count, const std::string& name) override;
        void                        destroyBuffer(Buffer* buffer) override;
        void                        destroyTexture(Texture* texture) override;
        void                        destroyHeap(Heap* heap) override;
        void                        destroySampler(Sampler* sampler) override;
        void                        destroyShader(Shader* shader) override;
        void                        destroyPipeline(Pipeline* pipeline) override;
        void                        destroyDescriptorSetLayout(DescriptorSetLayout* layout) override;
        void                        destroyDescriptorPool(DescriptorPool* pool) override;
        void                        destroyDescriptorSet(DescriptorSet* descriptorSet) override;
        void                        destroyCommandBuffer(CommandBuffer* commandBuffer) override;
        void                        destroyFence(Fence* fence) override;
        void                        destroySwapchain(Swapchain* swapchain) override;
        void                        destroyQueryPool(QueryPool* pool) override;
        Queue*                      getQueue(QueueType queueType) override;
        bool                        hasAsyncCompute() const override;
        bool                        fillBuffer(Buffer* buffer, const void* data, size_t size, size_t offset) override;
        bool                        readBuffer(Buffer* buffer, void* data, size_t size, size_t offset) override;
        void                        resetQueries(QueryPool* pool) override;
        bool                        getTimestamps(QueryPool* pool, uint32_t firstQuery, uint32_t count, uint64_t* nanoseconds) override;
        bool                        supportsTextureFormat(Format format) const override;
        uint32_t                    supportedSampleCounts() const override;
        bool                        fillTexture(Texture* texture, const TextureLevel* levels, uint32_t levelCount) override;
        void                        updateBindlessTexture(DescriptorSet* descriptorSet, uint32_t slot, Texture* texture, Sampler* sampler) override;
        void                        updateBindlessStorageImage(DescriptorSet* descriptorSet, uint32_t slot, Texture* texture, uint32_t mip) override;
        bool                        waitForFence(Fence* fence, uint64_t value, uint64_t timeout) override;
        uint64_t                    getFenceValue(Fence* fence) override;
        VRAMStats                   queryVRAMStats() override;
        void                        waitIdle() override;
        VkDevice                    getVkDevice() const;
        const VolkDeviceTable&      getVkTable() const;
        const vkb::Device&          getVkbDevice() const;
        VkPhysicalDevice            getVkPhysicalDevice() const;
        VkSurfaceKHR                getSurface() const;
        VulkanQueue*                getGraphicsQueue() const;
        bool                        hasMutableSwapchainFormat() const;
        void                        setObjectName(VkObjectType type, uint64_t handle, const std::string& name);

    private:
        // Per Format value: the device samples it with linear filtering, and the memory types a sampled image of it may
        // use when it is copied on the host (0: it is filled from the staging ring).
        struct FormatSupport
        {
            bool                        sampled             = false;
            uint32_t                    hostCopyTypes       = 0;
        };

        void                            queryFormatSupport(bool hostImageCopy);
        bool                            usesHostCopy(const TextureDesc& desc) const;
        uint32_t                        getImageMemoryTypes(const TextureDesc& desc, bool hostCopy);

        static constexpr uint32_t       FORMAT_COUNT        = static_cast<uint32_t>(Format::R16G16_SFLOAT) + 1;

        FormatSupport                   formatSupport[FORMAT_COUNT] = {};
        VmaAllocator                    allocator           = nullptr;
        vkb::Instance                   instance            = {};
        vkb::PhysicalDevice             physicalDevice      = {};
        vkb::Device                     device              = {};
        VolkDeviceTable                 vk                  = {};
        VulkanPipelineCache             pipelineCache       = {};
        std::unique_ptr<VulkanQueue>    graphicsQueue       = {};
        std::unique_ptr<VulkanQueue>    computeQueue        = {};
        uint32_t                        sharedFamilies[2]   = {};
        uint32_t                        sharedFamilyCount   = 0;
        VkSurfaceKHR                    surface             = VK_NULL_HANDLE;
        bool                            debugUtilsEnabled   = false;
        bool                            swapchainMutable    = false;    // VK_KHR_swapchain_mutable_format: the swapchain has a UNORM view
        uint64_t                        timestampMask       = 0;
        double                          timestampPeriod     = 0.0;
        VkDeviceSize                    maxBufferSize       = 0;
        uint32_t                        maxStorageImages    = 0;    // update-after-bind storage images in a set and in a stage
        uint32_t                        sampleCounts        = VK_SAMPLE_COUNT_1_BIT;
    };

    std::unique_ptr<Device>         createVulkanDevice(const DeviceDesc& desc);
}
// clang-format on
