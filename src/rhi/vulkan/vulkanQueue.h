// clang-format off
#pragma once

#include "rhi/device.h"
#include "rhi/vulkan/vulkanTypes.h"

#include <cstdint>

namespace rasm::rhi
{
    class VulkanQueue final : public Queue
    {
    public:
                                    VulkanQueue(const VolkDeviceTable& vk, VkQueue queue, uint32_t familyIndex);
        bool                        submit(const SubmitDesc& desc) override;
        VkQueue                     getVkQueue() const;
        uint32_t                    getFamilyIndex() const;

    private:
        const VolkDeviceTable&      vk;
        VkQueue                     queue                   = VK_NULL_HANDLE;
        uint32_t                    familyIndex             = 0;
    };
}
// clang-format on
