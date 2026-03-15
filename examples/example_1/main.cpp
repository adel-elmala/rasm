// Future API sketch (not implemented yet):
#include "rasm/core/rasm.h"
#include "rasm/core/engine.h"
#include "rasm/core/renderGraph.h"
#include "rasm/core/types.h"

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
        .preferredBackend = rasm::Backend::Vulkan};

    rasm::Engine engine(config);
    rasm::Scene scene = engine.createScene();
    rasm::Entity camera = scene.createEntity("MainCamera");

    // rasm::MaterialHandle bloomMaterial = engine.createMaterialFromGLSL(
    //     "shaders/fullscreen.vert",
    //     "shaders/bloom.frag"
    // );

    rasm::RenderGraph graph = engine.createRenderGraph();

    // Standard forward rendering pass.
    graph.addPass("Forward", [](rasm::PassBuilder &builder)
                  {
                      // In a real implementation, this would set up render targets, bind the scene, etc.
                  },
                  [](rasm::RenderContext &ctx)
                  {
                      // This is where the actual draw calls for the forward pass would go.
                  });

    auto bloomPass = CustomBloomPass(rasm::Material{} /*bloomMaterial*/, CustomBloomPass::Settings{1.0f, 0.5f});

    // Graph owns pass lifetime; no raw new/delete.
    graph.addPass(
        "Bloom",
        [&bloomPass](rasm::PassBuilder &builder)
        {
            // In a real implementation, this would set up render targets, bind the scene, etc.
            bloomPass.setup(builder);
        },
        [&bloomPass](rasm::RenderContext &ctx)
        {
            // This is where the actual draw calls for the forward pass would go.
            bloomPass.execute(ctx);
        });

    graph.compile();

    while (engine.running())
    {
        engine.beginFrame();
        graph.execute();
        engine.endFrame();
    }

    return 0;
}
