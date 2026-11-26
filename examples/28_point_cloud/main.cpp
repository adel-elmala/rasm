#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"
#include "exampleBench.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// Point cloud: a synthetic LiDAR scan of hills, buildings and trees, 6 000 km from the world's origin, colored by height
// through a colormap. The scan is cut into square tiles of 32 m, each a chunk of up to 64 K points (buildPointCloudChunk,
// on worker threads) and an entity placed in doubles. The left view is perspective with TAA, the right one an
// orthographic top view with 4x MSAA. Far chunks draw only the first points of their order (LOD by prefix). Points of
// 8 pixels or less (2 in the MSAA view) are written by a compute pass with 64-bit atomics where the GPU has them, the
// others by triangles.
// - Drag to orbit (left view) or pan (right view), wheel to zoom.
// A headless run (or --check) orbits the left view and picks the point under the right view's centre, which must be
// the point of the scan the shader drew there. A failed check exits with 1.
//   --points N     about N points in all (4 000 000 by default)
//   --rgb          the points' own colors (12 B a point) instead of their height through the colormap (8 B)
//   --shape S      square, round (default) or sphere
//   --size P       the points' size in pixels (2 by default); --metres M in metres instead
//   --density D    points kept per point's area of a chunk on screen (2 by default; PointMaterial::density)
//   --all-points   no LOD: every point of every chunk in view (density 0)
//   --swap-aa      the left view with 4x MSAA, the right one with TAA
//   --still        no scripted orbit
//   --zoom         a scripted slow zoom: in on the left view, out on the right one (the chunks' LOD changes)
//   --triangles    every point by triangles (Renderer::setComputePoints off)
//   --adaptive     the points grow near the camera where the scan is sparser than their size on screen
//                  (PointSizeMode::ADAPTIVE, up to 8 pixels), so the near ground closes; in the left view they stay
//                  on the compute path, which takes points of up to 8 pixels where a pixel has one sample
//   --bench        the GPU time per pass (exampleBench.h), at 1920 x 1080

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr double TILE_SIZE = 32.0;
    constexpr uint32_t TILE_POINTS = 60000;
    constexpr uint32_t PICK_FRAME = 24;
    constexpr uint32_t CHECK_DEADLINE = 60;
    constexpr uint32_t MAX_HITS = 8;
    constexpr float MIN_HEIGHT = -24.0f;
    constexpr float MAX_HEIGHT = 48.0f;
    constexpr glm::dvec3 FAR_ORIGIN = glm::dvec3(4.1e6, 320.0, -4.7e6);

    enum ViewIndex : uint32_t
    {
        PERSPECTIVE,
        TOP,
        VIEW_COUNT,
    };

    constexpr rasm::ViewRect RECTS[VIEW_COUNT] = {{.x = 0.0f, .y = 0.0f, .width = 0.5f, .height = 1.0f}, {.x = 0.5f, .y = 0.0f, .width = 0.5f, .height = 1.0f}};

    struct ViewCamera
    {
        rasm::CameraHandle camera;
        rasm::CameraRig rig;
        rasm::CameraInputState input;
        rasm::CameraProjection projection;
    };

    struct Options
    {
        uint64_t points = 4000000;
        bool rgb = false;
        rasm::PointShape shape = rasm::PointShape::ROUND;
        float size = 2.0f;
        rasm::VectorUnit unit = rasm::VectorUnit::PIXELS;
        float density = 2.0f;
        bool swapAa = false;
        bool still = false;
        bool zoom = false;
        bool triangles = false;
        bool adaptive = false;
    };

    struct TilePoints
    {
        std::vector<glm::vec3> positions;
        std::vector<float> scalars;
        std::vector<uint32_t> colors;
    };

    // Each tile has one building (a box on the ground) and a few trees (spheres of leaves on a trunk).
    struct TileLayout
    {
        glm::vec2 buildingMin;
        glm::vec2 buildingMax;
        float roof;
        glm::vec3 trees[4];
        float crowns[4];
    };

    struct Rng
    {
        uint64_t state;

        uint32_t next()
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const auto shifted = static_cast<uint32_t>(((state >> 18u) ^ state) >> 27u);
            const auto rot = static_cast<uint32_t>(state >> 59u);
            return (shifted >> rot) | (shifted << ((32u - rot) & 31u));
        }

        float uniform()
        {
            return static_cast<float>(next() >> 8) / 16777216.0f;
        }
    };
}

namespace
{
    // Rolling hills in metres, from the scan's own coordinates.
    float groundHeight(double x, double z)
    {
        return static_cast<float>(14.0 * std::sin(x / 95.0) * std::cos(z / 120.0) + 5.0 * std::sin((x + z) / 47.0) + 1.5 * std::sin(x / 13.0) * std::sin(z / 17.0));
    }

    TileLayout getTileLayout(int tx, int tz)
    {
        Rng rng{static_cast<uint64_t>(tx * 92821 + tz * 68917 + 1234567)};
        TileLayout layout{};
        const glm::vec2 size(6.0f + 10.0f * rng.uniform(), 6.0f + 10.0f * rng.uniform());
        const glm::vec2 corner(2.0f + (static_cast<float>(TILE_SIZE) - 4.0f - size.x) * rng.uniform(), 2.0f + (static_cast<float>(TILE_SIZE) - 4.0f - size.y) * rng.uniform());
        layout.buildingMin = corner;
        layout.buildingMax = corner + size;
        const double cx = tx * TILE_SIZE + corner.x + 0.5 * size.x;
        const double cz = tz * TILE_SIZE + corner.y + 0.5 * size.y;
        layout.roof = groundHeight(cx, cz) + 6.0f + 20.0f * rng.uniform() * rng.uniform();
        for (uint32_t i = 0; i < 4; ++i)
        {
            const float x = static_cast<float>(TILE_SIZE) * rng.uniform();
            const float z = static_cast<float>(TILE_SIZE) * rng.uniform();
            layout.crowns[i] = 2.0f + 2.5f * rng.uniform();
            layout.trees[i] = glm::vec3(x, groundHeight(tx * TILE_SIZE + x, tz * TILE_SIZE + z) + 3.0f + layout.crowns[i], z);
        }
        return layout;
    }

    bool insideBuilding(const TileLayout &layout, float x, float z)
    {
        return x >= layout.buildingMin.x && x <= layout.buildingMax.x && z >= layout.buildingMin.y && z <= layout.buildingMax.y;
    }

    // A tile's points relative to its corner: 70% ground (or roof), 12% walls, 18% tree crowns, the same every run.
    TilePoints makeTilePoints(int tx, int tz, uint32_t count, bool rgb)
    {
        const TileLayout layout = getTileLayout(tx, tz);
        Rng rng{static_cast<uint64_t>(tx * 73856093 ^ tz * 19349663) + 99991u};
        TilePoints points;
        points.positions.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const float kind = rng.uniform();
            glm::vec3 p(0.0f);
            if (kind < 0.7f)
            {
                p.x = static_cast<float>(TILE_SIZE) * rng.uniform();
                p.z = static_cast<float>(TILE_SIZE) * rng.uniform();
                p.y = insideBuilding(layout, p.x, p.z) ? layout.roof : groundHeight(tx * TILE_SIZE + p.x, tz * TILE_SIZE + p.z);
                p.y += 0.03f * (rng.uniform() - 0.5f);
            }
            else if (kind < 0.82f)
            {
                const glm::vec2 lo = layout.buildingMin;
                const glm::vec2 hi = layout.buildingMax;
                const float t = rng.uniform();
                const auto side = static_cast<uint32_t>(rng.uniform() * 4.0f) % 4;
                p.x = side < 2 ? lo.x + t * (hi.x - lo.x) : (side == 2 ? lo.x : hi.x);
                p.z = side < 2 ? (side == 0 ? lo.y : hi.y) : lo.y + t * (hi.y - lo.y);
                const float ground = groundHeight(tx * TILE_SIZE + p.x, tz * TILE_SIZE + p.z);
                p.y = ground + (layout.roof - ground) * rng.uniform();
            }
            else
            {
                const auto tree = static_cast<uint32_t>(rng.uniform() * 4.0f) % 4;
                const glm::vec3 d = glm::normalize(glm::vec3(rng.uniform() - 0.5f, rng.uniform() - 0.5f, rng.uniform() - 0.5f) + glm::vec3(0.0f, 1e-4f, 0.0f));
                p = layout.trees[tree] + d * layout.crowns[tree] * std::sqrt(rng.uniform());
            }
            points.positions.push_back(p);
            points.scalars.push_back(p.y);
            if (rgb)
            {
                const float shade = 0.75f + 0.25f * rng.uniform();
                const glm::vec3 color = kind < 0.7f ? (insideBuilding(layout, p.x, p.z) ? glm::vec3(0.55f, 0.27f, 0.2f) : glm::vec3(0.35f, 0.42f, 0.2f)) : (kind < 0.82f ? glm::vec3(0.7f) : glm::vec3(0.12f, 0.35f, 0.1f));
                points.colors.push_back(rasm::packVertexColor(glm::vec4(color * shade, 1.0f)));
            }
        }
        return points;
    }

    rasm::CameraProjection orthographic(float halfWidth, float aspect)
    {
        const float halfHeight = halfWidth / aspect;
        return {.type = rasm::CameraProjection::Type::ORTHOGRAPHIC, .orthographic{.left = -halfWidth, .right = halfWidth, .bottom = -halfHeight, .top = halfHeight, .nearZ = 1.0f, .farZ = 2000.0f}};
    }

    ViewCamera createViewCamera(rasm::Engine &engine, rasm::SceneHandle scene, const rasm::CameraProjection &projection, const glm::dvec3 &center, const glm::dvec3 &direction, double distance,
                                const glm::vec3 &up, float orbit)
    {
        ViewCamera view;
        view.projection = projection;
        const glm::dvec3 eye = center - direction * distance;
        const rasm::Transform transform{eye, glm::quatLookAt(glm::normalize(glm::vec3(center - eye)), up), glm::vec3(1.0f)};
        view.camera = engine.createCamera(projection, transform);
        engine.setCameraController(view.camera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
        engine.addCameraToScene(scene, view.camera);
        view.rig = rasm::createCameraRig(
            rasm::CameraController{.type = rasm::CameraController::Type::TRACK_BALL, .trackBall = {.radius = static_cast<float>(distance), .center = center, .orbitSensitivity = orbit}, .easingSpeed = 0.0f},
            transform);
        return view;
    }

    void updateViewCamera(rasm::Engine &engine, ViewCamera &view, const rasm::ViewRect &rect)
    {
        const rasm::Window &window = engine.getMainWindow();
        const rasm::ViewPixels pixels = rasm::getViewPixels(rect, window.pixelWidth, window.pixelHeight);
        if (view.projection.type == rasm::CameraProjection::Type::ORTHOGRAPHIC && pixels.width > 0 && pixels.height > 0)
        {
            rasm::CameraProjection::Orthographic &box = view.projection.orthographic;
            const float halfHeight = 0.5f * (box.right - box.left) * static_cast<float>(pixels.height) / static_cast<float>(pixels.width);
            const float middle = 0.5f * (box.bottom + box.top);
            box.bottom = middle - halfHeight;
            box.top = middle + halfHeight;
        }
        const rasm::InputArea area = rasm::getInputArea(window, rect);
        const rasm::UiInputCapture capture = engine.getUiInputCapture();
        uint32_t count = 0;
        const rasm::InputEvent *events = engine.getInputEvents(count);
        for (uint32_t i = 0; i < count; ++i)
            rasm::addCameraInputEvent(view.input, events[i], area, capture);
        rasm::updateCameraRig(view.rig, view.projection, rasm::takeCameraInput(view.input, capture, engine.getDeltaTime()));
        engine.setCameraTransform(view.camera, view.rig.transform);
        engine.setCameraProjection(view.camera, view.projection);
    }

    Options parseOptions(int argc, char **argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const bool value = i + 1 < argc;
            if (value && std::strcmp(argv[i], "--points") == 0)
                options.points = std::max<uint64_t>(1, std::strtoull(argv[++i], nullptr, 10));
            else if (value && std::strcmp(argv[i], "--shape") == 0)
            {
                const char *shape = argv[++i];
                options.shape = std::strcmp(shape, "square") == 0 ? rasm::PointShape::SQUARE : (std::strcmp(shape, "sphere") == 0 ? rasm::PointShape::SPHERE : rasm::PointShape::ROUND);
            }
            else if (value && std::strcmp(argv[i], "--size") == 0)
                options.size = static_cast<float>(std::atof(argv[++i]));
            else if (value && std::strcmp(argv[i], "--metres") == 0)
            {
                options.size = static_cast<float>(std::atof(argv[++i]));
                options.unit = rasm::VectorUnit::METRES;
            }
            else if (value && std::strcmp(argv[i], "--density") == 0)
                options.density = std::max(0.0f, static_cast<float>(std::atof(argv[++i])));
            options.rgb = options.rgb || std::strcmp(argv[i], "--rgb") == 0;
            if (std::strcmp(argv[i], "--all-points") == 0)
                options.density = 0.0f;
            options.swapAa = options.swapAa || std::strcmp(argv[i], "--swap-aa") == 0;
            options.still = options.still || std::strcmp(argv[i], "--still") == 0;
            options.zoom = options.zoom || std::strcmp(argv[i], "--zoom") == 0;
            options.triangles = options.triangles || std::strcmp(argv[i], "--triangles") == 0;
            options.adaptive = options.adaptive || std::strcmp(argv[i], "--adaptive") == 0;
        }
        return options;
    }

    // The pick's check: its nearest hit is a point of a chunk, at the place the scan has that point.
    bool checkPick(const rasm::PickHit *hits, uint32_t count, const std::vector<rasm::EntityHandle> &chunks, int tilesPerSide, uint32_t tilePoints, const glm::dvec3 &origin, bool rgb)
    {
        const auto found = count > 0 ? std::find(chunks.begin(), chunks.end(), hits[0].entity) : chunks.end();
        bool ok = found != chunks.end() && (hits[0].flags & rasm::PICK_HIT_POINT) != 0 && hits[0].feature < tilePoints;
        double error = 0.0;
        if (ok)
        {
            const auto tile = static_cast<int>(found - chunks.begin());
            const int tx = tile % tilesPerSide;
            const int tz = tile / tilesPerSide;
            const TilePoints points = makeTilePoints(tx, tz, tilePoints, rgb);
            const glm::dvec3 expected = origin + glm::dvec3(tx * TILE_SIZE, 0.0, tz * TILE_SIZE) + glm::dvec3(points.positions[hits[0].feature]);
            error = glm::length(hits[0].position - expected);
            ok = error < 0.01;
        }
        std::printf("point cloud check %-14s %s: %s, point %u, %.4f m from the scan's point\n", "pick", ok ? "ok  " : "FAIL", count == 0 ? "no hit" : (found == chunks.end() ? "not a chunk" : "a chunk"),
                    count > 0 ? hits[0].feature : 0u, error);
        return ok;
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    const Options options = parseOptions(argc, argv);
    bool check = args.headless;
    for (int i = 1; i < argc; ++i)
        check = check || std::strcmp(argv[i], "--check") == 0;

    const auto tilesPerSide = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(options.points) / TILE_POINTS)));
    const auto tileCount = static_cast<uint32_t>(tilesPerSide * tilesPerSide);
    const auto tilePoints = static_cast<uint32_t>(std::min<uint64_t>((options.points + tileCount - 1) / tileCount, rasm::MAX_POINT_CHUNK_POINTS));
    rasm::EngineConfig config = {
        .appName = "RASM: point cloud",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR};
    // A chunk takes its header and a unit of 8 B a point, half a unit more with colors.
    config.geometryPoolPoints = static_cast<uint32_t>(tileCount * (8 + tilePoints + (options.rgb ? tilePoints / 2 + 1 : 0)));
    config.computePoints = !options.triangles;
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

    // The chunks are built on worker threads, then uploaded here.
    std::vector<rasm::MeshBuild> builds(tileCount);
    const uint32_t threads = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
    std::vector<std::thread> workers;
    for (uint32_t w = 0; w < threads; ++w)
    {
        workers.emplace_back([&, w]()
                             {
                                 for (uint32_t tile = w; tile < tileCount; tile += threads)
                                 {
                                     const TilePoints points = makeTilePoints(static_cast<int>(tile) % tilesPerSide, static_cast<int>(tile) / tilesPerSide, tilePoints, options.rgb);
                                     builds[tile] = rasm::buildPointCloudChunk({.positions = points.positions.data(),
                                                                                 .colors = options.rgb ? points.colors.data() : nullptr,
                                                                                 .scalars = options.rgb ? nullptr : points.scalars.data(),
                                                                                 .count = tilePoints});
                                 } });
    }
    for (std::thread &worker : workers)
        worker.join();

    const rasm::SceneHandle scene = engine.createScene();
    const rasm::TextureHandle colormap = engine.createColormap(rasm::ColormapPreset::TURBO);
    const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::POINTS);
    rasm::Material style = engine.getMaterial(material);
    style.points = {.size = options.size,
                    .sizeUnit = options.unit,
                    .shape = options.shape,
                    .colorMode = options.rgb ? rasm::PointColorMode::COLOR : rasm::PointColorMode::SCALAR,
                    .colormap = colormap,
                    .range = {.min = MIN_HEIGHT, .max = MAX_HEIGHT},
                    .density = options.density,
                    .sizeMode = options.adaptive ? rasm::PointSizeMode::ADAPTIVE : rasm::PointSizeMode::FIXED};
    engine.updateMaterial(material, style);
    const glm::dvec3 origin = FAR_ORIGIN;
    std::vector<rasm::EntityHandle> chunks(tileCount);
    for (uint32_t tile = 0; tile < tileCount; ++tile)
    {
        const glm::dvec3 corner = origin + glm::dvec3((tile % tilesPerSide) * TILE_SIZE, 0.0, (tile / tilesPerSide) * TILE_SIZE);
        chunks[tile] = engine.createEntity("chunk", engine.createMesh(std::move(builds[tile])), material, rasm::Transform{corner});
        engine.addEntityToScene(scene, chunks[tile]);
    }
    builds.clear();
    const double side = tilesPerSide * TILE_SIZE;
    std::printf("point cloud: %u chunks of %u points (%.1f M), %.0f m across, %u B a point, small points by %s\n", tileCount, tilePoints, tileCount * static_cast<double>(tilePoints) / 1e6, side,
                options.rgb ? 12u : 8u, engine.getComputePoints() && engine.getRenderContext().hasBufferInt64Atomics() ? "compute" : "triangles");

    const float aspect = 0.5f * static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight);
    const glm::dvec3 center = origin + glm::dvec3(0.5 * side, 0.0, 0.5 * side);
    ViewCamera views[VIEW_COUNT] = {
        createViewCamera(engine, scene, {.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = 50.0f, .aspect = aspect, .nearZ = 0.5f}}, center,
                         glm::normalize(glm::dvec3(-0.55, -0.42, -0.72)), 0.75 * side, glm::vec3(0.0f, 1.0f, 0.0f), 1.0f),
        createViewCamera(engine, scene, orthographic(static_cast<float>(0.55 * side), aspect), center + glm::dvec3(0.0, 100.0, 0.0), glm::dvec3(0.0, -1.0, 0.0), 400.0,
                         glm::vec3(0.0f, 0.0f, -1.0f), 0.0f),
    };
    views[TOP].input.buttons.left = rasm::CameraDrag::PAN;
    rasm::View list[VIEW_COUNT];
    const rasm::AntiAliasing perspectiveAa = options.swapAa ? rasm::AntiAliasing::MSAA_4X : rasm::AntiAliasing::TAA;
    const rasm::AntiAliasing topAa = options.swapAa ? rasm::AntiAliasing::TAA : rasm::AntiAliasing::MSAA_4X;
    for (uint32_t i = 0; i < VIEW_COUNT; ++i)
        list[i] = {.scene = scene, .camera = views[i].camera, .output = {.rect = RECTS[i]}, .antiAliasing = i == PERSPECTIVE ? perspectiveAa : topAa};

    if (args.waitForLoads || check)
        engine.waitForLoads();

    const rasm::Window &window = engine.getMainWindow();
    const float quarterWidth = 0.25f * static_cast<float>(window.width);
    const float halfHeight = 0.5f * static_cast<float>(window.height);
    uint32_t failures = 0;
    uint32_t checksLeft = check ? 1 : 0;
    rasm::PickHandle pick;
    uint32_t frame = 0;
    while (exampleRunning(engine, args))
    {
        ++frame;
        // A headless run orbits the left view: a drag that goes on for the whole run.
        if ((check || bench.on) && !options.still && !options.zoom)
        {
            if (frame == 2)
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_BUTTON, .down = true, .x = quarterWidth, .y = halfHeight});
            else if (frame > 2)
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_MOVE, .x = quarterWidth + 2.0f * static_cast<float>(frame - 2), .y = halfHeight});
        }
        // The wheel at each view's centre, a tenth of a step a frame.
        if (options.zoom && frame > 2)
        {
            engine.addInputEvent({.type = rasm::InputEventType::MOUSE_MOVE, .x = quarterWidth, .y = halfHeight});
            engine.addInputEvent({.type = rasm::InputEventType::MOUSE_WHEEL, .y = 0.1f});
            engine.addInputEvent({.type = rasm::InputEventType::MOUSE_MOVE, .x = 3.0f * quarterWidth, .y = halfHeight});
            engine.addInputEvent({.type = rasm::InputEventType::MOUSE_WHEEL, .y = -0.1f});
        }
        engine.pollEvents();
        for (ViewCamera &view : views)
            updateViewCamera(engine, view, RECTS[&view - views]);
        engine.setMouseGrab(views[PERSPECTIVE].input.dragActive || views[TOP].input.dragActive);
        if (check && frame == PICK_FRAME)
        {
            const rasm::ViewPixels pixels = rasm::getViewPixels(RECTS[TOP], window.pixelWidth, window.pixelHeight);
            pick = engine.pick(list[TOP], {.rect = {.x = pixels.width / 2, .y = pixels.height / 2, .width = 1, .height = 1}});
        }
        engine.render(list, VIEW_COUNT);
        addBenchFrame(bench, engine.getFrameStats());

        rasm::PickHit hits[MAX_HITS];
        uint32_t hitCount = 0;
        if (pick.isValid() && engine.isPickReady(pick))
        {
            failures += engine.takePick(pick, hits, MAX_HITS, hitCount) && checkPick(hits, hitCount, chunks, tilesPerSide, tilePoints, origin, options.rgb) ? 0 : 1;
            pick = {};
            --checksLeft;
        }
        if (check && frame == CHECK_DEADLINE && checksLeft > 0)
        {
            std::printf("point cloud check: %u checks got no answer by frame %u\n", checksLeft, CHECK_DEADLINE);
            failures += checksLeft;
            checksLeft = 0;
        }
    }
    if (bench.on && bench.measured > 0)
        std::printf("  %-22s %8.2f M a frame\n", "points drawn", bench.triangles / bench.measured / 1e6);
    printBench(bench, config, engine.getRenderContext().hasMeshShaders());
    if (check && checksLeft > 0)
    {
        std::printf("point cloud check: %u checks got no answer before the run ended\n", checksLeft);
        failures += checksLeft;
    }
    if (check)
        std::printf("point cloud checks: %s (%u failed)\n", failures == 0 ? "all passed" : "FAILED", failures);
    return failures > 0 ? 1 : exampleExit(args);
}
