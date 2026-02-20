int main() { return 0; }

// Future API sketch (not implemented yet):
#if 0
#include "rasm/core/rasm.h"

// User-defined pass with explicit typed settings and material handle ownership.
class CustomBloomPass : public rasm::RenderPass {
public:
    struct Settings {
        float threshold = 1.0f;
        float intensity = 0.5f;
    };

    CustomBloomPass(rasm::MaterialHandle material, Settings settings)
        : material_(material), settings_(settings) {}

    void setup(rasm::PassBuilder& builder) override {
        // Slot names are the graph-level contract for this pass.
        input_ = builder.readTexture("scene_color");

        rasm::TextureDesc desc;
        desc.width = builder.getTextureWidth(input_);
        desc.height = builder.getTextureHeight(input_);
        desc.format = rasm::Format::RGBA16F;
        desc.usage = rasm::TextureUsage::RenderTarget | rasm::TextureUsage::Sampled;

        output_ = builder.createTexture("bloom_color", desc);
        builder.writeTexture(output_);
    }

    void execute(rasm::RenderContext& ctx) override {
        ctx.setRenderTarget(output_);
        ctx.clear(glm::vec4(0.0f));

        ctx.bindMaterial(material_);
        ctx.bindTexture("u_sceneColor", input_);
        ctx.setFloat("u_threshold", settings_.threshold);
        ctx.setFloat("u_intensity", settings_.intensity);
        ctx.drawFullscreenQuad();
    }

private:
    rasm::TextureHandle input_;
    rasm::TextureHandle output_;
    rasm::MaterialHandle material_;
    Settings settings_;
};

int main() {
    rasm::EngineConfig config;
    config.appName = "My Game";
    config.windowWidth = 1920;
    config.windowHeight = 1080;
    config.enableValidation = true;

    rasm::Engine engine(config);
    rasm::SceneHandle scene = engine.createScene();
    rasm::Entity camera = scene.createEntity("MainCamera");

    rasm::MaterialHandle bloomMaterial = engine.createMaterialFromGLSL(
        "shaders/fullscreen.vert",
        "shaders/bloom.frag"
    );

    rasm::RenderGraphHandle graph = engine.createRenderGraph();

    // Standard forward rendering pass.
    rasm::TextureHandle sceneColor = graph.addForwardPass(scene, camera);

    // Graph owns pass lifetime; no raw new/delete.
    rasm::PassHandle<CustomBloomPass> bloom = graph.addPass<CustomBloomPass>(
        "Bloom",
        bloomMaterial,
        CustomBloomPass::Settings{1.0f, 0.5f}
    );
    graph.connect(sceneColor, bloom.input("scene_color"));

    // Output token is symbolic and valid before compile.
    rasm::TextureHandle bloomColor = bloom.output("bloom_color");
    graph.addTonemappingPass(bloomColor);

    graph.compile();
    engine.setRenderGraph(graph);

    while (engine.running()) {
        engine.beginFrame();
        engine.render(scene, camera);
        engine.endFrame();
    }

    return 0;
}
#endif
