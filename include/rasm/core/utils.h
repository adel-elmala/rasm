#pragma once

#include "rasm/core/camera.h"
#include "glm/gtc/matrix_transform.hpp"


namespace rasm
{
    glm::mat4 getProjectionMatrix(CameraProjection projection);
    std::string readFile(const std::string& path);
}