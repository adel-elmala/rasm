#include "rasm/renderer.h"
#include "renderer/gpuScene.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    static_assert(static_cast<uint32_t>(DebugView::MESHLETS) == 7, "shaders/lib/debugViews.slang numbers the debug views the same");

    // A new view starts TAA over, so its first frame does not blend with the frames of the view before.
    void Renderer::setDebugView(DebugView view)
    {
        if (static_cast<uint32_t>(view) > static_cast<uint32_t>(DebugView::MESHLETS))
        {
            spdlog::error("setDebugView: {} is not a DebugView.", static_cast<uint32_t>(view));
            return;
        }
        if (view == debugView)
            return;
        debugView = view;
        // Its shaders go to the shader workers now, for every kind of scene target warmed so far (warmScenePipelines'
        // bits: 1 and 2 without a motion target, 4 and 8 with one).
        if (view != DebugView::NONE && (warmedSceneVariants & 3u) != 0)
            warmDebugPipelines(false);
        if (view != DebugView::NONE && (warmedSceneVariants & 12u) != 0)
            warmDebugPipelines(true);
        for (Camera &camera : registry.cameras)
            ++camera.historyResets;
    }

    DebugView Renderer::getDebugView() const
    {
        return debugView;
    }

    void Renderer::setCullingFrozen(bool frozen)
    {
        cullingFrozen = frozen;
    }

    // What the camera's shaded views drew this frame, for the passes after them: NONE while a view drew shaded, as it
    // does until its debug pipeline exists and while its scene loads.
    DebugView Renderer::getDrawnDebugView(CameraHandle camera) const
    {
        for (const SceneView &view : gpuScene->views)
        {
            if (view.graphs > 0 && !view.depthOnly && view.camera == camera && view.debugView != DebugView::NONE)
                return view.debugView;
        }
        return DebugView::NONE;
    }
}
