// clang-format off
#pragma once

#include "glm/glm.hpp"

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

    class Engine;

    class Camera
    {
    friend class Engine;
    public:
                    Camera(CameraType type = CameraType::PERSPECTIVE);
                    ~Camera();
        void        setPerspective(float fovY, float aspect, float nearZ, float farZ);
        void        setOrthographic(float left, float right, float bottom, float top, float nearZ, float farZ);
        glm::mat4   getProjectionMatrix() const;

    protected:
        CameraProjection projection;
    };

}
// clang-format on
