// clang-format off
#pragma once

#include "rasm/camera.h"
#include "glm/gtc/matrix_transform.hpp"

#include <filesystem>
#include <string>

namespace rasm
{
    glm::mat4   getProjectionMatrix(CameraProjection projection);
    std::string readFile(const std::filesystem::path& path);
}
// clang-format on
