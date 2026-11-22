#include "core/cacheFile.h"

#include "doctest/doctest.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using rasm::CacheFileStatus;
using rasm::CacheKey;

namespace
{
    namespace fs = std::filesystem;

    constexpr uint32_t MAGIC = 0x54534554; // "TEST"

    struct TempFolder
    {
        TempFolder() : path(fs::temp_directory_path() / ("rasm_cache_file_tests_" + std::to_string(nextId++)))
        {
            fs::remove_all(path);
            fs::create_directories(path);
        }
        ~TempFolder()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }

        const fs::path path;
        static inline uint32_t nextId = 0;
    };

    CacheKey keyOf(const std::string &text)
    {
        CacheKey key;
        rasm::hashString(key, text);
        return key;
    }

    std::vector<uint8_t> readBytes(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void writeBytes(const fs::path &path, const std::vector<uint8_t> &bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

TEST_CASE("the cache key is FNV-1a 128 and the checksum FNV-1a 64")
{
    CacheKey empty;
    CHECK(rasm::toHex(empty) == "6c62272e07bb014262b821756295c58d");

    CacheKey a;
    rasm::hashBytes(a, "a", 1);
    CHECK(rasm::toHex(a) == "d228cb696f1a8caf78912b704e4a8964");

    CacheKey foobar;
    rasm::hashBytes(foobar, "foobar", 6);
    CHECK(rasm::toHex(foobar) == "343e1662793c64bf6f0d3597ba446f18");

    CHECK(rasm::checksum("a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(rasm::checksum("foobar", 6) == 0x85944171f73967e8ull);
}

TEST_CASE("hashString counts the length, so the same bytes split differently give another key")
{
    CacheKey first;
    rasm::hashString(first, "ab");
    rasm::hashString(first, "c");
    CacheKey second;
    rasm::hashString(second, "a");
    rasm::hashString(second, "bc");
    CHECK_FALSE(first == second);
}

TEST_CASE("a cache file reads back only with its magic, version and key")
{
    TempFolder folder;
    const fs::path path = folder.path / "entry.bin";
    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::vector<uint8_t> read;

    CHECK(rasm::readCacheFile(path, MAGIC, 1, keyOf("a"), read) == CacheFileStatus::MISSING);
    REQUIRE(rasm::writeCacheFile(path, MAGIC, 1, keyOf("a"), payload.data(), payload.size()));
    CHECK(rasm::readCacheFile(path, MAGIC, 1, keyOf("a"), read) == CacheFileStatus::OK);
    CHECK(read == payload);

    // Another GPU, driver or input gives another key; another file layout another version.
    CHECK(rasm::readCacheFile(path, MAGIC, 1, keyOf("b"), read) == CacheFileStatus::STALE);
    CHECK(read.empty());
    CHECK(rasm::readCacheFile(path, MAGIC, 2, keyOf("a"), read) == CacheFileStatus::STALE);
    CHECK(rasm::readCacheFile(path, MAGIC + 1, 1, keyOf("a"), read) == CacheFileStatus::CORRUPT);

    // The rename left no temporary file.
    uint32_t files = 0;
    for ([[maybe_unused]] const auto &entry : fs::directory_iterator(folder.path))
        ++files;
    CHECK(files == 1);
}

TEST_CASE("a short or changed cache file is corrupt")
{
    TempFolder folder;
    const fs::path path = folder.path / "entry.bin";
    const std::vector<uint8_t> payload(100, 7);
    REQUIRE(rasm::writeCacheFile(path, MAGIC, 1, keyOf("a"), payload.data(), payload.size()));
    const std::vector<uint8_t> good = readBytes(path);
    std::vector<uint8_t> read;

    std::vector<uint8_t> truncated(good.begin(), good.end() - 1);
    writeBytes(path, truncated);
    CHECK(rasm::readCacheFile(path, MAGIC, 1, keyOf("a"), read) == CacheFileStatus::CORRUPT);

    std::vector<uint8_t> flipped = good;
    flipped.back() ^= 1;
    writeBytes(path, flipped);
    CHECK(rasm::readCacheFile(path, MAGIC, 1, keyOf("a"), read) == CacheFileStatus::CORRUPT);
    CHECK(read.empty());

    writeBytes(path, std::vector<uint8_t>(good.begin(), good.begin() + 10));
    CHECK(rasm::readCacheFile(path, MAGIC, 1, keyOf("a"), read) == CacheFileStatus::CORRUPT);

    writeBytes(path, good);
    CHECK(rasm::readCacheFile(path, MAGIC, 1, keyOf("a"), read) == CacheFileStatus::OK);
}

TEST_CASE("pruning a cache folder removes the files used longest ago")
{
    TempFolder folder;
    const std::vector<uint8_t> payload(1000, 1);
    const auto now = fs::file_time_type::clock::now();
    for (int i = 0; i < 4; ++i)
    {
        const fs::path path = folder.path / ("entry" + std::to_string(i));
        REQUIRE(rasm::writeCacheFile(path, MAGIC, 1, keyOf("a"), payload.data(), payload.size()));
        fs::last_write_time(path, now - std::chrono::hours(4 - i));
    }

    rasm::pruneCacheFolder(folder.path, 10000);
    CHECK(fs::exists(folder.path / "entry0"));

    // Four files of 1040 bytes are over 3000, so the oldest go until at most 2250 are left.
    rasm::pruneCacheFolder(folder.path, 3000);
    CHECK_FALSE(fs::exists(folder.path / "entry0"));
    CHECK_FALSE(fs::exists(folder.path / "entry1"));
    CHECK(fs::exists(folder.path / "entry2"));
    CHECK(fs::exists(folder.path / "entry3"));
}
