#include "rasm/core/camera.h"
#include "rasm/core/engine.h"

#include <cmath>

namespace rasm
{
    CameraHandle Engine::createCamera(CameraProjection projection, Transform transform)
    {
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

        registery.cameras.resize(handle.index + 1);
        registery.cameras[handle.index] = Camera{
            .projection = projection,
            .controller = CameraController{.type = CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = radius,
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
        if (!camera.isValid() || camera.index >= registery.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return;
        }

        auto &cam = registery.cameras[camera.index];
        cam.transform = transform;
        cam.targetTransform = transform;
    }

    void Engine::setCameraProjection(CameraHandle camera, CameraProjection projection)
    {
        if (!camera.isValid() || camera.index >= registery.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return;
        }

        registery.cameras[camera.index].projection = projection;
    }

    void Engine::setCameraController(CameraHandle camera, CameraController controller)
    {
        if (!camera.isValid() || camera.index >= registery.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return;
        }

        auto &cam = registery.cameras[camera.index];

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
        if (!camera.isValid() || camera.index >= registery.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return CameraController{};
        }

        return registery.cameras[camera.index].controller;
    }

    Transform Engine::getCameraTransform(CameraHandle camera)
    {
        if (!camera.isValid() || camera.index >= registery.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return Transform{};
        }

        return registery.cameras[camera.index].transform;
    }

    CameraProjection Engine::getCameraProjection(CameraHandle camera)
    {
        if (!camera.isValid() || camera.index >= registery.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return CameraProjection{};
        }

        return registery.cameras[camera.index].projection;
    }

    void Engine::updateCameraAspectRatio(float aspect)
    {
        for (auto &camera : registery.cameras)
        {
            if (camera.projection.type == CameraProjection::Type::PERSPECTIVE)
            {
                camera.projection.perspective.aspect = aspect;
            }
            else if (camera.projection.type == CameraProjection::Type::ORTHOGRAPHIC)
            {
                // calculate the orthographic aspect ratio based on the new aspect
                auto centerX = (camera.projection.orthographic.left + camera.projection.orthographic.right) * 0.5f;
                auto centerY = (camera.projection.orthographic.bottom + camera.projection.orthographic.top) * 0.5f;
                auto halfWidth = (camera.projection.orthographic.right - camera.projection.orthographic.left) * 0.5f;
                auto halfHeight = halfWidth / aspect;

                camera.projection.orthographic.left = centerX - halfWidth;
                camera.projection.orthographic.right = centerX + halfWidth;
                camera.projection.orthographic.bottom = centerY - halfHeight;
                camera.projection.orthographic.top = centerY + halfHeight;
            }
        }
    }

    glm::mat4 Engine::getProjectionMatrix(CameraHandle camera)
    {
        if (!camera.isValid() || camera.index >= registery.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return glm::mat4(1.0f);
        }

        auto &projection = registery.cameras[camera.index].projection;

        if (projection.type == CameraProjection::Type::PERSPECTIVE)
        {
            return glm::perspective(glm::radians(projection.perspective.fovY), projection.perspective.aspect, projection.perspective.nearZ, projection.perspective.farZ);
        }
        else
        {
            return glm::ortho(projection.orthographic.left, projection.orthographic.right, projection.orthographic.bottom, projection.orthographic.top, projection.orthographic.nearZ, projection.orthographic.farZ);
        }
    }

    glm::mat4 Engine::getViewMatrix(CameraHandle camera)
    {
        if (!camera.isValid() || camera.index >= registery.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return glm::mat4(1.0f);
        }

        auto &transform = registery.cameras[camera.index].transform;

        // Compute the view matrix from the camera's world transform.
        // The view matrix is the inverse of the camera's world transform.
        // world-to-view = inverse(world-to-camera) = inverse( T * R ) = inverse(R) * inverse(T)
        auto view = glm::mat4_cast(glm::conjugate(transform.rotation)) *
                    glm::translate(glm::mat4(1.0f), -transform.position);

        return view;
    }

    glm::vec3 projectOnSphere(const glm::vec2 &NDCPoint)
    {
        float x = NDCPoint.x;
        float y = NDCPoint.y;
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
        if (easingSpeed <= 0.0f || deltaTime <= 0.0f)
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

    void Engine::updateCamera(CameraHandle camera, const CameraInput &input)
    {
        if (!camera.isValid() || camera.index >= registery.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return;
        }

        auto &cam = registery.cameras[camera.index];

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
