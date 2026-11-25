#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// A city map drawn from vector tiles: roads, a railway, a river, lakes with islands, parks and points of interest, on a
// flat ground 8 x 8 km wide, streamed by a TileStreamer as a quadtree of 4 levels.
// - Each layer is a VECTOR material shared by every tile (rasm/vector.h): its color, width (in pixels or metres), an
//   outline (a road's casing), dashes (the railway), its draw order, and whether it lies in the ground's plane or faces
//   the screen. A coarse tile keeps the major roads and drops the streets and the points.
// - A tile the streamer hands out is made by a server thread: it clips the world's features to the tile, builds each
//   layer's geometry and its mesh data (buildVectorMesh), so the main thread only uploads it (createMesh(MeshBuild)).
// - A route drawn as an OVERLAY stays over everything; it is one entity outside the tiles.
// - Two views: the camera, which circles the city at 400 to 1600 m and looks down at 40 degrees, and the whole map,
//   orthographic, in the corner.
// Every frame it checks the budget and, after the first frames, that no wanted tile is a hole; a headless run that
// broke a check exits with 1.
//   --culling        DebugView::CULLING: drawn draws green or yellow, culled ones as ghosts (their padded spheres)

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr double WORLD_SIZE = 8192.0;
    constexpr int32_t MAX_LEVEL = 3;
    constexpr double REFINE_PIXELS = 4.0;
    constexpr double MERGE_PIXELS = 0.8 * REFINE_PIXELS;
    constexpr uint32_t SERVER_THREADS = 3;
    constexpr uint32_t FETCH_FRAMES = 2;
    constexpr uint32_t HOLE_CHECK_AFTER = 30;
    constexpr uint64_t TILE_BYTES = 512ull << 10;

    enum Layer : uint32_t
    {
        PARKS,
        WATER,
        RIVER,
        STREETS,
        ROADS,
        RAIL,
        PLACES,
        LAYER_COUNT,
    };

    // The world's features, made once; the server threads read them.
    struct Features
    {
        std::vector<std::vector<glm::dvec3>> roads;
        std::vector<std::vector<glm::dvec3>> streets;
        std::vector<glm::dvec3> rail;
        std::vector<glm::dvec3> river;
        std::vector<std::vector<std::vector<glm::dvec3>>> lakes; // outline, then an island
        std::vector<std::vector<glm::dvec3>> parks;
        std::vector<glm::dvec3> places;
    };

    struct TileData
    {
        int32_t level = 0;
        int32_t x = 0;
        int32_t y = 0;
        rasm::MeshBuild layers[LAYER_COUNT];
        bool done = false;
    };

    struct Node
    {
        int32_t level = 0;
        int32_t x = 0;
        int32_t y = 0;
        rasm::TileHandle tile;
        bool refined = false;
    };

    struct Fetch
    {
        rasm::TileHandle tile;
        uint64_t key = 0;
        uint32_t arrival = 0;
        std::shared_ptr<TileData> data;
    };

    double tileSize(int32_t level)
    {
        return WORLD_SIZE / static_cast<double>(1 << level);
    }

    glm::dvec3 tileCenter(int32_t level, int32_t x, int32_t y)
    {
        const double size = tileSize(level);
        return {(x + 0.5) * size, 0.0, (y + 0.5) * size};
    }

    uint64_t nodeKey(int32_t level, int32_t x, int32_t y)
    {
        return (static_cast<uint64_t>(level) << 48) | (static_cast<uint64_t>(x) << 24) | static_cast<uint64_t>(y);
    }

    uint32_t nextRandom(uint32_t &seed)
    {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    }

    double random01(uint32_t &seed)
    {
        return static_cast<double>(nextRandom(seed)) / static_cast<double>(1u << 24);
    }

    Features makeFeatures()
    {
        Features f;
        uint32_t seed = 7;
        // Major roads: long wavy lines across the map, a point every 32 m.
        for (int32_t r = 0; r < 8; ++r)
        {
            std::vector<glm::dvec3> road;
            const bool across = r % 2 == 0;
            const double at = (0.12 + 0.76 * (r / 7.0)) * WORLD_SIZE;
            for (double t = 0.0; t <= WORLD_SIZE; t += 32.0)
            {
                const double wobble = 260.0 * std::sin(t / 900.0 + r) + 90.0 * std::sin(t / 230.0 + 2.0 * r);
                road.push_back(across ? glm::dvec3(t, 0.0, at + wobble) : glm::dvec3(at + wobble, 0.0, t));
            }
            f.roads.push_back(std::move(road));
        }
        // Streets: a grid in the middle 4 km, with corners.
        for (double s = 2048.0; s <= 6144.0; s += 192.0)
        {
            for (int32_t axis = 0; axis < 2; ++axis)
            {
                std::vector<glm::dvec3> street;
                for (double t = 2048.0; t <= 6144.0; t += 128.0)
                {
                    const double bend = 30.0 * std::sin(t / 300.0 + s / 500.0);
                    street.push_back(axis == 0 ? glm::dvec3(t, 0.0, s + bend) : glm::dvec3(s + bend, 0.0, t));
                }
                f.streets.push_back(std::move(street));
            }
        }
        for (double t = 0.0; t <= WORLD_SIZE; t += 48.0)
            f.rail.push_back({t, 0.0, 1600.0 + 0.35 * t + 180.0 * std::sin(t / 1400.0)});
        for (double t = 0.0; t <= WORLD_SIZE; t += 24.0)
            f.river.push_back({5200.0 + 700.0 * std::sin(t / 1100.0) + 160.0 * std::sin(t / 260.0), 0.0, t});
        for (int32_t l = 0; l < 6; ++l)
        {
            const glm::dvec3 center(900.0 + 6400.0 * random01(seed), 0.0, 900.0 + 6400.0 * random01(seed));
            const double radius = 220.0 + 260.0 * random01(seed);
            std::vector<glm::dvec3> outline;
            std::vector<glm::dvec3> island;
            for (int32_t k = 0; k < 64; ++k)
            {
                const double a = 2.0 * 3.14159265358979 * k / 64.0;
                const double r = radius * (1.0 + 0.25 * std::sin(3.0 * a + l) + 0.1 * std::sin(7.0 * a));
                outline.push_back(center + glm::dvec3(r * std::cos(a), 0.0, 0.7 * r * std::sin(a)));
                island.push_back(center + glm::dvec3(0.25 * radius * std::cos(-a) + 40.0, 0.0, 0.2 * radius * std::sin(-a)));
            }
            f.lakes.push_back({outline, island});
        }
        for (int32_t p = 0; p < 10; ++p)
        {
            const glm::dvec3 corner(600.0 + 6800.0 * random01(seed), 0.0, 600.0 + 6800.0 * random01(seed));
            const double w = 150.0 + 350.0 * random01(seed);
            const double h = 150.0 + 350.0 * random01(seed);
            f.parks.push_back({corner, corner + glm::dvec3(w, 0.0, 30.0), corner + glm::dvec3(w + 40.0, 0.0, h), corner + glm::dvec3(-20.0, 0.0, h * 0.8)});
        }
        for (int32_t p = 0; p < 300; ++p)
            f.places.push_back({2048.0 + 4096.0 * random01(seed), 0.0, 2048.0 + 4096.0 * random01(seed)});
        return f;
    }

    // The parts of a line inside [low, high] (x and z), each a line of its own.
    void clipLine(const std::vector<glm::dvec3> &line, const glm::dvec2 &low, const glm::dvec2 &high, uint32_t stride, std::vector<glm::dvec3> &points, std::vector<uint32_t> &sizes)
    {
        uint32_t open = 0;
        auto close = [&]()
        {
            if (open >= 2)
                sizes.push_back(open);
            else
                points.resize(points.size() - open);
            open = 0;
        };
        for (size_t i = 0; i + stride < line.size() + stride; i += stride)
        {
            const size_t j = std::min(i + stride, line.size() - 1);
            if (j == i)
                break;
            // Liang-Barsky: the segment's part inside the box.
            const glm::dvec3 a = line[i];
            const glm::dvec3 d = line[j] - a;
            double t0 = 0.0;
            double t1 = 1.0;
            const double p[4] = {-d.x, d.x, -d.z, d.z};
            const double q[4] = {a.x - low.x, high.x - a.x, a.z - low.y, high.y - a.z};
            bool inside = true;
            for (int k = 0; k < 4 && inside; ++k)
            {
                if (std::abs(p[k]) < 1e-12)
                    inside = q[k] >= 0.0;
                else if (p[k] < 0.0)
                    t0 = std::max(t0, q[k] / p[k]);
                else
                    t1 = std::min(t1, q[k] / p[k]);
            }
            if (!inside || t0 > t1)
            {
                close();
                continue;
            }
            if (open == 0 || t0 > 0.0)
            {
                close();
                points.push_back(a + d * t0);
                ++open;
            }
            points.push_back(a + d * t1);
            ++open;
            if (t1 < 1.0)
                close();
        }
        close();
    }

    // A ring's part inside the box (Sutherland-Hodgman, one edge at a time).
    std::vector<glm::dvec3> clipRing(const std::vector<glm::dvec3> &ring, const glm::dvec2 &low, const glm::dvec2 &high, uint32_t stride)
    {
        std::vector<glm::dvec3> result;
        for (size_t i = 0; i < ring.size(); i += stride)
            result.push_back(ring[i]);
        for (int edge = 0; edge < 4 && result.size() >= 3; ++edge)
        {
            const int axis = edge / 2 == 0 ? 0 : 2;
            const double limit = edge == 0 ? low.x : edge == 1 ? high.x
                                                 : edge == 2   ? low.y
                                                               : high.y;
            const double sign = edge % 2 == 0 ? 1.0 : -1.0;
            std::vector<glm::dvec3> next;
            for (size_t i = 0; i < result.size(); ++i)
            {
                const glm::dvec3 &a = result[i];
                const glm::dvec3 &b = result[(i + 1) % result.size()];
                const double fa = sign * (a[axis] - limit);
                const double fb = sign * (b[axis] - limit);
                if (fa >= 0.0)
                    next.push_back(a);
                if ((fa >= 0.0) != (fb >= 0.0))
                    next.push_back(a + (b - a) * (fa / (fa - fb)));
            }
            result = std::move(next);
        }
        return result.size() >= 3 ? result : std::vector<glm::dvec3>{};
    }

    // A tile's layers on a server thread: the features clipped to it, built into meshes' data.
    void buildTile(const Features &features, TileData &data)
    {
        const double size = tileSize(data.level);
        const glm::dvec2 low(data.x * size, data.y * size);
        const glm::dvec2 high = low + glm::dvec2(size);
        const glm::dvec3 origin = tileCenter(data.level, data.x, data.y);
        // A coarse tile keeps fewer points: one in 2^(levels below the finest).
        const uint32_t stride = 1u << (MAX_LEVEL - data.level);
        rasm::VectorGeometry geometry[LAYER_COUNT];
        std::vector<glm::dvec3> points;
        std::vector<uint32_t> sizes;
        auto addLines = [&](Layer layer, const std::vector<std::vector<glm::dvec3>> &lines, const rasm::LineShape &shape)
        {
            points.clear();
            sizes.clear();
            for (const std::vector<glm::dvec3> &line : lines)
                clipLine(line, low, high, stride, points, sizes);
            rasm::addVectorLines(geometry[layer], origin, points.data(), sizes.data(), static_cast<uint32_t>(sizes.size()), shape);
        };
        addLines(ROADS, features.roads, {.join = rasm::LineJoin::ROUND, .cap = rasm::LineCap::ROUND});
        if (data.level >= 2)
            addLines(STREETS, features.streets, {.join = rasm::LineJoin::MITER, .cap = rasm::LineCap::SQUARE});
        addLines(RAIL, {features.rail}, {.join = rasm::LineJoin::BEVEL});
        addLines(RIVER, {features.river}, {.join = rasm::LineJoin::ROUND, .cap = rasm::LineCap::BUTT});

        // Lakes with their islands, and parks.
        points.clear();
        std::vector<uint32_t> ringSizes;
        std::vector<uint32_t> ringCounts;
        for (const std::vector<std::vector<glm::dvec3>> &lake : features.lakes)
        {
            uint32_t rings = 0;
            for (const std::vector<glm::dvec3> &ring : lake)
            {
                const std::vector<glm::dvec3> clipped = clipRing(ring, low, high, std::min(stride, 4u));
                if (clipped.empty() || (rings == 0 && clipped.size() < 3))
                    break;
                points.insert(points.end(), clipped.begin(), clipped.end());
                ringSizes.push_back(static_cast<uint32_t>(clipped.size()));
                ++rings;
            }
            if (rings > 0)
                ringCounts.push_back(rings);
        }
        rasm::addVectorPolygons(geometry[WATER], origin, points.data(), ringSizes.data(), ringCounts.data(), static_cast<uint32_t>(ringCounts.size()));
        points.clear();
        ringSizes.clear();
        ringCounts.clear();
        for (const std::vector<glm::dvec3> &park : features.parks)
        {
            const std::vector<glm::dvec3> clipped = clipRing(park, low, high, 1);
            if (clipped.empty())
                continue;
            points.insert(points.end(), clipped.begin(), clipped.end());
            ringSizes.push_back(static_cast<uint32_t>(clipped.size()));
            ringCounts.push_back(1);
        }
        rasm::addVectorPolygons(geometry[PARKS], origin, points.data(), ringSizes.data(), ringCounts.data(), static_cast<uint32_t>(ringCounts.size()));

        if (data.level == MAX_LEVEL)
        {
            points.clear();
            for (const glm::dvec3 &place : features.places)
                if (place.x >= low.x && place.x < high.x && place.z >= low.y && place.z < high.y)
                    points.push_back(place);
            rasm::addVectorPoints(geometry[PLACES], origin, points.data(), static_cast<uint32_t>(points.size()));
        }
        for (uint32_t layer = 0; layer < LAYER_COUNT; ++layer)
            if (!geometry[layer].indices.empty())
                data.layers[layer] = rasm::buildVectorMesh(std::move(geometry[layer]));
    }

    class TileServer
    {
    public:
        explicit TileServer(const Features &features) : features(features)
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
                buildTile(features, *data);
                lock.lock();
                data->done = true;
                finished.notify_all();
            }
        }

        const Features &features;
        std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable finished;
        std::deque<std::shared_ptr<TileData>> queue;
        bool stop = false;
        std::thread threads[SERVER_THREADS];
    };

    // The application's tree: a node per place it visited, made with its tile record on the first visit.
    struct Walk
    {
        rasm::TileStreamer &streamer;
        std::unordered_map<uint64_t, Node> &nodes;
        std::vector<rasm::TileHandle> &cut;
    };

    // A tile refines while its error on screen is over REFINE_PIXELS, and merges again under MERGE_PIXELS.
    void visit(Walk &walk, int32_t level, int32_t x, int32_t y, rasm::TileHandle parent)
    {
        Node &node = walk.nodes[nodeKey(level, x, y)];
        if (!node.tile.isValid())
        {
            const double size = tileSize(level);
            node = {.level = level, .x = x, .y = y};
            node.tile = walk.streamer.createTile({.parent = parent,
                                                  .center = tileCenter(level, x, y),
                                                  .radius = 0.75 * size,
                                                  .geometricError = level == MAX_LEVEL ? 0.0 : size / 512.0,
                                                  .estimatedBytes = TILE_BYTES});
        }
        const double error = walk.streamer.getScreenError(node.tile);
        node.refined = level < MAX_LEVEL && error > (node.refined ? MERGE_PIXELS : REFINE_PIXELS);
        if (!node.refined)
        {
            walk.cut.push_back(node.tile);
            return;
        }
        const rasm::TileHandle tile = node.tile;
        for (int32_t c = 0; c < 4; ++c)
            visit(walk, level + 1, 2 * x + (c & 1), 2 * y + (c >> 1), tile);
    }

    rasm::MaterialHandle makeLayer(rasm::Engine &engine, const rasm::VectorMaterial &style)
    {
        const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::VECTOR);
        rasm::Material values = engine.getMaterial(material);
        values.vector = style;
        engine.updateMaterial(material, values);
        return material;
    }

    // The camera circles the city, rising and falling between 400 and 1600 m.
    rasm::Transform flight(double seconds)
    {
        const double angle = seconds * 0.05;
        const glm::dvec3 center(0.5 * WORLD_SIZE, 0.0, 0.5 * WORLD_SIZE);
        const double height = 1000.0 + 600.0 * std::sin(seconds * 0.21);
        const glm::dvec3 eye = center + glm::dvec3(2600.0 * std::cos(angle), height, 2600.0 * std::sin(angle));
        const glm::dvec3 target = center + glm::dvec3(1200.0 * std::cos(angle + 0.8), 0.0, 1200.0 * std::sin(angle + 0.8));
        const glm::vec3 forward = glm::vec3(glm::normalize(target - eye));
        return rasm::Transform{eye, glm::quatLookAt(forward, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)};
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    bool culling = false;
    for (int i = 1; i < argc; ++i)
        culling = culling || std::strcmp(argv[i], "--culling") == 0;

    rasm::EngineConfig config = {
        .appName = "example_25",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .antiAliasing = rasm::AntiAliasing::TAA};
    applyExampleArgs(args, config);
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;
    if (culling)
        engine.setDebugView(rasm::DebugView::CULLING);

    const rasm::SceneHandle scene = engine.createScene();
    engine.addLightToScene(scene, engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f), glm::dvec3(0.0), glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f)), 2.5f, 0.0f));

    // The ground: one quad under the whole map.
    const rasm::MaterialHandle groundMaterial = engine.createMaterial(rasm::MaterialType::UNLIT);
    rasm::Material groundValues = engine.getMaterial(groundMaterial);
    for (uint32_t c = 0; c < 3; ++c)
        groundValues.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + c] = glm::vec3(0.86f, 0.84f, 0.78f)[c];
    engine.updateMaterial(groundMaterial, groundValues);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    std::vector<rasm::Vertex> quad;
    for (const glm::vec2 c : {glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(0, 1), glm::vec2(1, 1)})
        quad.push_back({.pos = glm::vec3(c.x * WORLD_SIZE, 0.0f, c.y * WORLD_SIZE), .normal = up, .uv = {c, c, c}, .tangent = glm::vec4(0.0f)});
    const rasm::EntityHandle ground = engine.createEntity("ground", engine.createMesh(quad, {0, 2, 1, 1, 2, 3}), groundMaterial);
    engine.addEntityToScene(scene, ground);

    // The layers, shared by every tile: the order a lower one is drawn under a higher one.
    rasm::MaterialHandle layers[LAYER_COUNT];
    layers[PARKS] = makeLayer(engine, {.color = glm::vec4(0.36f, 0.62f, 0.32f, 1.0f), .drawOrder = -40});
    layers[WATER] = makeLayer(engine, {.color = glm::vec4(0.22f, 0.45f, 0.78f, 1.0f), .drawOrder = -30});
    layers[RIVER] = makeLayer(engine, {.color = glm::vec4(0.22f, 0.45f, 0.78f, 1.0f), .width = 40.0f, .widthUnit = rasm::VectorUnit::METRES, .facing = rasm::VectorFacing::GROUND, .drawOrder = -29});
    layers[STREETS] = makeLayer(engine, {.color = glm::vec4(1.0f), .width = 3.0f, .outlineColor = glm::vec4(0.45f, 0.45f, 0.45f, 1.0f), .outlineWidth = 0.75f, .drawOrder = -20});
    layers[ROADS] = makeLayer(engine, {.color = glm::vec4(1.0f, 0.75f, 0.25f, 1.0f), .width = 7.0f, .outlineColor = glm::vec4(0.55f, 0.32f, 0.08f, 1.0f), .outlineWidth = 1.5f, .drawOrder = -10});
    layers[RAIL] = makeLayer(engine, {.color = glm::vec4(0.15f, 0.15f, 0.15f, 1.0f), .width = 2.5f, .dashLength = 10.0f, .gapLength = 6.0f, .drawOrder = -5});
    layers[PLACES] = makeLayer(engine, {.color = glm::vec4(0.85f, 0.12f, 0.15f, 1.0f), .width = 11.0f, .outlineColor = glm::vec4(1.0f), .outlineWidth = 2.0f, .drawOrder = 10});

    // The route: an overlay over everything, one entity outside the tiles.
    const Features features = makeFeatures();
    const rasm::MaterialHandle routeMaterial = makeLayer(engine, {.color = glm::vec4(0.75f, 0.1f, 0.85f, 0.9f), .width = 5.0f, .drawOrder = 60, .depth = rasm::VectorDepth::OVERLAY});
    {
        std::vector<glm::dvec3> route;
        for (size_t i = 20; i < features.roads[1].size() / 2; ++i)
            route.push_back(features.roads[1][i]);
        const glm::dvec3 origin = route.front();
        rasm::VectorGeometry geometry;
        const auto count = static_cast<uint32_t>(route.size());
        rasm::addVectorLines(geometry, origin, route.data(), &count, 1, {.join = rasm::LineJoin::ROUND, .cap = rasm::LineCap::ROUND});
        const rasm::EntityHandle entity = engine.createEntity("route", engine.createMesh(rasm::buildVectorMesh(std::move(geometry))), routeMaterial, rasm::Transform{origin});
        engine.addEntityToScene(scene, entity);
    }

    const rasm::CameraHandle camera =
        engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = 50.0f, .aspect = static_cast<float>(WIDTH) / HEIGHT, .nearZ = 1.0f}}, flight(0.0));
    const auto mapHalf = static_cast<float>(0.52 * WORLD_SIZE);
    const rasm::CameraHandle map = engine.createCamera(
        rasm::CameraProjection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC, .orthographic{.left = -mapHalf, .right = mapHalf, .bottom = -mapHalf, .top = mapHalf, .nearZ = 10.0f, .farZ = 6000.0f}},
        rasm::Transform{glm::dvec3(0.5 * WORLD_SIZE, 3000.0, 0.5 * WORLD_SIZE), glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(1.0f)});
    for (const rasm::CameraHandle each : {camera, map})
    {
        engine.setCameraController(each, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
        engine.addCameraToScene(scene, each);
    }

    rasm::TileStreamer streamer(engine, scene, {.memoryBudget = 64ull << 20});
    TileServer server(features);
    std::unordered_map<uint64_t, Node> nodes;
    std::vector<rasm::TileHandle> cut;
    std::vector<Fetch> fetches;
    std::vector<rasm::View> views;
    cut.reserve(256);
    fetches.reserve(256);
    views.reserve(2);
    uint32_t frame = 0;
    uint32_t holes = 0;
    uint32_t overBudget = 0;
    uint32_t maxDrawn = 0;
    double seconds = 0.0;
    double longestFrameMs = 0.0;

    if (args.waitForLoads)
        engine.waitForLoads();
    while (exampleRunning(engine, args))
    {
        const auto frameStart = std::chrono::steady_clock::now();
        ++frame;
        seconds += 1.0 / 60.0;
        engine.setCameraTransform(camera, flight(seconds));
        views.clear();
        views.push_back({.scene = scene, .camera = camera});
        views.push_back({.scene = scene, .camera = map, .output = {.rect = {.x = 0.73f, .y = 0.02f, .width = 0.25f, .height = 0.25f * 16.0f / 9.0f}}, .antiAliasing = rasm::AntiAliasing::MSAA_4X});

        // Tiles that arrived: their layers' meshes uploaded, one entity each, the layer's material shared.
        for (size_t i = 0; i < fetches.size();)
        {
            const Fetch &fetch = fetches[i];
            const bool wanted = streamer.getTileState(fetch.tile) == rasm::TileState::REQUESTED;
            if (wanted && fetch.arrival > frame)
            {
                ++i;
                continue;
            }
            if (wanted)
            {
                server.wait(*fetch.data);
                const Node &node = nodes[fetch.key];
                rasm::EntityHandle entities[LAYER_COUNT];
                rasm::MeshHandle meshes[LAYER_COUNT];
                uint32_t count = 0;
                for (uint32_t layer = 0; layer < LAYER_COUNT; ++layer)
                {
                    if (!fetch.data->layers[layer].isValid())
                        continue;
                    meshes[count] = engine.createMesh(std::move(fetch.data->layers[layer]));
                    entities[count] = engine.createEntity("layer", meshes[count], layers[layer], rasm::Transform{tileCenter(node.level, node.x, node.y)});
                    ++count;
                }
                streamer.setTileContent(fetch.tile, {.entities = entities, .entityCount = count, .meshes = meshes, .meshCount = count});
            }
            fetches[i] = fetches.back();
            fetches.pop_back();
        }

        streamer.setViews(views.data(), static_cast<uint32_t>(views.size()));
        cut.clear();
        Walk walk{.streamer = streamer, .nodes = nodes, .cut = cut};
        visit(walk, 0, 0, 0, rasm::TileHandle{});
        streamer.update(cut.data(), static_cast<uint32_t>(cut.size()));
        uint32_t count = 0;
        const rasm::TileHandle *toLoad = streamer.getTilesToLoad(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            for (const auto &[key, node] : nodes)
            {
                if (!(node.tile == toLoad[i]))
                    continue;
                fetches.push_back({.tile = toLoad[i], .key = key, .arrival = frame + FETCH_FRAMES, .data = server.request(node.level, node.x, node.y)});
                break;
            }
        }

        const rasm::TileStreamerStats &stats = streamer.getStats();
        holes += frame > HOLE_CHECK_AFTER ? stats.holes : 0;
        overBudget += stats.bytes > stats.budget ? 1 : 0;
        maxDrawn = std::max(maxDrawn, stats.drawn);
        engine.render(views.data(), static_cast<uint32_t>(views.size()));
        if (frame > HOLE_CHECK_AFTER)
            longestFrameMs = std::max(longestFrameMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count());
    }

    const rasm::TileStreamerStats &stats = streamer.getStats();
    std::printf("%u frames: %llu loads, %llu drops, at most %u tiles drawn, peak %.1f of %.1f MiB, longest frame %.1f ms\n", frame, static_cast<unsigned long long>(stats.totalLoads),
                static_cast<unsigned long long>(stats.totalDrops), maxDrawn, static_cast<double>(stats.peakBytes) / (1024.0 * 1024.0), static_cast<double>(stats.budget) / (1024.0 * 1024.0),
                longestFrameMs);
    if (holes > 0 || overBudget > 0)
    {
        std::fprintf(stderr, "error: %u holes after frame %u, %u frames over the budget.\n", holes, HOLE_CHECK_AFTER, overBudget);
        return 1;
    }
    return exampleExit(args);
}
