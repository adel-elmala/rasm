#include "rasm/rasm.h"

int main()
{
    rasm::EngineConfig config = {
        .appName = "example_1",
        .windowWidth = 1920,
        .windowHeight = 1080,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR};

    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    // set up the scene
    rasm::SceneHandle scene = engine.createScene();

    // Create a camera
    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f}},
                                                    rasm::Transform{
                                                        glm::vec3(0.0f, 0.0f, -1.0f),      // position
                                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                        glm::vec3(1.0f)                    // scale
                                                    });

    // Orbit the trackball around the scene.
    const glm::vec3 orbitCenter(0.0f, 0.0f, -10.0f);
    engine.setCameraController(camera, rasm::CameraController{
                                           .type = rasm::CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = glm::length(orbitCenter - engine.getCameraTransform(camera).position),
                                                         .center = orbitCenter,
                                                         .orbitSensitivity = 1.0f},
                                           .easingSpeed = 6.0f});
    // Load resources
    rasm::MeshHandle bunnyMesh = engine.loadMesh("models/obj/bunny/bunny.obj");

    rasm::TextureHandle albedoTexture = engine.loadTexture("models/obj/bunny/bunny-atlas.jpg");

    if (!bunnyMesh.isValid() || !albedoTexture.isValid())
    {
        return 1;
    }

    // Create a material
    std::vector<rasm::TextureHandle> textures = {albedoTexture};
    rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::UNLIT, textures);

    // Create entities in the scene
    rasm::EntityHandle bunny = engine.createEntity("bunny", bunnyMesh, material, rasm::Transform{
                                                                                     glm::vec3(0.0f, 0.0f, -10.0f),     // position
                                                                                     glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                                     glm::vec3(0.005f)                  // scale
                                                                                 });

    engine.addEntityToScene(scene, bunny);
    engine.addCameraToScene(scene, camera);

    rasm::RenderGraph graph = engine.createRenderGraph();
    const rasm::Format colorFormat = engine.getRenderContext().getSwapchainImageFormat();

    // Targets made through the builder belong to the graph. Without history they are transient: each lives only from
    // the first pass that uses it to the last, so the graph lets offscreen_rt and composite_rt share memory.
    rasm::RenderTargetHandle rtHandle{};
    graph.addPass(
        "Forward",
        [&rtHandle, colorFormat](rasm::PassBuilder &builder)
        {
            rtHandle = builder.createRenderTarget({
                .name = "offscreen_rt",
                .swapchainScale = 1.0f,
                .colorFormats = {colorFormat},
                .depthFormat = rasm::Format::D32_SFLOAT,
                .clearColor = glm::vec4(0.0f), // alpha 0 marks the background
            });
            builder.write(rtHandle);
        },

        [&engine, &scene, &camera](rasm::RenderContext &)
        {
            engine.drawScene(scene, camera);
        });

    // One triangle covers the screen, so the full-screen passes need no vertex data.
    const std::string fullScreenTriangle = R"(
        struct VSOutput
        {
            float4 Pos : SV_POSITION;
            float2 UV : TEXCOORD0;
        };

        [shader("vertex")]
        VSOutput vertMain(uint vertexID : SV_VertexID)
        {
            VSOutput output;
            output.UV = float2((vertexID << 1) & 2, vertexID & 2);
            // Clip space is Y-up and UV v = 0 is the image's top row.
            output.Pos = float4(output.UV.x * 2.0 - 1.0, 1.0 - output.UV.y * 2.0, 0.0, 1.0);
            return output;
        }

        [[vk::binding(0, 0)]]
        Sampler2D gTextures[];

        struct PushConstants
        {
            uint indices[2];
        };

        [[vk::push_constant]]
        ConstantBuffer<PushConstants> pc;
    )";

    // Blends the forward image over last frame's trail, so a moving bunny leaves a fading ghost.
    auto trailPipeline = engine.createPipeline("trail", fullScreenTriangle + R"(
        [shader("fragment")]
        float4 fragMain(VSOutput input) : SV_TARGET
        {
            float4 current = gTextures[pc.indices[0]].Sample(input.UV);
            float4 previous = gTextures[pc.indices[1]].Sample(input.UV);
            return max(current, previous * 0.85);
        }
    )");

    // Paints the background green, where Background's mask is 0.
    auto presentPipeline = engine.createPipeline("present", fullScreenTriangle + R"(
        [shader("fragment")]
        float4 fragMain(VSOutput input) : SV_TARGET
        {
            float4 color = gTextures[pc.indices[0]].Sample(input.UV);
            float mask = gTextures[pc.indices[1]].Sample(input.UV).r;
            if (mask < 0.5)
                return float4(0.0, 1.0, 0.0, 1.0);
            return float4(color.rgb, 1.0);
        }
    )");

    if (!trailPipeline.isValid() || !presentPipeline.isValid())
    {
        return 1;
    }

    // Records one full-screen triangle that samples the given bindless textures.
    auto drawFullScreen = [&engine](rasm::RenderContext &ctx, rasm::PipelineHandle pipeline, uint32_t first, uint32_t second)
    {
        auto commandBuffer = engine.getCurrentFrameResources().commandBuffer;
        const uint32_t indices[2] = {first, second};
        ctx.bindPipeline(commandBuffer, pipeline);
        ctx.bindDescriptorSet(commandBuffer, pipeline, engine.getBindlessDescriptorSet(), 0);
        ctx.pushConstants(commandBuffer, pipeline, rasm::ShaderType::VERTEX_FRAGMENT, indices, sizeof(indices), 0);
        ctx.draw(commandBuffer, 3);
    };

    // A history target lives across frames with a copy per frame slot, so the pass can read the frame before while
    // it draws this one. Fetch both textures every frame: they swap.
    rasm::RenderTargetHandle trailHandle{};
    graph.addPass(
        "Trail",
        [&rtHandle, &trailHandle, colorFormat](rasm::PassBuilder &builder)
        {
            trailHandle = builder.createRenderTarget({
                .name = "trail_rt",
                .swapchainScale = 1.0f,
                .colorFormats = {colorFormat},
                .depthFormat = rasm::Format::D32_SFLOAT,
                .clearColor = glm::vec4(0.0f),
                .history = true,
            });
            builder.read(rtHandle);
            builder.readPrevious(trailHandle);
            builder.write(trailHandle);
        },

        [&engine, &drawFullScreen, &trailPipeline, &rtHandle, &trailHandle](rasm::RenderContext &ctx)
        {
            drawFullScreen(ctx, trailPipeline, engine.getBindlessTextureIndex(engine.getRenderTargetTexture(rtHandle)),
                           engine.getBindlessTextureIndex(engine.getPreviousRenderTargetTexture(trailHandle)));
        });

    // Two color attachments: the trail's color, and a mask that is 1 where the bunny writes alpha 1 over the clear
    // color's 0. The pipeline takes its formats from the target it draws into.
    rasm::RenderTargetHandle compositeHandle{};
    rasm::PipelineHandle backgroundPipeline{};
    graph.addPass(
        "Background",
        [&trailHandle, &compositeHandle, colorFormat](rasm::PassBuilder &builder)
        {
            compositeHandle = builder.createRenderTarget({
                .name = "composite_rt",
                .swapchainScale = 1.0f,
                .colorFormats = {colorFormat, rasm::Format::R8G8B8A8_UNORM},
                .depthFormat = rasm::Format::D32_SFLOAT,
            });
            builder.read(trailHandle);
            builder.write(compositeHandle);
        },

        [&engine, &drawFullScreen, &backgroundPipeline, &trailHandle](rasm::RenderContext &ctx)
        {
            drawFullScreen(ctx, backgroundPipeline, engine.getBindlessTextureIndex(engine.getRenderTargetTexture(trailHandle)), 0);
        });

    backgroundPipeline = engine.createPipeline("background", fullScreenTriangle + R"(
        struct Targets
        {
            float4 color : SV_TARGET0;
            float4 mask : SV_TARGET1;
        };

        [shader("fragment")]
        Targets fragMain(VSOutput input)
        {
            Targets targets;
            targets.color = gTextures[pc.indices[0]].Sample(input.UV);
            targets.mask = float4(targets.color.a < 0.5 ? 0.0 : 1.0);
            return targets;
        }
    )",
                                               compositeHandle);

    if (!backgroundPipeline.isValid())
    {
        return 1;
    }

    graph.addPass(
        "Present",
        [&compositeHandle](rasm::PassBuilder &builder)
        {
            builder.read(compositeHandle);
            builder.writeSwapchain();
        },

        [&engine, &drawFullScreen, &presentPipeline, &compositeHandle](rasm::RenderContext &ctx)
        {
            drawFullScreen(ctx, presentPipeline, engine.getBindlessTextureIndex(engine.getRenderTargetTexture(compositeHandle, 0)),
                           engine.getBindlessTextureIndex(engine.getRenderTargetTexture(compositeHandle, 1)));
        });

    graph.compile();

    while (engine.running())
    {
        graph.execute(camera);
    }

    return 0;
}
