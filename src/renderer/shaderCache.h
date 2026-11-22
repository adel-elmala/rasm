// clang-format off
#pragma once

#include "core/cacheFile.h"
#include "renderer/shaderCompiler.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rasm
{
    constexpr uint32_t          SHADER_CACHE_VERSION    = 1;
    constexpr uint64_t          MAX_SHADER_CACHE_BYTES  = 256ull << 20;

    struct CachedShader
    {
        std::vector<uint8_t>    code;
        std::string             pushConstants;
        StructLayouts           layouts;
    };

    bool                        readCachedShader(const std::filesystem::path& path, const CacheKey& key, CachedShader& shader);
    bool                        writeCachedShader(const std::filesystem::path& path, const CacheKey& key, const CachedShader& shader);
}
// clang-format on
