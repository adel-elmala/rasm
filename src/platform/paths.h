// clang-format off
#pragma once

#include <filesystem>
#include <string>

namespace rasm
{
    std::filesystem::path   pathFromUtf8(const std::string& text);
    std::string             pathToUtf8(const std::filesystem::path& path);
    std::string             resolveFromExecutableDir(const std::string& path);
}
// clang-format on
