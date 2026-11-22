#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Headless rendering: no window, no events. The engine renders the DamagedHelmet under image-based lighting with a
// cascaded sun shadow into an offscreen swapchain, then copies two images back to the CPU:
// - the final frame (readFrame), as RGBA8 sRGB, written to a PNG;
// - the HDR scene color (readTexture of a readable render target), as RGBA32 float, whose brightest and mean values
//   it prints.
// The copies are asynchronous: they are recorded at the end of a frame and ready once the GPU has finished it, so the
// example keeps rendering and polls isReadbackReady instead of waiting.
//   --out <file.png>   where the frame goes (default headless.png in the working directory)
//   --frames <n>       frames rendered before the copies (default 16)

namespace
{
    constexpr float ENVIRONMENT_ROTATION = 1.15f;
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;

    // A flat square on y = 0, facing up, for the helmet's shadow.
    rasm::MeshHandle createGround(rasm::Engine &engine, float halfSize)
    {
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        std::vector<rasm::Vertex> vertices;
        for (const glm::vec2 corner : {glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, -1.0f)})
        {
            const glm::vec2 uv = corner * 0.5f + 0.5f;
            vertices.push_back({.pos = glm::vec3(corner.x, 0.0f, corner.y) * halfSize, .normal = up, .uv = {uv, uv, uv}, .tangent = glm::vec4(0.0f)});
        }
        return engine.createMesh(std::move(vertices), {0, 1, 2, 0, 2, 3});
    }

    float luminance(const float *rgba)
    {
        return 0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2];
    }
}

int main(int argc, char **argv)
{
    const ExampleArgs args = parseExampleArgs(argc, argv);
    std::string outPath = "headless.png";
    uint32_t frames = 16;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--out") == 0)
            outPath = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0)
            frames = static_cast<uint32_t>(std::max(1, std::atoi(argv[++i])));
    }
    (void)args; // a headless run always waits for its loads below

    rasm::EngineConfig config = {
        .appName = "example_16",
        .platform = rasm::PlatformType::HEADLESS,
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR};

    const auto start = std::chrono::steady_clock::now();
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    rasm::SceneHandle scene = engine.createScene();
    const rasm::EnvironmentHandle sunset = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    const rasm::MeshHandle helmet = engine.loadGLTF("models/gltf/DamagedHelmet/DamagedHelmet.gltf");
    if (!sunset.isValid() || !helmet.isValid())
        return 1;
    engine.setSceneEnvironment(scene, rasm::SceneEnvironment{.environment = sunset, .intensity = 1.0f, .rotation = ENVIRONMENT_ROTATION});
    const rasm::MaterialHandle stone = engine.createMaterial(rasm::MaterialType::PBR);
    rasm::Material stoneValues = engine.getMaterial(stone);
    stoneValues.params[static_cast<uint32_t>(rasm::PbrParam::METALLIC)] = 0.0f;
    stoneValues.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = 0.85f;
    engine.updateMaterial(stone, stoneValues);
    engine.addEntityToScene(scene, engine.createEntity("ground", createGround(engine, 30.0f), stone, rasm::Transform{glm::vec3(0.0f, -1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)}));
    engine.addEntityToScene(scene, engine.createEntity("helmet", helmet, {}, rasm::Transform{glm::vec3(0.0f), glm::angleAxis(glm::radians(-25.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)}));

    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 40.0f, .aspect = static_cast<float>(WIDTH) / static_cast<float>(HEIGHT), .nearZ = 0.1f}},
                                                    rasm::Transform{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
    const glm::vec3 eye(0.0f, 0.4f, 3.6f);
    engine.setCameraTransform(camera, rasm::Transform{eye, glm::quatLookAt(glm::normalize(-eye), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)});
    engine.addCameraToScene(scene, camera);

    // A key light from high on the left, so the shadow falls on the ground in view; the photo's low sun lights the rest.
    const glm::vec3 sunDirection = glm::normalize(glm::vec3(0.6f, -1.0f, -0.35f));
    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.85f, 0.7f), glm::vec3(0.0f), sunDirection, 3.0f, 0.0f);
    engine.addLightToScene(scene, sun);

    // A readable scene target gets memory of its own, so its HDR color can be copied after the frame.
    rasm::RenderGraph graph = engine.createRenderGraph();
    const rasm::RenderTargetHandle sceneTarget = graph.createRenderTarget({
        .name = "scene_hdr",
        .swapchainScale = 1.0f,
        .colorFormats = {rasm::SCENE_COLOR_FORMAT},
        .depthFormat = rasm::Format::D32_SFLOAT,
        .readable = true,
    });
    const rasm::SceneShadow shadow = engine.addShadowPasses(graph, scene, camera, rasm::ShadowDesc{.light = sun, .cascadeCount = 4, .size = 2048, .distance = 20.0f, .splitLambda = 0.7f});
    if (shadow.mapCount == 0 || !engine.addScenePasses(graph, scene, camera, sceneTarget, &shadow, 1) || !engine.addToneMapPass(graph, sceneTarget, camera))
        return 1;

    // Every load is in place before the first frame, so frame N looks the same on every run.
    engine.waitForLoads();
    for (uint32_t frame = 0; frame < frames; ++frame)
        graph.execute(camera);

    // Both copies go into the next frame. The scene target's texture is the one of that frame's slot.
    const rasm::ReadbackHandle frameCopy = engine.readFrame(rasm::ReadbackFormat::RGBA8_SRGB);
    const rasm::ReadbackHandle hdrCopy = engine.readTexture(engine.getRenderTargetTexture(sceneTarget), rasm::ReadbackFormat::RGBA32_FLOAT);
    if (!frameCopy.isValid() || !hdrCopy.isValid())
        return 1;
    uint32_t extraFrames = 0;
    do
    {
        graph.execute(camera);
        ++extraFrames;
    } while (!(engine.isReadbackReady(frameCopy) && engine.isReadbackReady(hdrCopy)) && extraFrames < 8);

    rasm::ReadbackImage image;
    rasm::ReadbackImage hdr;
    if (!engine.takeReadback(frameCopy, image) || !engine.takeReadback(hdrCopy, hdr) || !rasm::writePng(outPath, image))
        return 1;

    float brightest = 0.0f;
    double sum = 0.0;
    const auto *texels = reinterpret_cast<const float *>(hdr.pixels.data());
    const size_t count = static_cast<size_t>(hdr.width) * hdr.height;
    for (size_t i = 0; i < count; ++i)
    {
        const float value = luminance(texels + i * 4);
        brightest = std::max(brightest, value);
        sum += value;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("Wrote %s (%u x %u) after %u + %u frames, %.2f s in all.\n", outPath.c_str(), image.width, image.height, frames, extraFrames, seconds);
    std::printf("HDR scene color: mean luminance %.3f, brightest %.3f.\n", sum / static_cast<double>(count), brightest);
    return 0;
}
