#include "glm/glm.hpp"
#include "glm/gtc/constants.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"
#include "exampleBench.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>

// Dense meshes, culled by meshlets. A hilly terrain in 16 x 16 tiles of 8192 triangles each and 2500 rocks of 20480
// triangles each: about 53 million triangles at LOD 0. The camera flies low through the valleys, so most of what is
// in front of it hides behind a hill, and every rock shows only its front half. With meshlets (EngineConfig::meshletPath,
// which this example turns on; the engine's default is off) each visible tile and rock is culled again per meshlet: by
// the frustum, by its normal cone (the back half of a rock) and by the Hi-Z. The sun casts four shadow cascades, which
// cull their meshlets the same way. The RASM window (F1) switches the path while it runs.
//
//   --meshlets off|vp   draw each tile and rock whole, or by meshlets (vertex pulling, the default here)
//   --bench             at 1920x1080, without validation and without the UI: prints the GPU time of each pass and
//                       what culling kept, averaged over the frames after the warm-up (exampleBench.h)
//   --debug-view <none|normals|uvs|overdraw|lods|culling|light-count|meshlets>
//   --no-shadows        no sun shadow: only the camera's view
//   --lod-error <px>    EngineConfig::lodErrorPixels (1); 0 draws every mesh at LOD 0, where triangles cost the most
//   --async-compute     EngineConfig::asyncCompute: culling runs on the compute queue, next to the draws

namespace
{
    constexpr int TILES = 16;
    constexpr int TILE_QUADS = 64;
    constexpr float TILE_SIZE = 150.0f;
    constexpr float TERRAIN_HALF = TILES * TILE_SIZE * 0.5f;
    constexpr uint32_t ROCK_COUNT = 2500;
    constexpr int ROCK_SUBDIVISIONS = 5; // 20 * 4^5 = 20480 triangles
    constexpr float FLIGHT_RADIUS = 700.0f;
    constexpr float FLIGHT_SPEED = 28.0f; // meters per second
    constexpr float FLIGHT_HEIGHT = 22.0f;
    constexpr double FIXED_STEP_SECONDS = 1.0 / 60.0; // the flight advances per frame, so frame N is the same on every run

    // The same rocks on every run.
    struct Random
    {
        uint32_t state = 20261123u;

        float next()
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
        }

        float range(float low, float high) { return low + (high - low) * next(); }
    };

    // Hills and valleys from a few waves: the same on every platform with the same libm.
    float terrainHeight(float x, float z)
    {
        return 70.0f * std::sin(x * 0.0041f + 1.3f) * std::cos(z * 0.0037f - 0.7f) + 28.0f * std::sin(x * 0.011f - z * 0.0072f + 2.1f) +
               9.0f * std::sin(x * 0.027f + 0.5f) * std::sin(z * 0.031f + 1.9f) + 2.5f * std::sin(x * 0.09f + z * 0.05f);
    }

    glm::vec3 terrainNormal(float x, float z)
    {
        constexpr float STEP = 0.5f;
        const float dx = terrainHeight(x + STEP, z) - terrainHeight(x - STEP, z);
        const float dz = terrainHeight(x, z + STEP) - terrainHeight(x, z - STEP);
        return glm::normalize(glm::vec3(-dx, 2.0f * STEP, -dz));
    }

    // Grass on flat ground, bare earth on slopes, lighter on the hilltops: a vertex color, so one material serves all tiles.
    glm::vec4 terrainColor(float height, const glm::vec3 &normal)
    {
        const glm::vec3 grass(0.10f, 0.22f, 0.06f);
        const glm::vec3 earth(0.24f, 0.18f, 0.12f);
        const glm::vec3 top(0.40f, 0.38f, 0.33f);
        glm::vec3 color = glm::mix(grass, earth, glm::smoothstep(0.88f, 0.75f, normal.y));
        color = glm::mix(color, top, glm::smoothstep(60.0f, 95.0f, height));
        return glm::vec4(color, 1.0f);
    }

    // One tile of the terrain, its vertices shared, so meshoptimizer can build its LODs and meshlets. Neighbouring tiles
    // share their edge vertices' positions, and the LODs keep the borders, so they meet at any LOD.
    rasm::MeshHandle createTile(rasm::Engine &engine, int tileX, int tileZ)
    {
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> colors;
        const float x0 = -TERRAIN_HALF + static_cast<float>(tileX) * TILE_SIZE;
        const float z0 = -TERRAIN_HALF + static_cast<float>(tileZ) * TILE_SIZE;
        for (int z = 0; z <= TILE_QUADS; ++z)
        {
            for (int x = 0; x <= TILE_QUADS; ++x)
            {
                const float wx = x0 + static_cast<float>(x) * TILE_SIZE / TILE_QUADS;
                const float wz = z0 + static_cast<float>(z) * TILE_SIZE / TILE_QUADS;
                const float height = terrainHeight(wx, wz);
                const glm::vec3 normal = terrainNormal(wx, wz);
                const glm::vec2 uv(wx * 0.1f, wz * 0.1f);
                vertices.push_back({.pos = glm::vec3(wx, height, wz), .normal = normal, .uv = {uv, uv, uv}, .tangent = glm::vec4(0.0f)});
                colors.push_back(rasm::packVertexColor(terrainColor(height, normal)));
            }
        }
        std::vector<uint32_t> indices;
        const auto row = static_cast<uint32_t>(TILE_QUADS + 1);
        for (uint32_t z = 0; z < TILE_QUADS; ++z)
        {
            for (uint32_t x = 0; x < TILE_QUADS; ++x)
            {
                const uint32_t a = z * row + x;
                // Counter-clockwise seen from above.
                for (uint32_t index : {a, a + row, a + 1, a + 1, a + row, a + row + 1})
                    indices.push_back(index);
            }
        }
        return engine.createMesh(std::move(vertices), std::move(indices), std::move(colors));
    }

    uint32_t midpoint(std::vector<glm::vec3> &points, std::unordered_map<uint64_t, uint32_t> &cache, uint32_t a, uint32_t b)
    {
        const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
        if (auto it = cache.find(key); it != cache.end())
            return it->second;
        points.push_back(glm::normalize(points[a] + points[b]));
        const auto index = static_cast<uint32_t>(points.size() - 1);
        cache.emplace(key, index);
        return index;
    }

    // A lumpy subdivided icosahedron: a rock of 20480 triangles.
    rasm::MeshHandle createRockMesh(rasm::Engine &engine)
    {
        const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
        std::vector<glm::vec3> points = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
        for (auto &point : points)
            point = glm::normalize(point);
        std::vector<uint32_t> triangles = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                                           3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
        std::unordered_map<uint64_t, uint32_t> cache;
        for (int level = 0; level < ROCK_SUBDIVISIONS; ++level)
        {
            std::vector<uint32_t> finer;
            finer.reserve(triangles.size() * 4);
            for (size_t i = 0; i < triangles.size(); i += 3)
            {
                const uint32_t a = triangles[i];
                const uint32_t b = triangles[i + 1];
                const uint32_t c = triangles[i + 2];
                const uint32_t ab = midpoint(points, cache, a, b);
                const uint32_t bc = midpoint(points, cache, b, c);
                const uint32_t ca = midpoint(points, cache, c, a);
                for (uint32_t index : {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca})
                    finer.push_back(index);
            }
            triangles = std::move(finer);
        }

        std::vector<rasm::Vertex> vertices(points.size());
        for (size_t i = 0; i < points.size(); ++i)
        {
            const glm::vec3 &p = points[i];
            const float lumps = 0.18f * std::sin(3.1f * p.x + 0.7f) * std::sin(2.9f * p.y + 1.4f) * std::sin(3.3f * p.z + 2.2f) +
                                0.06f * std::sin(9.0f * p.x + 1.3f) * std::sin(8.0f * p.z + 0.1f) + 0.03f * std::sin(17.0f * p.y + 0.9f);
            vertices[i] = {.pos = p * (1.0f + lumps), .normal = glm::vec3(0.0f), .uv = {}, .tangent = glm::vec4(0.0f)};
        }
        // The icosahedron's faces do not all wind the same way, so each triangle is turned to face outward.
        for (size_t i = 0; i < triangles.size(); i += 3)
        {
            const glm::vec3 &a = vertices[triangles[i]].pos;
            const glm::vec3 &b = vertices[triangles[i + 1]].pos;
            const glm::vec3 &c = vertices[triangles[i + 2]].pos;
            glm::vec3 faceNormal = glm::cross(b - a, c - a);
            if (glm::dot(faceNormal, a + b + c) < 0.0f)
            {
                std::swap(triangles[i + 1], triangles[i + 2]);
                faceNormal = -faceNormal;
            }
            for (size_t corner = 0; corner < 3; ++corner)
                vertices[triangles[i + corner]].normal += faceNormal;
        }
        for (auto &vertex : vertices)
            vertex.normal = glm::normalize(vertex.normal);
        return engine.createMesh(std::move(vertices), std::move(triangles));
    }

    rasm::MaterialHandle createColorMaterial(rasm::Engine &engine, const glm::vec3 &color, float roughness)
    {
        const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::PBR);
        rasm::Material values = engine.getMaterial(material);
        for (uint32_t i = 0; i < 3; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[i];
        values.params[static_cast<uint32_t>(rasm::PbrParam::METALLIC)] = 0.0f;
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        engine.updateMaterial(material, values);
        return material;
    }

    // The camera's place on its loop through the valleys at a time: above the ground ahead of it, looking along the
    // loop and a little down.
    rasm::Transform flightTransform(double seconds)
    {
        const float angle = static_cast<float>(std::fmod(seconds * FLIGHT_SPEED / FLIGHT_RADIUS, glm::two_pi<double>()));
        auto pathPoint = [](float a)
        { return glm::vec3(std::cos(a) * FLIGHT_RADIUS, 0.0f, std::sin(a) * FLIGHT_RADIUS * 0.8f); };
        glm::vec3 position = pathPoint(angle);
        float ground = terrainHeight(position.x, position.z);
        for (int i = 1; i <= 4; ++i)
        {
            const glm::vec3 ahead = pathPoint(angle + static_cast<float>(i) * 0.012f);
            ground = std::max(ground, terrainHeight(ahead.x, ahead.z) - 4.0f * static_cast<float>(i));
        }
        position.y = ground + FLIGHT_HEIGHT;
        const glm::vec3 ahead = pathPoint(angle + 0.05f);
        const glm::vec3 forward = glm::normalize(glm::vec3(ahead.x - position.x, -0.06f * glm::length(ahead - position), ahead.z - position.z));
        const float yaw = std::atan2(-forward.x, -forward.z);
        const float pitch = std::asin(glm::clamp(forward.y, -1.0f, 1.0f));
        const glm::quat rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(pitch, glm::vec3(1.0f, 0.0f, 0.0f));
        return rasm::Transform{position, glm::normalize(rotation), glm::vec3(1.0f)};
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
    rasm::EngineConfig config = {
        .appName = "rasm meshlets",
        .windowWidth = 1280,
        .windowHeight = 720,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .geometryPoolColors = 1u << 21, // the terrain's vertex colors: 1.1M
        .meshletPath = rasm::MeshletPath::VERTEX_PULLING,
        .ui = true};
    applyExampleArgs(args, config);
    applyMeshletFlag(argc, argv, config);
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--lod-error") == 0)
            config.lodErrorPixels = static_cast<float>(std::atof(argv[i + 1]));
        if (std::strcmp(argv[i], "--async-compute") == 0)
            config.asyncCompute = true;
    }
    ExampleBench bench{.on = hasBenchFlag(argc, argv)};
    if (bench.on)
    {
        config.enableValidation = false;
        config.ui = false;
        config.showStats = false;
        config.windowWidth = 1920;
        config.windowHeight = 1080;
    }
    bool shadows = true;
    for (int i = 1; i < argc; ++i)
        shadows = shadows && std::strcmp(argv[i], "--no-shadows") != 0;

    rasm::Engine engine(config);
    if (!engine.running())
        return 1;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--debug-view") == 0)
            engine.setDebugView(parseDebugView(argv[i + 1]));
    }

    const rasm::SceneHandle scene = engine.createScene();
    const rasm::MaterialHandle ground = createColorMaterial(engine, glm::vec3(1.0f), 0.95f);
    const rasm::MaterialHandle stone = createColorMaterial(engine, glm::vec3(0.32f, 0.30f, 0.28f), 0.85f);
    for (int z = 0; z < TILES; ++z)
    {
        for (int x = 0; x < TILES; ++x)
        {
            const rasm::MeshHandle tile = createTile(engine, x, z);
            if (!tile.isValid())
                return 1;
            engine.addEntityToScene(scene, engine.createEntity("tile", tile, ground));
        }
    }

    // Rocks of every size, squashed and turned, half sunk into the ground; none on the flight path.
    const rasm::MeshHandle rock = createRockMesh(engine);
    if (!rock.isValid())
        return 1;
    Random random;
    for (uint32_t placed = 0; placed < ROCK_COUNT;)
    {
        const float x = random.range(-TERRAIN_HALF + 20.0f, TERRAIN_HALF - 20.0f);
        const float z = random.range(-TERRAIN_HALF + 20.0f, TERRAIN_HALF - 20.0f);
        const float size = random.range(2.0f, 9.0f);
        const glm::vec3 scale(size * random.range(0.8f, 1.6f), size * random.range(0.5f, 1.0f), size * random.range(0.8f, 1.6f));
        const float yaw = random.range(0.0f, glm::two_pi<float>());
        const float flightDistance = std::abs(std::sqrt(x * x + z * z / 0.64f) - FLIGHT_RADIUS);
        if (flightDistance < 25.0f)
            continue;
        const rasm::Transform transform{glm::vec3(x, terrainHeight(x, z) + 0.2f * scale.y, z), glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)), scale};
        engine.addEntityToScene(scene, engine.createEntity("rock", rock, stone, transform));
        ++placed;
    }

    const rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                                 .perspective{.fovY = 60.0f,
                                                                                              .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                              .nearZ = 0.3f}},
                                                          flightTransform(0.0));
    engine.addCameraToScene(scene, camera);
    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.93f, 0.82f), glm::vec3(0.0f), glm::normalize(glm::vec3(-0.45f, -0.6f, -0.3f)), 3.5f, 0.0f);
    engine.addLightToScene(scene, sun);
    if (!camera.isValid() || !sun.isValid())
        return 1;

    rasm::RenderGraph graph = engine.createRenderGraph();
    const rasm::RenderTargetHandle sceneTarget = graph.createRenderTarget({
        .name = "scene_rt",
        .swapchainScale = 1.0f,
        .colorFormats = {rasm::SCENE_COLOR_FORMAT},
        .depthFormat = rasm::Format::D32_SFLOAT,
        .clearColor = glm::vec4(0.45f, 0.6f, 0.85f, 1.0f),
    });
    rasm::SceneShadow shadow = {};
    if (shadows)
    {
        shadow = engine.addShadowPasses(graph, scene, camera, rasm::ShadowDesc{.light = sun, .cascadeCount = 4, .size = 2048, .distance = 500.0f, .splitLambda = 0.8f});
        if (shadow.mapCount == 0)
            return 1;
    }
    if (!engine.addScenePasses(graph, scene, camera, sceneTarget, shadows ? &shadow : nullptr, shadows ? 1u : 0u) || !engine.addToneMapPass(graph, sceneTarget, camera))
        return 1;
    engine.addUiPass(graph);
    graph.compile();

    std::printf("rasm meshlets: %d terrain tiles and %u rocks. F1: stats.\n", TILES * TILES, ROCK_COUNT);
    std::fflush(stdout);
    if (args.waitForLoads)
        engine.waitForLoads();

    double seconds = 0.0;
    while (exampleRunning(engine, args))
    {
        engine.setCameraTransform(camera, flightTransform(seconds));
        graph.execute(camera);
        if (bench.on)
            addBenchFrame(bench, engine.getFrameStats());
        seconds += FIXED_STEP_SECONDS;
    }
    printBench(bench, config);
    return exampleExit(args);
}
