// clang-format off
#pragma once

#include "rasm/core/transform.h"

namespace rasm {
    enum class CameraType
    {
        PERSPECTIVE,
        ORTHOGRAPHIC
    };

    struct CameraProjection
    {
        CameraType type;

        union
        {
            struct Perspective
            {
                float fovY;
                float aspect;
                float nearZ;
                float farZ;
            } perspective;
            
            struct Orthographic
            {
                float left;
                float right;
                float bottom;
                float top;
                float nearZ;
                float farZ;
            } orthographic;
        };
    };

    struct Camera 
    {
        CameraProjection projection;
        Transform transform;
    };

}
// clang-format on
