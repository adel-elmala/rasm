// clang-format off
#pragma once

#include "core/cacheFile.h"
#include "rhi/vulkan/vulkanTypes.h"

#include <filesystem>
#include <string>

namespace rasm::rhi
{
    class VulkanPipelineCache
    {
    public:
        void                            load(const VolkDeviceTable& vk, VkDevice device, const VkPhysicalDeviceProperties& properties, const std::string& cacheDir);
        void                            save(const VolkDeviceTable& vk, VkDevice device);
        VkPipelineCache                 get() const;

    private:
        VkPipelineCache                 cache               = VK_NULL_HANDLE;
        std::filesystem::path           path;
        CacheKey                        identity;
        uint64_t                        loadedChecksum      = 0;
        uint32_t                        vendorID            = 0;
        uint32_t                        deviceID            = 0;
        uint8_t                         uuid[VK_UUID_SIZE]  = {};
    };
}
// clang-format on
