// clang-format off
#pragma once

#include "rhi/device.h"

#include "vulkan/vulkan.h"

#include <string>

namespace rasm::rhi
{
    class VulkanDevice;

    class VulkanCommandBuffer final : public CommandBuffer
    {
    public:
                                    VulkanCommandBuffer(VulkanDevice& device);
                                    ~VulkanCommandBuffer() override;
        bool                        initialize(uint32_t queueFamilyIndex, const std::string& name);
        bool                        begin() override;
        bool                        end() override;
        bool                        transitionTexture(Texture* texture, TextureUsage oldUsage, TextureUsage newUsage) override;
        void                        beginRendering(Texture* colorAttachment, Texture* depthAttachment, const glm::vec4& clearColor, const AttachmentOps& ops) override;
        void                        endRendering() override;
        void                        setViewport(float x, float y, float width, float height, float minDepth, float maxDepth) override;
        void                        setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override;
        void                        setCullMode(CullMode cullMode) override;
        void                        bindPipeline(Pipeline* pipeline) override;
        void                        bindIndexBuffer(Buffer* buffer, uint64_t offset, Format indexType) override;
        void                        bindDescriptorSet(Pipeline* pipeline, DescriptorSet* descriptorSet, uint32_t setIndex) override;
        void                        pushConstants(Pipeline* pipeline, ShaderType stages, const void* data, uint32_t size, uint32_t offset) override;
        void                        draw(uint32_t vertexCount, uint32_t instanceCount) override;
        void                        drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t vertexOffset, uint32_t firstIndex, uint32_t firstInstance) override;
        void                        drawIndexedIndirect(Buffer* buffer, uint64_t offset, uint32_t drawCount, uint32_t stride) override;
        void                        drawIndexedIndirectCount(Buffer* buffer, uint64_t offset, Buffer* countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride) override;
        void                        dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
        void                        memoryBarrier(PipelineStage before, PipelineStage after) override;
        void                        copyBuffers(Buffer* src, const BufferCopy* copies, uint32_t copyCount) override;
        void                        writeTimestamp(QueryPool* queryPool, uint32_t index) override;
        VkCommandBuffer             getVkCommandBuffer() const;

    private:
        VulkanDevice&               device;
        VkCommandPool               pool                    = VK_NULL_HANDLE;
        VkCommandBuffer             commandBuffer           = VK_NULL_HANDLE;
    };
}
// clang-format on
