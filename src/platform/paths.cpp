#include "platform/paths.h"

#include "spdlog/spdlog.h"

#include <cstdlib>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace rasm
{
    namespace
    {
        // The C API's shared library sets it to an address in its own code (setBaseModule); a static application leaves it null.
        const void *baseModule = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set once, before any path resolves

        // The folder of the binary that holds RASM. A static application's is the executable's, as SDL_GetBasePath gave
        // it: in a macOS app bundle, Contents/Resources beside the executable's Contents/MacOS. The C API's is the shared
        // library's, so its shaders beside it are found under any host process (python.exe).
        std::filesystem::path getBaseFolder()
        {
            const std::string binary = getModulePath(baseModule);
            if (binary.empty())
            {
                // An empty result leaves callers relative to the working directory, which is the best guess left.
                spdlog::error("Failed to find the folder of the binary that holds RASM.");
                return {};
            }
            const std::filesystem::path folder = pathFromUtf8(binary).lexically_normal().parent_path();
#if defined(__APPLE__)
            if (baseModule == nullptr && folder.filename() == "MacOS" && folder.parent_path().filename() == "Contents")
                return folder.parent_path() / "Resources";
#endif
            return folder;
        }

        std::string getExecutableName()
        {
            const std::string name = pathToUtf8(pathFromUtf8(getModulePath(nullptr)).stem());
            return name.empty() ? "app" : name;
        }

        // The user's folder for local data an application can rebuild: not roamed, not backed up.
        std::filesystem::path getUserCacheDir()
        {
#if defined(_WIN32)
            wchar_t *localAppData = nullptr;
            size_t length = 0;
            std::filesystem::path folder;
            if (_wdupenv_s(&localAppData, &length, L"LOCALAPPDATA") == 0 && localAppData != nullptr && localAppData[0] != 0)
                folder = localAppData;
            std::free(localAppData);
            return folder;
#else
            const char *xdgCache = std::getenv("XDG_CACHE_HOME"); // NOLINT(concurrency-mt-unsafe): read once, at startup
            const char *home = std::getenv("HOME");               // NOLINT(concurrency-mt-unsafe): read once, at startup
#if defined(__APPLE__)
            xdgCache = nullptr;
#endif
            if (xdgCache != nullptr && pathFromUtf8(xdgCache).is_absolute())
                return pathFromUtf8(xdgCache);
            if (home == nullptr || home[0] == 0)
                return {};
#if defined(__APPLE__)
            return pathFromUtf8(home) / "Library" / "Caches";
#else
            return pathFromUtf8(home) / ".cache";
#endif
#endif
        }

        // The folder exists, or was made, and a file can be written in it.
        bool isWritableFolder(const std::filesystem::path &folder)
        {
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            if (error || !std::filesystem::is_directory(folder, error))
                return false;
            const std::filesystem::path probe = folder / ".rasm-write-test";
            const bool written = static_cast<bool>(std::ofstream(probe, std::ios::binary) << 'x');
            std::filesystem::remove(probe, error);
            return written;
        }
    }

    // std::filesystem::path treats a narrow string as the Windows ANSI code page, and path::string() throws on
    // characters that page can't hold, so a folder with a non-Latin name needs these. SDL and Slang both use UTF-8.
    std::filesystem::path pathFromUtf8(const std::string &text)
    {
        try
        {
            return std::filesystem::path(reinterpret_cast<const char8_t *>(text.c_str()));
        }
        catch (const std::system_error &) // NOLINT(bugprone-empty-catch): falls through to the code page below
        {
            // Not UTF-8, which is what path::string() gives for a non-ASCII path in the ANSI code page.
        }

        try
        {
            return std::filesystem::path(text);
        }
        catch (const std::system_error &error)
        {
            spdlog::error("Path '{}' is neither UTF-8 nor in the system code page: {}", text, error.what());
            return {};
        }
    }

    std::string pathToUtf8(const std::filesystem::path &path)
    {
        const std::u8string text = path.generic_u8string();
        return std::string(reinterpret_cast<const char *>(text.c_str()), text.size());
    }

    void setBaseModule(const void *address)
    {
        baseModule = address;
    }

    std::string getBaseDir()
    {
        return pathToUtf8(getBaseFolder());
    }

    std::string resolveFromBaseDir(const std::string &path)
    {
        // operator/ keeps an absolute right-hand side as it is.
        return pathToUtf8((getBaseFolder() / pathFromUtf8(path)).lexically_normal());
    }

    std::string resolveCacheDir(const std::string &cacheDir)
    {
        if (cacheDir.empty())
            return {};
        const std::filesystem::path configured = pathFromUtf8(cacheDir);
        const std::filesystem::path folder = (getBaseFolder() / configured).lexically_normal();
        if (isWritableFolder(folder))
            return pathToUtf8(folder);
        // An installed application's folder is often read-only; a relative folder then goes to the user's cache folder.
        const std::filesystem::path userDir = getUserCacheDir();
        const std::filesystem::path fallback = userDir.empty() ? std::filesystem::path() : userDir / "rasm" / pathFromUtf8(getExecutableName());
        if (!configured.is_absolute() && !fallback.empty() && isWritableFolder(fallback))
        {
            spdlog::info("Cannot write {}, so the shader and pipeline caches are in {}.", pathToUtf8(folder), pathToUtf8(fallback));
            return pathToUtf8(fallback);
        }
        spdlog::warn("Cannot write the cache folder {}; shaders and pipelines are built without disk caches.", pathToUtf8(folder));
        return {};
    }

    // The executable for nullptr, else the executable or shared library whose code holds address.
    std::string getModulePath(const void *address)
    {
#if defined(_WIN32)
        HMODULE module = nullptr;
        if (address != nullptr && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module))
            return {};
        // A path may be longer than MAX_PATH; a full buffer means it was cut.
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
                return {};
            if (length < buffer.size())
                return pathToUtf8(std::filesystem::path(buffer.substr(0, length)));
            buffer.resize(buffer.size() * 2);
        }
#else
        if (address != nullptr)
        {
            Dl_info info = {};
            return dladdr(address, &info) != 0 && info.dli_fname != nullptr ? pathToUtf8(pathFromUtf8(info.dli_fname)) : std::string();
        }
#if defined(__APPLE__)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buffer(size, '\0');
        return _NSGetExecutablePath(buffer.data(), &size) == 0 ? pathToUtf8(pathFromUtf8(buffer.c_str())) : std::string();
#else
        std::error_code error;
        return pathToUtf8(std::filesystem::read_symlink("/proc/self/exe", error));
#endif
#endif
    }

    std::FILE *openFileForReading(const std::string &path)
    {
#ifdef _WIN32
        std::FILE *file = nullptr;
        return _wfopen_s(&file, pathFromUtf8(path).c_str(), L"rb") == 0 ? file : nullptr;
#else
        return std::fopen(pathFromUtf8(path).c_str(), "rb");
#endif
    }
}
