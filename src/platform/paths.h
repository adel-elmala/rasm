// clang-format off
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

namespace rasm
{
    std::filesystem::path   pathFromUtf8(const std::string& text);
    std::string             pathToUtf8(const std::filesystem::path& path);
    // Relative paths resolve against the folder of the binary that holds RASM: the executable, or the shared library
    // that passed an address in its code here (the C API) before anything resolved.
    void                    setBaseModule(const void* address);
    std::string             getBaseDir();
    std::string             resolveFromBaseDir(const std::string& path);
    std::string             resolveCacheDir(const std::string& cacheDir);
    std::string             getModulePath(const void* address);
    // fopen for a UTF-8 path, which the C runtime reads in the ANSI code page on Windows. Binary read; null on failure.
    std::FILE*              openFileForReading(const std::string& path);
}
// clang-format on
