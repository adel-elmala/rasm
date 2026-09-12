// Future API sketch (not implemented yet):
#include "rasm/rasm.h"

int main()
{
    rasm::EngineConfig config = {
        .appName = "example_1",
        .windowWidth = 1920,
        .windowHeight = 1080,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN};

    rasm::Engine engine(config);

    rasm::Scene scene = engine.createScene();

    rasm::Entity camera = scene.createEntity("MainCamera");
    rasm::Camera &cameraLens = camera.addComponent<rasm::Camera>(rasm::CameraType::PERSPECTIVE);
    cameraLens.setPerspective(45.0f, static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight), 0.1f, 32.0f);
    camera.addComponent<rasm::Transform>(glm::vec3(0.0f, 0.0f, -1.0f));

    // Load resources
    rasm::MeshHandle bunnyMesh = engine.loadMesh("assets/models/bunny.obj");
    rasm::TextureHandle albedoTexture = engine.loadTexture("assets/textures/test1.jpg");
    rasm::TextureHandle normalTexture = engine.loadTexture("assets/textures/test1.jpg");

    // Create a material
    rasm::Material *material = engine.createMaterial(rasm::MaterialTemplate::PBR);
    material->setTexture(rasm::PbrSlot::ALBEDO, albedoTexture);
    material->setTexture(rasm::PbrSlot::NORMAL, normalTexture);
    material->setFloat(rasm::PbrParam::ROUGHNESS, 0.5f);
    material->setFloat(rasm::PbrParam::METALLIC, 0.0f);

    // Create entities in the scene
    rasm::Entity bunny = scene.createEntity("bunny");
    bunny.addComponent<rasm::Mesh>(bunnyMesh);
    bunny.addComponent<rasm::Material>(*material);
    bunny.addComponent<rasm::Transform>(
        glm::vec3(0.0f, 0.0f, -10.0f),     // position
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
        glm::vec3(0.05f)                   // scale
    );

    rasm::RenderGraph graph = engine.createRenderGraph();

    rasm::ResourceHandle rtHandle{};
    // Standard forward rendering pass.
    graph.addPass(
        "Forward",
        [&config, &rtHandle, &engine](rasm::PassBuilder &builder)
        {
            auto rtDesc = rasm::ResourceDesc{};
            rtDesc.name = "offscreen_rt";
            rtDesc.type = rasm::ResourceType::RENDER_TARGET;
            rtDesc.renderTarget.width = config.windowWidth;
            rtDesc.renderTarget.height = config.windowHeight;
            rtDesc.renderTarget.colorFormat = engine.getRenderContext().getSwapchainImageFormat();
            rtDesc.renderTarget.depthFormat = rasm::Format::D24_UNORM_S8_UINT;

            rtHandle = builder.createResource(rtDesc);
            builder.write(rtHandle);
        },
        [&engine, &scene, &camera, &rtHandle](rasm::RenderContext &ctx)
        {
            // TODO: This is a temporary workaround to get the render target handle from the resource handle.
            rasm::RenderTargetHandle renderTargetHandle{rtHandle.index, rtHandle.generation};
            engine.render(scene, camera, renderTargetHandle);
        });

    auto overlayScene = engine.createScene();

    graph.addPass(
        "overlay",
        [&engine, &overlayScene, &rtHandle](rasm::PassBuilder &builder)
        {
            builder.read(rtHandle);
            std::vector<rasm::Vertex> vertices = {
                {.pos = glm::vec3(-1.0f, -1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(0.0f, 0.0f)},
                {.pos = glm::vec3(1.0f, -1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(1.0f, 0.0f)},
                {.pos = glm::vec3(1.0f, 1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(1.0f, 1.0f)},
                {.pos = glm::vec3(-1.0f, 1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(0.0f, 1.0f)}};

            std::vector<uint32_t> indices = {
                0, 1, 2, // first triangle
                2, 3, 0  // second triangle
            };

            auto overlayMesh = engine.createMesh(vertices, indices);

            auto overlayRect = overlayScene.createEntity("overlayRect");
            overlayRect.addComponent<rasm::Mesh>(overlayMesh);
            overlayRect.addComponent<rasm::Transform>();

            auto overlayMaterial = engine.createMaterial(rasm::MaterialTemplate::SHADER);
            overlayMaterial->setShaderName("overlayShader");
            overlayMaterial->setShaderSource(R"(
                struct VSInput
                {
                    float3 Pos;
                    float3 Normal;
                    float2 UV;
                };

                struct ShaderData
                {
                    float4x4 projection;
                    float4x4 view;
                    float4x4 model;
                };

                struct VSOutput
                {
                    float4 Pos : SV_POSITION;
                    float3 Normal : NORMAL;
                    float2 UV : TEXCOORD0;
                };

                [shader("vertex")]
                VSOutput vertMain(VSInput input, uniform ShaderData *shaderData)
                {
                    VSOutput output;
                    float4x4 modelMat = shaderData->model;
                    float4x4 viewMat = shaderData->view;
                    float4x4 projectionMat = shaderData->projection;

                    // output.Pos = mul(projectionMat, mul(viewMat, mul(modelMat, float4(input.Pos.xyz, 1.0))));
                    output.Pos = float4(input.Pos.xyz, 1.0);
                    output.Normal = input.Normal;
                    output.UV = input.UV;

                    return output;
                }

                [[vk::binding(0, 0)]]
                Sampler2D gTextures[];


                [shader("fragment")]
                float4 fragMain(VSOutput input) : SV_TARGET
                {
                    float3 color = gTextures[6].Sample(input.UV).rgb;
                    return float4(color, 1.0);
                }
            )");

            overlayRect.addComponent<rasm::Material>(*overlayMaterial);
        },
        [&engine, &overlayScene, &camera](rasm::RenderContext &ctx)
        {
            engine.render(overlayScene, camera);
        });

    graph.compile();

    while (engine.running())
    {
        graph.execute();
    }

    return 0;
}
