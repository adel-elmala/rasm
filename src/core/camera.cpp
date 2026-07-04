#include "rasm/core/camera.h"
#include "glm/gtc/matrix_transform.hpp"

namespace rasm
{
    Camera::Camera(CameraType type) : projection{.type = type} {}
    Camera::~Camera() {}

    void Camera::setPerspective(float fovY, float aspect, float nearZ, float farZ)
    {
        projection.type = CameraType::PERSPECTIVE;
        projection.perspective.fovY = fovY;
        projection.perspective.aspect = aspect;
        projection.perspective.nearZ = nearZ;
        projection.perspective.farZ = farZ;
    }

    void Camera::setOrthographic(float left, float right, float bottom, float top, float nearZ, float farZ)
    {
        projection.type = CameraType::ORTHOGRAPHIC;
        projection.orthographic.left = left;
        projection.orthographic.right = right;
        projection.orthographic.bottom = bottom;
        projection.orthographic.top = top;
        projection.orthographic.nearZ = nearZ;
        projection.orthographic.farZ = farZ;
    }

    glm::mat4 Camera::getProjectionMatrix() const
    {
        if (projection.type == CameraType::PERSPECTIVE)
        {
            return glm::perspective(glm::radians(projection.perspective.fovY), projection.perspective.aspect, projection.perspective.nearZ, projection.perspective.farZ);
        }
        else
        {
            return glm::ortho(projection.orthographic.left, projection.orthographic.right, projection.orthographic.bottom, projection.orthographic.top, projection.orthographic.nearZ, projection.orthographic.farZ);
        }
    }
}
