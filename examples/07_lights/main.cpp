#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include <cmath>
#include <cstdint>
#include <vector>

// Three lights that cast shadows, read by one view: a weak moon (a directional light, in cascades), a spot lamp (one
// perspective map along its cone) and a lantern (a point light, in a cube map). The cube beside the lantern turns, so
// its shadow moves over the ground and the wall.

namespace
{
    rasm::MeshHandle createCube(rasm::Engine &engine)
    {
        // One quad per face, so each face has its own normal.
        const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const auto &n : normals)
        {
            // Two axes in the face's plane, ordered so the quad is counter-clockwise seen from outside.
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

    rasm::MaterialHandle createColorMaterial(rasm::Engine &engine, const glm::vec3 &color, float roughness, const glm::vec3 &emissive = glm::vec3(0.0f))
    {
        auto material = engine.createMaterial(rasm::MaterialType::PBR);
        auto values = engine.getMaterial(material);
        for (uint32_t i = 0; i < 3; ++i)
        {
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[i];
            values.params[static_cast<uint32_t>(rasm::PbrParam::EMISSIVE_FACTOR) + i] = emissive[i];
        }
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        engine.updateMaterial(material, values);
        return material;
    }

    rasm::Transform makeTransform(const glm::vec3 &position, const glm::vec3 &scale)
    {
        return rasm::Transform{position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), scale};
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    rasm::EngineConfig config = {
        .appName = "example_7",
        .windowWidth = 1280,
        .windowHeight = 720,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .ui = true};

    applyExampleArgs(args, config);
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    rasm::SceneHandle scene = engine.createScene();

    rasm::MeshHandle cube = createCube(engine);
    rasm::MeshHandle bunnyMesh = engine.loadMesh("models/obj/bunny/bunny.obj");
    if (!cube.isValid() || !bunnyMesh.isValid())
        return 1;

    const rasm::MaterialHandle ground = createColorMaterial(engine, glm::vec3(0.7f), 0.9f);
    const rasm::MaterialHandle wall = createColorMaterial(engine, glm::vec3(0.8f, 0.78f, 0.72f), 0.8f);
    const rasm::MaterialHandle warm = createColorMaterial(engine, glm::vec3(0.9f, 0.5f, 0.25f), 0.5f);
    const rasm::MaterialHandle cool = createColorMaterial(engine, glm::vec3(0.25f, 0.5f, 0.9f), 0.4f);
    const rasm::MaterialHandle glow = createColorMaterial(engine, glm::vec3(0.0f), 1.0f, glm::vec3(8.0f, 5.0f, 2.0f));

    engine.addEntityToScene(scene, engine.createEntity("ground", cube, ground, makeTransform(glm::vec3(0.0f, -0.25f, 0.0f), glm::vec3(60.0f, 0.5f, 60.0f))));
    engine.addEntityToScene(scene, engine.createEntity("wall", cube, wall, makeTransform(glm::vec3(0.0f, 3.0f, -6.0f), glm::vec3(24.0f, 6.0f, 0.5f))));
    // The bunny's vertices are centered on its origin and reach 247.6 below it, so it is lifted to stand on the ground.
    engine.addEntityToScene(scene, engine.createEntity("bunny", bunnyMesh, warm, makeTransform(glm::vec3(-1.0f, 247.6f * 0.01f, -1.0f), glm::vec3(0.01f))));
    for (int i = 0; i < 3; ++i)
    {
        const glm::vec3 position(-6.0f + 6.0f * static_cast<float>(i), 1.0f, 2.5f - 1.5f * static_cast<float>(i));
        engine.addEntityToScene(scene, engine.createEntity("pillar", cube, cool, makeTransform(position, glm::vec3(0.8f, 2.0f, 0.8f))));
    }

    // The lantern hangs in a small glowing box. The box's faces turn away from the light inside it, so the cube's faces
    // cull them and the box casts no shadow.
    const glm::vec3 lanternPosition(4.5f, 1.6f, 2.5f);
    engine.addEntityToScene(scene, engine.createEntity("lantern", cube, glow, makeTransform(lanternPosition, glm::vec3(0.2f))));
    rasm::EntityHandle spinner = engine.createEntity("spinner", cube, warm, makeTransform(lanternPosition + glm::vec3(-1.6f, -0.4f, 0.4f), glm::vec3(0.9f)));
    engine.addEntityToScene(scene, spinner);

    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f}},
                                                    rasm::Transform{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
    const glm::vec3 eye(2.0f, 9.0f, 17.0f);
    const glm::vec3 center(0.0f, 1.0f, 0.0f);
    engine.setCameraTransform(camera, rasm::Transform{eye, glm::quatLookAt(glm::normalize(center - eye), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)});
    engine.setCameraController(camera, rasm::CameraController{
                                           .type = rasm::CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = glm::length(eye - center),
                                                         .center = center,
                                                         .orbitSensitivity = 1.0f},
                                           .easingSpeed = 6.0f});
    engine.addCameraToScene(scene, camera);

    const rasm::LightHandle moon = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(0.55f, 0.65f, 1.0f), glm::vec3(0.0f),
                                                      glm::normalize(glm::vec3(0.4f, -1.0f, -0.6f)), 0.4f, 0.0f);
    const glm::vec3 lampPosition(-8.0f, 8.0f, 6.0f);
    const rasm::LightHandle lamp = engine.createLight(rasm::LightType::SPOT, glm::vec3(1.0f, 0.95f, 0.8f), lampPosition,
                                                      glm::normalize(glm::vec3(0.0f, 0.0f, -2.0f) - lampPosition), 900.0f, 40.0f,
                                                      glm::radians(14.0f), glm::radians(22.0f));
    const rasm::LightHandle lantern = engine.createLight(rasm::LightType::POINT, glm::vec3(1.0f, 0.6f, 0.3f), lanternPosition, glm::vec3(0.0f), 25.0f, 14.0f);
    engine.addLightToScene(scene, moon);
    engine.addLightToScene(scene, lamp);
    engine.addLightToScene(scene, lantern);
    if (!camera.isValid() || !moon.isValid() || !lamp.isValid() || !lantern.isValid())
        return 1;

    rasm::RenderGraph graph = engine.createRenderGraph();

    const rasm::RenderTargetHandle sceneTarget = graph.createRenderTarget({
        .name = "scene_rt",
        .swapchainScale = 1.0f,
        .colorFormats = {rasm::SCENE_COLOR_FORMAT},
        .depthFormat = rasm::Format::D32_SFLOAT,
        .clearColor = glm::vec4(0.02f, 0.025f, 0.05f, 1.0f),
    });

    // 1. Shadows: each light's maps, depth only. 4 cascades + 1 spot map + 6 cube faces are 11 scene views.
    // 2. Forward: the scene from the camera, which reads all of them and darkens each light by its own shadow.
    // 3. Tone map: into the swapchain.
    const rasm::SceneShadow shadows[] = {
        engine.addShadowPasses(graph, scene, camera, rasm::ShadowDesc{.light = moon, .cascadeCount = 4, .size = 2048, .distance = 40.0f}),
        engine.addShadowPasses(graph, scene, camera, rasm::ShadowDesc{.light = lamp, .size = 2048, .distance = 40.0f}),
        engine.addShadowPasses(graph, scene, camera, rasm::ShadowDesc{.light = lantern, .size = 1024, .distance = 14.0f}),
    };
    for (const rasm::SceneShadow &shadow : shadows)
    {
        if (shadow.mapCount == 0)
            return 1;
    }
    if (!engine.addScenePasses(graph, scene, camera, sceneTarget, shadows, 3) || !engine.addToneMapPass(graph, sceneTarget, camera))
        return 1;

    // The RASM window (F1) over the finished image.
    engine.addUiPass(graph);
    graph.compile();

    if (args.waitForLoads)
        engine.waitForLoads();

    // Animation advances per frame, not per second, so a given frame renders the same on every run.
    while (exampleRunning(engine, args))
    {
        engine.updateTransform(spinner, engine.getTransform(spinner).rotate(glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)), glm::radians(1.0f)));
        graph.execute(camera);
    }

    return 0;
}
