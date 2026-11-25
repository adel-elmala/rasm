// Example 23: a scene 6370 km from the origin, on the Earth's surface in Earth-centred coordinates (ECEF, metres).
//
// Positions are doubles on the CPU (Transform::position, Light::position). Each view draws in its camera's render space:
// the world moved so that the camera is at the origin, where floats are precise again. The GPU rows keep each position
// as two floats, a high and a low part, and every view subtracts its own origin from them, so moving the camera
// rewrites no row. Near the camera the scene is as sharp as at the origin: no jitter, no z-fighting.
//
// The scene is built in a local frame, metres east, up and south of a place (ENU, with RASM's Y up), and placed at
// --origin turned so that its up is the ellipsoid's normal there: a map engine's setup. It has sun cascades, a spot and a
// point light with shadows, local lights in the light clusters, skinned characters, an animated node, a grid of
// pillars for culling, and TAA. The camera flies past it.
//
// Options:
//   --origin x,y,z       where the scene is, in metres (default 4197000,168000,4786000: Paris, on the ellipsoid)
//   --compare            the same scene twice, side by side: at the world's origin (left) and moved to --origin by a
//                        pure translation (right), with the same camera path. Headless, it compares the two halves of
//                        the last frame and fails when more than 0.1% of their pixels differ by more than 8 levels.
//   --pillars N          N pillars in a grid (default 400): more entities for culling and for the CPU's numbers
//   --debug-view <name>  a debug view, as the RASM window's View does (culling: what the early and late passes drew)
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
    constexpr uint32_t LOCAL_LIGHTS = 24;
    constexpr uint32_t COMPARE_THRESHOLD = 8;
    constexpr double COMPARE_FRACTION = 0.001;
    constexpr glm::dvec3 PARIS(4197000.0, 168000.0, 4786000.0);
    // WGS84
    constexpr double EQUATOR_RADIUS = 6378137.0;
    constexpr double POLAR_RADIUS = 6356752.314245;

    // Local to world: world = origin + rotation * local.
    struct Placement
    {
        glm::dvec3 origin = glm::dvec3(0.0);
        glm::dquat rotation = glm::dquat(1.0, 0.0, 0.0, 0.0);
    };

    struct Meshes
    {
        rasm::MeshHandle cube;
        rasm::MeshHandle man;
        rasm::MeshHandle fox;
        rasm::MeshHandle box;
    };

    struct Content
    {
        rasm::SceneHandle scene;
        rasm::CameraHandle camera;
        rasm::LightHandle sun;
        rasm::LightHandle spot;
        rasm::LightHandle lamp;
        Placement placement;
    };

    // The frame whose X is east, Y up (the ellipsoid's normal) and Z south, at a point near the surface. Near the
    // world's origin there is no surface: the frame is the world's.
    glm::dquat getEnuRotation(const glm::dvec3 &position)
    {
        if (glm::length(position) < 1000.0)
            return glm::dquat(1.0, 0.0, 0.0, 0.0);
        const glm::dvec3 up = glm::normalize(glm::dvec3(position.x / (EQUATOR_RADIUS * EQUATOR_RADIUS), position.y / (EQUATOR_RADIUS * EQUATOR_RADIUS),
                                                        position.z / (POLAR_RADIUS * POLAR_RADIUS)));
        const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up));
        const glm::dvec3 south = glm::cross(east, up);
        return glm::quat_cast(glm::dmat3(east, up, south));
    }

    rasm::Transform place(const Placement &placement, const glm::vec3 &position, const glm::quat &rotation, const glm::vec3 &scale)
    {
        return rasm::Transform{placement.origin + placement.rotation * glm::dvec3(position), glm::quat(placement.rotation) * rotation, scale};
    }

    rasm::Transform place(const Placement &placement, const glm::vec3 &position, float yaw, float scale)
    {
        return place(placement, position, glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(scale));
    }

    glm::vec3 placeDirection(const Placement &placement, const glm::vec3 &direction)
    {
        return glm::vec3(placement.rotation * glm::dvec3(direction));
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

    void addEntity(rasm::Engine &engine, rasm::SceneHandle scene, const char *name, rasm::MeshHandle mesh, rasm::MaterialHandle material, const rasm::Transform &transform,
                   int32_t clip)
    {
        const rasm::EntityHandle entity = engine.createEntity(name, mesh, material, transform);
        engine.addEntityToScene(scene, entity);
        if (clip >= 0)
            engine.playAnimation(entity, rasm::AnimationPlay{.clip = static_cast<uint32_t>(clip)});
    }

    // Where the camera is in the local frame at a time: it flies east past the scene and looks at its middle.
    rasm::Transform getCameraPose(const Placement &placement, double seconds)
    {
        const auto t = static_cast<float>(seconds);
        const glm::vec3 eye(-9.0f + 1.8f * t, 2.4f + 0.4f * std::sin(0.7f * t), 9.0f - 0.6f * t);
        const glm::vec3 target(0.5f * t - 1.0f, 0.8f, 0.0f);
        const glm::quat look = glm::quatLookAt(glm::normalize(target - eye), glm::vec3(0.0f, 1.0f, 0.0f));
        return place(placement, eye, look, glm::vec3(1.0f));
    }

    // The scene in its local frame, placed with placement.
    bool buildScene(rasm::Engine &engine, const Meshes &meshes, const Placement &placement, uint32_t pillars, int32_t foxRun, float aspect, Content &content)
    {
        content.placement = placement;
        content.scene = engine.createScene();
        const rasm::MaterialHandle ground = createColorMaterial(engine, glm::vec3(0.45f, 0.5f, 0.42f), 0.9f);
        const rasm::MaterialHandle stone = createColorMaterial(engine, glm::vec3(0.75f, 0.72f, 0.68f), 0.6f);
        addEntity(engine, content.scene, "ground", meshes.cube, ground, place(placement, glm::vec3(0.0f, -0.05f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(120.0f, 0.1f, 120.0f)), -1);

        const auto side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(pillars))));
        for (uint32_t i = 0; i < pillars; ++i)
        {
            const uint32_t row = i / side;
            const float x = (static_cast<float>(i % side) - 0.5f * static_cast<float>(side - 1)) * 2.5f;
            const float z = -6.0f - static_cast<float>(row) * 2.5f;
            const float height = 0.6f + 0.5f * static_cast<float>((i * 7) % 5);
            addEntity(engine, content.scene, "pillar", meshes.cube, stone, place(placement, glm::vec3(x, 0.5f * height, z), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.4f, height, 0.4f)), -1);
        }

        addEntity(engine, content.scene, "CesiumMan", meshes.man, {}, place(placement, glm::vec3(-1.0f, 0.0f, 1.5f), 0.6f, 1.0f), 0);
        addEntity(engine, content.scene, "Fox", meshes.fox, {}, place(placement, glm::vec3(1.2f, 0.0f, 0.5f), -0.8f, 0.012f), foxRun);
        addEntity(engine, content.scene, "BoxAnimated", meshes.box, {}, place(placement, glm::vec3(3.5f, 0.0f, 2.0f), 0.3f, 0.6f), 0);

        content.sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.92f, 0.82f), placement.origin, placeDirection(placement, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.55f))), 2.5f, 0.0f);
        content.spot = engine.createLight(rasm::LightType::SPOT, glm::vec3(1.0f, 0.85f, 0.6f), place(placement, glm::vec3(4.0f, 5.0f, 3.0f), 0.0f, 1.0f).position,
                                          placeDirection(placement, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f))), 120.0f, 14.0f, 0.3f, 0.5f);
        content.lamp = engine.createLight(rasm::LightType::POINT, glm::vec3(0.5f, 0.7f, 1.0f), place(placement, glm::vec3(-3.5f, 1.6f, -2.5f), 0.0f, 1.0f).position,
                                          glm::vec3(0.0f, -1.0f, 0.0f), 40.0f, 10.0f);
        for (rasm::LightHandle light : {content.sun, content.spot, content.lamp})
            engine.addLightToScene(content.scene, light);
        for (uint32_t i = 0; i < LOCAL_LIGHTS; ++i)
        {
            const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(LOCAL_LIGHTS);
            const glm::vec3 color(0.5f + 0.5f * std::cos(angle), 0.5f + 0.5f * std::cos(angle + 2.1f), 0.5f + 0.5f * std::cos(angle + 4.2f));
            const glm::vec3 position(9.0f * std::cos(angle), 0.4f, -14.0f + 9.0f * std::sin(angle));
            const rasm::LightHandle light = engine.createLight(rasm::LightType::POINT, color, place(placement, position, 0.0f, 1.0f).position, glm::vec3(0.0f, -1.0f, 0.0f), 6.0f, 3.0f);
            engine.addLightToScene(content.scene, light);
        }

        content.camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = 50.0f, .aspect = aspect, .nearZ = 0.05f}},
                                             getCameraPose(placement, 0.0));
        engine.setCameraController(content.camera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
        engine.addCameraToScene(content.scene, content.camera);
        return content.scene.isValid() && content.camera.isValid() && content.sun.isValid() && content.spot.isValid() && content.lamp.isValid();
    }

    // The sun's cascades, the spot's map and the lamp's cube, the shaded view, TAA and the tone map into output.
    bool addViewPasses(rasm::Engine &engine, rasm::RenderGraph &graph, const Content &content, uint32_t width, uint32_t height, const rasm::ViewRect &output)
    {
        const rasm::RenderTargetHandle target = graph.createRenderTarget({
            .name = "scene_hdr_" + std::to_string(content.camera.index),
            .width = width,
            .height = height,
            .colorFormats = {rasm::SCENE_COLOR_FORMAT, rasm::MOTION_FORMAT},
            .depthFormat = rasm::Format::D32_SFLOAT,
            .clearColor = glm::vec4(0.32f, 0.45f, 0.68f, 1.0f),
        });
        const rasm::SceneShadow shadows[3] = {
            engine.addShadowPasses(graph, content.scene, content.camera, rasm::ShadowDesc{.light = content.sun, .cascadeCount = 4, .size = 2048, .distance = 40.0f}),
            engine.addShadowPasses(graph, content.scene, content.camera, rasm::ShadowDesc{.light = content.spot, .size = 1024, .distance = 20.0f}),
            engine.addShadowPasses(graph, content.scene, content.camera, rasm::ShadowDesc{.light = content.lamp, .size = 512, .distance = 12.0f}),
        };
        for (const rasm::SceneShadow &shadow : shadows)
        {
            if (shadow.mapCount == 0)
                return false;
        }
        if (!engine.addScenePasses(graph, content.scene, content.camera, target, shadows, 3))
            return false;
        const rasm::RenderTargetHandle resolved = engine.addTaaPass(graph, target, content.camera);
        return resolved.isValid() && engine.addToneMapPass(graph, resolved, content.camera, {}, rasm::ViewOutput{.rect = output});
    }

    bool parseOrigin(const char *text, glm::dvec3 &origin)
    {
        char *end = nullptr;
        for (int i = 0; i < 3; ++i)
        {
            origin[i] = std::strtod(text, &end);
            if (end == text || (i < 2 && *end != ','))
                return false;
            text = end + 1;
        }
        return *end == '\0';
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

    // The left half of the image against the right half: the scene at the origin and the same scene far away.
    bool compareHalves(const char *path)
    {
        rasm::ReadbackImage image;
        if (!rasm::readPng(path, image))
        {
            std::fprintf(stderr, "error: cannot read %s.\n", path);
            return false;
        }
        const uint32_t half = image.width / 2;
        uint64_t differing = 0;
        uint32_t maxDiff = 0;
        for (uint32_t y = 0; y < image.height; ++y)
        {
            for (uint32_t x = 0; x < half; ++x)
            {
                const uint8_t *left = &image.pixels[(static_cast<size_t>(y) * image.width + x) * 4];
                const uint8_t *right = left + static_cast<size_t>(half) * 4;
                uint32_t pixelDiff = 0;
                for (uint32_t c = 0; c < 3; ++c)
                    pixelDiff = std::max(pixelDiff, static_cast<uint32_t>(std::abs(static_cast<int>(left[c]) - static_cast<int>(right[c]))));
                maxDiff = std::max(maxDiff, pixelDiff);
                differing += pixelDiff > COMPARE_THRESHOLD ? 1 : 0;
            }
        }
        const double fraction = static_cast<double>(differing) / static_cast<double>(static_cast<uint64_t>(half) * image.height);
        std::printf("Origin vs far: %.4f%% of the pixels differ by more than %u levels, the largest difference is %u.\n", 100.0 * fraction, COMPARE_THRESHOLD, maxDiff);
        return fraction <= COMPARE_FRACTION;
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    glm::dvec3 origin = PARIS;
    bool compare = false;
    uint32_t pillars = 400;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--origin") == 0 && i + 1 < argc)
        {
            ++i;
            if (!parseOrigin(argv[i], origin))
            {
                std::fprintf(stderr, "error: --origin takes x,y,z in metres.\n");
                return 1;
            }
        }
        if (std::strcmp(argv[i], "--pillars") == 0 && i + 1 < argc)
            pillars = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        compare = compare || std::strcmp(argv[i], "--compare") == 0;
    }

    rasm::EngineConfig config = {
        .appName = "example_23",
        .windowWidth = 1280,
        .windowHeight = 720,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .ui = !compare};
    applyExampleArgs(args, config);
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--debug-view") == 0)
            engine.setDebugView(parseDebugView(argv[i + 1]));
    }

    const Meshes meshes = {.cube = createCube(engine),
                           .man = engine.loadGLTF("models/gltf/CesiumMan/CesiumMan.glb"),
                           .fox = engine.loadGLTF("models/gltf/Fox/Fox.glb"),
                           .box = engine.loadGLTF("models/gltf/BoxAnimated/BoxAnimated.glb")};
    // The clips' names are known once the meshes are loaded.
    engine.waitForLoads();
    const int32_t foxRun = engine.findAnimation(meshes.fox, "Run");
    if (!engine.isLoaded(meshes.man) || !engine.isLoaded(meshes.fox) || !engine.isLoaded(meshes.box) || foxRun < 0)
        return 1;

    // --compare: the scene at the world's origin on the left, the same scene moved by a pure translation on the right.
    // Otherwise: one scene at the origin's place, turned to its local east, up and south.
    const uint32_t width = compare ? config.windowWidth / 2 : static_cast<uint32_t>(config.windowWidth);
    const auto aspect = static_cast<float>(width) / static_cast<float>(config.windowHeight);
    std::vector<Content> contents(compare ? 2 : 1);
    const Placement placements[2] = {compare ? Placement{} : Placement{.origin = origin, .rotation = getEnuRotation(origin)}, Placement{.origin = origin}};
    for (size_t i = 0; i < contents.size(); ++i)
    {
        if (!buildScene(engine, meshes, placements[i], pillars, foxRun, aspect, contents[i]))
            return 1;
    }

    rasm::RenderGraph graph = engine.createRenderGraph();
    for (size_t i = 0; i < contents.size(); ++i)
    {
        const rasm::ViewRect rect = compare ? rasm::ViewRect{.x = 0.5f * static_cast<float>(i), .width = 0.5f} : rasm::ViewRect{};
        if (!addViewPasses(engine, graph, contents[i], width, static_cast<uint32_t>(config.windowHeight), rect))
            return 1;
    }
    if (!compare)
        engine.addUiPass(graph);

    if (args.waitForLoads)
        engine.waitForLoads();

    double seconds = 0.0;
    auto lastTime = std::chrono::steady_clock::now();
    while (exampleRunning(engine, args))
    {
        const auto now = std::chrono::steady_clock::now();
        seconds += args.fixedStep ? FIXED_STEP_SECONDS : std::chrono::duration<double>(now - lastTime).count();
        lastTime = now;
        engine.setAnimationTime(seconds);
        for (const Content &content : contents)
            engine.setCameraTransform(content.camera, getCameraPose(content.placement, seconds));
        graph.execute(contents[0].camera);
    }

    if (compare && args.headless && args.written && !compareHalves(args.out))
        return 1;
    return exampleExit(args);
}
