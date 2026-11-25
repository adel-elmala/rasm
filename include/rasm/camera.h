// clang-format off
#pragma once

#include "rasm/cameraController.h"
#include "rasm/grid.h"
#include "rasm/transform.h"

namespace rasm
{

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

    struct Camera
    {
        CameraProjection    projection;
        CameraRig           rig;                        // the controller, the rendered transform and the target it eases toward
        float               exposure        = 1.0f;
        float               bloomStrength   = 0.04f;    // the share of bloom in the tone-mapped image (addBloomPasses)
        uint32_t            historyResets   = 0;        // resetCameraHistory counts up; a TAA pass that sees it change starts over
        Grid                grid            = {};       // drawn in every shaded view of this camera (Renderer::setCameraGrid)
    };

}
// clang-format on
