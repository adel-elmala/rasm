// clang-format off
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rasm
{
    struct CacheKey
    {
        uint64_t                high                = 0x6c62272e07bb0142ull;
        uint64_t                low                 = 0x62b821756295c58dull;

        bool                    operator==(const CacheKey& other) const = default;
    };

    enum class CacheFileStatus
    {
        OK,
        MISSING,
        STALE,
        CORRUPT,
    };

    void                        hashBytes(CacheKey& key, const void* data, size_t size);
    void                        hashString(CacheKey& key, const std::string& text);
    void                        hashNumber(CacheKey& key, uint64_t value);
    std::string                 toHex(const CacheKey& key);
    uint64_t                    checksum(const void* data, size_t size);

    CacheFileStatus             readCacheFile(const std::filesystem::path& path, uint32_t magic, uint32_t version, const CacheKey& key, std::vector<uint8_t>& payload);
    bool                        writeCacheFile(const std::filesystem::path& path, uint32_t magic, uint32_t version, const CacheKey& key, const void* payload, size_t size);
    void                        pruneCacheFolder(const std::filesystem::path& folder, uint64_t maxBytes);
}
// clang-format on
