#include "rasm/core/camera.h"

namespace rasm
{
    Camera::Camera(CameraType type) : type(type) {}
    Camera::~Camera() {}

    void Camera::setPerspective(float fovY, float aspect, float nearZ, float farZ) {
        (void)fovY;
        (void)aspect;
        (void)nearZ;
        (void)farZ;
    }
    void Camera::setOrthographic(float left, float right, float bottom, float top, float nearZ, float farZ) {
        (void)left;
        (void)right;
        (void)bottom;
        (void)top;
        (void)nearZ;
        (void)farZ;
    }
}
