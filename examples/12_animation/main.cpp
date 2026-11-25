// Example 12: glTF node animation, posed on the GPU.
//
// - AnimatedCube turns (one rotation channel, LINEAR).
// - BoxAnimated moves a box up and down and turns its child (a node tree of three).
// - InterpolationTest plays its nine clips at once (ALL_ANIMATION_CLIPS): STEP, LINEAR and CUBICSPLINE for
//   translation, rotation and scale.
// - AnimationChainTest bends a chain of 100 nodes, more than one 64-thread group of the Animate pass.
//
// The animation clock comes from the application: setAnimationTime once per frame. With --fixed-step (verify-change
// passes it) each frame advances 1/60 s, so a given frame looks the same on every run; without it the clock follows
// the wall clock.
//
// Options:
//   --aa taa             the scene target gets a motion color and the frames blend (addTaaPass)
//   --crowd N            N more BoxAnimated in a grid, each at its own phase, for the CPU and GPU numbers
//   --crowd-chain N      N more chains (100 nodes each)
//   --bench              at 1920x1080 without validation (it would change the timings): print the average GPU time
//                        of each pass over frames 300 to 600, then quit
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

namespace
{
    constexpr uint32_t CASCADE_COUNT = 4;
    constexpr double FIXED_STEP_SECONDS = 1.0 / 60.0;
    const glm::vec3 SUN_DIRECTION = glm::vec3(-0.4f, -1.0f, -0.6f);

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

    rasm::Transform makeTransform(const glm::vec3 &position, float scale)
    {
        return rasm::Transform{position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(scale)};
    }

    // N copies of a mesh in a grid around `center`, each playing every clip at its own phase.
    void addCrowd(rasm::Engine &engine, rasm::SceneHandle scene, rasm::MeshHandle mesh, uint32_t count, const glm::vec3 &center, float spacing, float scale)
    {
        const auto side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
        for (uint32_t i = 0; i < count; ++i)
        {
            const glm::vec3 offset((static_cast<float>(i % side) - 0.5f * static_cast<float>(side - 1)) * spacing, 0.0f, -static_cast<float>(i / side) * spacing);
            const rasm::EntityHandle entity = engine.createEntity("crowd " + std::to_string(i), mesh, {}, makeTransform(center + offset, scale));
            engine.addEntityToScene(scene, entity);
            engine.playAnimation(entity, rasm::AnimationPlay{.clip = rasm::ALL_ANIMATION_CLIPS, .clipTime = 0.37f * static_cast<float>(i)});
        }
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    bool taa = false;
    bool bench = false;
    uint32_t crowd = 0;
    uint32_t crowdChains = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--aa") == 0 && i + 1 < argc)
            taa = std::strcmp(argv[++i], "taa") == 0;
        if (std::strcmp(argv[i], "--crowd") == 0 && i + 1 < argc)
            crowd = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        if (std::strcmp(argv[i], "--crowd-chain") == 0 && i + 1 < argc)
            crowdChains = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        if (std::strcmp(argv[i], "--bench") == 0)
            bench = true;
    }

    rasm::EngineConfig config = {
        .appName = "example_12",
        .windowWidth = bench ? 1920 : 1280,
        .windowHeight = bench ? 1080 : 720,
        .enableValidation = !bench,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .maxInstances = 1u << 18,
        .ui = true};

    applyExampleArgs(args, config);
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    rasm::SceneHandle scene = engine.createScene();
    const rasm::MeshHandle cubeMesh = engine.loadGLTF("models/gltf/AnimatedCube/AnimatedCube.gltf");
    const rasm::MeshHandle boxMesh = engine.loadGLTF("models/gltf/BoxAnimated/BoxAnimated.glb");
    const rasm::MeshHandle interpolationMesh = engine.loadGLTF("models/gltf/InterpolationTest/InterpolationTest.glb");
    const rasm::MeshHandle chainMesh = engine.loadGLTF("models/gltf/AnimationChainTest/AnimationChainTest.gltf");
    const rasm::MeshHandle groundMesh = createCube(engine);
    if (!cubeMesh.isValid() || !boxMesh.isValid() || !interpolationMesh.isValid() || !chainMesh.isValid() || !groundMesh.isValid())
        return 1;

    const rasm::EntityHandle ground = engine.createEntity("ground", groundMesh, createColorMaterial(engine, glm::vec3(0.55f), 0.8f),
                                                          rasm::Transform{glm::vec3(0.0f, -0.05f, -4.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(40.0f, 0.1f, 40.0f)});
    const rasm::EntityHandle cube = engine.createEntity("AnimatedCube", cubeMesh, {}, makeTransform(glm::vec3(-4.5f, 1.6f, 0.0f), 0.8f));
    const rasm::EntityHandle box = engine.createEntity("BoxAnimated", boxMesh, {}, makeTransform(glm::vec3(-1.5f, 0.0f, 0.5f), 1.0f));
    const rasm::EntityHandle interpolation = engine.createEntity("InterpolationTest", interpolationMesh, {}, makeTransform(glm::vec3(3.5f, 0.9f, -1.5f), 0.3f));
    const rasm::EntityHandle chain = engine.createEntity("AnimationChainTest", chainMesh, {}, makeTransform(glm::vec3(0.8f, 0.0f, -1.0f), 0.6f));
    for (const rasm::EntityHandle entity : {ground, cube, box, interpolation, chain})
        engine.addEntityToScene(scene, entity);

    // The meshes are still loading: each play starts once its mesh is ready.
    for (const rasm::EntityHandle entity : {cube, box, interpolation, chain})
        engine.playAnimation(entity, rasm::AnimationPlay{.clip = rasm::ALL_ANIMATION_CLIPS});
    addCrowd(engine, scene, boxMesh, crowd, glm::vec3(0.0f, 0.0f, -8.0f), 1.6f, 1.0f);
    addCrowd(engine, scene, chainMesh, crowdChains, glm::vec3(0.0f, 0.0f, -8.0f), 1.2f, 0.6f);

    const rasm::CameraHandle camera = engine.createCamera(
        rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                               .perspective{.fovY = 45.0f, .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight), .nearZ = 0.1f}},
        rasm::Transform{glm::vec3(0.0f, 3.0f, 9.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
    const glm::vec3 orbitCenter(0.0f, 1.5f, -1.0f);
    engine.setCameraController(camera, rasm::CameraController{
                                           .type = rasm::CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = glm::length(orbitCenter - glm::vec3(engine.getCameraTransform(camera).position)), .center = orbitCenter, .orbitSensitivity = 1.0f},
                                           .easingSpeed = 6.0f});
    engine.addCameraToScene(scene, camera);

    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.9f, 0.8f), glm::vec3(0.0f), glm::normalize(SUN_DIRECTION), 3.0f, 0.0f);
    engine.addLightToScene(scene, sun);
    const rasm::EnvironmentHandle sky = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    engine.setSceneEnvironment(scene, rasm::SceneEnvironment{.environment = sky, .intensity = 0.6f});
    if (!camera.isValid() || !sun.isValid())
        return 1;

    // Shadow cascades, the shaded view, TAA with --aa taa, the tone map. The first scene view adds the Animate pass,
    // which every view (and every cascade) draws from.
    rasm::RenderGraph graph = engine.createRenderGraph();
    const rasm::RenderTargetHandle sceneTarget = graph.createRenderTarget({
        .name = "scene_hdr",
        .swapchainScale = 1.0f,
        .colorFormats = {rasm::SCENE_COLOR_FORMAT, taa ? rasm::MOTION_FORMAT : rasm::Format::UNKNOWN},
        .depthFormat = rasm::Format::D32_SFLOAT,
    });
    const rasm::SceneShadow shadow = engine.addShadowPasses(graph, scene, camera, rasm::ShadowDesc{.light = sun, .cascadeCount = CASCADE_COUNT, .size = 2048, .distance = 30.0f});
    if (shadow.mapCount == 0 || !engine.addScenePasses(graph, scene, camera, sceneTarget, &shadow, 1))
        return 1;
    const rasm::RenderTargetHandle resolved = taa ? engine.addTaaPass(graph, sceneTarget, camera) : sceneTarget;
    if (!resolved.isValid() || !engine.addToneMapPass(graph, resolved, camera))
        return 1;
    // The RASM window (F1) over the finished image.
    engine.addUiPass(graph);

    std::vector<std::string> timerNames;
    std::vector<double> timerSums;

    if (args.waitForLoads)
        engine.waitForLoads();

    double seconds = 0.0;
    auto lastTime = std::chrono::steady_clock::now();
    for (uint32_t frame = 0; exampleRunning(engine, args); ++frame)
    {
        const auto now = std::chrono::steady_clock::now();
        seconds += args.fixedStep ? FIXED_STEP_SECONDS : std::chrono::duration<double>(now - lastTime).count();
        lastTime = now;
        engine.setAnimationTime(seconds);
        graph.execute(camera);
        if (!bench)
            continue;

        const rasm::FrameStats &stats = engine.getFrameStats();
        if (frame >= 300)
        {
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
            for (size_t t = 0; t < timerNames.size(); ++t)
                std::printf("%-20s %.3f ms\n", timerNames[t].c_str(), timerSums[t] / 300.0);
            break;
        }
    }

    return exampleExit(args);
}
