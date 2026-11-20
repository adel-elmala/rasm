#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    // The pass reads the camera's exposure when it runs, so changing it needs no new graph. Its pipeline is made when
    // the pass first runs, and a shader error retries every frame, so a shader fixed on disk is picked up while the
    // application runs. Until then the pass draws nothing and the frame shows the clear color.
    bool Engine::addToneMapPass(RenderGraph &graph, const RenderTargetHandle &source, CameraHandle camera)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("addToneMapPass needs a valid camera handle.");
            return false;
        }
        if (!source.isValid() || ctx.getRenderTarget(source).colorCount == 0)
        {
            spdlog::error("addToneMapPass needs a render target with a color attachment.");
            return false;
        }

        graph.addPass(
            "Tone Map",
            [source](PassBuilder &builder)
            {
                builder.read(source);
                builder.writeSwapchain();
            },
            [this, source, camera](RenderContext &)
            {
                if (!toneMapPipeline.isValid())
                {
                    toneMapPipeline = createPipeline("toneMap", readFile(pathFromUtf8(config.shaderDir) / "passes/toneMap.slang"));
                    if (!toneMapPipeline.isValid())
                    {
                        spdlog::error("Failed to create the tone-map pipeline.");
                        return;
                    }
                }
                const auto commandBuffer = frameResources[frameCount].commandBuffer;
                const gfx::ToneMapPushConstants pushConstants{.source = getBindlessTextureIndex(getRenderTargetTexture(source)),
                                                              .exposure = registry.cameras[camera.index].exposure};
                ctx.bindPipeline(commandBuffer, toneMapPipeline);
                ctx.bindDescriptorSet(commandBuffer, toneMapPipeline, bindlessDescriptorSet, 0);
                ctx.pushConstants(commandBuffer, toneMapPipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
                ctx.draw(commandBuffer, 3);
            });
        return true;
    }
}
