#include "rasm/core/camera.h"
#include "rasm/core/engine.h"

namespace rasm
{
    CameraHandle Engine::createCamera(CameraProjection projection, Transform transform)
    {
        auto handle = getNextCameraHandle();
        registery.cameras[handle.index] = Camera{projection, transform};
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
}
