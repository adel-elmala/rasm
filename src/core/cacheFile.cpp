#include "core/cacheFile.h"
#include "platform/paths.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>

namespace rasm
{
    namespace
    {
        struct FileHeader
        {
            uint32_t magic;
            uint32_t version;
            uint64_t keyHigh;
            uint64_t keyLow;
            uint64_t payloadSize;
            uint64_t payloadChecksum;
        };
        static_assert(sizeof(FileHeader) == 40);

        struct FolderFile
        {
            std::filesystem::path path;
            std::filesystem::file_time_type time;
            uint64_t size;
        };
    }

    namespace
    {
        // The low 64 bits of x times the FNV-1a 128 prime's low part (0x13B), and the carry into the high 64 bits.
        uint64_t multiplyLow(uint64_t x, uint64_t &carry)
        {
            constexpr uint64_t PRIME_LOW = 0x13B;
            const uint64_t high = (x >> 32) * PRIME_LOW;
            const uint64_t low = (x & 0xffffffffull) * PRIME_LOW;
            carry = (high + (low >> 32)) >> 32;
            return (high << 32) + low;
        }
    }

    // FNV-1a 128: xor in a byte, multiply by 2^88 + 0x13B, which is a shift by 88 bits plus a multiply by a small number.
    void hashBytes(CacheKey &key, const void *data, size_t size)
    {
        const auto *bytes = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; ++i)
        {
            const uint64_t low = key.low ^ bytes[i];
            uint64_t carry = 0;
            key.low = multiplyLow(low, carry);
            key.high = key.high * 0x13B + carry + (low << 24);
        }
    }

    void hashString(CacheKey &key, const std::string &text)
    {
        hashNumber(key, text.size());
        hashBytes(key, text.data(), text.size());
    }

    void hashNumber(CacheKey &key, uint64_t value)
    {
        hashBytes(key, &value, sizeof(value));
    }

    std::string toHex(const CacheKey &key)
    {
        return fmt::format("{:016x}{:016x}", key.high, key.low);
    }

    uint64_t checksum(const void *data, size_t size)
    {
        const auto *bytes = static_cast<const uint8_t *>(data);
        uint64_t hash = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 0x100000001b3ull;
        return hash;
    }

    CacheFileStatus readCacheFile(const std::filesystem::path &path, uint32_t magic, uint32_t version, const CacheKey &key, std::vector<uint8_t> &payload)
    {
        payload.clear();
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return CacheFileStatus::MISSING;

        FileHeader header = {};
        if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)) || header.magic != magic)
            return CacheFileStatus::CORRUPT;
        if (header.version != version || header.keyHigh != key.high || header.keyLow != key.low)
            return CacheFileStatus::STALE;

        std::error_code error;
        const uint64_t fileSize = std::filesystem::file_size(path, error);
        if (error || fileSize != sizeof(header) + header.payloadSize)
            return CacheFileStatus::CORRUPT;
        payload.resize(header.payloadSize);
        if (!file.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(payload.size())) ||
            checksum(payload.data(), payload.size()) != header.payloadChecksum)
        {
            payload.clear();
            return CacheFileStatus::CORRUPT;
        }
        return CacheFileStatus::OK;
    }

    bool writeCacheFile(const std::filesystem::path &path, uint32_t magic, uint32_t version, const CacheKey &key, const void *payload, size_t size)
    {
        // A name of its own, so two runs that write the same file at once each rename a whole file.
        std::random_device random;
        std::filesystem::path temporary = path;
        temporary += fmt::format(".{:08x}{:08x}.tmp", random(), random());

        const FileHeader header = {.magic = magic, .version = version, .keyHigh = key.high, .keyLow = key.low, .payloadSize = size, .payloadChecksum = checksum(payload, size)};
        bool written = false;
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            written = file && file.write(reinterpret_cast<const char *>(&header), sizeof(header)) &&
                      file.write(static_cast<const char *>(payload), static_cast<std::streamsize>(size)) && file.flush();
        }

        std::error_code error;
        if (written)
            std::filesystem::rename(temporary, path, error);
        if (!written || error)
        {
            spdlog::warn("Could not write the cache file {}{}{}.", pathToUtf8(path), error ? ": " : "", error ? error.message() : "");
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }

    void pruneCacheFolder(const std::filesystem::path &folder, uint64_t maxBytes)
    {
        std::error_code error;
        std::vector<FolderFile> files;
        uint64_t total = 0;
        for (const auto &entry : std::filesystem::directory_iterator(folder, error))
        {
            std::error_code entryError;
            if (!entry.is_regular_file(entryError))
                continue;
            const uint64_t size = entry.file_size(entryError);
            const auto time = entry.last_write_time(entryError);
            if (entryError)
                continue;
            files.push_back({entry.path(), time, size});
            total += size;
        }
        if (error || total <= maxBytes)
            return;

        std::sort(files.begin(), files.end(), [](const FolderFile &a, const FolderFile &b)
                  { return a.time < b.time; });
        uint32_t removed = 0;
        for (const auto &file : files)
        {
            if (total <= maxBytes / 4 * 3)
                break;
            if (std::filesystem::remove(file.path, error))
            {
                total -= file.size;
                ++removed;
            }
        }
        spdlog::info("Cache folder {} was over {} MiB: removed the {} files used longest ago.", pathToUtf8(folder), maxBytes >> 20, removed);
    }
}
