#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

// Many small lights that drift over a yard of pillars and glass panes, under a weak moon with a shadow. Each light
// has a range, so the engine sorts the lights into clusters of the view and every pixel shades only the lights of
// its own cluster (EngineConfig::clusteredLighting). The glass is see-through and gets the same lights.
//
//   --lights N     the number of lights (1024); EngineConfig::maxLights grows to fit them
//   --all-lights   every pixel shades every light, as without clusters: the reference image, and the cost to compare
//   --ortho        an orthographic camera
//   --orbit        the camera circles the yard on its own
//   --bench        at 1920x1080, without validation (it would change the timings) and without the moon's shadow (its
//                  passes would take the GPU timers the scene needs, D21): prints the GPU time of each pass, averaged
//                  over 240 frames, and exits
//   --inspect <target> <color|depth|hiz|clusters> <n>
//                  opens the RASM window on that resource in its Buffers section, as picking it there does: n is the
//                  color, the cube face, the Hi-Z level or the cluster slice (24: the screen tiles)
//
// The lights cover about the same share of the ground for any N: their range shrinks as there are more of them.

namespace
{
    constexpr float YARD_SIZE = 60.0f;
    constexpr uint32_t COLOR_COUNT = 8;
    constexpr uint32_t BENCH_WARMUP_FRAMES = 60;
    constexpr uint32_t BENCH_FRAMES = 240;

    struct Options
    {
        uint32_t lights = 1024;
        bool allLights = false;
        bool ortho = false;
        bool orbit = false;
        bool bench = false;
        bool inspect = false;
        rasm::Inspection inspection;
    };

    struct MovingLight
    {
        rasm::LightHandle light;
        rasm::EntityHandle marker;
        glm::vec3 home;
        float radius;
        float speed; // radians per frame
        float phase;
    };

    // The same yard and the same lights on every run.
    struct Random
    {
        uint32_t state = 12345u;

        float next()
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
        }

        float range(float low, float high) { return low + (high - low) * next(); }
    };

    Options parseOptions(int argc, char **argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--lights") == 0 && i + 1 < argc)
                options.lights = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            else if (std::strcmp(argv[i], "--all-lights") == 0)
                options.allLights = true;
            else if (std::strcmp(argv[i], "--ortho") == 0)
                options.ortho = true;
            else if (std::strcmp(argv[i], "--orbit") == 0)
                options.orbit = true;
            else if (std::strcmp(argv[i], "--bench") == 0)
                options.bench = true;
            else if (std::strcmp(argv[i], "--inspect") == 0 && i + 3 < argc)
            {
                // The kinds in the order of rasm::InspectKind.
                const char *kinds[] = {"color", "depth", "hiz", "clusters"};
                rasm::Inspection &inspection = options.inspection;
                inspection.target = argv[++i];
                const char *kind = argv[++i];
                for (uint32_t k = 0; k < std::size(kinds); ++k)
                {
                    if (std::strcmp(kind, kinds[k]) == 0)
                        inspection.kind = static_cast<rasm::InspectKind>(k);
                }
                const auto n = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
                if (inspection.kind == rasm::InspectKind::COLOR)
                    inspection.attachment = n;
                else if (inspection.kind == rasm::InspectKind::DEPTH)
                    inspection.face = n;
                else
                    inspection.level = n;
                options.inspect = true;
            }
        }
        return options;
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

    rasm::MaterialHandle createColorMaterial(rasm::Engine &engine, const glm::vec4 &color, float roughness, const glm::vec3 &emissive = glm::vec3(0.0f),
                                             rasm::AlphaMode alphaMode = rasm::AlphaMode::NONE)
    {
        auto material = engine.createMaterial(rasm::MaterialType::PBR);
        auto values = engine.getMaterial(material);
        for (uint32_t i = 0; i < 4; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[i];
        for (uint32_t i = 0; i < 3; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::EMISSIVE_FACTOR) + i] = emissive[i];
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        values.alphaMode = alphaMode;
        values.doubleSided = alphaMode == rasm::AlphaMode::BLEND;
        engine.updateMaterial(material, values);
        return material;
    }

    rasm::Transform makeTransform(const glm::vec3 &position, const glm::vec3 &scale)
    {
        return rasm::Transform{position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), scale};
    }

    glm::vec3 getHue(uint32_t i)
    {
        const glm::vec3 hues[COLOR_COUNT] = {{1.0f, 0.55f, 0.25f}, {0.3f, 0.6f, 1.0f}, {1.0f, 0.3f, 0.45f}, {0.4f, 1.0f, 0.5f}, {1.0f, 0.85f, 0.4f}, {0.7f, 0.4f, 1.0f}, {0.35f, 0.95f, 1.0f}, {1.0f, 1.0f, 0.95f}};
        return hues[i % COLOR_COUNT];
    }

    glm::vec3 getLightPosition(const MovingLight &moving, uint32_t frame)
    {
        const float angle = moving.phase + moving.speed * static_cast<float>(frame);
        return moving.home + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * moving.radius;
    }

    rasm::Transform getCameraTransform(float angle, float distance)
    {
        const glm::vec3 center(0.0f, 0.0f, 0.0f);
        const glm::vec3 eye = center + glm::vec3(std::sin(angle) * distance, distance * 0.6f, std::cos(angle) * distance);
        return rasm::Transform{eye, glm::quatLookAt(glm::normalize(center - eye), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)};
    }

    // The average time of each GPU timer over the frames, in the order they first ran.
    struct BenchTimers
    {
        std::vector<std::string> names;
        std::map<std::string, double> sums;

        void add(const rasm::FrameStats &stats)
        {
            for (uint32_t i = 0; i < stats.gpuTimerCount; ++i)
            {
                const std::string &name = stats.gpuTimers[i].name;
                if (sums.find(name) == sums.end())
                    names.push_back(name);
                sums[name] += stats.gpuTimers[i].milliseconds;
            }
            sums["GPU frame"] += stats.gpuFrameMs;
        }

        void print(const Options &options, const rasm::FrameStats &stats) const
        {
            std::printf("lights %u, %s, %s camera: %u visible, %u global, %u dropped\n", options.lights, options.allLights ? "every light per pixel" : "clustered",
                        options.ortho ? "orthographic" : "perspective", stats.lightsVisible, stats.lightsGlobal, stats.lightsDropped);
            for (const std::string &name : names)
                std::printf("  %-16s %8.3f ms\n", name.c_str(), sums.at(name) / BENCH_FRAMES);
            std::printf("  %-16s %8.3f ms\n", "GPU frame", sums.at("GPU frame") / BENCH_FRAMES);
            std::fflush(stdout);
        }
    };

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
    const Options options = parseOptions(argc, argv);
    rasm::EngineConfig config = {
        .appName = "example_11",
        .windowWidth = options.bench ? 1920 : 1280,
        .windowHeight = options.bench ? 1080 : 720,
        .enableValidation = !options.bench,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .maxLights = std::max(options.lights + 1, 1u << 14), // and the moon
        .showStats = options.inspect,
        .clusteredLighting = !options.allLights,
        .ui = true};

    rasm::Engine engine(config);
    if (!engine.running())
        return 1;
    if (options.inspect)
        engine.setInspection(options.inspection);

    // --debug-view <name> shows the scene's debug view, as the RASM window's View does.
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--debug-view") == 0)
            engine.setDebugView(parseDebugView(argv[i + 1]));
    }

    rasm::SceneHandle scene = engine.createScene();
    rasm::MeshHandle cube = createCube(engine);
    if (!cube.isValid())
        return 1;

    // The yard: a dark floor, a grid of pillars, and glass panes between them.
    const rasm::MaterialHandle ground = createColorMaterial(engine, glm::vec4(0.55f, 0.55f, 0.55f, 1.0f), 0.8f);
    const rasm::MaterialHandle stone = createColorMaterial(engine, glm::vec4(0.75f, 0.72f, 0.68f, 1.0f), 0.6f);
    const rasm::MaterialHandle glass = createColorMaterial(engine, glm::vec4(0.6f, 0.8f, 0.9f, 0.3f), 0.1f, glm::vec3(0.0f), rasm::AlphaMode::BLEND);
    engine.addEntityToScene(scene, engine.createEntity("ground", cube, ground, makeTransform(glm::vec3(0.0f, -0.25f, 0.0f), glm::vec3(YARD_SIZE + 10.0f, 0.5f, YARD_SIZE + 10.0f))));
    for (int x = 0; x < 10; ++x)
    {
        for (int z = 0; z < 10; ++z)
        {
            const glm::vec3 position(-27.0f + 6.0f * static_cast<float>(x), 1.5f, -27.0f + 6.0f * static_cast<float>(z));
            engine.addEntityToScene(scene, engine.createEntity("pillar", cube, stone, makeTransform(position, glm::vec3(0.8f, 3.0f, 0.8f))));
            if ((x + z) % 3 == 0 && x < 9)
                engine.addEntityToScene(scene, engine.createEntity("glass", cube, glass, makeTransform(position + glm::vec3(3.0f, -0.25f, 0.0f), glm::vec3(4.4f, 2.5f, 0.05f))));
        }
    }

    // Each light has a small glowing marker of its color. A quarter of them are spot lights that point down.
    rasm::MaterialHandle glows[COLOR_COUNT];
    for (uint32_t i = 0; i < COLOR_COUNT; ++i)
        glows[i] = createColorMaterial(engine, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), 1.0f, getHue(i) * 6.0f);
    const float range = std::fmin(std::fmax(3.5f * std::sqrt(1024.0f / static_cast<float>(std::max(options.lights, 1u))), 0.75f), 8.0f);
    Random random;
    std::vector<MovingLight> lights;
    for (uint32_t i = 0; i < options.lights; ++i)
    {
        MovingLight moving{.home = glm::vec3(random.range(-0.5f, 0.5f) * YARD_SIZE, random.range(0.4f, 2.0f), random.range(-0.5f, 0.5f) * YARD_SIZE),
                           .radius = random.range(0.5f, 2.5f),
                           .speed = random.range(0.005f, 0.02f) * (random.next() < 0.5f ? -1.0f : 1.0f),
                           .phase = random.range(0.0f, 6.2831853f)};
        const glm::vec3 position = getLightPosition(moving, 0);
        const bool spot = i % 4 == 3;
        moving.light = engine.createLight(spot ? rasm::LightType::SPOT : rasm::LightType::POINT, getHue(i), position, glm::vec3(0.0f, -1.0f, 0.0f),
                                          0.12f * range * range, range, glm::radians(25.0f), glm::radians(40.0f));
        if (!moving.light.isValid())
            return 1;
        moving.marker = engine.createEntity("marker", cube, glows[i % COLOR_COUNT], makeTransform(position, glm::vec3(0.1f)));
        engine.addEntityToScene(scene, moving.marker);
        engine.addLightToScene(scene, moving.light);
        lights.push_back(moving);
    }
    const rasm::LightHandle moon = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(0.5f, 0.6f, 1.0f), glm::vec3(0.0f),
                                                      glm::normalize(glm::vec3(0.4f, -1.0f, -0.6f)), 0.15f, 0.0f);
    engine.addLightToScene(scene, moon);

    rasm::CameraProjection projection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                      .perspective{.fovY = 50.0f,
                                                   .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                   .nearZ = 0.1f}};
    if (options.ortho)
        projection = rasm::CameraProjection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC,
                                            .orthographic{.left = -36.0f, .right = 36.0f, .bottom = -20.25f, .top = 20.25f, .nearZ = 0.1f, .farZ = 200.0f}};
    const float cameraDistance = 42.0f;
    rasm::CameraHandle camera = engine.createCamera(projection, getCameraTransform(0.0f, cameraDistance));
    if (!options.orbit && !options.bench)
        engine.setCameraController(camera, rasm::CameraController{
                                               .type = rasm::CameraController::Type::TRACK_BALL,
                                               .trackBall = {.radius = cameraDistance * std::sqrt(1.36f), .center = glm::vec3(0.0f), .orbitSensitivity = 1.0f},
                                               .easingSpeed = 6.0f});
    engine.addCameraToScene(scene, camera);
    if (!camera.isValid() || !moon.isValid())
        return 1;

    rasm::RenderGraph graph = engine.createRenderGraph();
    const rasm::RenderTargetHandle sceneTarget = graph.createRenderTarget({
        .name = "scene_rt",
        .swapchainScale = 1.0f,
        .colorFormats = {rasm::SCENE_COLOR_FORMAT},
        .depthFormat = rasm::Format::D32_SFLOAT,
        .clearColor = glm::vec4(0.01f, 0.012f, 0.02f, 1.0f),
    });
    // The moon is the one shadowed light; the others light without a shadow.
    rasm::SceneShadow shadow{};
    if (!options.bench)
    {
        shadow = engine.addShadowPasses(graph, scene, camera, rasm::ShadowDesc{.light = moon, .cascadeCount = 4, .size = 2048, .distance = 90.0f});
        if (shadow.mapCount == 0)
            return 1;
    }
    if (!engine.addScenePasses(graph, scene, camera, sceneTarget, options.bench ? nullptr : &shadow, options.bench ? 0 : 1) ||
        !engine.addToneMapPass(graph, sceneTarget, camera))
        return 1;
    // The RASM window (F1) over the finished image.
    engine.addUiPass(graph);
    graph.compile();

    std::printf("%u lights of range %.1f, %s. F1: stats.\n", options.lights, range, options.allLights ? "every light per pixel" : "in clusters");
    std::fflush(stdout);

    // Animation advances per frame, not per second, so a given frame renders the same on every run.
    BenchTimers bench;
    if (options.bench)
        engine.getFrameStats();
    uint32_t frame = 0;
    while (engine.running())
    {
        ++frame;
        for (const MovingLight &moving : lights)
        {
            rasm::Light light = engine.getLight(moving.light);
            light.position = getLightPosition(moving, frame);
            engine.updateLight(moving.light, light);
            engine.updateTransform(moving.marker, makeTransform(light.position, glm::vec3(0.1f)));
        }
        if (options.orbit || options.bench)
            engine.setCameraTransform(camera, getCameraTransform(glm::radians(0.3f) * static_cast<float>(frame), cameraDistance));
        graph.execute(camera);

        if (options.bench && frame > BENCH_WARMUP_FRAMES)
            bench.add(engine.getFrameStats());
        if (options.bench && frame == BENCH_WARMUP_FRAMES + BENCH_FRAMES)
        {
            bench.print(options, engine.getFrameStats());
            break;
        }
    }

    return 0;
}
