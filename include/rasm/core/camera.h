// clang-format off
#pragma once

#include "rasm/core/transform.h"

namespace rasm {

    struct CameraInput
    {
        glm::vec2   mouseStart = glm::vec2(0.0f);  // NDC coordinates [-1, 1]
        glm::vec2   mouseEnd   = glm::vec2(0.0f);  // NDC coordinates [-1, 1]
        float       wheelDelta = 0.0f;             // scroll wheel delta
        float       deltaTime  = 0.0f;             // frame time in seconds

        bool        forward  = false;
        bool        backward = false;
        bool        left     = false;
        bool        right    = false;
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
            float       radius;            // target orbit radius, wheel zoom eases toward it
            glm::vec3   center;
            float       orbitSensitivity; // multiplier for drag rotation
        };

        struct FreeFly
        {
            float yaw;
            float pitch;
            float speed;            // units per second
            float lookSensitivity;  // radians per NDC unit
        };

        Type type;
        union
        {
            TrackBall   trackBall;
            FreeFly     freeFly;
        };

        // Exponential smoothing speed for the rendered transform. Higher values
        // are snappier; <= 0 disables easing and snaps to the target directly.
        float easingSpeed = 12.0f;
    };

    struct Camera 
    {
        CameraProjection    projection;
        CameraController    controller;
        Transform           transform;       // current (rendered) camera transform
        Transform           targetTransform; // desired transform, eased toward each frame
    };

}
// clang-format on
