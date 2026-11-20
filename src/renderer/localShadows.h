// clang-format off
#pragma once

#include "glm/glm.hpp"

#include <cstdint>

namespace rasm
{

    glm::mat4                   getSpotShadowView(const glm::vec3& position, const glm::vec3& direction);
    glm::mat4                   getSpotShadowProjection(float outerConeAngle, float distance);
    glm::mat4                   getCubeFaceView(const glm::vec3& position, uint32_t face);
    glm::mat4                   getCubeFaceProjection(float distance);

}
// clang-format on
