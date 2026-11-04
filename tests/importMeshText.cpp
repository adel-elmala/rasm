#include "importMeshText.h"

#include "platform/paths.h"

#include <filesystem>
#include <fstream>
#include <random>

// Writes `contents` to a temp file and imports it. The random name keeps test processes that run at once apart.
rasm::GLTFRaw importMeshText(const std::string &contents, const std::string &extension)
{
    const std::string name = "rasm_tests_" + std::to_string(std::random_device{}()) + extension;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    {
        std::ofstream file(path);
        file << contents;
    }

    rasm::GLTFRaw raw;
    try
    {
        raw = rasm::importMeshFile(rasm::pathToUtf8(path));
    }
    catch (...)
    {
        std::filesystem::remove(path);
        throw;
    }
    std::filesystem::remove(path);
    return raw;
}
