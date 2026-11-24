#include "doctest/doctest.h"

#include "rasm/engine.h"

// Every renderer field of an EngineConfig must reach the Renderer: a field added to both structs but not to
// toRendererConfig keeps its default here, and its check fails.
TEST_CASE("toRendererConfig copies every renderer field")
{
    int display = 0;
    int window = 0;
    const rasm::EngineConfig engine = {
        .appName = "config test",
        .platform = rasm::PlatformType::HEADLESS,
        .windowWidth = 321,
        .windowHeight = 123,
        .nativeWindow = {.type = rasm::NativeWindowType::X11, .display = &display, .window = &window, .pixelWidth = 640, .pixelHeight = 246, .contentScale = 1.5f},
        .enableValidation = true,
        .enableSyncValidation = true,
        .enableGpuValidation = true,
        .preferredBackend = rasm::Backend::METAL,
        .assetRoot = "assets here",
        .shaderDir = "shaders here",
        .cacheDir = "cache here",
        .geometryPoolVertices = 11,
        .geometryPoolColors = 12,
        .geometryPoolSkins = 13,
        .geometryPoolMorphs = 14,
        .geometryPoolIndices = 15,
        .geometryPoolSubMeshes = 16,
        .geometryPoolMeshlets = 17,
        .geometryPoolMeshletData = 18,
        .maxInstances = 19,
        .maxLights = 20,
        .maxMaterials = 21,
        .maxAnimatedEntities = 22,
        .animationPoolBytes = 23,
        .maxMorphWeights = 24,
        .loadThreads = 25,
        .loadMemoryBudget = 26,
        .showStats = true,
        .fixedDeltaTime = 0.25f,
        .occlusionCulling = false,
        .meshLods = false,
        .lodErrorPixels = 2.5f,
        .forcedLod = 3,
        .meshletPath = rasm::MeshletPath::VERTEX_PULLING,
        .maxViewMeshlets = 27,
        .asyncCompute = true,
        .transferQueue = false,
        .clusteredLighting = false,
        .hostImageCopy = false,
        .bloom = true,
        .ui = true,
        .antiAliasing = rasm::AntiAliasing::TAA,
    };
    const rasm::RendererConfig defaults = {};
    const rasm::RendererConfig renderer = rasm::toRendererConfig(engine);

    CHECK(renderer.appName == "config test");
    CHECK(renderer.windowWidth == 321);
    CHECK(renderer.windowHeight == 123);
    CHECK(renderer.nativeWindow.type == rasm::NativeWindowType::X11);
    CHECK(renderer.nativeWindow.display == &display);
    CHECK(renderer.nativeWindow.window == &window);
    CHECK(renderer.nativeWindow.pixelWidth == 640);
    CHECK(renderer.nativeWindow.pixelHeight == 246);
    CHECK(renderer.nativeWindow.contentScale == 1.5f);
    CHECK(renderer.enableValidation != defaults.enableValidation);
    CHECK(renderer.enableSyncValidation != defaults.enableSyncValidation);
    CHECK(renderer.enableGpuValidation != defaults.enableGpuValidation);
    CHECK(renderer.preferredBackend == rasm::Backend::METAL);
    CHECK(renderer.assetRoot == "assets here");
    CHECK(renderer.shaderDir == "shaders here");
    CHECK(renderer.cacheDir == "cache here");
    CHECK(renderer.geometryPoolVertices == 11);
    CHECK(renderer.geometryPoolColors == 12);
    CHECK(renderer.geometryPoolSkins == 13);
    CHECK(renderer.geometryPoolMorphs == 14);
    CHECK(renderer.geometryPoolIndices == 15);
    CHECK(renderer.geometryPoolSubMeshes == 16);
    CHECK(renderer.geometryPoolMeshlets == 17);
    CHECK(renderer.geometryPoolMeshletData == 18);
    CHECK(renderer.maxInstances == 19);
    CHECK(renderer.maxLights == 20);
    CHECK(renderer.maxMaterials == 21);
    CHECK(renderer.maxAnimatedEntities == 22);
    CHECK(renderer.animationPoolBytes == 23);
    CHECK(renderer.maxMorphWeights == 24);
    CHECK(renderer.loadThreads == 25);
    CHECK(renderer.loadMemoryBudget == 26);
    CHECK(renderer.showStats != defaults.showStats);
    CHECK(renderer.fixedDeltaTime == 0.25f);
    CHECK(renderer.occlusionCulling != defaults.occlusionCulling);
    CHECK(renderer.meshLods != defaults.meshLods);
    CHECK(renderer.lodErrorPixels == 2.5f);
    CHECK(renderer.forcedLod == 3);
    CHECK(renderer.meshletPath == rasm::MeshletPath::VERTEX_PULLING);
    CHECK(renderer.maxViewMeshlets == 27);
    CHECK(renderer.asyncCompute != defaults.asyncCompute);
    CHECK(renderer.transferQueue != defaults.transferQueue);
    CHECK(renderer.clusteredLighting != defaults.clusteredLighting);
    CHECK(renderer.hostImageCopy != defaults.hostImageCopy);
    CHECK(renderer.bloom != defaults.bloom);
    CHECK(renderer.ui != defaults.ui);
    CHECK(renderer.antiAliasing == rasm::AntiAliasing::TAA);
}

// A Renderer that can't start (no window size) runs nothing and stays safe to call, without SDL or a GPU.
TEST_CASE("A Renderer that did not start is safe")
{
    rasm::Renderer renderer(rasm::RendererConfig{.cacheDir = ""});
    CHECK_FALSE(renderer.running());
    CHECK(renderer.createScene().isValid());
    renderer.render(rasm::SceneHandle{}, rasm::CameraHandle{});
    rasm::Renderer *base = new rasm::Engine(rasm::EngineConfig{.platform = rasm::PlatformType::HEADLESS, .cacheDir = ""});
    CHECK_FALSE(base->running());
    delete base;
}
