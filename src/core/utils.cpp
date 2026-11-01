#include <fstream>
#include <sstream>

#include "core/utils.h"
#include "platform/paths.h"
#include "spdlog/spdlog.h"

namespace rasm
{
    std::string readFile(const std::filesystem::path &filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            spdlog::error("Failed to open shader file: {}", pathToUtf8(filePath));
            return "";
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
}