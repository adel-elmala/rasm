#include "renderer/taa.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/gpuScene.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"

#include <cmath>
#include <memory>
#include <string>

namespace rasm
{
    namespace
    {
        // What one TAA pass remembers between frames, to know when its history no longer fits this frame.
        struct TaaState
        {
            glm::uvec2 size = glm::uvec2(0);
            TextureHandle texture = {}; // the history's first copy: a resize or a new swapchain makes new, unwritten ones
            uint32_t historyResets = 0;
            glm::vec3 position = glm::vec3(0.0f);
            glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            bool valid = false;
        };
    }

    namespace
    {
        // numthreads of taaMain in shaders/passes/taa.slang.
        constexpr uint32_t TAA_GROUP_SIZE = 8;
        // The flags in the high half of TaaPushConstants::targetAndReset. Without a sky, nothing writes the motion of the
        // background, which holds the target's clear color, so the shader takes it as still.
        constexpr uint32_t TAA_RESET = 1;
        constexpr uint32_t TAA_NO_SKY = 2;

        float getHalton(uint32_t index, uint32_t base)
        {
            float result = 0.0f;
            float fraction = 1.0f;
            while (index > 0)
            {
                fraction /= static_cast<float>(base);
                result += fraction * static_cast<float>(index % base);
                index /= base;
            }
            return result;
        }

        // The distance a camera may move in one frame before its history is dropped: a perspective camera's scale is
        // its near plane, an orthographic camera's the width it shows.
        float getJumpDistance(const Camera &camera)
        {
            if (camera.projection.type == CameraProjection::Type::PERSPECTIVE)
                return TAA_JUMP_NEAR_PLANES * camera.projection.perspective.nearZ;
            return 0.5f * std::abs(camera.projection.orthographic.right - camera.projection.orthographic.left);
        }

        // Bindless indices and storage slots are below 2^16 (MAX_BINDLESS_TEXTURES, MAX_BINDLESS_STORAGE_IMAGES), so two
        // fit in a push-constant word.
        uint32_t packIndices(uint32_t low, uint32_t high)
        {
            return (low & 0xFFFFu) | (high << 16);
        }
    }

    // Frames 1 to TAA_JITTER_PHASES of the sequence: index 0 is (0, 0) in both bases, which would put the first
    // frame's samples on the pixels' centers twice in a cycle.
    glm::vec2 getTaaJitter(uint32_t frame)
    {
        const uint32_t index = frame % TAA_JITTER_PHASES + 1;
        return glm::vec2(getHalton(index, 2), getHalton(index, 3)) - glm::vec2(0.5f);
    }

    bool isCameraJump(const glm::vec3 &previousPosition, const glm::quat &previousRotation, const glm::vec3 &position, const glm::quat &rotation, float jumpDistance)
    {
        const float cosHalfAngle = std::min(std::abs(glm::dot(previousRotation, rotation)), 1.0f);
        const float angle = 2.0f * std::acos(cosHalfAngle);
        return angle > TAA_JUMP_ANGLE || glm::length(position - previousPosition) > jumpDistance;
    }

    // One compute pass that blends the source's color into a history target, a storage target with a copy per frame
    // slot, which it returns for the bloom and the tone map. The scene view that draws the source is jittered from
    // now on. The pass drops its history when it was just made, when its textures were made again (a resize, a new
    // swapchain), after resetCameraHistory, and when the camera jumps (isCameraJump); it reads the camera when it
    // runs, so the result depends only on the frames.
    RenderTargetHandle Engine::addTaaPass(RenderGraph &graph, const RenderTargetHandle &source, CameraHandle camera)
    {
        if (!isLive(camera))
        {
            spdlog::error("addTaaPass needs a valid camera handle.");
            return {};
        }
        if (!source.isValid())
        {
            spdlog::error("addTaaPass needs a valid render target.");
            return {};
        }
        const RenderTarget &sourceData = ctx.getRenderTarget(source);
        if (sourceData.samples > 1)
        {
            spdlog::error("addTaaPass takes a single-sample target: TAA and MSAA exclude each other.");
            return {};
        }
        if (sourceData.colorCount != 2 || sourceData.colorFormats[0] != SCENE_COLOR_FORMAT || sourceData.colorFormats[1] != MOTION_FORMAT ||
            sourceData.depthFormat != Format::D32_SFLOAT)
        {
            spdlog::error("addTaaPass needs a target with a SCENE_COLOR_FORMAT color, a MOTION_FORMAT color and D32_SFLOAT depth.");
            return {};
        }
        // The view that draws the source writes its motion; once the pass is added, it is jittered too.
        SceneView *sourceView = nullptr;
        for (uint32_t heldView : graph.sceneViews)
        {
            SceneView &view = gpuScene->views[heldView];
            if (view.renderTarget == source && !view.depthOnly)
                sourceView = &view;
        }
        if (!sourceView)
        {
            spdlog::error("addTaaPass needs the scene passes of its source in the same graph: call addScenePasses first.");
            return {};
        }

        const RenderTargetDesc sourceDesc = ctx.getRenderTargetDesc(source);
        const RenderTargetHandle history = graph.createRenderTarget({.name = sourceDesc.name + "_taa",
                                                                     .width = sourceDesc.width,
                                                                     .height = sourceDesc.height,
                                                                     .swapchainScale = sourceDesc.swapchainScale,
                                                                     .colorFormats = {SCENE_COLOR_FORMAT},
                                                                     .history = true,
                                                                     .storage = true});
        if (!history.isValid())
        {
            spdlog::error("addTaaPass could not create its history target.");
            return {};
        }

        sourceView->jitter = true;
        const auto state = std::make_shared<TaaState>();
        graph.addComputePass(
            "TAA",
            [source, history](PassBuilder &builder)
            {
                builder.read(source);
                builder.readDepth(source);
                builder.readPrevious(history);
                builder.write(history);
            },
            [this, source, history, camera, state, sourceView](RenderContext &)
            {
                if (!taaPipeline.isValid())
                {
                    taaPipeline = createComputePipeline("taa", readFile(pathFromUtf8(config.shaderDir) / "passes/taa.slang"), "taaMain");
                    if (!taaPipeline.isValid())
                    {
                        spdlog::error("Failed to create the TAA pipeline; the frame shows what its history target held.");
                        return;
                    }
                }
                const Camera &cameraData = registry.cameras[camera.index];
                const RenderTarget &target = ctx.getRenderTarget(history);
                const glm::uvec2 size(target.width, target.height);
                const TextureHandle texture = target.attachments[0].color[0];
                const bool reset = !state->valid || size != state->size || texture != state->texture || cameraData.historyResets != state->historyResets ||
                                   isCameraJump(state->position, state->rotation, cameraData.rig.transform.position, cameraData.rig.transform.rotation, getJumpDistance(cameraData));
                *state = {.size = size, .texture = texture, .historyResets = cameraData.historyResets, .position = cameraData.rig.transform.position, .rotation = cameraData.rig.transform.rotation, .valid = true};

                const gfx::TaaPushConstants pushConstants{
                    .colorAndMotion = packIndices(getBindlessTextureIndex(getRenderTargetTexture(source, 0)), getBindlessTextureIndex(getRenderTargetTexture(source, 1))),
                    .depthAndHistory = packIndices(getBindlessTextureIndex(getRenderTargetDepthTexture(source)), getBindlessTextureIndex(getPreviousRenderTargetTexture(history))),
                    .targetAndReset = packIndices(getRenderTargetStorageIndex(history), (reset ? TAA_RESET : 0u) | (sourceView->sky ? 0u : TAA_NO_SKY)),
                    .exposure = cameraData.exposure};
                const auto commandBuffer = frameResources[frameCount].commandBuffer;
                ctx.bindPipeline(commandBuffer, taaPipeline);
                ctx.bindDescriptorSet(commandBuffer, taaPipeline, bindlessDescriptorSet, 0);
                ctx.pushConstants(commandBuffer, taaPipeline, &pushConstants, sizeof(pushConstants), 0);
                ctx.dispatch(commandBuffer, (size.x + TAA_GROUP_SIZE - 1) / TAA_GROUP_SIZE, (size.y + TAA_GROUP_SIZE - 1) / TAA_GROUP_SIZE);
            });
        return history;
    }
}
