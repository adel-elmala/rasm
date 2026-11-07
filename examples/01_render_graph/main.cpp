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

    rasm::RenderTargetHandle rtHandle{};
    // Standard forward rendering pass.
    graph.addPass(
        "Forward",
        [&config, &rtHandle, &engine](rasm::PassBuilder &builder)
        {
            rtHandle = builder.createRenderTarget({
                .name = "offscreen_rt",
                .width = static_cast<uint32_t>(config.windowWidth),
                .height = static_cast<uint32_t>(config.windowHeight),
                .colorFormat = engine.getRenderContext().getSwapchainImageFormat(),
                .depthFormat = rasm::Format::D32_SFLOAT,
                .clearColor = glm::vec4(0.0f), // alpha 0 marks the background for the overlay
            });
            builder.write(rtHandle);
        },

        [&engine, &scene, &camera](rasm::RenderContext &)
        {
            engine.drawScene(scene, camera);
        });

    // One triangle covers the screen, so the overlay needs no vertex data.
    auto overlayPipeline = engine.createPipeline("overlay", R"(
        struct PushConstants
        {
            uint textureIndex;
        };

        [[vk::push_constant]]
        ConstantBuffer<PushConstants> pc;

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

        [shader("fragment")]
        float4 fragMain(VSOutput input) : SV_TARGET
        {
            float4 color = gTextures[pc.textureIndex].Sample(input.UV);
            // The bunny writes alpha 1 over the clear color's 0, so only the background is transparent.
            if (color.a < 0.5)
                return float4(0.0, 1.0, 0.0, 1.0);
            return float4(color.rgb, 1.0);
        }
    )");

    if (!overlayPipeline.isValid())
    {
        return 1;
    }

    graph.addPass(
        "overlay",
        [&rtHandle](rasm::PassBuilder &builder)
        {
            builder.read(rtHandle);
        },

        [&config, &engine, &overlayPipeline, &rtHandle](rasm::RenderContext &ctx)
        {
            auto commandBuffer = engine.getCurrentFrameResources().commandBuffer;

            // Each frame slot renders to its own attachment.
            auto textureIndex = engine.getBindlessTextureIndex(engine.getRenderTargetTexture(rtHandle));

            // The render target keeps its size when the window resizes, so fit it inside without changing its shape.
            const auto &window = engine.getMainWindow();
            const float windowWidth = static_cast<float>(window.pixelWidth);
            const float windowHeight = static_cast<float>(window.pixelHeight);
            const float scale = glm::min(windowWidth / config.windowWidth, windowHeight / config.windowHeight);
            const float width = config.windowWidth * scale;
            const float height = config.windowHeight * scale;

            ctx.setViewport(commandBuffer, (windowWidth - width) * 0.5f, (windowHeight - height) * 0.5f, width, height);
            ctx.bindPipeline(commandBuffer, overlayPipeline);
            ctx.bindDescriptorSet(commandBuffer, overlayPipeline, engine.getBindlessDescriptorSet(), 0);
            ctx.pushConstants(commandBuffer, overlayPipeline, rasm::ShaderType::VERTEX_FRAGMENT, &textureIndex, sizeof(textureIndex), 0);
            ctx.draw(commandBuffer, 3);
        });

    graph.compile();

    while (engine.running())
    {
        graph.execute(camera);
    }

    return 0;
}
