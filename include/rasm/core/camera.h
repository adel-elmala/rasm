#pragma once

#include <stdint.h>

namespace rasm {
    
    enum class CameraType
    {
        Perspective,
        Orthographic
    };

    class Camera
    {
    private:
        uint32_t handle;
        CameraType type;
    public:
        Camera(CameraType type = CameraType::Perspective);
        ~Camera();

        void setPerspective(float fovY, float aspect, float nearZ, float farZ);
        void setOrthographic(float left, float right, float bottom, float top, float nearZ, float farZ);
    };

}