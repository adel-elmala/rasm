// clang-format off
#pragma once

#include "rasm/core/transform.h"

namespace rasm {

    struct CameraInput
    {
        enum class Key { W, A, S, D, NONE };

        Key         key;
        glm::vec2   mouseStart;  // NDC coordinates [-1, 1]
        glm::vec2   mouseEnd;    // NDC coordinates [-1, 1]
        float       wheelDelta;  // scroll wheel delta
    };


    struct CameraProjection
    {
        enum class Type { PERSPECTIVE, ORTHOGRAPHIC };

        struct Perspective
        {
            float fovY;
            float aspect;
            float nearZ;
            float farZ;
        };

        struct Orthographic
        {
            float left;
            float right;
            float bottom;
            float top;
            float nearZ;
            float farZ;
        };

        Type type;
        union
        {
            Perspective     perspective;
            Orthographic    orthographic;
        };
    };

    struct CameraController
    {
        enum class Type { TRACK_BALL, FREE_FLY };

        struct TrackBall
        {
            float       radius;
            glm::vec3   center;
        };

        struct FreeFly
        {
            float yaw;
            float pitch;
            float speed;
        };

        Type type;
        union
        {
            TrackBall   trackBall;
            FreeFly     freeFly;
        };
    };

    struct Camera 
    {
        CameraProjection    projection;
        CameraController    controller;
        Transform           transform;  // camera to world transform -  (world position, rotation quaternion, scale not used)
    };

}
// clang-format on
