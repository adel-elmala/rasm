#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include <cmath>
#include <cstdint>
#include <vector>

// Image-based lighting: a sunset photo (an equirectangular HDR image) lights the scene and shows behind it. A 7x7 grid
// of spheres goes from rough (left) to smooth (right) and from plastic (bottom) to metal (top), beside the
// DamagedHelmet. A directional light where the photo's sun is casts cascaded shadows; the photo's own light is not
// shadowed.

namespace
{
    constexpr uint32_t GRID = 7;
    constexpr uint32_t CASCADE_COUNT = 4;

    // The sun in venice_sunset_2k.hdr, as a direction from the scene, before the environment is turned: 3.4 degrees
    // above the horizon (found from the image's brightest texels).
    const glm::vec3 SUN_IN_IMAGE(0.586f, 0.0587f, -0.808f);
    // Turns the environment so the sun sits low on the horizon, left of where the camera looks.
    constexpr float ENVIRONMENT_ROTATION = 1.15f;

    rasm::MeshHandle createSphere(rasm::Engine &engine, uint32_t rings, uint32_t segments)
    {
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (uint32_t ring = 0; ring <= rings; ++ring)
        {
            const float theta = glm::pi<float>() * static_cast<float>(ring) / static_cast<float>(rings);
            for (uint32_t segment = 0; segment <= segments; ++segment)
            {
                const float phi = 2.0f * glm::pi<float>() * static_cast<float>(segment) / static_cast<float>(segments);
                const glm::vec3 n(std::sin(theta) * std::cos(phi), std::cos(theta), -std::sin(theta) * std::sin(phi));
                const glm::vec2 uv(static_cast<float>(segment) / static_cast<float>(segments), static_cast<float>(ring) / static_cast<float>(rings));
                vertices.push_back({.pos = n * 0.5f, .normal = n, .uv = {uv, uv, uv}, .tangent = glm::vec4(0.0f)});
            }
        }
        // Counter-clockwise seen from outside.
        for (uint32_t ring = 0; ring < rings; ++ring)
        {
            for (uint32_t segment = 0; segment < segments; ++segment)
            {
                const uint32_t a = ring * (segments + 1) + segment;
                const uint32_t b = a + segments + 1;
                for (uint32_t index : {a, b, a + 1, a + 1, b, b + 1})
                    indices.push_back(index);
            }
        }
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

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

    rasm::MaterialHandle createPbrMaterial(rasm::Engine &engine, const glm::vec3 &color, float metallic, float roughness)
    {
        auto material = engine.createMaterial(rasm::MaterialType::PBR);
        auto values = engine.getMaterial(material);
        for (uint32_t i = 0; i < 3; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[i];
        values.params[static_cast<uint32_t>(rasm::PbrParam::METALLIC)] = metallic;
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        engine.updateMaterial(material, values);
        return material;
    }

    rasm::Transform makeTransform(const glm::vec3 &position, const glm::vec3 &scale)
    {
        return rasm::Transform{position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), scale};
    }

    // The environment turned by its rotation, counter-clockwise around +Y seen from above, as the shaders turn it.
    glm::vec3 turnAroundY(const glm::vec3 &d, float angle)
    {
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        return glm::vec3(d.x * c + d.z * s, d.y, -d.x * s + d.z * c);
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    rasm::EngineConfig config = {
        .appName = "example_8",
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

    // The photo decodes in the background; until its first frame is baked the scene has a constant ambient.
    const rasm::EnvironmentHandle sunset = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    rasm::MeshHandle sphere = createSphere(engine, 32, 64);
    rasm::MeshHandle cube = createCube(engine);
    rasm::MeshHandle helmet = engine.loadGLTF("models/gltf/DamagedHelmet/DamagedHelmet.gltf");
    if (!sunset.isValid() || !sphere.isValid() || !cube.isValid() || !helmet.isValid())
        return 1;
    engine.setSceneEnvironment(scene, rasm::SceneEnvironment{.environment = sunset, .intensity = 1.0f, .rotation = ENVIRONMENT_ROTATION});

    const rasm::MaterialHandle ground = createPbrMaterial(engine, glm::vec3(0.5f), 0.0f, 0.9f);
    engine.addEntityToScene(scene, engine.createEntity("ground", cube, ground, makeTransform(glm::vec3(0.0f, -0.25f, 0.0f), glm::vec3(60.0f, 0.5f, 60.0f))));

    // Roughness grows from right to left and metal from bottom to top, so the top right is a mirror.
    for (uint32_t row = 0; row < GRID; ++row)
    {
        for (uint32_t column = 0; column < GRID; ++column)
        {
            const float metallic = static_cast<float>(row) / static_cast<float>(GRID - 1);
            const float roughness = 1.0f - static_cast<float>(column) / static_cast<float>(GRID - 1);
            const rasm::MaterialHandle material = createPbrMaterial(engine, glm::vec3(0.95f, 0.64f, 0.54f), metallic, roughness);
            const glm::vec3 position(-4.2f + 0.9f * static_cast<float>(column), 0.55f + 0.9f * static_cast<float>(row), 0.0f);
            engine.addEntityToScene(scene, engine.createEntity("sphere", sphere, material, makeTransform(position, glm::vec3(0.8f))));
        }
    }
    // DamagedHelmet spans y = -0.9 .. 0.9 after its node rotation, so it is lifted to stand on the ground.
    engine.addEntityToScene(scene, engine.createEntity("helmet", helmet, {}, makeTransform(glm::vec3(2.6f, 1.25f, 0.8f), glm::vec3(1.3f))));

    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f}},
                                                    rasm::Transform{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
    const glm::vec3 eye(0.0f, 3.2f, 11.0f);
    const glm::vec3 center(-0.6f, 2.6f, 0.0f);
    engine.setCameraTransform(camera, rasm::Transform{eye, glm::quatLookAt(glm::normalize(center - eye), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)});
    engine.setCameraController(camera, rasm::CameraController{
                                           .type = rasm::CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = glm::length(eye - center),
                                                         .center = center,
                                                         .orbitSensitivity = 1.0f},
                                           .easingSpeed = 6.0f});
    engine.addCameraToScene(scene, camera);

    // The light comes from where the turned photo shows its sun, so the shadows fall the way the sky says.
    const glm::vec3 sunDirection = -glm::normalize(turnAroundY(SUN_IN_IMAGE, ENVIRONMENT_ROTATION));
    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.72f, 0.45f), glm::vec3(0.0f), sunDirection, 3.0f, 0.0f);
    engine.addLightToScene(scene, sun);
    if (!camera.isValid() || !sun.isValid())
        return 1;

    // Shadow cascades from the sun, the shaded view with the sky behind it, then the tone map.
    rasm::RenderGraph graph = engine.createRenderGraph();
    const rasm::RenderTargetHandle sceneTarget = graph.createRenderTarget({
        .name = "scene_hdr",
        .swapchainScale = 1.0f,
        .colorFormats = {rasm::SCENE_COLOR_FORMAT},
        .depthFormat = rasm::Format::D32_SFLOAT,
    });
    const rasm::SceneShadow shadow = engine.addShadowPasses(graph, scene, camera, rasm::ShadowDesc{.light = sun, .cascadeCount = CASCADE_COUNT, .size = 2048, .distance = 40.0f, .splitLambda = 0.7f});
    if (shadow.mapCount == 0 || !engine.addScenePasses(graph, scene, camera, sceneTarget, &shadow, 1) || !engine.addToneMapPass(graph, sceneTarget, camera))
        return 1;

    // The RASM window (F1) over the finished image.
    engine.addUiPass(graph);

    if (args.waitForLoads)
        engine.waitForLoads();

    while (exampleRunning(engine, args))
        graph.execute(camera);

    return exampleExit(args);
}
