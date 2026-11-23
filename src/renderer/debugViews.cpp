#include "rasm/engine.h"
#include "renderer/gpuScene.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    static_assert(static_cast<uint32_t>(DebugView::MESHLETS) == 7, "shaders/lib/debugViews.slang numbers the debug views the same");

    // A new view starts TAA over, so its first frame does not blend with the frames of the view before.
    void Engine::setDebugView(DebugView view)
    {
        if (static_cast<uint32_t>(view) > static_cast<uint32_t>(DebugView::MESHLETS))
        {
            spdlog::error("setDebugView: {} is not a DebugView.", static_cast<uint32_t>(view));
            return;
        }
        if (view == debugView)
            return;
        debugView = view;
        for (Camera &camera : registry.cameras)
            ++camera.historyResets;
    }

    void Engine::setCullingFrozen(bool frozen)
    {
        cullingFrozen = frozen;
    }

    // What the camera's shaded views drew this frame, for the passes after them: NONE while a view drew shaded, as it
    // does until its debug pipeline exists and while its scene loads.
    DebugView Engine::getDrawnDebugView(CameraHandle camera) const
    {
        for (const SceneView &view : gpuScene->views)
        {
            if (view.graphs > 0 && !view.depthOnly && view.camera == camera && view.debugView != DebugView::NONE)
                return view.debugView;
        }
        return DebugView::NONE;
    }
}
