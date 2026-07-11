// Future API sketch (not implemented yet):
#include "rasm/rasm.h"

// User-defined pass with explicit typed settings and material handle ownership.
class CustomBloomPass
{
public:
    struct Settings
    {
        float threshold = 1.0f;
        float intensity = 0.5f;
    };

    CustomBloomPass(rasm::Material material, Settings settings)
        : material_(material), settings_(settings) {}

    void setup(rasm::PassBuilder &builder)
    {
        // Slot names are the graph-level contract for this pass.
        // input_ = builder.read("scene_color");

        // rasm::ResourceDesc desc = {};
        // desc.texture.width = builder.getTextureWidth(input_);
        // desc.texture.height = builder.getTextureHeight(input_);
        // desc.texture.format = rasm::Format::RGBA16F;
        // desc.usage = rasm::TextureUsage::RenderTarget | rasm::TextureUsage::Sampled;

        // output_ = builder.createTexture("bloom_color", desc);
        // builder.write(output_);
    }

    void execute(rasm::RenderContext &ctx)
    {
        // ctx.setRenderTarget(output_);
        // ctx.clear(glm::vec4(0.0f));

        // ctx.bindMaterial(material_);
        // ctx.bindTexture("u_sceneColor", input_);
        // ctx.setFloat("u_threshold", settings_.threshold);
        // ctx.setFloat("u_intensity", settings_.intensity);
        // ctx.drawFullscreenQuad();
    }

private:
    rasm::ResourceHandle input_;
    rasm::ResourceHandle output_;
    rasm::Material material_;
    Settings settings_;
};

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
    rasm::TextureHandle albedoTexture = engine.loadTexture("assets/textures/test0.jpg");
    rasm::TextureHandle normalTexture = engine.loadTexture("assets/textures/test1.jpg");

    // Create a material
    rasm::Material material = engine.createMaterial(rasm::MaterialTemplate::PBR);
    material.setTexture(rasm::PbrSlot::ALBEDO, albedoTexture);
    material.setTexture(rasm::PbrSlot::NORMAL, normalTexture);
    material.setFloat(rasm::PbrParam::ROUGHNESS, 0.5f);
    material.setFloat(rasm::PbrParam::METALLIC, 0.0f);

    // Create entities in the scene
    rasm::Entity bunny = scene.createEntity("bunny");
    bunny.addComponent<rasm::Mesh>(bunnyMesh);
    bunny.addComponent<rasm::Material>(material);
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
            rtDesc.type = rasm::ResourceDesc::Type::RENDER_TARGET;
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

    auto bloomPass = CustomBloomPass(rasm::Material{} /*bloomMaterial*/, CustomBloomPass::Settings{1.0f, 0.5f});

    graph.addPass(
        "Bloom",
        [&bloomPass](rasm::PassBuilder &builder)
        {
            bloomPass.setup(builder);
        },
        [&bloomPass](rasm::RenderContext &ctx)
        {
            bloomPass.execute(ctx);
        });

    graph.compile();

    while (engine.running())
    {
        graph.execute();
    }

    return 0;
}
