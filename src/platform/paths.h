#pragma once

#include <filesystem>
#include <string>

namespace rasm
{
    std::filesystem::path   pathFromUtf8(const std::string &text);
    std::string             pathToUtf8(const std::filesystem::path &path);
    std::filesystem::path   getExecutableDir();
    std::string             resolveFromExecutableDir(const std::string &path);
}
