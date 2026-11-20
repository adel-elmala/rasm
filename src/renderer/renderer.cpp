#include "renderer/renderer.h"
#include "rasm/engine.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

namespace rasm
{
    Renderer::Renderer(Engine &owner) : engine(owner), graph(&owner) {}

    // The frame is a graph of the view's scene passes into an HDR target, then the tone-map pass into the swapchain.
    // It is built for one view and built again only when the view changes. Adding the passes fails on bad handles or
    // a failed allocation, and building again every frame would make a new target each time, so such a view runs the
    // passes it has, or an empty frame that presents the clear color. Shader errors retry inside the passes. F3 logs
    // the graph's dump after it runs, so it shows the compiled order.
    void Renderer::render(const RenderView &view, bool dumpGraph)
    {
        ZoneScoped;

        if (!built || !(view.scene == graphView.scene && view.camera == graphView.camera))
        {
            graph = engine.createRenderGraph();
            const RenderTargetHandle sceneColor = graph.createRenderTarget({
                .name = "scene_hdr",
                .swapchainScale = 1.0f,
                .colorFormats = {SCENE_COLOR_FORMAT},
                .depthFormat = Format::D32_SFLOAT,
                .clearColor = BACKGROUND_COLOR,
            });
            if (engine.addScenePasses(graph, view.scene, view.camera, sceneColor))
                engine.addToneMapPass(graph, sceneColor, view.camera);
            built = true;
            graphView = view;
        }
        graph.execute(view.camera);
        if (dumpGraph)
            spdlog::info("{}", graph.dump());
    }
}
