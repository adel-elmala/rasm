// clang-format off
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

namespace rasm
{
    std::filesystem::path   pathFromUtf8(const std::string& text);
    std::string             pathToUtf8(const std::filesystem::path& path);
    std::string             resolveFromExecutableDir(const std::string& path);
    std::string             resolveCacheDir(const std::string& cacheDir);
    // fopen for a UTF-8 path, which the C runtime reads in the ANSI code page on Windows. Binary read; null on failure.
    std::FILE*              openFileForReading(const std::string& path);
}
// clang-format on
