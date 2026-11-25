#include "rasm/camera.h"
#include "rasm/renderer.h"
#include "renderer/viewGraph.h"

#include "spdlog/spdlog.h"

#include <cmath>
#include <limits>

namespace rasm
{
    namespace
    {
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

    CameraHandle Renderer::createCamera(CameraProjection projection, Transform transform)
    {
        if (!isValidProjection(projection))
            return {};

        const HandleSlot slot = registry.cameraHandles.allocate();
        if (slot.index == 0)
        {
            spdlog::error("createCamera: {} cameras are alive, the most there can be (MAX_CAMERAS - 1); destroy one first.", registry.cameraHandles.liveCount());
            return {};
        }
        const CameraHandle handle{slot.index, slot.generation};

        // Initialize the trackball so the orbit formula reproduces the given transform:
        // position = center + radius * (rotation * +Z). Placing the center one radius
        // ahead of the camera keeps the initial pose unchanged on the first update.
        const float radius = 1.0f;
        const glm::vec3 forward = transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);

        registry.cameras.resize(registry.cameraHandles.size());
        registry.cameras[handle.index] = Camera{
            .projection = projection,
            .rig = createCameraRig(CameraController{.type = CameraController::Type::TRACK_BALL,
                                                    .trackBall = {
                                                        .radius = radius,
                                                        .center = transform.position + glm::dvec3(forward * radius),
                                                        .orbitSensitivity = 1.0f,
                                                    }},
                                   transform),
        };

        return handle;
    }

    void Renderer::destroyCamera(CameraHandle camera)
    {
        if (!isLive(camera))
        {
            spdlog::error("destroyCamera: camera handle {{{}, {}}} is invalid or was destroyed.", camera.index, camera.generation);
            return;
        }
        if (viewHolds({}, camera, "destroyCamera"))
            return;
        if (viewGraph)
            viewGraph->release({}, camera);
        for (Scene &scene : registry.scenes)
            std::erase(scene.cameras, camera);
        registry.cameras[camera.index] = {};
        registry.cameraHandles.release(camera.index, camera.generation);
    }

    void Renderer::setCameraTransform(CameraHandle camera, Transform transform)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return;
        }

        auto &rig = registry.cameras[camera.index].rig;
        rig.transform = transform;
        rig.targetTransform = transform;
    }

    void Renderer::setCameraProjection(CameraHandle camera, CameraProjection projection)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return;
        }
        if (!isValidProjection(projection))
            return;

        registry.cameras[camera.index].projection = projection;
    }

    void Renderer::setCameraController(CameraHandle camera, CameraController controller)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return;
        }

        // The new controller starts from the rendered pose.
        auto &rig = registry.cameras[camera.index].rig;
        rig = createCameraRig(controller, rig.transform);
    }

    void Renderer::setCameraExposure(CameraHandle camera, float exposure)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return;
        }
        if (!(exposure > 0.0f))
        {
            spdlog::error("A camera's exposure must be positive, got {}.", exposure);
            return;
        }

        registry.cameras[camera.index].exposure = exposure;
    }

    float Renderer::getCameraExposure(CameraHandle camera)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return 1.0f;
        }

        return registry.cameras[camera.index].exposure;
    }

    void Renderer::setCameraBloomStrength(CameraHandle camera, float strength)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return;
        }
        if (!(strength >= 0.0f && strength <= 1.0f))
        {
            spdlog::error("A camera's bloom strength must be from 0 to 1, got {}.", strength);
            return;
        }

        registry.cameras[camera.index].bloomStrength = strength;
    }

    // addTaaPass compares the count with the one it saw last frame, so the camera's next frame drops its history.
    void Renderer::resetCameraHistory(CameraHandle camera)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return;
        }
        ++registry.cameras[camera.index].historyResets;
    }

    float Renderer::getCameraBloomStrength(CameraHandle camera)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return 0.0f;
        }

        return registry.cameras[camera.index].bloomStrength;
    }

    CameraController Renderer::getCameraController(CameraHandle camera)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return CameraController{};
        }

        return registry.cameras[camera.index].rig.controller;
    }

    Transform Renderer::getCameraTransform(CameraHandle camera)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return Transform{};
        }

        return registry.cameras[camera.index].rig.transform;
    }

    CameraProjection Renderer::getCameraProjection(CameraHandle camera)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return CameraProjection{};
        }

        return registry.cameras[camera.index].projection;
    }

    void Renderer::updateCameraAspectRatio(float aspect)
    {
        for (auto &camera : registry.cameras)
        {
            setProjectionAspect(camera.projection, aspect);
        }
    }

    glm::mat4 Renderer::getProjectionMatrix(CameraHandle camera)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return glm::mat4(1.0f);
        }

        return toProjectionMatrix(registry.cameras[camera.index].projection);
    }

    glm::mat4 Renderer::getProjectionMatrix(CameraHandle camera, float aspect)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return glm::mat4(1.0f);
        }

        auto projection = registry.cameras[camera.index].projection;
        setProjectionAspect(projection, aspect);
        return toProjectionMatrix(projection);
    }

    glm::dmat4 Renderer::getViewMatrix(CameraHandle camera)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return glm::dmat4(1.0);
        }

        const auto &transform = registry.cameras[camera.index].rig.transform;

        // Compute the view matrix from the camera's world transform.
        // The view matrix is the inverse of the camera's world transform.
        // world-to-view = inverse(world-to-camera) = inverse( T * R ) = inverse(R) * inverse(T)
        // In doubles: far from the origin the translation needs them. The views draw with it moved into their render
        // space (toRenderMatrix), where it is the rotation alone.
        return glm::dmat4(glm::mat4_cast(glm::conjugate(transform.rotation))) * glm::translate(glm::dmat4(1.0), -transform.position);
    }

    void Renderer::updateCamera(CameraHandle camera, const CameraInput &input)
    {
        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle.");
            return;
        }

        updateCameraRig(registry.cameras[camera.index].rig, input);
    }

}
