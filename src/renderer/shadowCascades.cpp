#include "renderer/shadowCascades.h"

#include "glm/gtc/matrix_transform.hpp"

#include <algorithm>
#include <cmath>

namespace rasm
{
    // Each split is where a cascade ends, as a view depth. A logarithmic split gives every cascade the same ratio of
    // far to near, so the texels on screen stay the same size; an even split gives the far cascades more of the
    // near ones' detail. splitLambda mixes the two. The last split is distance. An orthographic camera's near plane
    // can be at or behind it, where only the even split exists.
    void getShadowSplits(float nearZ, float distance, float splitLambda, uint32_t count, float *splits)
    {
        for (uint32_t i = 1; i <= count; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(count);
            const float even = nearZ + (distance - nearZ) * t;
            const float logarithmic = nearZ > 0.0f ? nearZ * std::pow(distance / nearZ, t) : even;
            splits[i - 1] = splitLambda * logarithmic + (1.0f - splitLambda) * even;
        }
        splits[count - 1] = distance;
    }

    // The cascade covers the slice of the camera's view between nearDepth and farDepth with the smallest sphere
    // centred on the view axis. The sphere depends only on the projection and the depths, so turning the camera does
    // not change its size, and the map's texels keep their size in the world. The box is moved in whole texels along
    // the light's axes, so moving the camera does not move the texel grid either: shadow edges do not crawl. Casters up
    // to casterDistance in front of the sphere, toward the light, are in the box too. The grid is the world's, in
    // doubles, so it stays put far from the origin too, whatever render space the views draw in.
    ShadowCascade fitShadowCascade(const glm::mat4 &cameraProjection, const glm::dmat4 &cameraWorld, const glm::vec3 &lightDirection, float nearDepth, float farDepth, uint32_t mapSize, float casterDistance)
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
        const glm::dvec3 center = glm::dvec3(cameraWorld * glm::dvec4(glm::dvec2(axis), -static_cast<double>(centerDepth), 1.0));

        const glm::dvec3 forward = glm::normalize(glm::dvec3(lightDirection));
        const glm::dvec3 up = std::abs(forward.y) > 0.99 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0);
        const glm::dmat4 rotation = glm::lookAt(glm::dvec3(0.0), forward, up);
        const double texel = 2.0 * static_cast<double>(radius) / static_cast<double>(mapSize);
        glm::dvec3 lightCenter = glm::dvec3(rotation * glm::dvec4(center, 1.0));
        lightCenter.x = std::floor(lightCenter.x / texel) * texel;
        lightCenter.y = std::floor(lightCenter.y / texel) * texel;
        const glm::dvec3 snapped = glm::dvec3(glm::transpose(rotation) * glm::dvec4(lightCenter, 1.0));

        const glm::dvec3 eye = snapped - forward * static_cast<double>(radius + casterDistance);
        ShadowCascade cascade;
        cascade.view = glm::lookAt(eye, eye + forward, up);
        // Reversed-Z, like every RASM projection: the near plane is depth 1.
        cascade.projection = glm::ortho(-radius, radius, -radius, radius, 2.0f * radius + casterDistance, 0.0f);
        return cascade;
    }
}
