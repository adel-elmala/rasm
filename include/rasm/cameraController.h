// clang-format off
#pragma once

#include "rasm/transform.h"

#include <cstdint>

namespace rasm
{

    struct CameraProjection;

    // A wheel step scales a trackball's radius, or an orthographic view's box, by this: toward the target when the
    // wheel turns forward (a positive step), away from it when backward.
    constexpr float     CAMERA_WHEEL_ZOOM       = 0.9f;
    // The wheel never brings a trackball's radius under this, so it can zoom out again.
    constexpr float     MIN_TRACK_BALL_RADIUS   = 1e-4f;

    // One frame of input for a camera controller, from takeCameraInput (rasm/cameraInput.h) or the application.
    struct CameraInput
    {
        glm::vec2   mouseStart = glm::vec2(0.0f);  // the orbit drag (a free-fly camera's look): NDC coordinates [-1, 1]
        glm::vec2   mouseEnd   = glm::vec2(0.0f);  // NDC coordinates [-1, 1]
        float       wheelDelta = 0.0f;             // scroll wheel delta
        float       deltaTime  = 0.0f;             // the frame's step in seconds: EngineConfig::fixedDeltaTime, else the frame time

        bool        forward  = false;
        bool        backward = false;
        bool        left     = false;
        bool        right    = false;

        glm::vec2   panStart   = glm::vec2(0.0f);  // the pan drag (a free-fly camera looks with it too), NDC
        glm::vec2   panEnd     = glm::vec2(0.0f);
        glm::vec2   pointer    = glm::vec2(0.0f);  // NDC: the point an orthographic view zooms at; 0 is the center
    };

    struct CameraController
    {
        // NONE: no controller. updateCameraRig only eases, and beginFrame does not move an engine camera that has it.
        enum class Type { TRACK_BALL, FREE_FLY, NONE };

        // TURNTABLE: the drag's x turns around up, its y tilts, short of the poles, with no roll. ARCBALL: free rotation
        // from the drag's points on a virtual sphere (Holroyd's sphere and hyperbola), rolls included.
        enum class OrbitStyle : uint32_t { TURNTABLE, ARCBALL };

        // The orbit drag turns around center, a pan drag moves center across the view, and the wheel scales radius by
        // CAMERA_WHEEL_ZOOM a step, or an orthographic view's box at the pointer. orbitSensitivity 0: a plan view.
        struct TrackBall
        {
            float       radius;            // target orbit radius, wheel zoom eases toward it
            glm::dvec3  center;            // world space, in doubles
            float       orbitSensitivity; // multiplier for drag rotation: 1 turns 90 degrees per NDC unit of drag
            OrbitStyle  style;            // TURNTABLE by default
            glm::vec3   up;               // the turntable's axis (a globe's local up); zero: +Y
        };

        struct FreeFly
        {
            float yaw;
            float pitch;
            float speed;            // units per second
            float lookSensitivity;  // radians per NDC unit
            glm::vec3 up;           // the axis yaw turns around and pitch is measured from (a globe's local up); zero: +Y
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
    // One frame: the controller moves targetTransform by the input, then transform eases toward it. A trackball pans
    // as a 45 degree perspective camera would.
    void            updateCameraRig(CameraRig& rig, const CameraInput& input);
    // The same, for the camera's own projection: a trackball pans by the projection's size at center, and the wheel
    // zooms an orthographic projection's box at the pointer, at once, so the point under the pointer stays put.
    void            updateCameraRig(CameraRig& rig, CameraProjection& projection, const CameraInput& input);

}
// clang-format on
