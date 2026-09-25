#include <fstream>
#include <sstream>

#include "rasm/core/utils.h"
#include "spdlog/spdlog.h"

namespace rasm
{
    glm::mat4 getProjectionMatrix(CameraProjection projection)
    {
        if (projection.type == CameraType::PERSPECTIVE)
        {
            return glm::perspective(glm::radians(projection.perspective.fovY), projection.perspective.aspect, projection.perspective.nearZ, projection.perspective.farZ);
        }
        else
        {
            return glm::ortho(projection.orthographic.left, projection.orthographic.right, projection.orthographic.bottom, projection.orthographic.top, projection.orthographic.nearZ, projection.orthographic.farZ);
        }
    }

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