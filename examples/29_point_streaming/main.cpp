#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"
#include "exampleBench.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Point cloud streaming: a LiDAR tile in Earth-centred coordinates (ECEF), larger than the GPU memory it may take,
// flown through. At its first start the example writes a synthetic scan (rolling ground, roads, buildings and trees, 1 km
// across, near Paris) as a LAS file into its own folder; AssetLoader::loadPointCloud reads it in doubles, and
// buildPointCloudOctree makes Potree's octree of it: each node a sample of its cube, its children the detail it lacks.
// Each node is a TileStreamer tile with TileRefine::ADD, built into a chunk on worker threads when the streamer hands
// it out, and drawn with its parents. The streamer keeps the loaded nodes under --budget; nodes nobody wants go first.
// The cut: a node's children are wanted while its spacing on screen passes --error pixels. A node fades in by its
// prefix as it shows, and out before it goes (PointChunkLod::drawn), and its parent's adaptive points shrink as it
// fills their place (PointChunkLod::finer), so nothing pops. The left view flies through with TAA, the right one is an
// orthographic top view over the camera with 4x MSAA.
// A headless run (or --check) checks every frame that the nodes hold no more than the budget and, once the first cut
// is in, that no wanted node is a hole; it picks the point under the left view's centre, which must be a point of the
// file. A failed check exits with 1.
//   --points N        about N points in the scan written at the first start (8 000 000)
//   --file F          a LAS or LAZ file of your own, in its own coordinates, instead (--z-up for a projected one)
//   --budget MB       the streamer's budget (48 MiB by default, half of the scan's 92 MiB)
//   --error P         the spacing in pixels past which a node's children come in (1.25)
//   --fade S          seconds a node takes to fade in or out (0.35); 0: at once
//   --speed V         the flight's speed in metres a second (14)
//   --density D       PointMaterial::density (2): the points a far chunk keeps per point's area on screen
//   --fixed-size      points of a fixed size (PointSizeMode::FIXED), not adaptive
//   --swap-aa         the left view with 4x MSAA, the right one with TAA
//   --sequence A DIR  headless: also write frames A to --frames as DIR/frame_NNNN.png
//   --stats           print the streamer's numbers every 30 frames
//   --bench           the GPU time per pass (exampleBench.h), at 1920 x 1080

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr double SCAN_SIDE = 1024.0;
    constexpr double CELL = 32.0;
    constexpr double FLIGHT_HEIGHT = 45.0;
    constexpr uint32_t PICK_FRAME = 40;
    constexpr uint32_t MAX_HITS = 4;
    constexpr uint32_t BUILD_THREADS = 4;
    constexpr uint32_t WARMUP_LIMIT = 600;
    constexpr glm::dvec3 PARIS(4197000.0, 168000.0, 4786000.0);
    constexpr double EQUATOR_RADIUS = 6378137.0;
    constexpr double POLAR_RADIUS = 6356752.314245;

    enum ViewIndex : uint32_t
    {
        FLIGHT,
        TOP,
        VIEW_COUNT,
    };

    constexpr rasm::ViewRect RECTS[VIEW_COUNT] = {{.x = 0.0f, .y = 0.0f, .width = 0.5f, .height = 1.0f}, {.x = 0.5f, .y = 0.0f, .width = 0.5f, .height = 1.0f}};

    struct Options
    {
        uint32_t points = 8000000;
        std::string file;
        bool zUp = false;
        double budgetMiB = 48.0;
        double error = 1.25;
        double fade = 0.35;
        double speed = 14.0;
        float density = 2.0f;
        bool fixedSize = false;
        bool swapAa = false;
        bool stats = false;
        uint32_t sequenceFrom = 0;
        std::string sequenceDir;
    };

    // Local to world: world = origin + rotation * local, the local frame east, up and south (RASM's Y up).
    struct Placement
    {
        glm::dvec3 origin = glm::dvec3(0.0);
        glm::dquat rotation = glm::dquat(1.0, 0.0, 0.0, 0.0);
    };

    struct Rng
    {
        uint64_t state = 0;

        double uniform()
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<double>(state >> 11) / static_cast<double>(1ull << 53);
        }
    };

    // A node's state in the example: its entity once it has content, and how far it has faded in.
    struct NodeState
    {
        rasm::TileHandle tile;
        rasm::EntityHandle entity;
        float fade = 0.0f;
        bool shown = false; // its parent's spacing on screen passes the error: it fades in, else out
        bool drawn = false;
        uint32_t lastWanted = 0;
    };

    struct BuildJob
    {
        rasm::TileHandle tile;
        uint32_t node = 0;
        rasm::MeshBuild build;
    };

    // Chunks built on worker threads: the main thread queues nodes, takes the builds back and uploads them.
    struct Builders
    {
        const rasm::PointCloudOctree *octree = nullptr;
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<BuildJob> queued;
        std::deque<BuildJob> done;
        bool stop = false;
        std::thread threads[BUILD_THREADS];
    };

    struct StreamTotals
    {
        double updateMs = 0.0;
        double maxUpdateMs = 0.0;
        double uploadMs = 0.0;
        double maxUploadMs = 0.0;
        uint64_t uploads = 0;
        double maxFrameMs = 0.0;
        uint32_t maxFrame = 0;
        uint64_t peakBytes = 0;
        uint32_t frames = 0;
        uint32_t maxWaiting = 0;
        uint32_t maxDrawn = 0;
    };
}

namespace
{
    glm::dquat getEnuRotation(const glm::dvec3 &position)
    {
        const glm::dvec3 up = glm::normalize(glm::dvec3(position.x / (EQUATOR_RADIUS * EQUATOR_RADIUS), position.y / (EQUATOR_RADIUS * EQUATOR_RADIUS),
                                                        position.z / (POLAR_RADIUS * POLAR_RADIUS)));
        const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up));
        const glm::dvec3 south = glm::cross(east, up);
        return glm::quat_cast(glm::dmat3(east, up, south));
    }

    double groundHeight(double x, double z)
    {
        return 6.0 * std::sin(x * 0.011) * std::cos(z * 0.008) + 2.5 * std::sin(x * 0.037 + z * 0.021);
    }

    bool onRoad(double x, double z)
    {
        return std::fmod(x, 256.0) < 7.0 || std::fmod(z, 256.0) < 7.0;
    }

    uint32_t packColor(double r, double g, double b)
    {
        const auto channel = [](double value)
        { return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0)); };
        return channel(r) | channel(g) << 8 | channel(b) << 16;
    }

    // A cell of 32 m: a building (unless a road crosses it) and four trees on the ground, scanned from above: 70% of its
    // points on the ground or the roof, 14% on the walls, 16% in the crowns. Positions in the local frame, colors RGB8.
    void scanCell(int cx, int cz, uint32_t count, std::vector<glm::dvec3> &positions, std::vector<uint32_t> &colors)
    {
        Rng rng{(static_cast<uint64_t>(cx) * 73856093u ^ static_cast<uint64_t>(cz) * 19349663u) + 7u};
        const double x0 = cx * CELL;
        const double z0 = cz * CELL;
        const bool road = onRoad(x0, z0) || onRoad(x0 + CELL - 1.0, z0 + CELL - 1.0);
        const double width = 7.0 + 9.0 * rng.uniform();
        const double depth = 7.0 + 9.0 * rng.uniform();
        const double bx = x0 + 9.0 + (CELL - 18.0 - width) * rng.uniform();
        const double bz = z0 + 9.0 + (CELL - 18.0 - depth) * rng.uniform();
        const double roof = groundHeight(bx, bz) + 5.0 + 14.0 * rng.uniform();
        const double roofShade = rng.uniform();
        glm::dvec3 trees[4];
        double crowns[4];
        for (uint32_t i = 0; i < 4; ++i)
        {
            const double tx = x0 + CELL * rng.uniform();
            const double tz = z0 + CELL * rng.uniform();
            crowns[i] = 2.0 + 2.5 * rng.uniform();
            trees[i] = glm::dvec3(tx, groundHeight(tx, tz) + 3.0 + crowns[i], tz);
        }
        const auto inBuilding = [&](double x, double z)
        { return !road && x >= bx && x <= bx + width && z >= bz && z <= bz + depth; };
        for (uint32_t i = 0; i < count; ++i)
        {
            const double kind = rng.uniform();
            const double shade = 0.85 + 0.15 * rng.uniform();
            glm::dvec3 p(0.0);
            uint32_t color = 0;
            if (kind < 0.7 || road)
            {
                p.x = x0 + CELL * rng.uniform();
                p.z = z0 + CELL * rng.uniform();
                if (inBuilding(p.x, p.z))
                {
                    p.y = roof;
                    color = roofShade < 0.5 ? packColor(0.62 * shade, 0.3 * shade, 0.22 * shade) : packColor(0.45 * shade, 0.45 * shade, 0.48 * shade);
                }
                else
                {
                    p.y = groundHeight(p.x, p.z);
                    color = onRoad(p.x, p.z) ? packColor(0.33 * shade, 0.33 * shade, 0.35 * shade) : packColor((0.3 + 0.01 * p.y) * shade, (0.45 + 0.01 * p.y) * shade, 0.2 * shade);
                }
                p.y += 0.03 * (rng.uniform() - 0.5);
            }
            else if (kind < 0.84)
            {
                const double t = rng.uniform();
                const auto side = static_cast<uint32_t>(rng.uniform() * 4.0) % 4;
                p.x = side < 2 ? bx + t * width : (side == 2 ? bx : bx + width);
                p.z = side < 2 ? (side == 0 ? bz : bz + depth) : bz + t * depth;
                const double ground = groundHeight(p.x, p.z);
                p.y = ground + (roof - ground) * rng.uniform();
                color = packColor(0.78 * shade, 0.76 * shade, 0.7 * shade);
            }
            else
            {
                const auto tree = static_cast<uint32_t>(rng.uniform() * 4.0) % 4;
                const glm::dvec3 d = glm::normalize(glm::dvec3(rng.uniform() - 0.5, rng.uniform() - 0.5, rng.uniform() - 0.5) + glm::dvec3(0.0, 1e-4, 0.0));
                p = trees[tree] + d * crowns[tree] * std::sqrt(rng.uniform());
                color = packColor(0.12 * shade, (0.3 + 0.05 * d.y) * shade, 0.1 * shade);
            }
            positions.push_back(p);
            colors.push_back(color);
        }
    }

    void putBytes(std::vector<uint8_t> &bytes, size_t at, const void *value, size_t size)
    {
        std::memcpy(bytes.data() + at, value, size);
    }

    // LAS 1.2, point format 2 (positions, intensity, RGB), its coordinates ECEF in millimetres around an offset, with
    // the WGS 84 geocentric system as WKT.
    bool writeScan(const std::string &path, uint32_t points, const Placement &placement)
    {
        const int cells = static_cast<int>(SCAN_SIDE / CELL);
        const uint32_t perCell = std::max(1u, points / static_cast<uint32_t>(cells * cells));
        const uint32_t total = perCell * static_cast<uint32_t>(cells * cells);
        const std::string wkt = "GEOCCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"metre\",1],AXIS[\"Geocentric X\",OTHER],"
                                "AXIS[\"Geocentric Y\",OTHER],AXIS[\"Geocentric Z\",NORTH],AUTHORITY[\"EPSG\",\"4978\"]]";
        constexpr uint32_t HEADER = 227;
        constexpr uint32_t RECORD = 26;
        const uint32_t vlr = 54 + static_cast<uint32_t>(wkt.size()) + 1;
        const glm::dvec3 offset = glm::round(placement.origin);
        const double scale = 0.001;
        std::vector<uint8_t> header(HEADER + vlr, 0);
        std::memcpy(header.data(), "LASF", 4);
        header[24] = 1;
        header[25] = 2;
        const uint16_t headerSize = HEADER;
        const uint32_t pointOffset = HEADER + vlr;
        const uint32_t vlrCount = 1;
        const uint16_t recordLength = RECORD;
        putBytes(header, 94, &headerSize, 2);
        putBytes(header, 96, &pointOffset, 4);
        putBytes(header, 100, &vlrCount, 4);
        header[104] = 2;
        putBytes(header, 105, &recordLength, 2);
        putBytes(header, 107, &total, 4);
        std::memcpy(header.data() + HEADER + 2, "LASF_Projection", 15);
        const uint16_t wktId = 2112;
        const auto wktLength = static_cast<uint16_t>(wkt.size() + 1);
        putBytes(header, HEADER + 18, &wktId, 2);
        putBytes(header, HEADER + 20, &wktLength, 2);
        std::memcpy(header.data() + HEADER + 54, wkt.c_str(), wkt.size() + 1);

        std::ofstream file(std::filesystem::path(path), std::ios::binary);
        file.write(reinterpret_cast<const char *>(header.data()), static_cast<std::streamsize>(header.size()));
        glm::dvec3 low(1e300);
        glm::dvec3 high(-1e300);
        std::vector<glm::dvec3> positions;
        std::vector<uint32_t> colors;
        std::vector<uint8_t> records;
        for (int cz = 0; cz < cells; ++cz)
        {
            for (int cx = 0; cx < cells; ++cx)
            {
                positions.clear();
                colors.clear();
                scanCell(cx, cz, perCell, positions, colors);
                records.assign(static_cast<size_t>(perCell) * RECORD, 0);
                for (uint32_t i = 0; i < perCell; ++i)
                {
                    const glm::dvec3 local = positions[i] - glm::dvec3(0.5 * SCAN_SIDE, 0.0, 0.5 * SCAN_SIDE);
                    const glm::dvec3 world = placement.origin + placement.rotation * local;
                    const glm::dvec3 steps = glm::round((world - offset) / scale);
                    low = glm::min(low, steps * scale + offset);
                    high = glm::max(high, steps * scale + offset);
                    uint8_t *record = records.data() + static_cast<size_t>(i) * RECORD;
                    for (int a = 0; a < 3; ++a)
                    {
                        const auto value = static_cast<int32_t>(steps[a]);
                        std::memcpy(record + static_cast<ptrdiff_t>(4 * a), &value, 4);
                    }
                    const auto intensity = static_cast<uint16_t>(std::clamp(positions[i].y * 1000.0 + 20000.0, 0.0, 65535.0));
                    std::memcpy(record + 12, &intensity, 2);
                    record[14] = 0x09;
                    record[15] = 2;
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        const auto value = static_cast<uint16_t>(((colors[i] >> (8 * c)) & 0xFFu) * 257u);
                        std::memcpy(record + 20 + static_cast<size_t>(2 * c), &value, 2);
                    }
                }
                file.write(reinterpret_cast<const char *>(records.data()), static_cast<std::streamsize>(records.size()));
            }
        }
        // The scale, the offset and the bounds, now known.
        file.seekp(131);
        const double fields[12] = {scale, scale, scale, offset.x, offset.y, offset.z, high.x, low.x, high.y, low.y, high.z, low.z};
        file.write(reinterpret_cast<const char *>(fields), sizeof(fields));
        return static_cast<bool>(file);
    }

    // The points a LAS file says it holds, 0 when it is not one.
    uint32_t readPointCount(const std::string &path)
    {
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        uint8_t header[111] = {};
        if (!file.read(reinterpret_cast<char *>(header), sizeof(header)) || std::memcmp(header, "LASF", 4) != 0)
            return 0;
        uint32_t count = 0;
        std::memcpy(&count, header + 107, 4);
        return count;
    }

    void runBuilder(Builders &builders)
    {
        for (;;)
        {
            BuildJob job;
            {
                std::unique_lock<std::mutex> lock(builders.mutex);
                builders.wake.wait(lock, [&builders]()
                                   { return builders.stop || !builders.queued.empty(); });
                if (builders.stop)
                    return;
                job = std::move(builders.queued.front());
                builders.queued.pop_front();
            }
            job.build = rasm::buildPointCloudChunk(rasm::getPointCloudNodeChunk(*builders.octree, job.node));
            std::lock_guard<std::mutex> lock(builders.mutex);
            builders.done.push_back(std::move(job));
        }
    }

    Options parseOptions(int argc, char **argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const bool value = i + 1 < argc;
            if (value && std::strcmp(argv[i], "--points") == 0)
                options.points = static_cast<uint32_t>(std::max(1000.0, std::atof(argv[++i])));
            else if (value && std::strcmp(argv[i], "--file") == 0)
                options.file = argv[++i];
            else if (value && std::strcmp(argv[i], "--budget") == 0)
                options.budgetMiB = std::max(1.0, std::atof(argv[++i]));
            else if (value && std::strcmp(argv[i], "--error") == 0)
                options.error = std::max(0.1, std::atof(argv[++i]));
            else if (value && std::strcmp(argv[i], "--density") == 0)
                options.density = static_cast<float>(std::max(0.0, std::atof(argv[++i])));
            else if (value && std::strcmp(argv[i], "--speed") == 0)
                options.speed = std::atof(argv[++i]);
            else if (value && std::strcmp(argv[i], "--fade") == 0)
                options.fade = std::max(0.0, std::atof(argv[++i]));
            else if (i + 2 < argc && std::strcmp(argv[i], "--sequence") == 0)
            {
                options.sequenceFrom = static_cast<uint32_t>(std::atoi(argv[++i]));
                options.sequenceDir = argv[++i];
            }
            options.zUp = options.zUp || std::strcmp(argv[i], "--z-up") == 0;
            options.fixedSize = options.fixedSize || std::strcmp(argv[i], "--fixed-size") == 0;
            options.swapAa = options.swapAa || std::strcmp(argv[i], "--swap-aa") == 0;
            options.stats = options.stats || std::strcmp(argv[i], "--stats") == 0;
        }
        return options;
    }

    // The flight over the scan, in its local frame: a slow S over the ground, looking ahead and down.
    void getFlight(double distance, glm::dvec3 &eye, glm::dvec3 &forward)
    {
        const double along = -0.42 * SCAN_SIDE + distance;
        const double across = 120.0 * std::sin(along * 0.006);
        const double heading = std::atan(120.0 * 0.006 * std::cos(along * 0.006));
        eye = glm::dvec3(along, FLIGHT_HEIGHT + groundHeight(along + 0.5 * SCAN_SIDE, across + 0.5 * SCAN_SIDE), across);
        forward = glm::normalize(glm::dvec3(std::cos(heading), -0.42, std::sin(heading)));
    }

    // The cut: a node's children are wanted while its spacing on screen passes the error, and while they fade out.
    void walkCut(rasm::TileStreamer &streamer, const rasm::PointCloudOctree &octree, std::vector<NodeState> &nodes, uint32_t node, double error, uint32_t frame,
                 std::vector<rasm::TileHandle> &wanted)
    {
        const rasm::PointCloudNode &record = octree.nodes[node];
        nodes[node].lastWanted = frame;
        wanted.push_back(nodes[node].tile);
        const bool refine = record.geometricError > 0.0f && streamer.getScreenError(record.boundsCenter, record.boundsRadius, record.geometricError) > error;
        for (uint32_t child : record.children)
        {
            if (child == rasm::POINT_NODE_NONE)
                continue;
            nodes[child].shown = refine;
            if (refine || nodes[child].fade > 0.0f)
                walkCut(streamer, octree, nodes, child, error, frame, wanted);
        }
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    const Options options = parseOptions(argc, argv);
    bool check = args.headless;
    for (int i = 1; i < argc; ++i)
        check = check || std::strcmp(argv[i], "--check") == 0;

    const uint64_t budget = static_cast<uint64_t>(options.budgetMiB * 1024.0 * 1024.0);
    rasm::EngineConfig config = {
        .appName = "RASM: point cloud streaming",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR};
    // The point pool holds the budget and a quarter more, for the frees the GPU still waits on.
    config.geometryPoolPoints = static_cast<uint32_t>(budget / 8 + budget / 32);
    applyExampleArgs(args, config);
    ExampleBench bench{.on = hasBenchFlag(argc, argv)};
    if (bench.on)
    {
        config.enableValidation = false;
        config.ui = false;
        config.showStats = false;
        config.windowWidth = 1920;
        config.windowHeight = 1080;
        check = false;
    }
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    // The scan: written at the first start, beside the example, then read back like any file.
    Placement placement;
    placement.origin = PARIS;
    placement.rotation = getEnuRotation(PARIS);
    std::string path = options.file;
    if (path.empty())
    {
        path = (std::filesystem::path(argv[0]).parent_path() / "29_point_streaming_scan.las").string();
        const uint32_t cells = static_cast<uint32_t>(SCAN_SIDE / CELL) * static_cast<uint32_t>(SCAN_SIDE / CELL);
        if (readPointCount(path) != std::max(1u, options.points / cells) * cells)
        {
            std::printf("point streaming: writing a scan of %u points to %s\n", options.points, path.c_str());
            if (!writeScan(path, options.points, placement))
            {
                std::fprintf(stderr, "error: %s could not be written.\n", path.c_str());
                return 1;
            }
        }
    }
    const auto loadStart = std::chrono::steady_clock::now();
    rasm::PointCloudData cloud;
    if (!engine.loadPointCloud(path, cloud, {.scalar = rasm::PointCloudScalar::NONE, .zUpToYUp = options.zUp}))
        return 1;
    const auto buildStart = std::chrono::steady_clock::now();
    rasm::PointCloudOctree octree;
    if (!rasm::buildPointCloudOctree({.positions = cloud.positions.data(), .colors = cloud.colors.empty() ? nullptr : cloud.colors.data(), .count = static_cast<uint32_t>(cloud.positions.size())},
                                     octree))
        return 1;
    const auto buildEnd = std::chrono::steady_clock::now();
    const double loadSeconds = std::chrono::duration<double>(buildStart - loadStart).count();
    const double buildSeconds = std::chrono::duration<double>(buildEnd - buildStart).count();
    const uint32_t pointBytes = cloud.colors.empty() ? 8 : 12;
    std::printf("point streaming: %.2f M points (LAS 1.%u format %u%s), read in %.2f s (%.1f M points/s), octree of %zu nodes in %.2f s (%.1f M points/s), %.0f MiB if all in GPU "
                "memory, budget %.0f MiB\n",
                static_cast<double>(cloud.positions.size()) / 1e6, cloud.versionMinor, cloud.pointFormat, cloud.compressed ? ", LAZ" : "", loadSeconds,
                static_cast<double>(cloud.positions.size()) / 1e6 / loadSeconds, octree.nodes.size(), buildSeconds, static_cast<double>(cloud.positions.size()) / 1e6 / buildSeconds,
                static_cast<double>(cloud.positions.size()) * pointBytes / 1048576.0, options.budgetMiB);
    // A file of your own is placed where it is: its own ground decides the frame.
    if (!options.file.empty())
    {
        placement.origin = octree.nodes[0].boundsCenter;
        placement.rotation = glm::length(placement.origin) > 1e6 ? getEnuRotation(placement.origin) : glm::dquat(1.0, 0.0, 0.0, 0.0);
    }

    const rasm::SceneHandle scene = engine.createScene();
    const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::POINTS);
    rasm::Material style = engine.getMaterial(material);
    style.points = {.size = 2.0f,
                    .shape = rasm::PointShape::ROUND,
                    .colorMode = rasm::PointColorMode::COLOR,
                    .density = options.density,
                    .sizeMode = options.fixedSize ? rasm::PointSizeMode::FIXED : rasm::PointSizeMode::ADAPTIVE,
                    .maxSize = 32.0f};
    engine.updateMaterial(material, style);

    rasm::TileStreamer streamer(engine, scene, {.memoryBudget = budget, .maxLoadsPerFrame = 8});
    std::vector<NodeState> nodes(octree.nodes.size());
    std::vector<uint32_t> nodeOfTile;
    for (uint32_t n = 0; n < octree.nodes.size(); ++n)
    {
        const rasm::PointCloudNode &node = octree.nodes[n];
        nodes[n].tile = streamer.createTile({.parent = node.parent == rasm::POINT_NODE_NONE ? rasm::TileHandle{} : nodes[node.parent].tile,
                                             .center = node.boundsCenter,
                                             .radius = node.boundsRadius,
                                             .geometricError = node.geometricError,
                                             .refine = rasm::TileRefine::ADD,
                                             .estimatedBytes = static_cast<uint64_t>(node.pointCount) * pointBytes});
        nodeOfTile.resize(std::max<size_t>(nodeOfTile.size(), nodes[n].tile.index + 1), rasm::POINT_NODE_NONE);
        nodeOfTile[nodes[n].tile.index] = n;
    }
    nodes[0].shown = true;

    Builders builders;
    builders.octree = &octree;
    for (std::thread &thread : builders.threads)
        thread = std::thread([&builders]()
                             { runBuilder(builders); });

    const float aspect = 0.5f * static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight);
    const glm::vec3 up = glm::vec3(placement.rotation * glm::dvec3(0.0, 1.0, 0.0));
    const rasm::CameraHandle flight = engine.createCamera({.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = 55.0f, .aspect = aspect, .nearZ = 0.5f}}, {});
    const float topHalf = 110.0f;
    const rasm::CameraHandle top = engine.createCamera(
        {.type = rasm::CameraProjection::Type::ORTHOGRAPHIC, .orthographic{.left = -topHalf, .right = topHalf, .bottom = -topHalf / aspect, .top = topHalf / aspect, .nearZ = 1.0f, .farZ = 2000.0f}}, {});
    for (const rasm::CameraHandle camera : {flight, top})
    {
        engine.setCameraController(camera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
        engine.addCameraToScene(scene, camera);
    }
    const rasm::AntiAliasing flightAa = options.swapAa ? rasm::AntiAliasing::MSAA_4X : rasm::AntiAliasing::TAA;
    const rasm::AntiAliasing topAa = options.swapAa ? rasm::AntiAliasing::TAA : rasm::AntiAliasing::MSAA_4X;
    rasm::View list[VIEW_COUNT] = {{.scene = scene, .camera = flight, .output = {.rect = RECTS[FLIGHT]}, .antiAliasing = flightAa},
                                   {.scene = scene, .camera = top, .output = {.rect = RECTS[TOP]}, .antiAliasing = topAa}};

    std::vector<rasm::TileHandle> wanted;
    std::vector<rasm::ReadbackHandle> sequence;
    std::vector<uint32_t> sequenceFrames;
    if (!options.sequenceDir.empty())
        std::filesystem::create_directories(options.sequenceDir);
    StreamTotals totals;
    uint32_t failures = 0;
    uint32_t frame = 0;
    // The flight starts once the first cut is in (or after WARMUP_LIMIT frames), so a headless run's frames are of a
    // flight, not of its first loads.
    uint32_t flightStart = 0;
    double flightSeconds = 0.0;
    rasm::PickHandle pick;
    bool picked = false;
    const rasm::Window &window = engine.getMainWindow();
    // The streamed nodes are not the AssetLoader's loads: with --wait-for-loads the frames before the first cut is in are
    // not counted, so frame N is the flight's N-th, and exampleRunning counts from there.
    if (args.waitForLoads)
        engine.waitForLoads();
    while (args.waitForLoads && flightStart == 0 ? engine.running() : exampleRunning(engine, args))
    {
        ++frame;
        engine.pollEvents();
        const double dt = args.fixedStep || args.headless ? 1.0 / 60.0 : engine.getDeltaTime();
        if (flightStart > 0)
            flightSeconds += dt;
        glm::dvec3 eye;
        glm::dvec3 forward;
        getFlight(flightSeconds * options.speed, eye, forward);
        const glm::dvec3 worldEye = placement.origin + placement.rotation * eye;
        const glm::dvec3 worldForward = placement.rotation * forward;
        engine.setCameraTransform(flight, {worldEye, glm::quatLookAt(glm::vec3(worldForward), up), glm::vec3(1.0f)});
        const glm::dvec3 above = placement.origin + placement.rotation * glm::dvec3(eye.x + 60.0 * forward.x, 400.0, eye.z + 60.0 * forward.z);
        engine.setCameraTransform(top, {above, glm::quatLookAt(glm::vec3(-(placement.rotation * glm::dvec3(0.0, 1.0, 0.0))), glm::vec3(placement.rotation * glm::dvec3(0.0, 0.0, -1.0))), glm::vec3(1.0f)});

        // The cut, the streamer's update, and the loads it hands out.
        const auto updateStart = std::chrono::steady_clock::now();
        streamer.setViews(list, VIEW_COUNT);
        wanted.clear();
        walkCut(streamer, octree, nodes, 0, options.error, frame, wanted);
        streamer.update(wanted.data(), static_cast<uint32_t>(wanted.size()));
        uint32_t loadCount = 0;
        const rasm::TileHandle *toLoad = streamer.getTilesToLoad(loadCount);
        {
            std::lock_guard<std::mutex> lock(builders.mutex);
            for (uint32_t i = 0; i < loadCount; ++i)
                builders.queued.push_back({.tile = toLoad[i], .node = nodeOfTile[toLoad[i].index]});
        }
        builders.wake.notify_all();
        const double updateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - updateStart).count();
        const auto uploadStart = std::chrono::steady_clock::now();
        std::deque<BuildJob> done;
        {
            std::lock_guard<std::mutex> lock(builders.mutex);
            done.swap(builders.done);
        }
        for (BuildJob &job : done)
        {
            const rasm::PointCloudNode &node = octree.nodes[job.node];
            const rasm::MeshHandle mesh = engine.createMesh(std::move(job.build));
            const rasm::EntityHandle entity = engine.createEntity("node", mesh, material, rasm::Transform{node.center});
            // Hidden until it fades in.
            engine.setPointChunkLod(entity, {.drawn = options.fade > 0.0 ? 0.0f : 1.0f});
            // A tile the streamer cancelled meanwhile refuses its content and destroys it: the node keeps what it had.
            if (streamer.setTileContent(job.tile, {.entities = &entity, .entityCount = 1, .meshes = &mesh, .meshCount = 1}))
            {
                nodes[job.node].entity = entity;
                nodes[job.node].fade = 0.0f;
            }
        }
        const double uploadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadStart).count();

        // Fades: a drawn node moves toward shown; one not drawn starts again from nothing. Its parent's adaptive points
        // shrink in its octant as it fills it.
        const float step = options.fade > 0.0 ? static_cast<float>(dt / options.fade) : 1.0f;
        uint32_t appeared = 0;
        uint32_t vanished = 0;
        for (uint32_t n = 0; n < nodes.size(); ++n)
        {
            NodeState &state = nodes[n];
            const bool drawn = state.lastWanted == frame && streamer.isTileDrawn(state.tile);
            appeared += drawn && !state.drawn ? 1 : 0;
            vanished += !drawn && state.drawn ? 1 : 0;
            state.drawn = drawn;
            state.fade = drawn ? std::clamp(state.fade + (state.shown ? step : -step), 0.0f, 1.0f) : 0.0f;
        }
        uint32_t drawnCount = 0;
        for (uint32_t n = 0; n < nodes.size(); ++n)
        {
            const NodeState &state = nodes[n];
            if (state.lastWanted != frame || !streamer.isTileDrawn(state.tile))
                continue;
            ++drawnCount;
            rasm::PointChunkLod lod{.drawn = state.fade};
            for (uint32_t octant = 0; octant < 8; ++octant)
            {
                const uint32_t child = octree.nodes[n].children[octant];
                lod.finer[octant] = child != rasm::POINT_NODE_NONE ? nodes[child].fade : 0.0f;
            }
            engine.setPointChunkLod(state.entity, lod);
        }

        const rasm::TileStreamerStats &stats = streamer.getStats();
        if (flightStart == 0 && ((stats.loading == 0 && stats.waitingForBudget == 0 && loadCount == 0 && frame > 2) || frame >= WARMUP_LIMIT))
        {
            flightStart = frame;
            std::printf("point streaming: the first cut is in at frame %u (%u nodes drawn, %.1f MiB)\n", frame, drawnCount, static_cast<double>(stats.bytes) / 1048576.0);
        }
        uint64_t wantedBytes = 0;
        for (uint32_t n = 0; n < nodes.size() && options.stats; ++n)
            wantedBytes += nodes[n].lastWanted == frame ? static_cast<uint64_t>(octree.nodes[n].pointCount) * pointBytes : 0;
        if (options.stats && flightStart > 0 && (appeared > 0 || vanished > 0))
            std::printf("frame %4u: %u nodes in, %u out\n", frame, appeared, vanished);
        if (options.stats && frame % 30 == 0)
            std::printf("frame %4u: %3u wanted (%.1f MiB), %3u drawn, %2u loading, %3u waiting for the budget, %u holes, %.2f of %.2f MiB, %llu loads, %llu drops, update %.3f ms\n", frame,
                        stats.wanted, static_cast<double>(wantedBytes) / 1048576.0, drawnCount, stats.loading, stats.waitingForBudget, stats.holes, static_cast<double>(stats.bytes) / 1048576.0,
                        static_cast<double>(stats.budget) / 1048576.0, static_cast<unsigned long long>(stats.totalLoads), static_cast<unsigned long long>(stats.totalDrops), updateMs);
        if (stats.bytes > stats.budget && stats.budget > 0)
        {
            std::printf("point streaming check budget FAIL: frame %u holds %.2f MiB of %.2f\n", frame, static_cast<double>(stats.bytes) / 1048576.0, static_cast<double>(stats.budget) / 1048576.0);
            ++failures;
        }
        if (check && flightStart > 0 && stats.holes > 0)
        {
            std::printf("point streaming check holes FAIL: frame %u has %u holes\n", frame, stats.holes);
            ++failures;
        }
        if (flightStart > 0)
        {
            ++totals.frames;
            totals.updateMs += updateMs;
            totals.maxUpdateMs = std::max(totals.maxUpdateMs, updateMs);
            totals.uploadMs += uploadMs;
            totals.maxUploadMs = std::max(totals.maxUploadMs, uploadMs);
            totals.uploads += done.size();
            if (engine.getFrameStats().cpuFrameMs > totals.maxFrameMs)
            {
                totals.maxFrameMs = engine.getFrameStats().cpuFrameMs;
                totals.maxFrame = frame;
            }
            totals.peakBytes = std::max(totals.peakBytes, stats.bytes);
            totals.maxWaiting = std::max(totals.maxWaiting, stats.waitingForBudget);
            totals.maxDrawn = std::max(totals.maxDrawn, drawnCount);
        }

        if (check && flightStart > 0 && frame == flightStart + PICK_FRAME)
        {
            const rasm::ViewPixels pixels = rasm::getViewPixels(RECTS[FLIGHT], window.pixelWidth, window.pixelHeight);
            pick = engine.pick(list[FLIGHT], {.rect = {.x = pixels.width / 2, .y = pixels.height / 2, .width = 1, .height = 1}});
        }
        if (args.headless && !options.sequenceDir.empty() && options.sequenceFrom > 0 && frame >= options.sequenceFrom && frame < args.frames)
        {
            sequence.push_back(engine.readFrame());
            sequenceFrames.push_back(frame);
        }
        engine.render(list, VIEW_COUNT);
        addBenchFrame(bench, engine.getFrameStats());
        while (!sequence.empty() && engine.isReadbackReady(sequence.front()))
        {
            rasm::ReadbackImage image;
            char name[64];
            std::snprintf(name, sizeof(name), "/frame_%04u.png", sequenceFrames.front());
            if (!engine.takeReadback(sequence.front(), image) || !rasm::writePng(options.sequenceDir + name, image))
                ++failures;
            sequence.erase(sequence.begin());
            sequenceFrames.erase(sequenceFrames.begin());
        }

        rasm::PickHit hits[MAX_HITS];
        uint32_t hitCount = 0;
        if (pick.isValid() && engine.isPickReady(pick))
        {
            // The hit's feature is its index in the node's chunk, which the octree turns into the file's point.
            bool ok = engine.takePick(pick, hits, MAX_HITS, hitCount) && hitCount > 0;
            double error = -1.0;
            const auto found = ok ? std::find_if(nodes.begin(), nodes.end(), [&](const NodeState &state)
                                                 { return state.entity == hits[0].entity; })
                                  : nodes.end();
            ok = ok && found != nodes.end();
            if (ok)
            {
                const rasm::PointCloudNode &node = octree.nodes[static_cast<size_t>(found - nodes.begin())];
                ok = hits[0].feature < node.pointCount;
                if (ok)
                {
                    error = glm::length(hits[0].position - cloud.positions[octree.indices[node.firstPoint + hits[0].feature]]);
                    ok = error < 0.01;
                }
            }
            std::printf("point streaming check pick %s: %u hits, %.4f m from the file's point\n", ok ? "ok  " : "FAIL", hitCount, error);
            failures += ok ? 0 : 1;
            pick = {};
            picked = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(builders.mutex);
        builders.stop = true;
    }
    builders.wake.notify_all();
    for (std::thread &thread : builders.threads)
        thread.join();

    const rasm::TileStreamerStats &stats = streamer.getStats();
    if (totals.frames > 0)
        std::printf("point streaming: %u frames of flight, %.1f s; budget %.1f MiB, peak %.2f MiB; %llu loads, %llu drops; cut and update %.3f ms (max %.3f); %llu chunks made, %.3f ms a frame (max %.3f); longest frame %.2f ms (CPU, frame %u); up to %u "
                    "nodes drawn, up to %u waiting for the budget\n",
                    totals.frames, flightSeconds, static_cast<double>(stats.budget) / 1048576.0, static_cast<double>(totals.peakBytes) / 1048576.0,
                    static_cast<unsigned long long>(stats.totalLoads), static_cast<unsigned long long>(stats.totalDrops), totals.updateMs / totals.frames, totals.maxUpdateMs, static_cast<unsigned long long>(totals.uploads), totals.uploadMs / totals.frames, totals.maxUploadMs, totals.maxFrameMs, totals.maxFrame,
                    totals.maxDrawn, totals.maxWaiting);
    if (bench.on && bench.measured > 0)
        std::printf("  %-22s %8.2f M a frame\n", "points drawn", bench.triangles / bench.measured / 1e6);
    printBench(bench, config, engine.getRenderContext().hasMeshShaders());
    // A run too short to reach the pick does not check it.
    if (check && pick.isValid() && !picked)
    {
        std::printf("point streaming check pick FAIL: no answer before the run ended\n");
        ++failures;
    }
    if (check)
        std::printf("point streaming checks: %s (%u failed)\n", failures == 0 ? "all passed" : "FAILED", failures);
    return failures > 0 ? 1 : exampleExit(args);
}
