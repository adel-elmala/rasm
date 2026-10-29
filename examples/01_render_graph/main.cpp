#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include <string>

namespace
{
    // Parameters of every bloom pass, read by the shaders through the push-constant pointer.
    // Must match PostParams in POST_COMMON byte for byte (scalar layout).
    struct PostParams
    {
        glm::vec2 texelStep;      // blur: UV offset between taps; bright pass: source texel size
        float     bloomThreshold; // bright pass: luminance where bloom starts
        float     bloomIntensity; // composite
        uint32_t  source;         // bindless index of the pass input
        uint32_t  bloom;          // composite: bindless index of the blurred bloom
    };
    static_assert(sizeof(PostParams) == 24, "PostParams must match the Slang struct");

    // Each pass writes its PostParams to its own slot of the frame's shader data buffer.
    constexpr size_t PARAMS_SLOT_SIZE = 256;

    // Shared by all bloom pipelines: the parameter struct, a fullscreen triangle and helpers.
    const std::string POST_COMMON = R"(
        struct PostParams
        {
            float2 texelStep;
            float  bloomThreshold;
            float  bloomIntensity;
            uint   source;
            uint   bloom;
        };

        struct VSOutput
        {
            float4 position : SV_POSITION;
            float2 uv       : TEXCOORD0;
        };

        // One triangle that covers the screen, generated from the vertex index: no vertex buffer.
        [shader("vertex")]
        VSOutput vertMain(uint vertexID : SV_VertexID)
        {
            VSOutput output;
            output.uv       = float2(float((vertexID << 1) & 2), float(vertexID & 2));
            output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
            return output;
        }

        [[vk::binding(0, 0)]]
        Sampler2D gTextures[];

        float3 sampleColor(uint texture, float2 uv)
        {
            return gTextures[texture].Sample(uv).rgb;
        }

        // The forward pass applies Reinhard tone mapping (pbr.slang). Inverting it recovers the scene
        // radiance, so bloom is thresholded and added in HDR before tone mapping again.
        float3 inverseReinhard(float3 color)
        {
            color = min(color, float3(0.999));
            return color / (1.0 - color);
        }

        float3 reinhard(float3 color)
        {
            return color / (color + 1.0);
        }
    )";

    // Downsamples the scene to half resolution and keeps only what is brighter than the threshold.
    const std::string BLOOM_BRIGHT_SHADER = POST_COMMON + R"(
        [shader("fragment")]
        float4 fragMain(VSOutput input, uniform PostParams *params) : SV_TARGET
        {
            // Four bilinear taps cover the 4x4 source texels around this half-resolution pixel. Weighting each
            // by 1 / (1 + luminance) (Karis average) keeps single very bright texels from flickering.
            float3 color = float3(0.0);
            float weightSum = 0.0;
            for (int i = 0; i < 4; ++i)
            {
                float2 offset = float2((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0);
                float3 tap = inverseReinhard(sampleColor(params->source, input.uv + params->texelStep * offset));
                float weight = 1.0 / (1.0 + dot(tap, float3(0.2126, 0.7152, 0.0722)));
                color += tap * weight;
                weightSum += weight;
            }
            color /= weightSum;

            // Soft threshold: a quadratic knee instead of a hard cut-off.
            float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
            float knee = params->bloomThreshold * 0.5;
            float soft = clamp(luminance - params->bloomThreshold + knee, 0.0, 2.0 * knee);
            soft = soft * soft / (4.0 * knee + 1e-4);
            float contribution = max(soft, luminance - params->bloomThreshold) / max(luminance, 1e-4);

            return float4(color * contribution, 1.0);
        }
    )";

    // One axis of a separable Gaussian blur; texelStep is one texel along the axis. Taps further apart than a
    // texel leave a grid pattern, and a kernel cut off before ~3 sigma gives square glows.
    const std::string BLUR_SHADER = POST_COMMON + R"(
        static const int   RADIUS = 15;
        static const float SIGMA  = 5.0;

        [shader("fragment")]
        float4 fragMain(VSOutput input, uniform PostParams *params) : SV_TARGET
        {
            float3 color = float3(0.0);
            float weightSum = 0.0;
            for (int i = -RADIUS; i <= RADIUS; ++i)
            {
                float weight = exp(-float(i * i) / (2.0 * SIGMA * SIGMA));
                color += weight * sampleColor(params->source, input.uv + float(i) * params->texelStep);
                weightSum += weight;
            }
            return float4(color / weightSum, 1.0);
        }
    )";

    // Adds the blurred bloom to the scene in HDR and tone-maps the result.
    const std::string COMPOSITE_SHADER = POST_COMMON + R"(
        [shader("fragment")]
        float4 fragMain(VSOutput input, uniform PostParams *params) : SV_TARGET
        {
            float3 color = inverseReinhard(sampleColor(params->source, input.uv));
            color += sampleColor(params->bloom, input.uv) * params->bloomIntensity;
            return float4(reinhard(color), 1.0);
        }
    )";

    rasm::ResourceDesc renderTargetDesc(const std::string &name, glm::uvec2 size, rasm::Format colorFormat)
    {
        auto desc = rasm::ResourceDesc{};
        desc.name = name;
        desc.type = rasm::ResourceType::RENDER_TARGET;
        desc.renderTarget.width = size.x;
        desc.renderTarget.height = size.y;
        desc.renderTarget.colorFormat = colorFormat;
        desc.renderTarget.depthFormat = rasm::Format::D32_SFLOAT_S8_UINT;
        return desc;
    }

    uint32_t colorIndex(rasm::Engine &engine, rasm::RenderTargetHandle renderTarget)
    {
        return engine.getBindlessTextureIndex(engine.getRenderTargetAttachments(renderTarget).color);
    }

    // Draws a fullscreen triangle into the current pass. The pipeline reads `params` through the push constant.
    void drawFullscreen(rasm::Engine &engine, rasm::RenderContext &ctx, rasm::PipelineHandle pipeline, const PostParams &params, uint32_t paramsSlot)
    {
        auto &currentFrame = engine.getCurrentFrameResources();
        auto offset = paramsSlot * PARAMS_SLOT_SIZE;
        ctx.fillBuffer(currentFrame.shaderDataBuffer, &params, sizeof(params), offset);
        uint64_t paramsAddress = ctx.getBufferDeviceAddress(currentFrame.shaderDataBuffer) + offset;

        ctx.bindPipeline(currentFrame.commandBuffer, pipeline);
        ctx.bindDescriptorSet(currentFrame.commandBuffer, pipeline, engine.getBindlessDescriptorSet(), 0);
        ctx.pushConstants(currentFrame.commandBuffer, pipeline, rasm::ShaderType::VERTEX_FRAGMENT, &paramsAddress, sizeof(paramsAddress), 0);
        ctx.draw(currentFrame.commandBuffer, 3);
    }
}

int main()
{
    rasm::EngineConfig config = {
        .appName = "example_1",
        .windowWidth = 1280,
        .windowHeight = 720,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN};

    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    // Bloom settings. The glow size is set by SIGMA in BLUR_SHADER.
    constexpr float BLOOM_THRESHOLD = 0.8f;
    constexpr float BLOOM_INTENSITY = 1.0f;

    // set up the scene
    rasm::SceneHandle scene = engine.createScene();

    rasm::MeshHandle helmetGLTF = engine.loadGLTF("assets/models/gltf/DamagedHelmet/DamagedHelmet.gltf");
    if (!helmetGLTF.isValid())
        return 1;

    rasm::EntityHandle helmet = engine.createEntity("helmet", helmetGLTF, {}, rasm::Transform{
                                                                                   glm::vec3(0.0f),                   // position
                                                                                   glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                                   glm::vec3(1.0f)                    // scale
                                                                               });

    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f,
                                                                                        .farZ = 32.0f}},
                                                    rasm::Transform{
                                                        glm::vec3(0.0f, 0.0f, 4.0f),       // position
                                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                        glm::vec3(1.0f)                    // scale
                                                    });

    // Orbit the trackball around the helmet.
    const glm::vec3 orbitCenter(0.0f);
    engine.setCameraController(camera, rasm::CameraController{
                                           .type = rasm::CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = glm::length(orbitCenter - engine.getCameraTransform(camera).position),
                                                         .center = orbitCenter,
                                                         .orbitSensitivity = 1.0f},
                                           .easingSpeed = 6.0f});

    // Warm key and cool fill, as in 02_gltf.
    rasm::LightHandle light = engine.createLight(rasm::LightType::DIRECTIONAL,
                                                 glm::vec3(1.0f, 0.92f, 0.82f),
                                                 glm::vec3(0.0f),
                                                 glm::normalize(glm::vec3(-0.5f, -0.8f, -1.0f)),
                                                 3.0f, 0.0f);
    rasm::LightHandle fillLight = engine.createLight(rasm::LightType::DIRECTIONAL,
                                                     glm::vec3(0.65f, 0.8f, 1.0f),
                                                     glm::vec3(0.0f),
                                                     glm::normalize(glm::vec3(0.8f, -0.2f, -0.4f)),
                                                     1.0f, 0.0f);

    engine.addEntityToScene(scene, helmet);
    engine.addCameraToScene(scene, camera);
    engine.addLightToScene(scene, light);
    engine.addLightToScene(scene, fillLight);

    // Bloom pipelines. Intermediate targets are HDR; the composite writes to the swapchain.
    auto &renderContext = engine.getRenderContext();
    const auto swapchainFormat = renderContext.getSwapchainImageFormat();
    const auto hdrFormat = rasm::Format::R16G16B16A16_SFLOAT;

    auto bloomBrightPipeline = engine.createPipeline("bloom_bright", BLOOM_BRIGHT_SHADER, hdrFormat);
    auto blurPipeline = engine.createPipeline("blur", BLUR_SHADER, hdrFormat);
    auto compositePipeline = engine.createPipeline("composite", COMPOSITE_SHADER, swapchainFormat);
    if (!bloomBrightPipeline.isValid() || !blurPipeline.isValid() || !compositePipeline.isValid())
        return 1;

    // Render targets keep the size the swapchain had at startup; bloom runs at half resolution.
    const glm::uvec2 fullSize = renderContext.getSwapchainExtent();
    const glm::uvec2 halfSize = glm::max(fullSize / 2u, glm::uvec2(1u));
    const glm::vec2 fullTexel = 1.0f / glm::vec2(fullSize);
    const glm::vec2 halfTexel = 1.0f / glm::vec2(halfSize);

    // Passes only declare what they read and write; the graph orders them:
    //   forward -> bloom bright -> bloom blur H -> bloom blur V -> composite -> swapchain
    //   forward ------------------------------------------------> composite
    rasm::RenderGraph graph = engine.createRenderGraph();

    rasm::RenderTargetHandle sceneRt{};
    graph.addPass(
        "Forward",
        [&](rasm::PassBuilder &builder)
        {
            // The scene pipeline targets the swapchain format (see Engine::drawScene).
            sceneRt = builder.createRenderTarget(renderTargetDesc("scene_rt", fullSize, swapchainFormat));
            builder.write(sceneRt);
        },
        [&](rasm::RenderContext &)
        {
            engine.drawScene(scene, camera);
        });

    rasm::RenderTargetHandle bloomBrightRt{};
    graph.addPass(
        "Bloom bright",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(sceneRt);
            bloomBrightRt = builder.createRenderTarget(renderTargetDesc("bloom_bright_rt", halfSize, hdrFormat));
            builder.write(bloomBrightRt);
        },
        [&](rasm::RenderContext &ctx)
        {
            PostParams params{};
            params.source = colorIndex(engine, sceneRt);
            params.texelStep = fullTexel;
            params.bloomThreshold = BLOOM_THRESHOLD;
            drawFullscreen(engine, ctx, bloomBrightPipeline, params, 0);
        });

    rasm::RenderTargetHandle bloomBlurHRt{};
    graph.addPass(
        "Bloom blur H",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(bloomBrightRt);
            bloomBlurHRt = builder.createRenderTarget(renderTargetDesc("bloom_blur_h_rt", halfSize, hdrFormat));
            builder.write(bloomBlurHRt);
        },
        [&](rasm::RenderContext &ctx)
        {
            PostParams params{};
            params.source = colorIndex(engine, bloomBrightRt);
            params.texelStep = glm::vec2(halfTexel.x, 0.0f);
            drawFullscreen(engine, ctx, blurPipeline, params, 1);
        });

    rasm::RenderTargetHandle bloomBlurVRt{};
    graph.addPass(
        "Bloom blur V",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(bloomBlurHRt);
            bloomBlurVRt = builder.createRenderTarget(renderTargetDesc("bloom_blur_v_rt", halfSize, hdrFormat));
            builder.write(bloomBlurVRt);
        },
        [&](rasm::RenderContext &ctx)
        {
            PostParams params{};
            params.source = colorIndex(engine, bloomBlurHRt);
            params.texelStep = glm::vec2(0.0f, halfTexel.y);
            drawFullscreen(engine, ctx, blurPipeline, params, 2);
        });

    // No write: a pass without an output render target draws to the swapchain.
    graph.addPass(
        "Composite",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(sceneRt);
            builder.read(bloomBlurVRt);
        },
        [&](rasm::RenderContext &ctx)
        {
            PostParams params{};
            params.source = colorIndex(engine, sceneRt);
            params.bloom = colorIndex(engine, bloomBlurVRt);
            params.bloomIntensity = BLOOM_INTENSITY;
            drawFullscreen(engine, ctx, compositePipeline, params, 3);
        });

    graph.compile();

    while (engine.running())
    {
        graph.execute();
    }

    return 0;
}
