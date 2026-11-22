#include "renderer/renderer.h"
#include "rasm/engine.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

namespace rasm
{
    namespace
    {
        // The samples per pixel of EngineConfig::antiAliasing, or the most the device has below them, with a warning.
        uint32_t getSceneSamples(RenderContext &ctx, AntiAliasing antiAliasing)
        {
            uint32_t samples = 1;
            if (antiAliasing == AntiAliasing::MSAA_2X)
                samples = 2;
            if (antiAliasing == AntiAliasing::MSAA_4X)
                samples = 4;
            if (antiAliasing == AntiAliasing::MSAA_8X)
                samples = 8;
            uint32_t supported = samples;
            while (supported > 1 && !ctx.supportsSampleCount(supported))
                supported /= 2;
            if (supported != samples)
                spdlog::warn("The device can't draw {} samples per pixel; the scene uses {}.", samples, supported);
            return supported;
        }
    }

    Renderer::Renderer(Engine &owner) : engine(owner), graph(&owner) {}

    // The frame is a graph of the view's scene passes into an HDR target (multisampled and resolved, or with motion and
    // TAA, by EngineConfig::antiAliasing), with EngineConfig::bloom the bloom passes, then the tone-map pass into the
    // swapchain and the UI pass over it.
    // It is built for one view and built again only when the view changes. Adding the passes fails on bad handles or
    // a failed allocation, and building again every frame would make a new target each time, so such a view runs the
    // passes it has, or an empty frame that presents the clear color. Shader errors retry inside the passes.
    void Renderer::render(const RenderView &view)
    {
        ZoneScoped;

        if (!built || !(view.scene == graphView.scene && view.camera == graphView.camera))
        {
            graph = engine.createRenderGraph();
            const EngineConfig config = engine.getConfig();
            // TAA needs each pixel's motion, as the target's second color.
            const bool taa = config.antiAliasing == AntiAliasing::TAA;
            const RenderTargetHandle sceneColor = graph.createRenderTarget({
                .name = "scene_hdr",
                .swapchainScale = 1.0f,
                .colorFormats = {SCENE_COLOR_FORMAT, taa ? MOTION_FORMAT : Format::UNKNOWN},
                .depthFormat = Format::D32_SFLOAT,
                .clearColor = BACKGROUND_COLOR,
                .samples = getSceneSamples(engine.getRenderContext(), config.antiAliasing),
            });
            if (sceneColor.isValid() && engine.addScenePasses(graph, view.scene, view.camera, sceneColor))
            {
                const bool multisampled = engine.getRenderContext().getRenderTarget(sceneColor).samples > 1;
                RenderTargetHandle resolved = sceneColor;
                if (multisampled)
                    resolved = engine.addMsaaResolvePass(graph, sceneColor, view.camera);
                if (taa)
                    resolved = engine.addTaaPass(graph, sceneColor, view.camera);
                const RenderTargetHandle bloom = config.bloom && resolved.isValid() ? engine.addBloomPasses(graph, resolved, view.camera) : RenderTargetHandle{};
                if (resolved.isValid() && engine.addToneMapPass(graph, resolved, view.camera, bloom))
                    engine.addUiPass(graph);
            }
            built = true;
            graphView = view;
        }
        graph.execute(view.camera);
    }

    bool Renderer::draws(SceneHandle scene, CameraHandle camera) const
    {
        return built && graphView.scene == scene && graphView.camera == camera;
    }

    void Renderer::release(SceneHandle scene, CameraHandle camera)
    {
        if (!built || !(graphView.scene == scene || graphView.camera == camera))
            return;
        graph = engine.createRenderGraph();
        built = false;
        graphView = {};
    }
}
