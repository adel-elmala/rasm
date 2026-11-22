// Example 14: glTF morph targets, morphed on the GPU once per frame.
//
// - MorphStressTest plays its clip TheWave: eight targets with POSITION and NORMAL deltas.
// - AnimatedMorphCube plays its clip Square: two targets, with TANGENT deltas too.
// - A second AnimatedMorphCube plays nothing: the application drives its first weight ("thin") with a sine through
//   setMorphWeights, from the same clock as the clips.
// - SimpleMorph plays its clip, and a second SimpleMorph plays nothing: it stands at its mesh's default weights
//   (0.5, 0.5), the rest state of a morphed mesh.
//
// The Animate pass writes every entity's weights and the Skin pass adds the weighted deltas to the vertices once per
// frame; the shaded view and the four shadow cascades all draw those vertices. With --fixed-step (verify-change passes
// it) each frame advances 1/60 s, so a given frame looks the same on every run.
//
// Options:
//   --aa taa             the scene target gets a motion color and the frames blend (addTaaPass)
//   --crowd N            N more MorphStressTest in a grid, each playing Pulse at its own phase
//   --active K           the crowd plays no clip: setMorphWeights gives it K weights of 0.5 and the other 8 - K weights 0
//   --cascades N         shadow cascades, 0 to 4 (default 4; 0: no shadows)
//   --bench              at 1920x1080 without validation (it would change the timings): print the average GPU time
//                        of each pass and the CPU frame time over frames 300 to 599, then quit
#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    constexpr double FIXED_STEP_SECONDS = 1.0 / 60.0;
    constexpr float CUBE_SCALE = 0.35f;
    constexpr uint32_t STRESS_TARGETS = 8;
    const glm::vec3 SUN_DIRECTION = glm::vec3(-0.4f, -1.0f, -0.6f);
    const glm::vec3 CAMERA_POSITION = glm::vec3(0.0f, 3.2f, 8.5f);
    const glm::vec3 ORBIT_CENTER = glm::vec3(0.0f, 0.4f, 0.0f);

    rasm::MeshHandle createCube(rasm::Engine &engine)
    {
        // One quad per face, so each face has its own normal.
        const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const auto &n : normals)
        {
            const glm::vec3 u = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 v = glm::cross(n, u);
            const glm::vec3 corners[4] = {n - u - v, n + u - v, n + u + v, n - u + v};

            const auto first = static_cast<uint32_t>(vertices.size());
            for (int i = 0; i < 4; ++i)
                vertices.push_back({.pos = corners[i] * 0.5f, .normal = n, .uv = {uvs[i], uvs[i], uvs[i]}, .tangent = glm::vec4(0.0f)});
            for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
                indices.push_back(first + index);
        }
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

    rasm::MaterialHandle createColorMaterial(rasm::Engine &engine, const glm::vec3 &color, float roughness)
    {
        auto material = engine.createMaterial(rasm::MaterialType::PBR);
        auto values = engine.getMaterial(material);
        for (uint32_t i = 0; i < 3; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[i];
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        engine.updateMaterial(material, values);
        return material;
    }

    // Turned by `yaw` radians around +Y.
    rasm::Transform makeTransform(const glm::vec3 &position, float scale, float yaw)
    {
        return rasm::Transform{position, glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(scale)};
    }

    rasm::EntityHandle addModel(rasm::Engine &engine, rasm::SceneHandle scene, const std::string &name, rasm::MeshHandle mesh, const rasm::Transform &transform)
    {
        const rasm::EntityHandle entity = engine.createEntity(name, mesh, {}, transform);
        engine.addEntityToScene(scene, entity);
        return entity;
    }

    // --debug-view's names, in the order of rasm::DebugView.
    rasm::DebugView parseDebugView(const char *name)
    {
        const char *names[] = {"none", "normals", "uvs", "overdraw", "lods", "culling", "light-count"};
        for (uint32_t i = 0; i < std::size(names); ++i)
        {
            if (std::strcmp(name, names[i]) == 0)
                return static_cast<rasm::DebugView>(i);
        }
        std::printf("Unknown debug view '%s'.\n", name);
        return rasm::DebugView::NONE;
    }
}

int main(int argc, char **argv)
{
    const ExampleArgs args = parseExampleArgs(argc, argv);
    bool taa = false;
    bool bench = false;
    uint32_t crowd = 0;
    int32_t active = -1;
    uint32_t cascades = 4;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--aa") == 0 && i + 1 < argc)
            taa = std::strcmp(argv[++i], "taa") == 0;
        if (std::strcmp(argv[i], "--crowd") == 0 && i + 1 < argc)
            crowd = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        if (std::strcmp(argv[i], "--active") == 0 && i + 1 < argc)
            active = std::min<int32_t>(static_cast<int32_t>(std::strtol(argv[++i], nullptr, 10)), STRESS_TARGETS);
        if (std::strcmp(argv[i], "--cascades") == 0 && i + 1 < argc)
            cascades = std::min<uint32_t>(static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10)), rasm::MAX_SHADOW_CASCADES);
        if (std::strcmp(argv[i], "--bench") == 0)
            bench = true;
    }

    rasm::EngineConfig config = {
        .appName = "example_14",
        .windowWidth = bench ? 1920 : 1280,
        .windowHeight = bench ? 1080 : 720,
        .enableValidation = !bench,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .ui = true};

    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    // --debug-view <name> shows the scene's debug view, as the RASM window's View does.
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--debug-view") == 0)
            engine.setDebugView(parseDebugView(argv[i + 1]));
    }

    rasm::SceneHandle scene = engine.createScene();
    const rasm::MeshHandle stressMesh = engine.loadGLTF("models/gltf/MorphStressTest/MorphStressTest.glb");
    const rasm::MeshHandle cubeMesh = engine.loadGLTF("models/gltf/AnimatedMorphCube/AnimatedMorphCube.glb");
    const rasm::MeshHandle triangleMesh = engine.loadGLTF("models/gltf/SimpleMorph/SimpleMorph.gltf");
    const rasm::MeshHandle groundMesh = createCube(engine);
    if (!stressMesh.isValid() || !cubeMesh.isValid() || !triangleMesh.isValid() || !groundMesh.isValid())
        return 1;
    // The clips' and targets' names are known once the meshes are loaded.
    engine.waitForLoads();
    const int32_t waveClip = engine.findAnimation(stressMesh, "TheWave");
    const int32_t pulseClip = engine.findAnimation(stressMesh, "Pulse");
    if (!engine.isLoaded(stressMesh) || !engine.isLoaded(cubeMesh) || !engine.isLoaded(triangleMesh) || waveClip < 0 || pulseClip < 0 ||
        engine.getMorphTargetNames(stressMesh).size() != STRESS_TARGETS)
        return 1;

    const rasm::EntityHandle ground = engine.createEntity("ground", groundMesh, createColorMaterial(engine, glm::vec3(0.55f), 0.8f),
                                                          rasm::Transform{glm::vec3(0.0f, -0.15f, -4.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(40.0f, 0.1f, 40.0f)});
    engine.addEntityToScene(scene, ground);

    const rasm::EntityHandle stress = addModel(engine, scene, "MorphStressTest", stressMesh, makeTransform(glm::vec3(0.0f), 1.0f, 0.0f));
    engine.playAnimation(stress, rasm::AnimationPlay{.clip = static_cast<uint32_t>(waveClip)});
    const rasm::EntityHandle cube = addModel(engine, scene, "AnimatedMorphCube", cubeMesh, makeTransform(glm::vec3(-2.9f, 0.4f, 1.0f), CUBE_SCALE, 0.6f));
    engine.playAnimation(cube, rasm::AnimationPlay{.clip = 0});
    // No play: the application sets its weights.
    const rasm::EntityHandle drivenCube = addModel(engine, scene, "AnimatedMorphCube driven", cubeMesh, makeTransform(glm::vec3(2.9f, 0.4f, 1.0f), CUBE_SCALE, -0.6f));
    const rasm::EntityHandle triangle = addModel(engine, scene, "SimpleMorph", triangleMesh, makeTransform(glm::vec3(-4.4f, 0.0f, -0.6f), 1.0f, 0.4f));
    engine.playAnimation(triangle, rasm::AnimationPlay{.clip = 0});
    // No play and no weights: the mesh's default weights.
    addModel(engine, scene, "SimpleMorph at rest", triangleMesh, makeTransform(glm::vec3(3.6f, 0.0f, -0.6f), 1.0f, -0.4f));

    std::vector<rasm::EntityHandle> crowdEntities;
    const auto side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(crowd))));
    for (uint32_t i = 0; i < crowd; ++i)
    {
        const glm::vec3 position((static_cast<float>(i % side) - 0.5f * static_cast<float>(side - 1)) * 4.5f, 0.0f, -2.5f - static_cast<float>(i / side) * 1.5f);
        const rasm::EntityHandle entity = addModel(engine, scene, "crowd " + std::to_string(i), stressMesh, makeTransform(position, 1.0f, 0.0f));
        if (active < 0)
            engine.playAnimation(entity, rasm::AnimationPlay{.clip = static_cast<uint32_t>(pulseClip), .clipTime = 0.37f * static_cast<float>(i)});
        crowdEntities.push_back(entity);
    }
    if (active >= 0)
    {
        float weights[STRESS_TARGETS] = {};
        std::fill(weights, weights + active, 0.5f);
        for (const rasm::EntityHandle entity : crowdEntities)
            engine.setMorphWeights(entity, weights, STRESS_TARGETS);
    }

    const rasm::CameraHandle camera = engine.createCamera(
        rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                               .perspective{.fovY = 40.0f, .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight), .nearZ = 0.1f}},
        rasm::Transform{CAMERA_POSITION, glm::quatLookAt(glm::normalize(ORBIT_CENTER - CAMERA_POSITION), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)});
    const glm::vec3 orbitCenter = ORBIT_CENTER;
    engine.setCameraController(camera, rasm::CameraController{
                                           .type = rasm::CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = glm::length(orbitCenter - engine.getCameraTransform(camera).position), .center = orbitCenter, .orbitSensitivity = 1.0f},
                                           .easingSpeed = 6.0f});
    engine.addCameraToScene(scene, camera);

    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.9f, 0.8f), glm::vec3(0.0f), glm::normalize(SUN_DIRECTION), 3.0f, 0.0f);
    engine.addLightToScene(scene, sun);
    const rasm::EnvironmentHandle sky = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    engine.setSceneEnvironment(scene, rasm::SceneEnvironment{.environment = sky, .intensity = 0.6f});
    if (!camera.isValid() || !sun.isValid())
        return 1;

    // Shadow cascades, the shaded view, TAA with --aa taa, the tone map. The first scene view adds the Animate and
    // Skin passes, which every view (and every cascade) draws from.
    rasm::RenderGraph graph = engine.createRenderGraph();
    const rasm::RenderTargetHandle sceneTarget = graph.createRenderTarget({
        .name = "scene_hdr",
        .swapchainScale = 1.0f,
        .colorFormats = {rasm::SCENE_COLOR_FORMAT, taa ? rasm::MOTION_FORMAT : rasm::Format::UNKNOWN},
        .depthFormat = rasm::Format::D32_SFLOAT,
    });
    rasm::SceneShadow shadow = {};
    if (cascades > 0)
    {
        shadow = engine.addShadowPasses(graph, scene, camera, rasm::ShadowDesc{.light = sun, .cascadeCount = cascades, .size = 2048, .distance = 20.0f});
        if (shadow.mapCount == 0)
            return 1;
    }
    if (!engine.addScenePasses(graph, scene, camera, sceneTarget, &shadow, cascades > 0 ? 1 : 0))
        return 1;
    const rasm::RenderTargetHandle resolved = taa ? engine.addTaaPass(graph, sceneTarget, camera) : sceneTarget;
    if (!resolved.isValid() || !engine.addToneMapPass(graph, resolved, camera))
        return 1;
    // The RASM window (F1) over the finished image.
    engine.addUiPass(graph);

    std::vector<std::string> timerNames;
    std::vector<double> timerSums;
    double cpuSum = 0.0;

    if (args.waitForLoads)
        engine.waitForLoads();

    double seconds = 0.0;
    auto lastTime = std::chrono::steady_clock::now();
    for (uint32_t frame = 0; engine.running(); ++frame)
    {
        const auto now = std::chrono::steady_clock::now();
        seconds += args.fixedStep ? FIXED_STEP_SECONDS : std::chrono::duration<double>(now - lastTime).count();
        lastTime = now;
        engine.setAnimationTime(seconds);
        // "thin" goes from 0 to 1 and back every 4 s; "angle" stays at 0.
        const float drivenWeights[2] = {0.5f - 0.5f * static_cast<float>(std::cos(seconds * 3.14159265358979 * 0.5)), 0.0f};
        engine.setMorphWeights(drivenCube, drivenWeights, 2);
        graph.execute(camera);
        if (!bench)
            continue;

        const rasm::FrameStats &stats = engine.getFrameStats();
        if (frame >= 300 && frame < 600)
        {
            cpuSum += stats.cpuFrameMs;
            for (uint32_t i = 0; i < stats.gpuTimerCount; ++i)
            {
                size_t t = 0;
                while (t < timerNames.size() && timerNames[t] != stats.gpuTimers[i].name)
                    ++t;
                if (t == timerNames.size())
                {
                    timerNames.push_back(stats.gpuTimers[i].name);
                    timerSums.push_back(0.0);
                }
                timerSums[t] += stats.gpuTimers[i].milliseconds;
            }
        }
        if (frame == 600)
        {
            std::printf("%-20s %.3f ms\n", "CPU frame", cpuSum / 300.0);
            for (size_t t = 0; t < timerNames.size(); ++t)
                std::printf("%-20s %.3f ms\n", timerNames[t].c_str(), timerSums[t] / 300.0);
            break;
        }
    }

    return 0;
}
