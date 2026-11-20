#include "renderer/shadowCascades.h"

#include "glm/gtc/matrix_transform.hpp"

#include <algorithm>
#include <cmath>

namespace rasm
{
    // Each split is where a cascade ends, as a view depth. A logarithmic split gives every cascade the same ratio of
    // far to near, so the texels on screen stay the same size; an even split gives the far cascades more of the
    // near ones' detail. splitLambda mixes the two. The last split is distance.
    void getShadowSplits(float nearZ, float distance, float splitLambda, uint32_t count, float *splits)
    {
        for (uint32_t i = 1; i <= count; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(count);
            const float logarithmic = nearZ * std::pow(distance / nearZ, t);
            const float even = nearZ + (distance - nearZ) * t;
            splits[i - 1] = splitLambda * logarithmic + (1.0f - splitLambda) * even;
        }
        splits[count - 1] = distance;
    }

    // The cascade covers the slice of the camera's view between nearDepth and farDepth with the smallest sphere
    // centred on the view axis. The sphere depends only on the projection and the depths, so turning the camera does
    // not change its size, and the map's texels keep their size in the world. The box is moved in whole texels along
    // the light's axes, so moving the camera does not move the texel grid either: shadow edges do not crawl. Casters up
    // to casterDistance in front of the sphere, toward the light, are in the box too.
    ShadowCascade fitShadowCascade(const glm::mat4 &cameraProjection, const glm::mat4 &cameraWorld, const glm::vec3 &lightDirection, float nearDepth, float farDepth, uint32_t mapSize, float casterDistance)
    {
        // A perspective slice widens with depth, half its diagonal is depth * halfDiagonal; an orthographic one keeps
        // halfDiagonal.
        const bool perspective = cameraProjection[3][3] == 0.0f;
        const float halfWidth = 1.0f / cameraProjection[0][0];
        const float halfHeight = 1.0f / cameraProjection[1][1];
        const float halfDiagonal = std::sqrt(halfWidth * halfWidth + halfHeight * halfHeight);
        const glm::vec2 axis = perspective ? glm::vec2(0.0f) : glm::vec2(-cameraProjection[3][0] * halfWidth, -cameraProjection[3][1] * halfHeight);
        const float nearRadius = perspective ? nearDepth * halfDiagonal : halfDiagonal;
        const float farRadius = perspective ? farDepth * halfDiagonal : halfDiagonal;

        // The depth on the axis that is as far from the near corners as from the far ones.
        float centerDepth = (farDepth * farDepth + farRadius * farRadius - nearDepth * nearDepth - nearRadius * nearRadius) / (2.0f * (farDepth - nearDepth));
        centerDepth = std::clamp(centerDepth, nearDepth, farDepth);
        const float nearDistance = std::sqrt((centerDepth - nearDepth) * (centerDepth - nearDepth) + nearRadius * nearRadius);
        const float farDistance = std::sqrt((farDepth - centerDepth) * (farDepth - centerDepth) + farRadius * farRadius);
        const float radius = std::max(nearDistance, farDistance);
        const glm::vec3 center = glm::vec3(cameraWorld * glm::vec4(axis, -centerDepth, 1.0f));

        const glm::vec3 forward = glm::normalize(lightDirection);
        const glm::vec3 up = std::abs(forward.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::mat4 rotation = glm::lookAt(glm::vec3(0.0f), forward, up);
        const float texel = 2.0f * radius / static_cast<float>(mapSize);
        glm::vec3 lightCenter = glm::vec3(rotation * glm::vec4(center, 1.0f));
        lightCenter.x = std::floor(lightCenter.x / texel) * texel;
        lightCenter.y = std::floor(lightCenter.y / texel) * texel;
        const glm::vec3 snapped = glm::vec3(glm::transpose(rotation) * glm::vec4(lightCenter, 1.0f));

        const glm::vec3 eye = snapped - forward * (radius + casterDistance);
        ShadowCascade cascade;
        cascade.view = glm::lookAt(eye, eye + forward, up);
        // Reversed-Z, like every RASM projection: the near plane is depth 1.
        cascade.projection = glm::ortho(-radius, radius, -radius, radius, 2.0f * radius + casterDistance, 0.0f);
        return cascade;
    }
}
