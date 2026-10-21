#include <fstream>
#include <sstream>

#include "rasm/core/utils.h"
#include "spdlog/spdlog.h"

namespace rasm
{
    std::string readFile(const std::string &filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            spdlog::error("Failed to open shader file: {}", filePath);
            return "";
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
}