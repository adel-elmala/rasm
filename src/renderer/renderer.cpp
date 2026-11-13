#include "renderer/renderer.h"
#include "rasm/engine.h"

#include "tracy/Tracy.hpp"

namespace rasm
{
    Renderer::Renderer(Engine &owner) : engine(owner), graph(&owner) {}

    // The frame is a graph of the view's scene passes into the swapchain. It is built for one view and built again
    // when the view changes; a view that fails to add its passes runs empty frames, which present the clear color.
    void Renderer::render(const RenderView &view)
    {
        ZoneScoped;

        if (!built || !(view.scene == graphView.scene && view.camera == graphView.camera))
        {
            graph = engine.createRenderGraph();
            engine.addScenePasses(graph, view.scene, view.camera);
            graphView = view;
            built = true;
        }
        graph.execute(view.camera);
    }
}
