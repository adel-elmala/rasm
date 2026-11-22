#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// Streaming a world in tiles, like a map engine or a level streamer. The camera flies along a road over ground made of
// tiles. Each tile has its own mesh (a hill of a heightfield), its own material and generated texture, a few houses of
// its own house mesh, a point light, and on some tiles a fox playing a clip. Tiles ahead are created and tiles behind
// are destroyed every few frames, so thousands of tiles go through the engine, and the pools stay flat: a destroyed
// object's geometry ranges, rows, bindless slot and handle index come back once the GPU is done with them.
// - Most tiles destroy their entities before their meshes; every third tile destroys its house mesh first, so the
//   houses draw nothing until their tile goes.
// - After frame 120, every 40 tiles the fox mesh is destroyed, with the materials and textures its glTF import made,
//   and loaded again.
// - Every 120 frames it prints getPoolStats; at the end the peaks, which a long run (--frames 6000) shows are flat.
//
// --burst <folder> asks for every image (loadTexture: .png, .jpg, .ktx2) in the folder in the first frame, like a
// map engine that needs a screen of tiles at once, and quits once all are in. After the first frame it destroys every
// fourth texture, like tiles the camera left before they loaded: their loads are dropped. It prints how long that took, the
// longest frame meanwhile, the load workers and the most decoded memory waiting at once (getLoadStats), which the
// workers keep under EngineConfig::loadMemoryBudget.

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr int32_t COLUMNS = 5;    // tiles across the road
    constexpr int32_t ROWS_AHEAD = 7; // rows of tiles in front of the camera
    constexpr int32_t ROWS_BEHIND = 1;
    constexpr float TILE_SIZE = 16.0f;
    constexpr uint32_t GRID = 17; // heightfield vertices per side
    constexpr uint32_t TEXTURE_SIZE = 64;
    constexpr uint32_t HOUSES = 3;
    constexpr float SPEED = 96.0f; // world units per second: 6 rows of tiles, 30 tiles
    constexpr float FOX_SCALE = 0.012f;
    constexpr uint32_t RELOAD_FOX_EVERY = 40; // tiles

    struct Tile
    {
        int32_t row = 0;
        int32_t column = 0;
        rasm::MeshHandle ground;
        rasm::MeshHandle house;
        rasm::MaterialHandle material;
        rasm::MaterialHandle houseMaterial;
        rasm::TextureHandle texture;
        rasm::EntityHandle groundEntity;
        rasm::EntityHandle houses[HOUSES];
        rasm::EntityHandle fox;
        rasm::LightHandle light;
    };

    struct Burst
    {
        std::vector<rasm::TextureHandle> textures;
        std::chrono::steady_clock::time_point start;
        double longestFrameMs = 0.0;
        uint32_t frames = 0;
        uint32_t dropped = 0;
    };

    uint32_t hash(uint32_t value)
    {
        value ^= value >> 16;
        value *= 0x7feb352dU;
        value ^= value >> 15;
        value *= 0x846ca68bU;
        value ^= value >> 16;
        return value;
    }

    float random01(uint32_t seed)
    {
        return static_cast<float>(hash(seed) & 0xFFFFu) / 65535.0f;
    }

    uint32_t tileSeed(int32_t row, int32_t column)
    {
        return hash(static_cast<uint32_t>(row) * 7919u + static_cast<uint32_t>(column + 64) * 104729u);
    }

    // A tile's own hill: a grid with heights from a few waves, flat at the edges so neighbours meet.
    rasm::MeshHandle createGround(rasm::Engine &engine, uint32_t seed)
    {
        const float phase = random01(seed) * 6.28f;
        const float height = 0.5f + 2.0f * random01(seed + 1);
        auto heightAt = [phase, height](float u, float v)
        {
            const float edge = std::sin(u * 3.14159f) * std::sin(v * 3.14159f);
            return height * edge * (0.6f + 0.4f * std::sin(u * 9.0f + phase) * std::cos(v * 7.0f - phase));
        };
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        const float step = 1.0f / static_cast<float>(GRID - 1);
        for (uint32_t z = 0; z < GRID; ++z)
        {
            for (uint32_t x = 0; x < GRID; ++x)
            {
                const float u = static_cast<float>(x) * step;
                const float v = static_cast<float>(z) * step;
                const glm::vec3 dx(2.0f * step * TILE_SIZE, heightAt(u + step, v) - heightAt(u - step, v), 0.0f);
                const glm::vec3 dz(0.0f, heightAt(u, v + step) - heightAt(u, v - step), 2.0f * step * TILE_SIZE);
                const glm::vec2 uv(u, v);
                vertices.push_back({.pos = glm::vec3((u - 0.5f) * TILE_SIZE, heightAt(u, v), (v - 0.5f) * TILE_SIZE),
                                    .normal = glm::normalize(glm::cross(dz, dx)),
                                    .uv = {uv, uv, uv},
                                    .tangent = glm::vec4(0.0f)});
            }
        }
        for (uint32_t z = 0; z + 1 < GRID; ++z)
        {
            for (uint32_t x = 0; x + 1 < GRID; ++x)
            {
                const uint32_t i = z * GRID + x;
                indices.insert(indices.end(), {i, i + GRID, i + 1, i + 1, i + GRID, i + GRID + 1});
            }
        }
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

    // A box of its own size, so each tile's house mesh takes its own pool ranges.
    rasm::MeshHandle createHouse(rasm::Engine &engine, uint32_t seed)
    {
        const glm::vec3 size(1.0f + random01(seed) * 1.5f, 1.5f + random01(seed + 1) * 3.0f, 1.0f + random01(seed + 2) * 1.5f);
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const glm::vec3 &n : normals)
        {
            const glm::vec3 a = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 b = glm::cross(n, a);
            const auto first = static_cast<uint32_t>(vertices.size());
            for (const glm::vec2 c : {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1)})
            {
                const glm::vec3 p = (n + a * c.x + b * c.y) * 0.5f * size + glm::vec3(0.0f, size.y * 0.5f, 0.0f);
                const glm::vec2 uv = c * 0.5f + 0.5f;
                vertices.push_back({.pos = p, .normal = n, .uv = {uv, uv, uv}, .tangent = glm::vec4(0.0f)});
            }
            indices.insert(indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
        }
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

    // Grass with a stripe pattern whose color comes from the tile.
    rasm::TextureHandle createGroundTexture(rasm::Engine &engine, uint32_t seed)
    {
        std::vector<uint8_t> pixels(TEXTURE_SIZE * TEXTURE_SIZE * 4);
        const auto base = static_cast<float>(seed & 0xFF) / 255.0f;
        for (uint32_t y = 0; y < TEXTURE_SIZE; ++y)
        {
            for (uint32_t x = 0; x < TEXTURE_SIZE; ++x)
            {
                const bool stripe = ((x + y) / 8) % 2 == 0;
                uint8_t *pixel = &pixels[(y * TEXTURE_SIZE + x) * 4];
                pixel[0] = static_cast<uint8_t>(60 + 80 * base + (stripe ? 20 : 0));
                pixel[1] = static_cast<uint8_t>(110 + 60 * (1.0f - base) + (stripe ? 25 : 0));
                pixel[2] = static_cast<uint8_t>(40 + (stripe ? 15 : 0));
                pixel[3] = 255;
            }
        }
        const rasm::TextureHandle texture = engine.createTexture({.name = "Tile Texture", .width = TEXTURE_SIZE, .height = TEXTURE_SIZE, .format = rasm::Format::R8G8B8A8_SRGB, .usage = rasm::TextureUsage::SAMPLED, .mipLevels = 1});
        if (texture.isValid() && !engine.getRenderContext().fillTexture(texture, pixels.data()))
        {
            engine.destroyTexture(texture);
            return {};
        }
        return texture;
    }

    glm::vec3 tileCenter(int32_t row, int32_t column)
    {
        return {static_cast<float>(column) * TILE_SIZE, 0.0f, -static_cast<float>(row) * TILE_SIZE};
    }

    bool createTile(rasm::Engine &engine, rasm::SceneHandle scene, rasm::MeshHandle fox, int32_t foxClips, Tile &tile)
    {
        const uint32_t seed = tileSeed(tile.row, tile.column);
        const glm::vec3 center = tileCenter(tile.row, tile.column);
        const rasm::Transform at{center, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)};

        tile.ground = createGround(engine, seed);
        tile.texture = createGroundTexture(engine, seed);
        tile.material = engine.createMaterial(rasm::MaterialType::PBR, {tile.texture});
        tile.house = createHouse(engine, seed + 3);
        tile.houseMaterial = engine.createMaterial(rasm::MaterialType::PBR);
        if (!tile.ground.isValid() || !tile.texture.isValid() || !tile.material.isValid() || !tile.house.isValid() || !tile.houseMaterial.isValid())
            return false;
        rasm::Material houseValues = engine.getMaterial(tile.houseMaterial);
        houseValues.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO)] = 0.5f + 0.5f * random01(seed + 4);
        houseValues.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + 1] = 0.4f + 0.3f * random01(seed + 5);
        houseValues.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + 2] = 0.3f + 0.2f * random01(seed + 6);
        houseValues.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = 0.7f;
        engine.updateMaterial(tile.houseMaterial, houseValues);

        tile.groundEntity = engine.createEntity("ground", tile.ground, tile.material, at);
        engine.addEntityToScene(scene, tile.groundEntity);
        for (uint32_t h = 0; h < HOUSES; ++h)
        {
            const glm::vec3 offset((random01(seed + 10 + h) - 0.5f) * TILE_SIZE * 0.7f, 0.0f, (random01(seed + 20 + h) - 0.5f) * TILE_SIZE * 0.7f);
            const glm::quat turn = glm::angleAxis(random01(seed + 30 + h) * 3.14159f, glm::vec3(0.0f, 1.0f, 0.0f));
            tile.houses[h] = engine.createEntity("house", tile.house, tile.houseMaterial, rasm::Transform{center + offset, turn, glm::vec3(1.0f)});
            engine.addEntityToScene(scene, tile.houses[h]);
        }
        if (fox.isValid() && seed % 4 == 0 && tile.column != 0)
        {
            const glm::quat facing = glm::angleAxis(random01(seed + 40) * 6.28f, glm::vec3(0.0f, 1.0f, 0.0f));
            tile.fox = engine.createEntity("fox", fox, {}, rasm::Transform{center + glm::vec3(0.0f, 0.3f, 0.0f), facing, glm::vec3(FOX_SCALE)});
            engine.addEntityToScene(scene, tile.fox);
            engine.playAnimation(tile.fox, rasm::AnimationPlay{.clip = static_cast<uint32_t>(seed % static_cast<uint32_t>(std::max(foxClips, 1)))});
        }
        const glm::vec3 color(0.9f + 0.1f * random01(seed + 50), 0.7f, 0.4f + 0.3f * random01(seed + 51));
        tile.light = engine.createLight(rasm::LightType::POINT, color, center + glm::vec3(0.0f, 4.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 30.0f, 12.0f);
        engine.addLightToScene(scene, tile.light);
        return true;
    }

    // Most tiles go entities first; every third tile destroys its house mesh while the houses still use it.
    void destroyTile(rasm::Engine &engine, Tile &tile)
    {
        const bool meshFirst = tileSeed(tile.row, tile.column) % 3 == 0;
        if (meshFirst)
            engine.destroyMesh(tile.house);
        engine.destroyEntity(tile.groundEntity);
        for (rasm::EntityHandle house : tile.houses)
            engine.destroyEntity(house);
        if (tile.fox.isValid())
            engine.destroyEntity(tile.fox);
        engine.destroyLight(tile.light);
        engine.destroyMesh(tile.ground);
        if (!meshFirst)
            engine.destroyMesh(tile.house);
        engine.destroyMaterial(tile.material);
        engine.destroyMaterial(tile.houseMaterial);
        engine.destroyTexture(tile.texture);
    }

    void printStats(const char *when, const rasm::PoolStats &stats, uint64_t tiles)
    {
        std::printf("%s: %llu tiles made; alive %u entities, %u meshes, %u materials, %u lights, %u textures (%u bindless); pools: %llu vertices, %llu indices, "
                    "%llu sub-meshes, %llu instance rows, %llu animation bytes; %u frees wait for the GPU\n",
                    when, static_cast<unsigned long long>(tiles), stats.entities, stats.meshes, stats.materials, stats.lights, stats.textures, stats.bindlessTextures,
                    static_cast<unsigned long long>(stats.vertices), static_cast<unsigned long long>(stats.indices), static_cast<unsigned long long>(stats.subMeshes),
                    static_cast<unsigned long long>(stats.instances), static_cast<unsigned long long>(stats.animationPoolBytes), stats.releasesWaiting);
    }

    bool startBurst(rasm::Engine &engine, const std::string &folder, Burst &burst)
    {
        std::error_code error;
        std::vector<std::filesystem::path> files;
        for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::absolute(folder), error))
        {
            if (entry.is_regular_file())
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        burst.start = std::chrono::steady_clock::now();
        for (const std::filesystem::path &file : files)
        {
            const std::string extension = file.extension().string();
            if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".ktx2")
                burst.textures.push_back(engine.loadTexture(file.string()));
        }
        if (burst.textures.empty())
        {
            std::fprintf(stderr, "error: --burst %s: no image in the folder.\n", folder.c_str());
            return false;
        }
        return true;
    }

    // A texture whose decode fails counts as loaded too (it stays white), so the burst always ends.
    bool burstLoaded(const rasm::Engine &engine, const Burst &burst)
    {
        for (rasm::TextureHandle texture : burst.textures)
        {
            if (texture.isValid() && !engine.isLoaded(texture))
                return false;
        }
        return true;
    }

    void keepPeak(rasm::PoolStats &peak, const rasm::PoolStats &stats)
    {
        peak.entities = std::max(peak.entities, stats.entities);
        peak.meshes = std::max(peak.meshes, stats.meshes);
        peak.materials = std::max(peak.materials, stats.materials);
        peak.lights = std::max(peak.lights, stats.lights);
        peak.textures = std::max(peak.textures, stats.textures);
        peak.bindlessTextures = std::max(peak.bindlessTextures, stats.bindlessTextures);
        peak.vertices = std::max(peak.vertices, stats.vertices);
        peak.indices = std::max(peak.indices, stats.indices);
        peak.subMeshes = std::max(peak.subMeshes, stats.subMeshes);
        peak.instances = std::max(peak.instances, stats.instances);
        peak.animationPoolBytes = std::max(peak.animationPoolBytes, stats.animationPoolBytes);
        peak.releasesWaiting = std::max(peak.releasesWaiting, stats.releasesWaiting);
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    std::string burstFolder;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--burst") == 0)
            burstFolder = argv[i + 1];
    }
    rasm::EngineConfig config = {
        .appName = "example_17",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        // The pools hold a little more than one window of tiles, so a leak would fill them within a long run.
        .geometryPoolVertices = 1u << 16,
        .geometryPoolIndices = 1u << 19,
        .geometryPoolSubMeshes = 1024,
        .maxInstances = 1024,
        .maxMaterials = 256,
        .animationPoolBytes = 1u << 20};

    applyExampleArgs(args, config);
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    rasm::SceneHandle scene = engine.createScene();
    rasm::MeshHandle fox = engine.loadGLTF("models/gltf/Fox/Fox.glb");
    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 50.0f, .aspect = static_cast<float>(WIDTH) / static_cast<float>(HEIGHT), .nearZ = 0.1f}},
                                                    rasm::Transform{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
    engine.addCameraToScene(scene, camera);
    engine.addLightToScene(scene, engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.95f, 0.85f), glm::vec3(0.0f),
                                                     glm::normalize(glm::vec3(0.4f, -1.0f, -0.3f)), 2.5f, 0.0f));
    if (args.waitForLoads)
        engine.waitForLoads();
    int32_t foxClips = static_cast<int32_t>(engine.getAnimations(fox).size());

    std::deque<Tile> tiles;
    int32_t nextRow = -ROWS_BEHIND;
    uint64_t tilesMade = 0;
    uint64_t tilesAtReload = 0;
    uint32_t frame = 0;
    rasm::PoolStats peak = {};
    double seconds = 0.0;
    Burst burst;
    if (!burstFolder.empty() && !startBurst(engine, burstFolder, burst))
        return 1;
    while (exampleRunning(engine, args))
    {
        // Fixed 1/60 s per frame, so a headless run shows the same tiles at frame N every time.
        seconds += 1.0 / 60.0;
        ++frame;
        const float distance = static_cast<float>(seconds) * SPEED;
        const auto cameraRow = static_cast<int32_t>(std::floor(distance / TILE_SIZE));
        const glm::vec3 eye(0.0f, 9.0f, -distance + 14.0f);
        engine.setCameraTransform(camera, rasm::Transform{eye, glm::quatLookAt(glm::normalize(glm::vec3(0.0f, -0.45f, -1.0f)), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)});
        engine.setAnimationTime(seconds);

        // Rows behind go, rows ahead come.
        while (!tiles.empty() && tiles.front().row < cameraRow - ROWS_BEHIND)
        {
            destroyTile(engine, tiles.front());
            tiles.pop_front();
        }
        while (nextRow <= cameraRow + ROWS_AHEAD)
        {
            for (int32_t column = -COLUMNS / 2; column <= COLUMNS / 2; ++column)
            {
                Tile tile{.row = nextRow, .column = column};
                if (!createTile(engine, scene, engine.isLoaded(fox) ? fox : rasm::MeshHandle{}, foxClips, tile))
                {
                    std::fprintf(stderr, "error: tile %d,%d could not be made after %llu tiles.\n", nextRow, column, static_cast<unsigned long long>(tilesMade));
                    return 1;
                }
                tiles.push_back(tile);
                ++tilesMade;
            }
            ++nextRow;
        }

        // The fox and what its import made go, and come back as a new mesh. Foxes still on the road draw nothing.
        if (frame > 120 && tilesMade - tilesAtReload >= RELOAD_FOX_EVERY)
        {
            tilesAtReload = tilesMade;
            engine.destroyMesh(fox);
            fox = engine.loadGLTF("models/gltf/Fox/Fox.glb");
        }
        if (engine.isLoaded(fox))
            foxClips = static_cast<int32_t>(engine.getAnimations(fox).size());

        const rasm::PoolStats stats = engine.getPoolStats();
        keepPeak(peak, stats);
        if (frame % 120 == 0)
            printStats(("frame " + std::to_string(frame)).c_str(), stats, tilesMade);
        const auto frameStart = std::chrono::steady_clock::now();
        engine.render(scene, camera);
        if (burstFolder.empty())
            continue;

        const auto frameEnd = std::chrono::steady_clock::now();
        burst.longestFrameMs = std::max(burst.longestFrameMs, std::chrono::duration<double, std::milli>(frameEnd - frameStart).count());
        ++burst.frames;
        if (burst.frames == 1)
        {
            for (size_t i = 3; i < burst.textures.size(); i += 4)
            {
                engine.destroyTexture(burst.textures[i]);
                burst.textures[i] = {};
                ++burst.dropped;
            }
        }
        if (burstLoaded(engine, burst))
        {
            const rasm::LoadStats loads = engine.getLoadStats();
            std::printf("burst: %zu textures (%u dropped) loaded in %.0f ms over %u frames, longest frame %.1f ms; %u load workers, at most %.1f MiB decoded at once\n",
                        burst.textures.size() - burst.dropped, burst.dropped, std::chrono::duration<double, std::milli>(frameEnd - burst.start).count(), burst.frames,
                        burst.longestFrameMs, loads.workers, static_cast<double>(loads.peakDecodedBytes) / (1024.0 * 1024.0));
            std::fflush(stdout);
            return 0;
        }
    }

    printStats("peak", peak, tilesMade);
    return exampleExit(args);
}
