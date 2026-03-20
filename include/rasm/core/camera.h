// clang-format off
#pragma once

namespace rasm {
    
    enum class CameraType
    {
        PERSPECTIVE,
        ORTHOGRAPHIC
    };

    class Engine;

    class Camera
    {
    friend class Engine;
    public:
             Camera(CameraType type = CameraType::PERSPECTIVE);
             ~Camera();
        void setPerspective(float fovY, float aspect, float nearZ, float farZ);
        void setOrthographic(float left, float right, float bottom, float top, float nearZ, float farZ);

    protected:
        CameraType type;
    };

}
// clang-format on
