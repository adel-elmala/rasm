#include "rasm/core/camera.h"
#include "rasm/core/engine.h"

namespace rasm
{
    CameraHandle Engine::createCamera(CameraProjection projection, Transform transform)
    {
        auto handle = getNextCameraHandle();
        if (handle.index >= MAX_CAMERAS)
        {
            spdlog::error("Exceeded maximum number of cameras.");
            isRunning = false;
            return {};
        }
        registery.cameras.resize(handle.index + 1);
        registery.cameras[handle.index] = Camera{ projection, transform };
        return handle;
    }

    void Engine::setCameraTransform(CameraHandle camera, Transform transform)
    {
        registery.cameras[camera.index].transform = transform;
    }

    void Engine::setCameraProjection(CameraHandle camera, CameraProjection projection)
    {
        registery.cameras[camera.index].projection = projection;
    }

    Transform Engine::getCameraTransform(CameraHandle camera)
    {
        return registery.cameras[camera.index].transform;
    }

    CameraProjection Engine::getCameraProjection(CameraHandle camera)
    {
        return registery.cameras[camera.index].projection;
    }
}
