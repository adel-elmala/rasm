#include "rasm/cameraController.h"

#include "glm/gtc/constants.hpp"

#include <algorithm>
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

        // The frame whose +Y is the free-fly controller's up: the shortest turn from +Y to it. A zero up, or +Y, is no
        // frame at all, so a controller without an up turns exactly as it always did.
        bool getUpFrame(const glm::vec3 &up, glm::quat &frame)
        {
            const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
            if (glm::dot(up, up) < 1e-12f || up == worldUp)
                return false;
            const glm::vec3 axis = glm::normalize(up);
            const float cosine = glm::dot(worldUp, axis);
            if (cosine < -0.999999f)
                frame = glm::angleAxis(glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
            else
                frame = glm::normalize(glm::quat(1.0f + cosine, glm::cross(worldUp, axis)));
            return true;
        }

        void updateFreeFly(CameraRig &rig, const CameraInput &input)
        {
            auto &freeFly = rig.controller.freeFly;
            auto &transform = rig.targetTransform;

            // Look: yaw around the controller's up, pitch around the camera's local right axis.
            // Rebuilding the rotation from yaw/pitch keeps the horizon level.
            const float deltaX = input.mouseEnd.x - input.mouseStart.x;
            const float deltaY = input.mouseEnd.y - input.mouseStart.y;

            freeFly.yaw -= deltaX * freeFly.lookSensitivity;
            freeFly.pitch += deltaY * freeFly.lookSensitivity;

            const float maxPitch = glm::radians(89.0f);
            freeFly.pitch = glm::clamp(freeFly.pitch, -maxPitch, maxPitch);

            transform.rotation = glm::normalize(glm::angleAxis(freeFly.yaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
                                                glm::angleAxis(freeFly.pitch, glm::vec3(1.0f, 0.0f, 0.0f)));
            glm::quat frame;
            if (getUpFrame(freeFly.up, frame))
                transform.rotation = glm::normalize(frame * transform.rotation);

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
                transform.position += glm::dvec3(glm::normalize(move) * freeFly.speed * input.deltaTime);
            }
        }

        void updateTrackBall(CameraRig &rig, const CameraInput &input)
        {
            auto &trackBall = rig.controller.trackBall;
            auto &transform = rig.targetTransform;

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

            angle *= trackBall.orbitSensitivity;

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
                auto radius = trackBall.radius - input.wheelDelta;
                trackBall.radius = glm::max(radius, 0.0f);
            }

            transform.position = trackBall.center + glm::dvec3(trackBall.radius * (transform.rotation * glm::vec3(0.0f, 0.0f, 1.0f)));
        }

        void ease(CameraRig &rig, float deltaTime)
        {
            Transform &current = rig.transform;
            const Transform &target = rig.targetTransform;
            const float easingSpeed = rig.controller.easingSpeed;
            // Easing between (nearly) equal values still rounds by an amount that depends on deltaTime, so a
            // camera at rest would move by float noise every frame. Close enough, it snaps to the target (B24).
            const glm::quat remaining = glm::conjugate(current.rotation) * target.rotation;
            // A trackball's positions are its center plus a float offset of its radius, which rounds by about radius x 1e-7,
            // so its tolerance grows with the radius. The position itself is a double, so the tolerance does not grow with
            // the distance from the origin, past the double's own rounding.
            const double reach = rig.controller.type == CameraController::Type::TRACK_BALL ? std::max(1.0, static_cast<double>(rig.controller.trackBall.radius)) : 1.0;
            const double tolerance = 1e-6 * reach + 4.0 * std::numeric_limits<double>::epsilon() * glm::length(target.position);
            const bool settled = glm::length(target.position - current.position) <= tolerance &&
                                 glm::length(glm::vec3(remaining.x, remaining.y, remaining.z)) <= 1e-6f &&
                                 glm::length(target.scale - current.scale) <= 1e-6f;
            if (easingSpeed <= 0.0f || deltaTime <= 0.0f || settled)
            {
                current = target;
                return;
            }

            // Exponential smoothing is frame-rate independent.
            const float t = 1.0f - std::exp(-easingSpeed * deltaTime);
            current.rotation = glm::normalize(glm::slerp(current.rotation, target.rotation, t));
            current.scale = glm::mix(current.scale, target.scale, t);

            if (rig.controller.type == CameraController::Type::TRACK_BALL)
            {
                // Keep the camera on the orbit sphere while its rotation eases.
                // Linearly interpolating the position would cut across the arc and
                // make the camera drift closer to the orbit center.
                const auto &trackBall = rig.controller.trackBall;
                const auto currentRadius = static_cast<float>(glm::length(current.position - trackBall.center));
                const float radius = glm::mix(currentRadius, trackBall.radius, t);
                current.position = trackBall.center + glm::dvec3(radius * (current.rotation * glm::vec3(0.0f, 0.0f, 1.0f)));
            }
            else
            {
                current.position = glm::mix(current.position, target.position, static_cast<double>(t));
            }
        }
    }

    CameraRig createCameraRig(const CameraController &controller, const Transform &transform)
    {
        CameraRig rig = {.controller = controller, .transform = transform, .targetTransform = transform};
        if (controller.type == CameraController::Type::FREE_FLY)
        {
            // Seed the free-fly angles from the current orientation so switching
            // controllers does not snap the view. Roll is intentionally discarded.
            glm::vec3 forward = transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
            glm::quat frame;
            if (getUpFrame(controller.freeFly.up, frame))
                forward = glm::conjugate(frame) * forward;
            rig.controller.freeFly.pitch = std::asin(glm::clamp(forward.y, -1.0f, 1.0f));
            rig.controller.freeFly.yaw = std::atan2(-forward.x, -forward.z);
        }
        return rig;
    }

    void updateCameraRig(CameraRig &rig, const CameraInput &input)
    {
        switch (rig.controller.type)
        {
        case CameraController::Type::FREE_FLY:
            updateFreeFly(rig, input);
            break;
        case CameraController::Type::TRACK_BALL:
            updateTrackBall(rig, input);
            break;
        case CameraController::Type::NONE:
            break;
        }
        ease(rig, input.deltaTime);
    }
}
