#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// KTX2 textures. The Flight Helmet's 15 textures are KHR_texture_basisu files (ETC1S base colors, UASTC normal and
// occlusion-roughness-metal maps, with their mips), and the bunny's color texture is a .ktx2 file passed to
// loadTexture. Each is transcoded on a worker thread to the best block format the GPU samples: BC7 on desktop GPUs,
// ASTC or ETC2 on mobile ones, RGBA8 where there is none. The stats overlay (F1) shows the video memory in use.
//
// --model <path> loads another glTF in place of the helmet, relative to the asset root, for instance the helmet's
// PNG variant. --measure prints the load time and the video memory the model takes, then quits.

namespace
{
    constexpr double MIB = 1024.0 * 1024.0;

    rasm::Transform makeTransform(const glm::vec3 &position, float scale)
    {
        return rasm::Transform{position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(scale)};
    }
}

int main(int argc, char **argv)
{
    std::string modelPath = "models/gltf/FlightHelmet/FlightHelmet.gltf";
    bool measure = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc)
            modelPath = argv[++i];
        if (std::strcmp(argv[i], "--measure") == 0)
            measure = true;
    }

    rasm::EngineConfig config = {
        .appName = "example_10",
        .windowWidth = 1280,
        .windowHeight = 720,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .showStats = true};

    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    rasm::SceneHandle scene = engine.createScene();
    const rasm::EnvironmentHandle sunset = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    const rasm::MeshHandle bunnyMesh = engine.loadMesh("models/obj/bunny/bunny.obj");
    const rasm::TextureHandle bunnyColor = engine.loadTexture("models/obj/bunny/bunny-atlas.ktx2", rasm::Format::R8G8B8A8_SRGB);
    if (!sunset.isValid() || !bunnyMesh.isValid() || !bunnyColor.isValid())
        return 1;
    engine.setSceneEnvironment(scene, rasm::SceneEnvironment{.environment = sunset, .intensity = 1.0f, .rotation = 1.15f});

    const rasm::MaterialHandle bunnyMaterial = engine.createMaterial(rasm::MaterialType::PBR, {bunnyColor});
    engine.addEntityToScene(scene, engine.createEntity("bunny", bunnyMesh, bunnyMaterial, makeTransform(glm::vec3(0.95f, -0.05f, 0.3f), 0.0016f)));

    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 40.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.05f}},
                                                    rasm::Transform{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
    const glm::vec3 eye(0.3f, 0.75f, 3.0f);
    const glm::vec3 center(0.3f, 0.45f, 0.0f);
    engine.setCameraTransform(camera, rasm::Transform{eye, glm::quatLookAt(glm::normalize(center - eye), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)});
    engine.setCameraController(camera, rasm::CameraController{
                                           .type = rasm::CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = glm::length(eye - center), .center = center, .orbitSensitivity = 1.0f},
                                           .easingSpeed = 6.0f});
    engine.addCameraToScene(scene, camera);

    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.85f, 0.7f), glm::vec3(0.0f), glm::normalize(glm::vec3(0.4f, -0.6f, -0.7f)), 2.0f, 0.0f);
    engine.addLightToScene(scene, sun);
    if (!camera.isValid() || !sun.isValid())
        return 1;

    // The bunny and the environment first, so the memory the model adds can be told apart.
    engine.waitForLoads();
    for (uint32_t frame = 0; frame < 4 && engine.running(); ++frame)
        engine.render(scene, camera);
    const uint64_t vramBefore = engine.getFrameStats().memory.vramUsage;

    const auto loadStart = std::chrono::steady_clock::now();
    const rasm::MeshHandle helmet = engine.loadGLTF(modelPath);
    if (!helmet.isValid())
        return 1;
    engine.addEntityToScene(scene, engine.createEntity("helmet", helmet, {}, makeTransform(glm::vec3(0.0f, 0.0f, 0.0f), 1.6f)));

    // The frame stats are two frames old, so the memory is read a few frames after the load finished.
    double loadMs = 0.0;
    bool loaded = false;
    uint32_t loadedFrame = 0;
    bool reported = false;
    for (uint32_t frame = 0; engine.running(); ++frame)
    {
        engine.render(scene, camera);
        if (reported)
            continue;
        if (!loaded && engine.isLoaded(helmet))
        {
            loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count();
            loaded = true;
            loadedFrame = frame;
        }
        // A model that fails to load never becomes loaded; the engine logs why.
        if (measure && !loaded && frame == 3000)
        {
            std::printf("%s did not load.\n", modelPath.c_str());
            return 1;
        }
        if (!loaded || frame < loadedFrame + 4)
            continue;
        const double vramAfter = static_cast<double>(engine.getFrameStats().memory.vramUsage);
        std::printf("%s: loaded in %.0f ms, %.1f MiB of video memory\n", modelPath.c_str(), loadMs, (vramAfter - static_cast<double>(vramBefore)) / MIB);
        std::fflush(stdout);
        reported = true;
        if (measure)
            break;
    }

    return 0;
}
