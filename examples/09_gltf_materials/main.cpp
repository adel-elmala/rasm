#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

// glTF materials beyond plain PBR, under image-based lighting with a sun's shadow:
// - AlphaBlendModeTest: OPAQUE, MASK at three cutoffs and BLEND side by side.
// - TextureTransformTest: KHR_texture_transform offsets, rotation and scale; every arrow should point at a green check.
// - VertexColorTest: COLOR_0 multiplies the base color.
// - EmissiveStrengthTest: KHR_materials_emissive_strength, cubes from 1x to 16x as bright.
// - Lantern (glTF-Quantized): KHR_mesh_quantization, with KHR_texture_transform to undo the quantized UVs.
// - A block of 1536 see-through cubes, more than one block of the GPU sort, with vertex colors from createMesh. Their
//   shadow is as dense as they are (hashed alpha in the shadow map).
//
// - Bloom: the glow around the bright emissive cubes and the sky (addBloomPasses, Camera::bloomStrength).
// - MSAA with --aa msaa2|msaa4|msaa8: the scene is drawn with several samples per pixel and resolved
//   (addMsaaResolvePass); the MASK leaves of AlphaBlendModeTest use alpha-to-coverage then.
// - TAA with --aa taa: the scene target gets a motion color, the view is jittered and the frames blend (addTaaPass).
//   The lantern turns, so the pass follows a moving model by its motion vectors.
//
// --glass <count> sets the number of cubes. --no-bloom leaves the bloom out, --bloom-strength <s> sets its share (0.04
// by default). --render-size <width> <height> draws the scene at that size instead of the window's, which the tone map
// scales to the window. --gpu-times prints the mean GPU time of each timed pass over frames 300 to 600, then quits.
// --inspect <target> <color|depth|hiz|clusters> <n> opens the RASM window on that resource in its Buffers section, as
// picking it there does: n is the color, the cube face, the Hi-Z level or the cluster slice (24: the screen tiles).
// --inspect-rows opens it on the lantern's rows in the GPU tables section.

namespace
{
    constexpr uint32_t CASCADE_COUNT = 1;
    constexpr uint32_t GLASS_X = 10;
    constexpr uint32_t GLASS_Y = 8;
    constexpr uint32_t GLASS_TINTS = 6;
    constexpr float ENVIRONMENT_ROTATION = 1.15f;
    // Higher than the photo's sun, which sits on the horizon, so the shadows are short enough to see.
    const glm::vec3 SUN_DIRECTION(0.45f, -0.75f, 0.5f);

    // One quad per face, each face's corners colored from dark at the bottom to light at the top.
    rasm::MeshHandle createColoredCube(rasm::Engine &engine)
    {
        const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<uint32_t> colors;
        for (const auto &n : normals)
        {
            const glm::vec3 u = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 v = glm::cross(n, u);
            const glm::vec3 corners[4] = {n - u - v, n + u - v, n + u + v, n - u + v};

            const auto first = static_cast<uint32_t>(vertices.size());
            for (int i = 0; i < 4; ++i)
            {
                vertices.push_back({.pos = corners[i] * 0.5f, .normal = n, .uv = {uvs[i], uvs[i], uvs[i]}, .tangent = glm::vec4(0.0f)});
                const float height = corners[i].y * 0.5f + 0.5f;
                colors.push_back(rasm::packVertexColor(glm::vec4(glm::mix(glm::vec3(0.25f), glm::vec3(1.0f), height), 1.0f)));
            }
            for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
                indices.push_back(first + index);
        }
        return engine.createMesh(std::move(vertices), std::move(indices), std::move(colors));
    }

    rasm::MaterialHandle createMaterial(rasm::Engine &engine, const glm::vec4 &color, float roughness, rasm::AlphaMode alphaMode)
    {
        auto material = engine.createMaterial(rasm::MaterialType::PBR);
        auto values = engine.getMaterial(material);
        for (uint32_t i = 0; i < 4; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[i];
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        values.alphaMode = alphaMode;
        engine.updateMaterial(material, values);
        return material;
    }

    rasm::Transform makeTransform(const glm::vec3 &position, float scale)
    {
        return rasm::Transform{position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(scale)};
    }

    // --inspect's kinds, in the order of rasm::InspectKind.
    bool parseInspection(int argc, char **argv, rasm::Inspection &inspection)
    {
        const char *kinds[] = {"color", "depth", "hiz", "clusters"};
        for (int i = 1; i + 3 < argc; ++i)
        {
            if (std::strcmp(argv[i], "--inspect") != 0)
                continue;
            inspection.target = argv[i + 1];
            for (uint32_t k = 0; k < std::size(kinds); ++k)
            {
                if (std::strcmp(argv[i + 2], kinds[k]) == 0)
                    inspection.kind = static_cast<rasm::InspectKind>(k);
            }
            const auto n = static_cast<uint32_t>(std::strtoul(argv[i + 3], nullptr, 10));
            if (inspection.kind == rasm::InspectKind::COLOR)
                inspection.attachment = n;
            else if (inspection.kind == rasm::InspectKind::DEPTH)
                inspection.face = n;
            else
                inspection.level = n;
            return true;
        }
        return false;
    }

    // --debug-view's names, in the order of rasm::DebugView.
    rasm::DebugView parseDebugView(const char *name)
    {
        const char *names[] = {"none", "normals", "uvs", "overdraw", "lods", "culling", "light-count", "meshlets"};
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
    ExampleArgs args = parseExampleArgs(argc, argv);
    uint32_t glassCount = 1536;
    bool printGpuTimes = false;
    bool bloom = true;
    float bloomStrength = -1.0f;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t samples = 1;
    bool taa = false;
    bool inspectRows = false;
    rasm::Inspection inspection;
    const bool inspect = parseInspection(argc, argv, inspection);
    for (int i = 1; i < argc; ++i)
    {
        inspectRows = inspectRows || std::strcmp(argv[i], "--inspect-rows") == 0;
        if (std::strcmp(argv[i], "--aa") == 0 && i + 1 < argc)
        {
            const char *mode = argv[++i];
            if (std::strcmp(mode, "msaa2") == 0)
                samples = 2;
            if (std::strcmp(mode, "msaa4") == 0)
                samples = 4;
            if (std::strcmp(mode, "msaa8") == 0)
                samples = 8;
            taa = std::strcmp(mode, "taa") == 0;
        }
        if (std::strcmp(argv[i], "--glass") == 0 && i + 1 < argc)
            glassCount = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        if (std::strcmp(argv[i], "--gpu-times") == 0)
            printGpuTimes = true;
        if (std::strcmp(argv[i], "--no-bloom") == 0)
            bloom = false;
        if (std::strcmp(argv[i], "--bloom-strength") == 0 && i + 1 < argc)
            bloomStrength = std::strtof(argv[++i], nullptr);
        if (std::strcmp(argv[i], "--render-size") == 0 && i + 2 < argc)
        {
            renderWidth = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            renderHeight = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }

    rasm::EngineConfig config = {
        .appName = "example_9",
        .windowWidth = 1280,
        .windowHeight = 720,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .showStats = inspect || inspectRows,
        .ui = true};

    applyExampleArgs(args, config);
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    // --debug-view <name> shows the scene's debug view, as the RASM window's View does.
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--debug-view") == 0)
            engine.setDebugView(parseDebugView(argv[i + 1]));
    }

    // A device that can't draw that many samples per pixel gets the most it can below them.
    while (samples > 1 && !engine.getRenderContext().supportsSampleCount(samples))
        samples /= 2;

    rasm::SceneHandle scene = engine.createScene();
    const rasm::EnvironmentHandle sunset = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    const rasm::MeshHandle alphaModes = engine.loadGLTF("models/gltf/AlphaBlendModeTest/AlphaBlendModeTest.glb");
    const rasm::MeshHandle textureTransforms = engine.loadGLTF("models/gltf/TextureTransformTest/TextureTransformTest.gltf");
    const rasm::MeshHandle vertexColors = engine.loadGLTF("models/gltf/VertexColorTest/VertexColorTest.glb");
    const rasm::MeshHandle emissiveStrength = engine.loadGLTF("models/gltf/EmissiveStrengthTest/EmissiveStrengthTest.glb");
    const rasm::MeshHandle lantern = engine.loadGLTF("models/gltf/LanternQuantized/Lantern.gltf");
    const rasm::MeshHandle cube = createColoredCube(engine);
    if (!sunset.isValid() || !alphaModes.isValid() || !textureTransforms.isValid() || !vertexColors.isValid() || !emissiveStrength.isValid() || !lantern.isValid() || !cube.isValid())
        return 1;
    engine.setSceneEnvironment(scene, rasm::SceneEnvironment{.environment = sunset, .intensity = 1.0f, .rotation = ENVIRONMENT_ROTATION});

    const rasm::MaterialHandle ground = createMaterial(engine, glm::vec4(0.5f, 0.5f, 0.5f, 1.0f), 0.9f, rasm::AlphaMode::NONE);
    engine.addEntityToScene(scene, engine.createEntity("ground", cube, ground, rasm::Transform{glm::vec3(0.0f, -0.25f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(60.0f, 0.5f, 60.0f)}));

    // The test models face +Z, toward the camera.
    engine.addEntityToScene(scene, engine.createEntity("texture transforms", textureTransforms, {}, makeTransform(glm::vec3(-7.4f, 1.4f, 1.0f), 1.0f)));
    engine.addEntityToScene(scene, engine.createEntity("vertex colors", vertexColors, {}, makeTransform(glm::vec3(-4.0f, 1.0f, 1.0f), 0.9f)));
    engine.addEntityToScene(scene, engine.createEntity("alpha modes", alphaModes, {}, makeTransform(glm::vec3(1.4f, 0.48f, 1.0f), 1.0f)));
    engine.addEntityToScene(scene, engine.createEntity("emissive strength", emissiveStrength, {}, makeTransform(glm::vec3(1.4f, 3.2f, -6.0f), 0.3f)));
    const rasm::EntityHandle lanternEntity = engine.createEntity("lantern", lantern, {}, makeTransform(glm::vec3(-7.0f, 0.0f, -5.0f), 0.12f));
    engine.addEntityToScene(scene, lanternEntity);
    if (inspectRows)
        inspection.entity = lanternEntity;
    if (inspect || inspectRows)
        engine.setInspection(inspection);

    // The glass block, a layer of GLASS_X x GLASS_Y cubes per row in depth, in a few tints and opacities.
    rasm::MaterialHandle glass[GLASS_TINTS];
    for (uint32_t i = 0; i < GLASS_TINTS; ++i)
    {
        const float hue = static_cast<float>(i) / static_cast<float>(GLASS_TINTS);
        const glm::vec3 tint = glm::clamp(glm::abs(glm::mod(hue * 6.0f + glm::vec3(0.0f, 4.0f, 2.0f), 6.0f) - 3.0f) - 1.0f, 0.0f, 1.0f);
        glass[i] = createMaterial(engine, glm::vec4(glm::mix(glm::vec3(1.0f), tint, 0.8f), 0.25f + 0.15f * static_cast<float>(i % 3)), 0.15f, rasm::AlphaMode::BLEND);
    }
    for (uint32_t i = 0; i < glassCount; ++i)
    {
        const uint32_t x = i % GLASS_X;
        const uint32_t y = i / GLASS_X % GLASS_Y;
        const uint32_t z = i / (GLASS_X * GLASS_Y);
        const glm::vec3 position(6.2f + 0.36f * static_cast<float>(x), 0.2f + 0.36f * static_cast<float>(y), 1.5f - 0.36f * static_cast<float>(z));
        engine.addEntityToScene(scene, engine.createEntity("glass", cube, glass[(x + y + z) % GLASS_TINTS], makeTransform(position, 0.22f)));
    }

    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 50.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f}},
                                                    rasm::Transform{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
    const glm::vec3 eye(0.8f, 4.0f, 13.5f);
    const glm::vec3 center(0.8f, 1.3f, -1.0f);
    engine.setCameraTransform(camera, rasm::Transform{eye, glm::quatLookAt(glm::normalize(center - eye), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)});
    engine.setCameraController(camera, rasm::CameraController{
                                           .type = rasm::CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = glm::length(eye - center), .center = center, .orbitSensitivity = 1.0f},
                                           .easingSpeed = 6.0f});
    engine.addCameraToScene(scene, camera);
    if (bloomStrength >= 0.0f)
        engine.setCameraBloomStrength(camera, bloomStrength);

    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.85f, 0.7f), glm::vec3(0.0f), glm::normalize(SUN_DIRECTION), 3.0f, 0.0f);
    engine.addLightToScene(scene, sun);
    if (!camera.isValid() || !sun.isValid())
        return 1;

    // Shadow cascades, the shaded view (its draws, the sky, then the see-through draws back to front), the MSAA resolve,
    // the bloom, the tone map.
    rasm::RenderGraph graph = engine.createRenderGraph();
    const bool fixedSize = renderWidth > 0 && renderHeight > 0;
    const rasm::RenderTargetHandle sceneTarget = graph.createRenderTarget({
        .name = "scene_hdr",
        .width = renderWidth,
        .height = renderHeight,
        .swapchainScale = fixedSize ? 0.0f : 1.0f,
        .colorFormats = {rasm::SCENE_COLOR_FORMAT, taa ? rasm::MOTION_FORMAT : rasm::Format::UNKNOWN},
        .depthFormat = rasm::Format::D32_SFLOAT,
        .samples = samples,
    });
    const rasm::SceneShadow shadow = engine.addShadowPasses(graph, scene, camera, rasm::ShadowDesc{.light = sun, .cascadeCount = CASCADE_COUNT, .size = 2048, .distance = 30.0f, .splitLambda = 0.7f});
    if (shadow.mapCount == 0 || !engine.addScenePasses(graph, scene, camera, sceneTarget, &shadow, 1))
        return 1;
    rasm::RenderTargetHandle resolved = sceneTarget;
    if (samples > 1)
        resolved = engine.addMsaaResolvePass(graph, sceneTarget, camera);
    if (taa)
        resolved = engine.addTaaPass(graph, sceneTarget, camera);
    if (!resolved.isValid())
        return 1;
    const rasm::RenderTargetHandle bloomTarget = bloom ? engine.addBloomPasses(graph, resolved, camera) : rasm::RenderTargetHandle{};
    if ((bloom && !bloomTarget.isValid()) || !engine.addToneMapPass(graph, resolved, camera, bloomTarget))
        return 1;
    // The RASM window (F1) over the finished image.
    engine.addUiPass(graph);

    std::vector<std::string> timerNames;
    std::vector<double> timerSums;

    if (args.waitForLoads)
        engine.waitForLoads();

    for (uint32_t frame = 0; exampleRunning(engine, args); ++frame)
    {
        // The lantern turns by a fixed angle per frame, so its motion (and TAA's motion vectors) is the same every run.
        engine.updateTransform(lanternEntity, engine.getTransform(lanternEntity).rotate(glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(1.0f)));
        graph.execute(camera);
        if (!printGpuTimes)
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
