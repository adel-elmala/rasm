#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    constexpr int GRID_SIZE = 100;
    constexpr float GRID_SPACING = 1.5f;
    constexpr int SPINNER_COUNT = 10;
    constexpr int SPAWN_INTERVAL = 20; // frames, so frame N looks the same on every run
    constexpr int MAX_SPAWNED_ROWS = 20;

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

    rasm::Transform cubeTransform(const glm::vec3 &position, float scale)
    {
        return rasm::Transform{position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(scale)};
    }

    glm::vec3 gridPosition(int x, int z)
    {
        const float half = (GRID_SIZE - 1) * GRID_SPACING * 0.5f;
        return glm::vec3(x * GRID_SPACING - half, 0.0f, z * GRID_SPACING - half);
    }
}

int main()
{
    rasm::EngineConfig config = {
        .appName = "example_3",
        .windowWidth = 720,
        .windowHeight = 480,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR

    };

    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    rasm::SceneHandle scene = engine.createScene();

    rasm::MeshHandle cube = createCube(engine);
    if (!cube.isValid())
        return 1;

    const rasm::MaterialHandle warm = createColorMaterial(engine, glm::vec3(0.9f, 0.55f, 0.3f), 0.6f);
    const rasm::MaterialHandle cool = createColorMaterial(engine, glm::vec3(0.3f, 0.5f, 0.85f), 0.4f);
    const rasm::MaterialHandle pulse = createColorMaterial(engine, glm::vec3(1.0f), 0.3f);

    // A static grid: once its rows are uploaded, it costs nothing per frame.
    for (int z = 0; z < GRID_SIZE; ++z)
    {
        for (int x = 0; x < GRID_SIZE; ++x)
        {
            const auto material = (x + z) % 2 == 0 ? warm : cool;
            engine.addEntityToScene(scene, engine.createEntity("grid", cube, material, cubeTransform(gridPosition(x, z), 1.0f)));
        }
    }

    // A few moving cubes: only their rows are uploaded each frame.
    std::vector<rasm::EntityHandle> spinners;
    for (int i = 0; i < SPINNER_COUNT; ++i)
    {
        const float angle = glm::radians(360.0f * i / SPINNER_COUNT);
        const glm::vec3 position(std::cos(angle) * 12.0f, 4.0f, std::sin(angle) * 12.0f);
        spinners.push_back(engine.createEntity("spinner", cube, pulse, cubeTransform(position, 2.0f)));
        engine.addEntityToScene(scene, spinners.back());
    }

    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f}},
                                                    rasm::Transform{
                                                        glm::vec3(0.0f, 45.0f, 95.0f),                                     // position
                                                        glm::angleAxis(glm::radians(-30.0f), glm::vec3(1.0f, 0.0f, 0.0f)), // rotation
                                                        glm::vec3(1.0f)                                                    // scale
                                                    });
    engine.setCameraController(camera, rasm::CameraController{
                                           .type = rasm::CameraController::Type::FREE_FLY,
                                           .freeFly = {.speed = 20.0f, .lookSensitivity = 1.0f},
                                           .easingSpeed = 6.0f});

    rasm::LightHandle light = engine.createLight(rasm::LightType::DIRECTIONAL,
                                                 glm::vec3(1.0f, 0.95f, 0.85f),
                                                 glm::vec3(0.0f),
                                                 glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f)),
                                                 3.0f, 0.0f);
    rasm::LightHandle fillLight = engine.createLight(rasm::LightType::DIRECTIONAL,
                                                     glm::vec3(0.6f, 0.75f, 1.0f),
                                                     glm::vec3(0.0f),
                                                     glm::normalize(glm::vec3(0.7f, -0.3f, 0.5f)),
                                                     0.8f, 0.0f);
    engine.addCameraToScene(scene, camera);
    engine.addLightToScene(scene, light);
    engine.addLightToScene(scene, fillLight);

    // Animation advances per frame, not per second, so a given frame renders the same on every run.
    uint64_t frame = 0;
    int spawnedRows = 0;
    while (engine.running())
    {
        for (int i = 0; i < SPINNER_COUNT; ++i)
        {
            const glm::vec3 axis = glm::normalize(glm::vec3(1.0f, 1.0f + i, 0.5f));
            engine.updateTransform(spinners[i], engine.getTransform(spinners[i]).rotate(axis, glm::radians(1.0f)));
        }

        // Entities added while running show up from the next frame on (D6).
        if (frame % SPAWN_INTERVAL == 0 && spawnedRows < MAX_SPAWNED_ROWS)
        {
            const int z = -2 - spawnedRows++;
            for (int x = 0; x < GRID_SIZE; ++x)
            {
                glm::vec3 position = gridPosition(x, z);
                position.y = 0.5f * std::sin(x * 0.3f);
                engine.addEntityToScene(scene, engine.createEntity("spawned", cube, pulse, cubeTransform(position, 0.8f)));
            }
        }

        // Editing a material rewrites one row of the material table; every cube using it changes color.
        auto values = engine.getMaterial(pulse);
        const float t = static_cast<float>(frame) * 0.02f;
        values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO)] = 0.5f + 0.5f * std::sin(t);
        values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + 1] = 0.5f + 0.5f * std::sin(t + 2.1f);
        values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + 2] = 0.5f + 0.5f * std::sin(t + 4.2f);
        engine.updateMaterial(pulse, values);

        engine.render(scene, camera);
        ++frame;
    }

    return 0;
}
