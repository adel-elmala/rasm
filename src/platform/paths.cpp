#include "platform/paths.h"

#include "SDL3/SDL_error.h"
#include "SDL3/SDL_filesystem.h"
#include "spdlog/spdlog.h"

namespace rasm
{
    // std::filesystem::path treats a narrow string as the Windows ANSI code page, and path::string() throws on
    // characters that page can't hold, so a folder with a non-Latin name needs these. SDL and Slang both use UTF-8.
    std::filesystem::path pathFromUtf8(const std::string &text)
    {
        return std::filesystem::path(reinterpret_cast<const char8_t *>(text.c_str()));
    }

    std::string pathToUtf8(const std::filesystem::path &path)
    {
        const std::u8string text = path.generic_u8string();
        return std::string(reinterpret_cast<const char *>(text.c_str()), text.size());
    }

    std::filesystem::path getExecutableDir()
    {
        const char *basePath = SDL_GetBasePath();
        if (basePath == nullptr)
        {
            // An empty result leaves callers relative to the working directory, which is the best guess left.
            spdlog::error("Failed to find the executable's directory: {}", SDL_GetError());
            return {};
        }
        return pathFromUtf8(basePath);
    }

    std::string resolveFromExecutableDir(const std::string &path)
    {
        // operator/ keeps an absolute right-hand side as it is.
        return pathToUtf8((getExecutableDir() / pathFromUtf8(path)).lexically_normal());
    }
}
