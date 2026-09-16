#include "rasm/core/camera.h"
#include "rasm/core/engine.h"

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

        registery.cameras.resize(handle.index + 1);
        registery.cameras[handle.index] = Camera{ projection, transform };

        return handle;
    }

    void Engine::setCameraTransform(CameraHandle camera, Transform transform)
    {
        if (!camera.isValid() || camera.index >= registery.cameras.size())
        {
            spdlog::error("Invalid camera handle.");
            return;
        }

        registery.cameras[camera.index].transform = transform;
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
}
