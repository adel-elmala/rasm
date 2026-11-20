// clang-format off
#pragma once

#include "glm/glm.hpp"

#include <cstdint>

namespace rasm
{

    struct ShadowCascade
    {
        glm::mat4               view;
        glm::mat4               projection;
    };

    void                        getShadowSplits(float nearZ, float distance, float splitLambda, uint32_t count, float* splits);
    ShadowCascade               fitShadowCascade(const glm::mat4& cameraProjection, const glm::mat4& cameraWorld, const glm::vec3& lightDirection, float nearDepth, float farDepth, uint32_t mapSize, float casterDistance);

}
// clang-format on
