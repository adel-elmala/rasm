#include "rasm/camera.h"
#include "rasm/engine.h"

#include "spdlog/spdlog.h"

#include <cmath>
#include <limits>

namespace rasm
{
    namespace
    {
        glm::vec3 projectOnSphere(const glm::vec2 &ndcPoint)
        {
            float x = ndcPoint.x;
            float y = ndcPoint.y;
            float z = 0.0f;

            float lengthSquared = x * x + y * y;
            if (lengthSquared <= 1.0f)
            {
                // Point is inside the sphere
                z = std::sqrt(1.0f - lengthSquared);
            }
            else
            {
                // Point is outside the sphere, project onto the sphere
                float length = std::sqrt(lengthSquared);
                x /= length;
                y /= length;
            }
            return glm::vec3(x, y, z);
        }

        void updateFreeFlyCamera(Camera &cam, const CameraInput &input)
        {
            auto &freeFly = cam.controller.freeFly;
            auto &transform = cam.targetTransform;

            // Look: yaw around world up, pitch around the camera's local right axis.
            // Rebuilding the rotation from yaw/pitch keeps the horizon level.
            const float deltaX = input.mouseEnd.x - input.mouseStart.x;
            const float deltaY = input.mouseEnd.y - input.mouseStart.y;

            freeFly.yaw -= deltaX * freeFly.lookSensitivity;
            freeFly.pitch += deltaY * freeFly.lookSensitivity;

            const float maxPitch = glm::radians(89.0f);
            freeFly.pitch = glm::clamp(freeFly.pitch, -maxPitch, maxPitch);

            transform.rotation = glm::normalize(glm::angleAxis(freeFly.yaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
                                                glm::angleAxis(freeFly.pitch, glm::vec3(1.0f, 0.0f, 0.0f)));

            // Movement: held keys, frame-time based.
            const glm::vec3 forward = transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
            const glm::vec3 right = transform.rotation * glm::vec3(1.0f, 0.0f, 0.0f);

            glm::vec3 move(0.0f);
            if (input.forward)
            {
                move += forward;
            }
            if (input.backward)
            {
                move -= forward;
            }
            if (input.right)
            {
                move += right;
            }
            if (input.left)
            {
                move -= right;
            }

            if (glm::dot(move, move) > 1e-6f)
            {
                transform.position += glm::normalize(move) * freeFly.speed * input.deltaTime;
            }
        }

        void updateTrackballCamera(Camera &cam, const CameraInput &input)
        {
            auto &transform = cam.targetTransform;

            // No drag delta means there is nothing to orbit.
            if (input.mouseStart == input.mouseEnd && input.wheelDelta == 0.0f)
            {
                return;
            }

            auto p0 = projectOnSphere(input.mouseStart);
            auto p1 = projectOnSphere(input.mouseEnd);

            // The sphere points are in camera space, so the axis is a camera-local axis.
            auto axis = glm::cross(p0, p1);
            auto angle = std::acos(glm::clamp(glm::dot(p0, p1), -1.0f, 1.0f));

            angle *= cam.controller.trackBall.orbitSensitivity;

            if (glm::dot(axis, axis) > 1e-12f)
            {
                axis = glm::normalize(axis);

                // Compose on the right to rotate in the camera's local frame.
                // The inverse rotation (-angle) makes the scene follow the cursor, as in a grab-style trackball.
                transform.rotation = glm::normalize(transform.rotation * glm::angleAxis(-angle, axis));
            }

            // Apply zoom based on the scroll wheel delta.
            if (input.wheelDelta != 0.0f)
            {
                auto radius = cam.controller.trackBall.radius - input.wheelDelta;
                cam.controller.trackBall.radius = glm::max(radius, 0.0f);
            }

            transform.position = cam.controller.trackBall.center +
                                 cam.controller.trackBall.radius * (transform.rotation * glm::vec3(0.0f, 0.0f, 1.0f));
        }

        void easeCamera(Camera &cam, float deltaTime)
        {
            const float easingSpeed = cam.controller.easingSpeed;
            // Easing between (nearly) equal values still rounds by an amount that depends on deltaTime, so a
            // camera at rest would move by float noise every frame. Close enough, it snaps to the target (B24).
            const glm::quat remaining = glm::conjugate(cam.transform.rotation) * cam.targetTransform.rotation;
            const bool settled = glm::length(cam.targetTransform.position - cam.transform.position) <= 1e-6f * glm::max(1.0f, glm::length(cam.targetTransform.position)) &&
                                 glm::length(glm::vec3(remaining.x, remaining.y, remaining.z)) <= 1e-6f &&
                                 glm::length(cam.targetTransform.scale - cam.transform.scale) <= 1e-6f;
            if (easingSpeed <= 0.0f || deltaTime <= 0.0f || settled)
            {
                cam.transform = cam.targetTransform;
                return;
            }

            // Exponential smoothing is frame-rate independent.
            const float t = 1.0f - std::exp(-easingSpeed * deltaTime);
            cam.transform.rotation = glm::normalize(glm::slerp(cam.transform.rotation, cam.targetTransform.rotation, t));
            cam.transform.scale = glm::mix(cam.transform.scale, cam.targetTransform.scale, t);

            if (cam.controller.type == CameraController::Type::TRACK_BALL)
            {
                // Keep the camera on the orbit sphere while its rotation eases.
                // Linearly interpolating the position would cut across the arc and
                // make the camera drift closer to the orbit center.
                const auto &trackBall = cam.controller.trackBall;
                const float currentRadius = glm::length(cam.transform.position - trackBall.center);
                const float radius = glm::mix(currentRadius, trackBall.radius, t);
                cam.transform.position = trackBall.center +
                                         radius * (cam.transform.rotation * glm::vec3(0.0f, 0.0f, 1.0f));
            }
            else
            {
                cam.transform.position = glm::mix(cam.transform.position, cam.targetTransform.position, t);
            }
        }

        void setProjectionAspect(CameraProjection &projection, float aspect)
        {
            if (projection.type == CameraProjection::Type::PERSPECTIVE)
            {
                projection.perspective.aspect = aspect;
            }
            else if (projection.type == CameraProjection::Type::ORTHOGRAPHIC)
            {
                // calculate the orthographic aspect ratio based on the new aspect
                auto centerX = (projection.orthographic.left + projection.orthographic.right) * 0.5f;
                auto centerY = (projection.orthographic.bottom + projection.orthographic.top) * 0.5f;
                auto halfWidth = (projection.orthographic.right - projection.orthographic.left) * 0.5f;
                auto halfHeight = halfWidth / aspect;

                projection.orthographic.left = centerX - halfWidth;
                projection.orthographic.right = centerX + halfWidth;
                projection.orthographic.bottom = centerY - halfHeight;
                projection.orthographic.top = centerY + halfHeight;
            }
        }

        bool hasInfiniteFar(const CameraProjection::Perspective &perspective)
        {
            return perspective.farZ == 0.0f || perspective.farZ == std::numeric_limits<float>::infinity();
        }

        // The checks are written so that NaN fails them.
        bool isValidProjection(const CameraProjection &projection)
        {
            if (projection.type == CameraProjection::Type::PERSPECTIVE)
            {
                const auto &perspective = projection.perspective;
                if (!(perspective.nearZ > 0.0f) || std::isinf(perspective.nearZ))
                {
                    spdlog::error("A perspective camera needs a finite nearZ greater than 0, got {}.", perspective.nearZ);
                    return false;
                }
                if (!hasInfiniteFar(perspective) && !(perspective.farZ > perspective.nearZ))
                {
                    spdlog::error("A perspective camera needs farZ greater than nearZ, or 0 or infinity for an infinite far plane, got {}.", perspective.farZ);
                    return false;
                }
                return true;
            }

            const auto &orthographic = projection.orthographic;
            if (!(orthographic.farZ > orthographic.nearZ) || std::isinf(orthographic.nearZ) || std::isinf(orthographic.farZ))
            {
                spdlog::error("An orthographic camera needs finite nearZ and farZ with farZ greater than nearZ, got {} and {}.", orthographic.nearZ, orthographic.farZ);
                return false;
            }
            return true;
        }

        // Reversed-Z: the near plane maps to depth 1 and the far plane to 0. Passing far as glm's near and near as
        // its far gives exactly that. glm has no reversed infinite projection, so that one is written by hand.
        glm::mat4 toProjectionMatrix(const CameraProjection &projection)
        {
            if (projection.type == CameraProjection::Type::ORTHOGRAPHIC)
            {
                const auto &orthographic = projection.orthographic;
                return glm::ortho(orthographic.left, orthographic.right, orthographic.bottom, orthographic.top, orthographic.farZ, orthographic.nearZ);
            }

            const auto &perspective = projection.perspective;
            const float fovY = glm::radians(perspective.fovY);
            if (!hasInfiniteFar(perspective))
                return glm::perspective(fovY, perspective.aspect, perspective.farZ, perspective.nearZ);

            // depth = nearZ / -viewZ
            const float focalLength = 1.0f / std::tan(fovY * 0.5f);
            glm::mat4 matrix(0.0f);
            matrix[0][0] = focalLength / perspective.aspect;
            matrix[1][1] = focalLength;
            matrix[2][3] = -1.0f;
            matrix[3][2] = perspective.nearZ;
            return matrix;
        }
    }

    CameraHandle Engine::createCamera(CameraProjection projection, Transform transform)
    {
        if (!isValidProjection(projection))
            return {};

        auto handle = handleManager.getNextCameraHandle();
        if (handle.index >= MAX_CAMERAS)
        {
            spdlog::warn("Exceeded maximum number of cameras.");
            return {};
        }

        // Initialize the trackball so the orbit formula reproduces the given transform:
        // position = center + radius * (rotation * +Z). Placing the center one radius
        // ahead of the camera keeps the initial pose unchanged on the first update.
        const float radius = 1.0f;
        const glm::vec3 forward = transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);

        registry.cameras.resize(handle.index + 1);
        registry.cameras[handle.index] = Camera{
            .projection = projection,
            .controller = CameraController{.type = CameraController::Type::TRACK_BALL,
                                           .trackBall = {
                                               .radius = radius,
                                               .center = transform.position + forward * radius,
                                               .orbitSensitivity = 1.0f,
                                           }},
            .transform = transform,
            .targetTransform = transform,
        };

        return handle;
    }

    void Engine::setCameraTransform(CameraHandle camera, Transform transform)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return;
        }

        auto &cam = registry.cameras[camera.index];
        cam.transform = transform;
        cam.targetTransform = transform;
    }

    void Engine::setCameraProjection(CameraHandle camera, CameraProjection projection)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return;
        }
        if (!isValidProjection(projection))
            return;

        registry.cameras[camera.index].projection = projection;
    }

    void Engine::setCameraController(CameraHandle camera, CameraController controller)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return;
        }

        auto &cam = registry.cameras[camera.index];

        // Start the new controller from the current rendered pose.
        cam.targetTransform = cam.transform;

        if (controller.type == CameraController::Type::FREE_FLY)
        {
            // Seed the free-fly angles from the current orientation so switching
            // controllers does not snap the view. Roll is intentionally discarded.
            const glm::vec3 forward = cam.transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
            controller.freeFly.pitch = std::asin(glm::clamp(forward.y, -1.0f, 1.0f));
            controller.freeFly.yaw = std::atan2(-forward.x, -forward.z);
        }

        cam.controller = controller;
    }

    CameraController Engine::getCameraController(CameraHandle camera)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return CameraController{};
        }

        return registry.cameras[camera.index].controller;
    }

    Transform Engine::getCameraTransform(CameraHandle camera)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return Transform{};
        }

        return registry.cameras[camera.index].transform;
    }

    CameraProjection Engine::getCameraProjection(CameraHandle camera)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return CameraProjection{};
        }

        return registry.cameras[camera.index].projection;
    }

    void Engine::updateCameraAspectRatio(float aspect)
    {
        for (auto &camera : registry.cameras)
        {
            setProjectionAspect(camera.projection, aspect);
        }
    }

    glm::mat4 Engine::getProjectionMatrix(CameraHandle camera)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return glm::mat4(1.0f);
        }

        return toProjectionMatrix(registry.cameras[camera.index].projection);
    }

    glm::mat4 Engine::getProjectionMatrix(CameraHandle camera, float aspect)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return glm::mat4(1.0f);
        }

        auto projection = registry.cameras[camera.index].projection;
        setProjectionAspect(projection, aspect);
        return toProjectionMatrix(projection);
    }

    glm::mat4 Engine::getViewMatrix(CameraHandle camera)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return glm::mat4(1.0f);
        }

        auto &transform = registry.cameras[camera.index].transform;

        // Compute the view matrix from the camera's world transform.
        // The view matrix is the inverse of the camera's world transform.
        // world-to-view = inverse(world-to-camera) = inverse( T * R ) = inverse(R) * inverse(T)
        auto view = glm::mat4_cast(glm::conjugate(transform.rotation)) *
                    glm::translate(glm::mat4(1.0f), -transform.position);

        return view;
    }

    void Engine::updateCamera(CameraHandle camera, const CameraInput &input)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return;
        }

        auto &cam = registry.cameras[camera.index];

        if (cam.controller.type == CameraController::Type::FREE_FLY)
        {
            updateFreeFlyCamera(cam, input);
        }
        else
        {
            updateTrackballCamera(cam, input);
        }

        easeCamera(cam, input.deltaTime);
    }

}
