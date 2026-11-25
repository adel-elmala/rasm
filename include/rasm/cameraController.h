// clang-format off
#pragma once

#include "rasm/transform.h"

namespace rasm
{

    // One frame of input for a camera controller, from takeCameraInput (rasm/cameraInput.h) or the application.
    struct CameraInput
    {
        glm::vec2   mouseStart = glm::vec2(0.0f);  // NDC coordinates [-1, 1]
        glm::vec2   mouseEnd   = glm::vec2(0.0f);  // NDC coordinates [-1, 1]
        float       wheelDelta = 0.0f;             // scroll wheel delta
        float       deltaTime  = 0.0f;             // the frame's step in seconds: EngineConfig::fixedDeltaTime, else the frame time

        bool        forward  = false;
        bool        backward = false;
        bool        left     = false;
        bool        right    = false;
    };

    struct CameraController
    {
        // NONE: no controller. updateCameraRig only eases, and beginFrame does not move an engine camera that has it.
        enum class Type { TRACK_BALL, FREE_FLY, NONE };

        struct TrackBall
        {
            float       radius;            // target orbit radius, wheel zoom eases toward it
            glm::dvec3  center;            // world space, in doubles
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

    // What a controller keeps for one camera: its state, the pose it wants, and the eased pose to render.
    struct CameraRig
    {
        CameraController    controller      = {.type = CameraController::Type::NONE};
        Transform           transform       = {};   // eased toward targetTransform: the pose to render
        Transform           targetTransform = {};   // the pose the controller wants
    };

    // A rig at rest at transform. A free-fly controller takes its yaw and pitch from the rotation (a roll is dropped),
    // so the view does not jump; a trackball keeps its center and radius.
    CameraRig       createCameraRig(const CameraController& controller, const Transform& transform);
    // One frame: the controller moves targetTransform by the input, then transform eases toward it.
    void            updateCameraRig(CameraRig& rig, const CameraInput& input);

}
// clang-format on
