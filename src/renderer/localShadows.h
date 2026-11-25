// clang-format off
#pragma once

#include "glm/glm.hpp"

#include <cstdint>

namespace rasm
{

    glm::dmat4                  getSpotShadowView(const glm::dvec3& position, const glm::vec3& direction);
    glm::mat4                   getSpotShadowProjection(float outerConeAngle, float distance);
    glm::dmat4                  getCubeFaceView(const glm::dvec3& position, uint32_t face);
    glm::mat4                   getCubeFaceProjection(float distance);

}
// clang-format on
