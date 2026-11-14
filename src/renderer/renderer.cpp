#include "renderer/renderer.h"
#include "rasm/engine.h"

#include "tracy/Tracy.hpp"

namespace rasm
{
    Renderer::Renderer(Engine &owner) : engine(owner), graph(&owner) {}

    // The frame is a graph of the view's scene passes into the swapchain. It is built for one view and built again
    // when the view changes. A view that fails to add its passes runs an empty frame, which presents the clear color,
    // and tries again next frame.
    void Renderer::render(const RenderView &view)
    {
        ZoneScoped;

        if (!built || !(view.scene == graphView.scene && view.camera == graphView.camera))
        {
            graph = engine.createRenderGraph();
            built = engine.addScenePasses(graph, view.scene, view.camera);
            graphView = view;
        }
        graph.execute(view.camera);
    }
}
