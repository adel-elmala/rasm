#include "rhi/vulkan/vulkanPipelineCache.h"
#include "platform/paths.h"
#include "rhi/vulkan/vulkanUtils.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <cstring>
#include <system_error>
#include <vector>

namespace rasm::rhi
{
    namespace
    {
        constexpr uint32_t PIPELINE_CACHE_MAGIC = 0x50534152; // "RASP"
        constexpr uint32_t PIPELINE_CACHE_VERSION = 1;
        // A Vulkan cache cannot drop the entries of pipelines no run makes any more, so past this it starts over.
        constexpr uint64_t MAX_PIPELINE_CACHE_BYTES = 256ull << 20;
    }

    namespace
    {
        // Some drivers crash on data from another device instead of ignoring it, so Vulkan's own header is checked too.
        bool matchesDevice(const std::vector<uint8_t> &data, uint32_t vendorID, uint32_t deviceID, const uint8_t *uuid)
        {
            VkPipelineCacheHeaderVersionOne header = {};
            if (data.size() < sizeof(header))
                return false;
            std::memcpy(&header, data.data(), sizeof(header));
            return header.headerSize >= sizeof(header) && header.headerSize <= data.size() && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                   header.vendorID == vendorID && header.deviceID == deviceID && std::memcmp(header.pipelineCacheUUID, uuid, VK_UUID_SIZE) == 0;
        }

        std::vector<uint8_t> getCacheData(const VolkDeviceTable &vk, VkDevice device, VkPipelineCache cache)
        {
            size_t size = 0;
            if (vk.vkGetPipelineCacheData(device, cache, &size, nullptr) != VK_SUCCESS)
                return {};
            std::vector<uint8_t> data(size);
            // VK_INCOMPLETE would mean a partial copy, which is not worth saving.
            if (vk.vkGetPipelineCacheData(device, cache, &size, data.data()) != VK_SUCCESS)
                return {};
            data.resize(size);
            return data;
        }

        VkPipelineCache createCache(const VolkDeviceTable &vk, VkDevice device, const std::vector<uint8_t> &data)
        {
            const VkPipelineCacheCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = data.size(), .pInitialData = data.empty() ? nullptr : data.data()};
            VkPipelineCache cache = VK_NULL_HANDLE;
            const VkResult result = vk.vkCreatePipelineCache(device, &info, nullptr, &cache);
            if (result != VK_SUCCESS)
            {
                spdlog::warn("Failed to create the pipeline cache. Error: {}", toVkResultString(result));
                return VK_NULL_HANDLE;
            }
            return cache;
        }
    }

    void VulkanPipelineCache::load(const VolkDeviceTable &vk, VkDevice device, const VkPhysicalDeviceProperties &properties, const std::string &cacheDir)
    {
        ZoneScoped;

        if (cacheDir.empty())
            return;
        vendorID = properties.vendorID;
        deviceID = properties.deviceID;
        std::memcpy(uuid, properties.pipelineCacheUUID, VK_UUID_SIZE);
        // One file per GPU, so a machine with two keeps both. A new driver changes the identity, and its file starts over.
        hashNumber(identity, vendorID);
        hashNumber(identity, deviceID);
        hashNumber(identity, properties.driverVersion);
        hashBytes(identity, uuid, VK_UUID_SIZE);
        path = pathFromUtf8(cacheDir) / fmt::format("vulkan-{:04x}-{:04x}.pipelines", vendorID, deviceID);

        std::vector<uint8_t> data;
        CacheFileStatus status = readCacheFile(path, PIPELINE_CACHE_MAGIC, PIPELINE_CACHE_VERSION, identity, data);
        if (status == CacheFileStatus::OK && !matchesDevice(data, vendorID, deviceID, uuid))
            status = CacheFileStatus::CORRUPT;
        if (status == CacheFileStatus::OK)
            spdlog::info("Pipeline cache: {} KiB from {}", data.size() >> 10, pathToUtf8(path));
        else if (status == CacheFileStatus::STALE)
            spdlog::info("Pipeline cache {} is from another driver or version; starting a new one.", pathToUtf8(path));
        else if (status == CacheFileStatus::CORRUPT)
            spdlog::warn("Pipeline cache {} is damaged; starting a new one.", pathToUtf8(path));
        if (status != CacheFileStatus::OK)
            data.clear();

        cache = createCache(vk, device, data);
        if (cache == VK_NULL_HANDLE && !data.empty())
        {
            data.clear();
            cache = createCache(vk, device, data);
        }
        loadedChecksum = checksum(data.data(), data.size());
    }

    void VulkanPipelineCache::save(const VolkDeviceTable &vk, VkDevice device)
    {
        ZoneScoped;

        if (cache == VK_NULL_HANDLE)
            return;
        std::vector<uint8_t> data = getCacheData(vk, device, cache);
        if (!data.empty() && checksum(data.data(), data.size()) != loadedChecksum)
        {
            // Another run may have saved since this one loaded; merging keeps its pipelines too.
            std::vector<uint8_t> saved;
            if (readCacheFile(path, PIPELINE_CACHE_MAGIC, PIPELINE_CACHE_VERSION, identity, saved) == CacheFileStatus::OK && matchesDevice(saved, vendorID, deviceID, uuid) &&
                checksum(saved.data(), saved.size()) != loadedChecksum)
            {
                VkPipelineCache other = createCache(vk, device, saved);
                if (other != VK_NULL_HANDLE && vk.vkMergePipelineCaches(device, cache, 1, &other) == VK_SUCCESS)
                    data = getCacheData(vk, device, cache);
                if (other != VK_NULL_HANDLE)
                    vk.vkDestroyPipelineCache(device, other, nullptr);
            }

            std::error_code error;
            if (data.size() > MAX_PIPELINE_CACHE_BYTES)
            {
                std::filesystem::remove(path, error);
                spdlog::info("Pipeline cache: over {} MiB, removed; the next run starts a new one.", MAX_PIPELINE_CACHE_BYTES >> 20);
            }
            else if (!data.empty() && writeCacheFile(path, PIPELINE_CACHE_MAGIC, PIPELINE_CACHE_VERSION, identity, data.data(), data.size()))
            {
                spdlog::info("Pipeline cache: saved {} KiB", data.size() >> 10);
            }
        }
        vk.vkDestroyPipelineCache(device, cache, nullptr);
        cache = VK_NULL_HANDLE;
    }

    VkPipelineCache VulkanPipelineCache::get() const
    {
        return cache;
    }
}
