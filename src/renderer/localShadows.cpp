#include "renderer/localShadows.h"

#include "glm/gtc/matrix_transform.hpp"

#include <algorithm>
#include <cmath>

// The shadow maps of local lights, the lights at a position: a spot light's one perspective map along its cone, and a
// point light's cube, one 90 degree view per face.

namespace rasm
{
    namespace
    {
        // The faces of a cube in Vulkan's order (+X, -X, +Y, -Y, +Z, -Z): the axis a face looks along, and the
        // directions in which its texel coordinates s and t grow (Vulkan's cube map face table). Must match
        // shaders/lib/cubeMap.slang.
        const glm::vec3 CUBE_FORWARD[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const glm::vec3 CUBE_S[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
        const glm::vec3 CUBE_T[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};

        // The near plane, as a part of the shadow's distance. Reversed-Z keeps the depth precise down to it, and a
        // caster nearer the light than that casts no shadow.
        constexpr float NEAR_PART = 0.005f;
        // A wider spot's map would spread its texels too thin; light outside the map is not shadowed.
        constexpr float MAX_SPOT_HALF_ANGLE = 1.396263402f; // 80 degrees
    }

    // The views are in world space, in doubles; each view that draws or reads the map moves them into its render space.
    glm::dmat4 getSpotShadowView(const glm::dvec3 &position, const glm::vec3 &direction)
    {
        const glm::dvec3 forward = glm::normalize(glm::dvec3(direction));
        const glm::dvec3 up = std::abs(forward.y) > 0.99 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0);
        return glm::lookAt(position, position + forward, up);
    }

    // The map's square covers the cone, which reaches outerConeAngle from the axis. Reversed-Z, like every RASM
    // projection: glm's near is the far plane.
    glm::mat4 getSpotShadowProjection(float outerConeAngle, float distance)
    {
        const float halfAngle = std::clamp(outerConeAngle, 0.001f, MAX_SPOT_HALF_ANGLE);
        return glm::perspective(2.0f * halfAngle, 1.0f, distance, distance * NEAR_PART);
    }

    // Vulkan samples a cube face's texel (s, t) at the direction forward + S * (2s - 1) + T * (2t - 1). The face's
    // camera puts s along clip x and t down the image (clip y up, as in every RASM target), so its rows are S, -T and
    // -forward. That frame is mirrored: seen through it, the world's front faces turn clockwise.
    glm::dmat4 getCubeFaceView(const glm::dvec3 &position, uint32_t face)
    {
        const glm::dvec3 rows[3] = {glm::dvec3(CUBE_S[face]), -glm::dvec3(CUBE_T[face]), -glm::dvec3(CUBE_FORWARD[face])};
        glm::dmat4 view(1.0);
        for (uint32_t row = 0; row < 3; ++row)
        {
            for (uint32_t column = 0; column < 3; ++column)
                view[column][row] = rows[row][column];
            view[3][row] = -glm::dot(rows[row], position);
        }
        return view;
    }

    glm::mat4 getCubeFaceProjection(float distance)
    {
        return glm::perspective(glm::radians(90.0f), 1.0f, distance, distance * NEAR_PART);
    }
}
