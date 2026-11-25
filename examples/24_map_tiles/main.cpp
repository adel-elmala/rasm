#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// A map engine's terrain, streamed under a GPU memory budget by rasm::TileStreamer. The world is a quadtree of tiles
// over 32 x 32 km: level 0 is one tile for all of it, level 7 tiles are 256 m wide. Each tile has its own heights (a
// 35 x 35 R16_UNORM texture: 32 x 32 cells and a border of one sample, which the GPU turns into terrain on a grid all
// tiles share, Renderer::createTerrainTile) and its own 128 x 128 image (with mips), whose color shows its level and
// whose dark edge shows its border. A skirt under each tile's edges hides the cracks between levels.
// - The application owns the tree. Each frame it walks it from the root and refines a tile whose error on screen is over
//   REFINE_PIXELS (getScreenError, the most over both views), and merges it again under MERGE_PIXELS, a little lower,
//   so a tile at the edge does not refine and merge on every frame. The tiles it stops at are the cut, which it
//   passes to TileStreamer::update.
// - The streamer decides what loads (the largest error first, so parents first), what is dropped to stay in the budget
//   (tiles nobody wants, the oldest first), and what is drawn: a tile once it is ready, else its parent in its place.
// - A tile the streamer hands out is asked from a server, a thread of the example's that makes its heights and image; it
//   arrives FETCH_FRAMES to FETCH_FRAMES + 5 frames later, and the main thread only uploads it then.
// - Six tiles of level LANDMARK_LEVEL also have a glTF model, loaded with Engine::loadGLTF, under an empty child tile
//   with ADD refinement: the streamer counts the model's tile ready only once its mesh and textures are in, destroying
//   it cancels its load, and the terrain around it refines without waiting for it.
// - Two views: the camera, which flies low, climbs to 6 km and dives again, and a map of the whole world in the corner,
//   a top-down orthographic view with a red box over the camera.
// Every frame it checks that the tiles hold no more than the budget, and after the first frames that no wanted tile is
// a hole; at the end it prints the numbers, and a headless run that broke a check exits with 1.
//   --budget <MiB>   the byte budget (48 MiB); 0: the automatic one, 80% of what the device's budget leaves
//   --fast           the fetches arrive at once, and every tile wanted is handed out in the same frame (as many as the limits let)

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr double WORLD_SIZE = 32768.0;
    constexpr int32_t MAX_LEVEL = 7;
    constexpr uint32_t CELLS = 32;                // a tile's grid: 2048 triangles, drawn on getTerrainMesh's shared grid
    constexpr uint32_t HEIGHTS_WIDTH = CELLS + 3; // the grid's points and a border of one sample around them
    constexpr double HEIGHT_STEP = 1.0 / 16.0;    // metres per R16_UNORM step: a power of two, so neighbours' edges are bit-equal
    constexpr uint32_t IMAGE_SIZE = 128;
    constexpr double SKIRT = 0.05; // of a tile's width: the skirt hides the cracks between levels
    constexpr double REFINE_PIXELS = 24.0;
    constexpr double MERGE_PIXELS = 0.8 * REFINE_PIXELS;
    constexpr uint32_t FETCH_FRAMES = 3;
    constexpr uint32_t SERVER_THREADS = 3;
    constexpr int32_t LANDMARK_LEVEL = 4;
    constexpr uint32_t FORGET_AFTER = 600; // frames a node is not visited before its tile record goes
    constexpr uint32_t HOLE_CHECK_AFTER = 30;
    // What the streamer may count on before it has measured a tile: a terrain tile's heights and image with its mips (the
    // grid is shared, and no tile's); a glTF model, a guess.
    constexpr uint64_t TERRAIN_TILE_BYTES = HEIGHTS_WIDTH * HEIGHTS_WIDTH * sizeof(uint16_t) + IMAGE_SIZE * IMAGE_SIZE * 4 * 4 / 3;
    constexpr uint64_t LANDMARK_BYTES = 16ull << 20;

    struct Landmark
    {
        const char *path;
        double x;
        double z;
        float scale;
    };

    // Along the flight's first minute, and one off it whose textures (107 MiB) are more than the budget: it waits.
    constexpr Landmark LANDMARKS[] = {
        {"models/gltf/Fox/Fox.glb", 27300.0, 17600.0, 1.2f},
        {"models/gltf/CesiumMan/CesiumMan.glb", 26500.0, 20500.0, 90.0f},
        {"models/gltf/LanternQuantized/Lantern.gltf", 24900.0, 23200.0, 4.0f},
        {"models/gltf/AnimatedCube/AnimatedCube.gltf", 21300.0, 25400.0, 40.0f},
        {"models/gltf/BoxAnimated/BoxAnimated.gltf", 17800.0, 26100.0, 40.0f},
        {"models/gltf/DamagedHelmet/DamagedHelmet.glb", 6500.0, 8200.0, 60.0f},
    };
    constexpr uint32_t LANDMARK_COUNT = sizeof(LANDMARKS) / sizeof(LANDMARKS[0]);

    struct Node
    {
        int32_t level = 0;
        int32_t x = 0;
        int32_t y = 0;
        int32_t landmark = -1; // a landmark tile or its group: its index in LANDMARKS
        bool group = false;    // the landmark's empty ADD parent
        rasm::TileHandle tile;
        bool refined = false;
        uint32_t lastVisit = 0;
    };

    // A terrain tile's data as a server sends it: made on the server's thread, uploaded on the main thread.
    struct TileData
    {
        int32_t level = 0;
        int32_t x = 0;
        int32_t y = 0;
        std::vector<uint16_t> heights; // HEIGHTS_WIDTH^2 steps of HEIGHT_STEP above heightOffset
        float heightOffset = 0.0f;
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
        std::vector<uint8_t> pixels; // every mip, one after another
        std::vector<size_t> offsets; // where each mip starts
        bool done = false;
    };

    struct Fetch
    {
        rasm::TileHandle tile;
        uint64_t key = 0;
        uint32_t arrival = 0;
        std::shared_ptr<TileData> data; // a terrain tile's; a glTF tile has none
    };

    struct Totals
    {
        uint32_t overBudget = 0;
        uint32_t holes = 0;
        uint32_t maxFallbacks = 0;
        uint32_t maxWaiting = 0;
        uint32_t maxDrawn = 0;
        uint32_t landmarksLoaded = 0;
        uint32_t landmarksCancelled = 0;
        double updateMs = 0.0;
        double maxUpdateMs = 0.0;
        double maxFrameMs = 0.0;
    };

    // The application's tree: a node per place it visited, made with its tile record on the first visit.
    struct Map
    {
        std::unordered_map<uint64_t, Node> nodes;
        std::unordered_map<uint64_t, uint64_t> nodeOfTile; // tile handle index -> node key
        std::vector<rasm::TileHandle> cut;
        uint32_t frame = 0;
        uint64_t tilesMade = 0;
    };

    double terrainHeight(double x, double z)
    {
        return 900.0 * std::sin(x / 5200.0) * std::cos(z / 4300.0) + 350.0 * std::sin(x / 1700.0 + z / 2300.0) + 90.0 * std::sin(x / 430.0) * std::sin(z / 370.0) +
               25.0 * std::cos(x / 97.0 + z / 131.0);
    }

    uint64_t nodeKey(int32_t level, int32_t x, int32_t y, int32_t landmark = -1)
    {
        return (static_cast<uint64_t>(landmark + 1) << 56) | (static_cast<uint64_t>(level) << 48) | (static_cast<uint64_t>(x) << 24) | static_cast<uint64_t>(y);
    }

    double tileSize(int32_t level)
    {
        return WORLD_SIZE / static_cast<double>(1 << level);
    }

    // The bounding sphere from the heights at 9 x 9 points, and the error: a tile's mesh misses the next level's detail
    // by about one of its cells.
    rasm::TileDesc describeTile(const Node &node, rasm::TileHandle parent)
    {
        const double size = tileSize(node.level);
        const double x0 = node.x * size;
        const double z0 = node.y * size;
        double low = 1e9;
        double high = -1e9;
        for (uint32_t j = 0; j <= 8; ++j)
        {
            for (uint32_t i = 0; i <= 8; ++i)
            {
                const double h = terrainHeight(x0 + size * i / 8.0, z0 + size * j / 8.0);
                low = std::min(low, h);
                high = std::max(high, h);
            }
        }
        const double margin = 60.0 + size * SKIRT;
        low -= margin;
        high += margin;
        const double half = 0.5 * size;
        return {.parent = parent,
                .center = glm::dvec3(x0 + half, 0.5 * (low + high), z0 + half),
                .radius = std::sqrt(2.0 * half * half + 0.25 * (high - low) * (high - low)),
                .geometricError = node.level == MAX_LEVEL ? 0.0 : size / static_cast<double>(CELLS),
                .estimatedBytes = TERRAIN_TILE_BYTES};
    }

    // A landmark hangs under an empty tile with ADD refinement, the child of the terrain tile it stands on: the terrain
    // tile's REPLACE children then do not wait for the model (an empty tile is ready at once), and the model adds to
    // what is drawn. The group's error is its terrain tile's, so the model ranks with the terrain's children.
    rasm::TileDesc describeGroup(const Landmark &landmark, rasm::TileHandle parent)
    {
        const double y = terrainHeight(landmark.x, landmark.z);
        return {.parent = parent,
                .center = glm::dvec3(landmark.x, y + 50.0, landmark.z),
                .radius = 200.0,
                .geometricError = tileSize(LANDMARK_LEVEL) / static_cast<double>(CELLS),
                .refine = rasm::TileRefine::ADD,
                .estimatedBytes = 1};
    }

    rasm::TileDesc describeLandmark(const Landmark &landmark, rasm::TileHandle parent)
    {
        const double y = terrainHeight(landmark.x, landmark.z);
        return {.parent = parent, .center = glm::dvec3(landmark.x, y + 50.0, landmark.z), .radius = 200.0, .geometricError = 0.0, .estimatedBytes = LANDMARK_BYTES};
    }

    glm::vec3 levelColor(int32_t level)
    {
        const glm::vec3 colors[8] = {{0.9f, 0.3f, 0.3f}, {0.9f, 0.6f, 0.2f}, {0.9f, 0.9f, 0.3f}, {0.4f, 0.9f, 0.3f}, {0.2f, 0.8f, 0.8f}, {0.3f, 0.5f, 1.0f}, {0.7f, 0.4f, 1.0f}, {1.0f, 1.0f, 1.0f}};
        return colors[std::clamp(level, 0, 7)];
    }

    glm::dvec3 tileOrigin(int32_t level, int32_t x, int32_t y)
    {
        const double size = tileSize(level);
        return {(x + 0.5) * size, 0.0, (y + 0.5) * size};
    }

    // The tile's heights on the server's thread: each sample on the world's grid of HEIGHT_STEP first, then stored as the
    // steps above the tile's lowest one, so a sample two tiles share is the same height in both.
    void buildTileHeights(TileData &data)
    {
        const double size = tileSize(data.level);
        const double cell = size / static_cast<double>(CELLS);
        const double x0 = data.x * size;
        const double z0 = data.y * size;
        std::vector<int64_t> steps(static_cast<size_t>(HEIGHTS_WIDTH) * HEIGHTS_WIDTH);
        int64_t lowest = INT64_MAX;
        int64_t highest = INT64_MIN;
        for (uint32_t j = 0; j < HEIGHTS_WIDTH; ++j)
        {
            for (uint32_t i = 0; i < HEIGHTS_WIDTH; ++i)
            {
                const double x = x0 + (static_cast<double>(i) - 1.0) * cell;
                const double z = z0 + (static_cast<double>(j) - 1.0) * cell;
                const int64_t step = std::llround(terrainHeight(x, z) / HEIGHT_STEP);
                steps[static_cast<size_t>(j) * HEIGHTS_WIDTH + i] = step;
                lowest = std::min(lowest, step);
                highest = std::max(highest, step);
            }
        }
        data.heights.resize(steps.size());
        for (size_t k = 0; k < steps.size(); ++k)
            data.heights[k] = static_cast<uint16_t>(std::min<int64_t>(steps[k] - lowest, 65535));
        data.heightOffset = static_cast<float>(static_cast<double>(lowest) * HEIGHT_STEP);
        data.minHeight = data.heightOffset;
        data.maxHeight = static_cast<float>(static_cast<double>(highest) * HEIGHT_STEP);
    }

    // Ground colors by height and slope, tinted by the tile's level, with a dark border; then its mips, by averaging.
    void buildTileImage(TileData &data)
    {
        const double size = tileSize(data.level);
        const double x0 = data.x * size;
        const double z0 = data.y * size;
        std::vector<uint8_t> &pixels = data.pixels;
        pixels.reserve(IMAGE_SIZE * IMAGE_SIZE * 4 * 4 / 3 + 4);
        const glm::vec3 tint = levelColor(data.level);
        for (uint32_t j = 0; j < IMAGE_SIZE; ++j)
        {
            for (uint32_t i = 0; i < IMAGE_SIZE; ++i)
            {
                const double x = x0 + size * (i + 0.5) / IMAGE_SIZE;
                const double z = z0 + size * (j + 0.5) / IMAGE_SIZE;
                const double h = terrainHeight(x, z);
                const float t = static_cast<float>(std::clamp((h + 1200.0) / 2600.0, 0.0, 1.0));
                glm::vec3 color = glm::mix(glm::vec3(0.18f, 0.32f, 0.12f), glm::vec3(0.45f, 0.38f, 0.28f), std::min(t * 1.6f, 1.0f));
                if (t > 0.78f)
                    color = glm::mix(color, glm::vec3(0.92f), (t - 0.78f) / 0.22f);
                // Fields: a pattern whose cells stay put in the world, so a finer tile shows the same ones sharper.
                const bool field = (static_cast<int64_t>(std::floor(x / 90.0)) + static_cast<int64_t>(std::floor(z / 70.0))) % 2 == 0;
                color *= field ? 1.0f : 0.88f;
                color = glm::mix(color, tint, 0.25f);
                if (i == 0 || j == 0 || i + 1 == IMAGE_SIZE || j + 1 == IMAGE_SIZE)
                    color *= 0.45f;
                pixels.push_back(static_cast<uint8_t>(255.0f * std::clamp(color.r, 0.0f, 1.0f)));
                pixels.push_back(static_cast<uint8_t>(255.0f * std::clamp(color.g, 0.0f, 1.0f)));
                pixels.push_back(static_cast<uint8_t>(255.0f * std::clamp(color.b, 0.0f, 1.0f)));
                pixels.push_back(255);
            }
        }
        // Each mip averages 2 x 2 texels of the one before (in sRGB, which is close enough for a map's colors).
        std::vector<size_t> &offsets = data.offsets;
        offsets.assign(1, 0);
        uint32_t side = IMAGE_SIZE;
        while (side > 1)
        {
            const size_t source = offsets.back();
            const uint32_t next = side / 2;
            offsets.push_back(pixels.size());
            for (uint32_t j = 0; j < next; ++j)
            {
                for (uint32_t i = 0; i < next; ++i)
                {
                    for (uint32_t c = 0; c < 4; ++c)
                    {
                        const auto at = [&](uint32_t u, uint32_t v)
                        { return static_cast<uint32_t>(pixels[source + (static_cast<size_t>(v) * side + u) * 4 + c]); };
                        pixels.push_back(static_cast<uint8_t>((at(2 * i, 2 * j) + at(2 * i + 1, 2 * j) + at(2 * i, 2 * j + 1) + at(2 * i + 1, 2 * j + 1) + 2) / 4));
                    }
                }
            }
            side = next;
        }
    }

    // The server: SERVER_THREADS threads that make the tiles' data in the order they are asked for, as a map server would
    // send them. The main thread only uploads what came back.
    class TileServer
    {
    public:
        TileServer()
        {
            for (std::thread &thread : threads)
                thread = std::thread([this]()
                                     { run(); });
        }

        ~TileServer()
        {
            {
                const std::lock_guard<std::mutex> lock(mutex);
                stop = true;
            }
            wake.notify_all();
            for (std::thread &thread : threads)
                thread.join();
        }

        std::shared_ptr<TileData> request(int32_t level, int32_t x, int32_t y)
        {
            auto data = std::make_shared<TileData>();
            data->level = level;
            data->x = x;
            data->y = y;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                queue.push_back(data);
            }
            wake.notify_one();
            return data;
        }

        // Waits only when the data is not there yet at its arrival frame, so frame N is the same on every run.
        void wait(const TileData &data)
        {
            std::unique_lock<std::mutex> lock(mutex);
            finished.wait(lock, [&data]()
                          { return data.done; });
        }

    private:
        void run()
        {
            std::unique_lock<std::mutex> lock(mutex);
            while (true)
            {
                wake.wait(lock, [this]()
                          { return stop || !queue.empty(); });
                if (stop)
                    return;
                const std::shared_ptr<TileData> data = queue.front();
                queue.pop_front();
                lock.unlock();
                buildTileHeights(*data);
                buildTileImage(*data);
                lock.lock();
                data->done = true;
                finished.notify_all();
            }
        }

        std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable finished;
        std::deque<std::shared_ptr<TileData>> queue;
        bool stop = false;
        std::thread threads[SERVER_THREADS];
    };

    // The fetch came in: the tile's heights, image, material and entity, handed to the streamer, which owns them now.
    void makeTerrainContent(rasm::Engine &engine, rasm::TileStreamer &streamer, const Node &node, TileData &data)
    {
        rasm::TextureHandle heights = engine.createTexture({.name = "Tile Heights", .width = HEIGHTS_WIDTH, .height = HEIGHTS_WIDTH, .format = rasm::Format::R16_UNORM, .usage = rasm::TextureUsage::SAMPLED});
        if (heights.isValid() && !engine.getRenderContext().fillTexture(heights, data.heights.data()))
        {
            engine.destroyTexture(heights);
            heights = {};
        }
        rasm::TextureLevel levels[16];
        uint32_t side = IMAGE_SIZE;
        for (size_t level = 0; level < data.offsets.size(); ++level, side = std::max(side / 2, 1u))
            levels[level] = {.data = data.pixels.data() + data.offsets[level], .size = static_cast<uint64_t>(side) * side * 4};
        const auto levelCount = static_cast<uint32_t>(data.offsets.size());
        rasm::TextureHandle image = engine.createTexture({.name = "Tile Image", .width = IMAGE_SIZE, .height = IMAGE_SIZE, .format = rasm::Format::R8G8B8A8_SRGB, .usage = rasm::TextureUsage::SAMPLED, .mipLevels = levelCount});
        if (image.isValid() && !engine.getRenderContext().fillTexture(image, levels, levelCount))
        {
            engine.destroyTexture(image);
            image = {};
        }
        const double size = tileSize(node.level);
        const rasm::TerrainTile terrain = heights.isValid() && image.isValid()
                                              ? engine.createTerrainTile({.center = tileOrigin(node.level, node.x, node.y),
                                                                          .size = size,
                                                                          .terrain = {.heights = heights,
                                                                                      .heightScale = static_cast<float>(HEIGHT_STEP),
                                                                                      .heightOffset = data.heightOffset,
                                                                                      .minHeight = data.minHeight,
                                                                                      .maxHeight = data.maxHeight,
                                                                                      .skirt = static_cast<float>(size * SKIRT)},
                                                                          .imagery = image,
                                                                          .roughness = 0.95f})
                                              : rasm::TerrainTile{};
        // An invalid handle (a full pool) fails the tile, and the streamer destroys what was made. The grid is shared, so
        // the content does not list it.
        const rasm::EntityHandle entities[1] = {terrain.entity};
        const rasm::MaterialHandle materials[1] = {terrain.material};
        const rasm::TextureHandle textures[2] = {heights, image};
        streamer.setTileContent(node.tile, {.entities = entities, .entityCount = 1, .materials = materials, .materialCount = 1, .textures = textures, .textureCount = 2});
    }

    // A glTF tile: the mesh loads on the loader's workers. Its import's materials and textures belong to the mesh.
    void makeLandmarkContent(rasm::Engine &engine, rasm::TileStreamer &streamer, const Node &node)
    {
        const Landmark &landmark = LANDMARKS[node.landmark];
        const rasm::MeshHandle mesh = engine.loadGLTF(landmark.path);
        const glm::dvec3 at(landmark.x, terrainHeight(landmark.x, landmark.z) + 2.0, landmark.z);
        const rasm::EntityHandle entity = mesh.isValid() ? engine.createEntity("landmark", mesh, {}, rasm::Transform{at, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(landmark.scale)}) : rasm::EntityHandle{};
        const rasm::EntityHandle entities[1] = {entity};
        const rasm::MeshHandle meshes[1] = {mesh};
        streamer.setTileContent(node.tile, {.entities = entities, .entityCount = 1, .meshes = meshes, .meshCount = 1});
    }

    Node &nodeFor(Map &map, rasm::TileStreamer &streamer, int32_t level, int32_t x, int32_t y, int32_t landmark, rasm::TileHandle parent, bool group = false)
    {
        const uint64_t key = nodeKey(level, x, y, group ? landmark + static_cast<int32_t>(LANDMARK_COUNT) : landmark);
        Node &node = map.nodes[key];
        if (!node.tile.isValid())
        {
            node.level = level;
            node.x = x;
            node.y = y;
            node.landmark = landmark;
            node.group = group;
            if (landmark < 0)
                node.tile = streamer.createTile(describeTile(node, parent));
            else
                node.tile = streamer.createTile(group ? describeGroup(LANDMARKS[landmark], parent) : describeLandmark(LANDMARKS[landmark], parent));
            map.nodeOfTile[node.tile.index] = key;
            ++map.tilesMade;
        }
        node.lastVisit = map.frame;
        return node;
    }

    // The application's traversal: a tile refines over REFINE_PIXELS and stays refined down to MERGE_PIXELS.
    void visit(Map &map, rasm::TileStreamer &streamer, int32_t level, int32_t x, int32_t y, rasm::TileHandle parent)
    {
        Node &node = nodeFor(map, streamer, level, x, y, -1, parent);
        const double error = streamer.getScreenError(node.tile);
        node.refined = level < MAX_LEVEL && error > (node.refined ? MERGE_PIXELS : REFINE_PIXELS);
        if (!node.refined)
        {
            map.cut.push_back(node.tile);
            return;
        }
        const rasm::TileHandle tile = node.tile;
        for (int32_t c = 0; c < 4; ++c)
            visit(map, streamer, level + 1, 2 * x + (c & 1), 2 * y + (c >> 1), tile);
        if (level != LANDMARK_LEVEL)
            return;
        const double size = tileSize(level);
        for (uint32_t l = 0; l < LANDMARK_COUNT; ++l)
        {
            if (static_cast<int32_t>(LANDMARKS[l].x / size) != x || static_cast<int32_t>(LANDMARKS[l].z / size) != y)
                continue;
            const rasm::TileHandle group = nodeFor(map, streamer, level + 1, x, y, static_cast<int32_t>(l), tile, true).tile;
            map.cut.push_back(nodeFor(map, streamer, level + 2, x, y, static_cast<int32_t>(l), group).tile);
        }
    }

    // Low over the hills, up to 6 km, and down again: 24 s a round.
    rasm::Transform flight(double seconds)
    {
        const double angle = seconds * 0.045;
        const glm::dvec3 center(0.5 * WORLD_SIZE, 0.0, 0.5 * WORLD_SIZE);
        const glm::dvec3 ground = center + glm::dvec3(11000.0 * std::cos(angle), 0.0, 9000.0 * std::sin(1.3 * angle));
        const double climb = std::pow(std::max(0.0, std::sin(seconds * 2.0 * 3.14159265 / 24.0 - 1.2)), 4.0);
        const glm::dvec3 eye(ground.x, terrainHeight(ground.x, ground.z) + 180.0 + 6000.0 * climb, ground.z);
        const glm::dvec3 ahead = center + glm::dvec3(11000.0 * std::cos(angle + 0.01), 0.0, 9000.0 * std::sin(1.3 * (angle + 0.01)));
        glm::dvec3 forward = glm::normalize(glm::dvec3(ahead.x - ground.x, 0.0, ahead.z - ground.z));
        const double pitch = glm::radians(-20.0 - 42.0 * climb);
        forward = glm::normalize(glm::dvec3(forward.x * std::cos(pitch), std::sin(pitch), forward.z * std::cos(pitch)));
        return rasm::Transform{eye, glm::quatLookAt(glm::vec3(forward), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)};
    }

    void printStats(const char *when, const rasm::TileStreamerStats &stats)
    {
        std::printf("%s: %u tiles, %u wanted, %u drawn (%u parents in place of children), %u holes, %u ready, %u loading, %u waiting for the budget; "
                    "%.1f of %.1f MiB; device %.0f of %.0f MiB\n",
                    when, stats.tiles, stats.wanted, stats.drawn, stats.fallbacks, stats.holes, stats.ready, stats.loading, stats.waitingForBudget,
                    static_cast<double>(stats.bytes) / (1024.0 * 1024.0), static_cast<double>(stats.budget) / (1024.0 * 1024.0),
                    static_cast<double>(stats.deviceUsage) / (1024.0 * 1024.0), static_cast<double>(stats.deviceBudget) / (1024.0 * 1024.0));
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    uint64_t budgetMiB = 48;
    bool fast = false;
    for (int i = 1; i < argc; ++i)
    {
        if (i + 1 < argc && std::strcmp(argv[i], "--budget") == 0)
            budgetMiB = static_cast<uint64_t>(std::max(0L, std::strtol(argv[++i], nullptr, 10)));
        fast = fast || std::strcmp(argv[i], "--fast") == 0;
    }

    rasm::EngineConfig config = {
        .appName = "example_24",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        // The quadtree is the terrain's level of detail.
        .meshLods = false};
    applyExampleArgs(args, config);
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    const rasm::SceneHandle scene = engine.createScene();
    const rasm::EnvironmentHandle sky = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    engine.setSceneEnvironment(scene, rasm::SceneEnvironment{.environment = sky, .intensity = 0.6f, .sky = false});
    engine.addLightToScene(scene, engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.93f, 0.82f), glm::dvec3(0.0), glm::normalize(glm::vec3(0.35f, -0.8f, 0.25f)), 3.0f, 0.0f));

    const rasm::CameraHandle camera = engine.createCamera(
        rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = 60.0f, .aspect = static_cast<float>(WIDTH) / HEIGHT, .nearZ = 1.0f}}, flight(0.0));
    const float mapHalf = static_cast<float>(0.52 * WORLD_SIZE);
    const rasm::CameraHandle map = engine.createCamera(
        rasm::CameraProjection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC,
                               .orthographic{.left = -mapHalf, .right = mapHalf, .bottom = -mapHalf, .top = mapHalf, .nearZ = 10.0f, .farZ = 30000.0f}},
        rasm::Transform{glm::dvec3(0.5 * WORLD_SIZE, 15000.0, 0.5 * WORLD_SIZE), glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(1.0f)});
    for (const rasm::CameraHandle each : {camera, map})
    {
        engine.setCameraController(each, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
        engine.addCameraToScene(scene, each);
    }
    // The camera's place on the map: a red box high over it, out of its own view.
    const rasm::MaterialHandle red = engine.createMaterial(rasm::MaterialType::UNLIT);
    rasm::Material redValues = engine.getMaterial(red);
    redValues.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO)] = 1.0f;
    redValues.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + 1] = 0.05f;
    redValues.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + 2] = 0.05f;
    engine.updateMaterial(red, redValues);
    std::vector<rasm::Vertex> boxVertices;
    std::vector<uint32_t> boxIndices;
    for (int face = 0; face < 6; ++face)
    {
        glm::vec3 n(0.0f);
        n[face / 2] = face % 2 == 0 ? 1.0f : -1.0f;
        const glm::vec3 a = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 b = glm::cross(n, a);
        const auto first = static_cast<uint32_t>(boxVertices.size());
        for (const glm::vec2 c : {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1)})
            boxVertices.push_back({.pos = (n + a * c.x + b * c.y) * 0.5f, .normal = n, .uv = {c, c, c}, .tangent = glm::vec4(0.0f)});
        boxIndices.insert(boxIndices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
    }
    const rasm::EntityHandle marker = engine.createEntity("camera on the map", engine.createMesh(boxVertices, boxIndices), red);
    engine.addEntityToScene(scene, marker);

    rasm::TileStreamer streamer(engine, scene, {.memoryBudget = budgetMiB << 20, .maxLoadsPerFrame = fast ? 64u : 4u, .maxLoadsInFlight = fast ? 256u : 16u, .maxDropsPerFrame = fast ? 256u : 32u});

    Map world;
    TileServer server;
    std::vector<Fetch> fetches;
    std::vector<rasm::View> views;
    world.cut.reserve(4096);
    fetches.reserve(512);
    views.reserve(2);
    Totals totals;
    double seconds = 0.0;
    rasm::TileState landmarkStates[LANDMARK_COUNT] = {};

    if (args.waitForLoads)
        engine.waitForLoads();
    while (exampleRunning(engine, args))
    {
        const auto frameStart = std::chrono::steady_clock::now();
        seconds += 1.0 / 60.0;
        const uint32_t frame = ++world.frame;
        const rasm::Transform eye = flight(seconds);
        engine.setCameraTransform(camera, eye);
        engine.updateTransform(marker, rasm::Transform{eye.position + glm::dvec3(0.0, 2500.0, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(700.0f)});

        views.clear();
        views.push_back({.scene = scene, .camera = camera});
        views.push_back({.scene = scene, .camera = map, .output = {.rect = {.x = 0.73f, .y = 0.02f, .width = 0.25f, .height = 0.25f * 16.0f / 9.0f}}, .bloom = rasm::ViewToggle::OFF});

        // The fetches that came in. A tile the streamer dropped meanwhile (EMPTY again) is not made.
        for (size_t i = 0; i < fetches.size();)
        {
            const Fetch &fetch = fetches[i];
            const auto found = world.nodes.find(fetch.key);
            const bool wanted = found != world.nodes.end() && streamer.getTileState(fetch.tile) == rasm::TileState::REQUESTED;
            if (wanted && fetch.arrival > frame)
            {
                ++i;
                continue;
            }
            if (wanted && found->second.group)
                streamer.setTileContent(fetch.tile, {});
            else if (wanted && found->second.landmark >= 0)
                makeLandmarkContent(engine, streamer, found->second);
            else if (wanted)
            {
                server.wait(*fetch.data);
                makeTerrainContent(engine, streamer, found->second, *fetch.data);
            }
            fetches[i] = fetches.back();
            fetches.pop_back();
        }

        const auto updateStart = std::chrono::steady_clock::now();
        streamer.setViews(views.data(), static_cast<uint32_t>(views.size()));
        world.cut.clear();
        visit(world, streamer, 0, 0, 0, rasm::TileHandle{});
        streamer.update(world.cut.data(), static_cast<uint32_t>(world.cut.size()));
        const double updateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - updateStart).count();

        uint32_t count = 0;
        const rasm::TileHandle *toLoad = streamer.getTilesToLoad(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto key = world.nodeOfTile.find(toLoad[i].index);
            if (key == world.nodeOfTile.end())
                continue;
            const Node &node = world.nodes[key->second];
            fetches.push_back({.tile = toLoad[i],
                               .key = key->second,
                               .arrival = frame + (fast || node.group ? 0 : FETCH_FRAMES + (static_cast<uint32_t>(toLoad[i].index) % 6)),
                               .data = node.landmark >= 0 ? nullptr : server.request(node.level, node.x, node.y)});
        }
        // A glTF tile that loaded, or that was dropped while its mesh still loaded (which cancelled the load).
        for (uint32_t l = 0; l < LANDMARK_COUNT; ++l)
        {
            const double size = tileSize(LANDMARK_LEVEL);
            const auto found = world.nodes.find(nodeKey(LANDMARK_LEVEL + 2, static_cast<int32_t>(LANDMARKS[l].x / size), static_cast<int32_t>(LANDMARKS[l].z / size), static_cast<int32_t>(l)));
            const rasm::TileState state = found != world.nodes.end() ? streamer.getTileState(found->second.tile) : rasm::TileState::EMPTY;
            totals.landmarksLoaded += state == rasm::TileState::READY && landmarkStates[l] != rasm::TileState::READY ? 1 : 0;
            totals.landmarksCancelled += state == rasm::TileState::EMPTY && landmarkStates[l] == rasm::TileState::LOADING ? 1 : 0;
            landmarkStates[l] = state;
        }

        // Tile records of places nobody visited for a while go, so the records stay bounded too.
        if (frame % 120 == 0)
        {
            for (auto it = world.nodes.begin(); it != world.nodes.end();)
            {
                if (frame - it->second.lastVisit > FORGET_AFTER)
                {
                    world.nodeOfTile.erase(it->second.tile.index);
                    streamer.destroyTile(it->second.tile);
                    it = world.nodes.erase(it);
                }
                else
                    ++it;
            }
        }

        const rasm::TileStreamerStats &stats = streamer.getStats();
        totals.updateMs += updateMs;
        totals.maxUpdateMs = std::max(totals.maxUpdateMs, updateMs);
        totals.overBudget += stats.bytes > stats.budget ? 1 : 0;
        totals.holes += frame > HOLE_CHECK_AFTER ? stats.holes : 0;
        totals.maxFallbacks = std::max(totals.maxFallbacks, stats.fallbacks);
        totals.maxWaiting = std::max(totals.maxWaiting, stats.waitingForBudget);
        totals.maxDrawn = std::max(totals.maxDrawn, stats.drawn);
        if (stats.bytes > stats.budget)
            std::fprintf(stderr, "error: frame %u: the tiles hold %llu bytes, over the budget of %llu.\n", frame, static_cast<unsigned long long>(stats.bytes),
                         static_cast<unsigned long long>(stats.budget));
        if (frame % 120 == 0)
            printStats(("frame " + std::to_string(frame)).c_str(), stats);

        engine.render(views.data(), static_cast<uint32_t>(views.size()));
        // After the first frames, which build pipelines.
        if (frame > HOLE_CHECK_AFTER)
            totals.maxFrameMs = std::max(totals.maxFrameMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count());
    }

    const rasm::TileStreamerStats &stats = streamer.getStats();
    printStats("end", stats);
    const uint32_t frame = world.frame;
    std::printf("%u frames: %llu tile records made, %llu loads, %llu drops; at most %u drawn, %u parents in place of children, %u waiting for the budget; "
                "peak %.1f of %.1f MiB; update %.3f ms on average, %.3f ms at most; longest frame %.1f ms; glTF tiles: %u loaded, %u cancelled while loading\n",
                frame, static_cast<unsigned long long>(world.tilesMade), static_cast<unsigned long long>(stats.totalLoads), static_cast<unsigned long long>(stats.totalDrops),
                totals.maxDrawn, totals.maxFallbacks, totals.maxWaiting, static_cast<double>(stats.peakBytes) / (1024.0 * 1024.0), static_cast<double>(stats.budget) / (1024.0 * 1024.0),
                frame > 0 ? totals.updateMs / frame : 0.0, totals.maxUpdateMs, totals.maxFrameMs, totals.landmarksLoaded, totals.landmarksCancelled);
    if (totals.overBudget > 0 || totals.holes > 0)
    {
        std::fprintf(stderr, "error: %u frames over the budget, %u holes after frame %u.\n", totals.overBudget, totals.holes, HOLE_CHECK_AFTER);
        return 1;
    }
    return exampleExit(args);
}
