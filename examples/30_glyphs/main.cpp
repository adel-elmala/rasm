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
#include <vector>

// Glyphs: a 3D vector field (the ABC flow) in a box 240 m wide, 6 000 km from the world's origin. An arrow per grid
// point (a glyph chunk of a block of the grid each: buildGlyphChunk, an entity placed in doubles) points along the flow,
// sized and colored by its speed through viridis; streamlines traced through the field (addVectorLines with a scalar per
// point) are colored by the speed through plasma. The left view is perspective with TAA, the right one an orthographic
// top view with 4x MSAA. Each frame a compute pass culls every arrow of the chunks in view and picks its LOD, and one
// indirect draw draws the visible ones.
// - Drag to orbit (left view) or pan (right view), wheel to zoom.
// A headless run (or --check) orbits the left view, zooms into the right one, and picks the arrow under the right view's
// centre, which must be the arrow the field has there. A failed check exits with 1.
//   --glyphs N     about N arrows in all (400 000 by default)
//   --lines N      streamlines (768 by default, 0: none)
//   --shape S      arrow (default), sphere or cube
//   --still        no scripted orbit and zoom
//   --swap-aa      the left view with 4x MSAA, the right one with TAA
//   --at-origin    the box at the world's origin instead
//   --wall         a wall in front of the box: the arrows behind it fail the Hi-Z test and are not drawn
//   --bench        the GPU time per pass (exampleBench.h), at 1920 x 1080

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr double BOX_SIZE = 240.0;
    constexpr double BOX_HEIGHT = 80.0;
    constexpr uint32_t BLOCK = 32; // a chunk is a block of BLOCK x BLOCK_HEIGHT x BLOCK arrows
    constexpr uint32_t BLOCK_HEIGHT = 8;
    constexpr uint32_t LINES_PER_MESH = 48;
    constexpr uint32_t LINE_POINTS = 120;
    constexpr double LINE_STEP = 1.0; // metres between a streamline's points
    constexpr uint32_t PICK_FRAME = 24;
    constexpr uint32_t CHECK_DEADLINE = 60;
    constexpr uint32_t MAX_HITS = 8;
    constexpr float SPEED_SCALE = 4.0f;
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
        uint64_t glyphs = 400000;
        uint32_t lines = 768;
        rasm::GlyphShape shape = rasm::GlyphShape::ARROW;
        bool still = false;
        bool swapAa = false;
        bool atOrigin = false;
        bool wall = false;
    };

    // The arrows: a grid of nx x ny x nz over the box, cut in blocks.
    struct FieldGrid
    {
        uint32_t nx = 0;
        uint32_t ny = 0;
        uint32_t nz = 0;
        double spacing = 0.0;
        uint32_t blocksX = 0;
        uint32_t blocksY = 0;
        uint32_t blocksZ = 0;
    };

    struct Block
    {
        glm::dvec3 corner;                // from the box's corner
        std::vector<glm::vec3> positions; // from the block's corner
        std::vector<glm::quat> orientations;
        std::vector<glm::vec3> scales;
        std::vector<float> speeds;
    };
}

namespace
{
    // The ABC (Arnold-Beltrami-Childress) flow over the box: smooth, three-dimensional, with fast jets and slow cores.
    glm::dvec3 flow(const glm::dvec3 &p)
    {
        constexpr double TAU = 6.283185307179586;
        constexpr double A = 1.0;
        const double b = std::sqrt(2.0 / 3.0);
        const double c = std::sqrt(1.0 / 3.0);
        const double kx = TAU / BOX_SIZE;
        const double ky = TAU / BOX_HEIGHT;
        return static_cast<double>(SPEED_SCALE) * glm::dvec3(A * std::sin(kx * p.z) + c * std::cos(ky * p.y), b * std::sin(kx * p.x) + A * std::cos(kx * p.z), c * std::sin(ky * p.y) + b * std::cos(kx * p.x));
    }

    float maxSpeed()
    {
        return SPEED_SCALE * static_cast<float>(1.0 + std::sqrt(2.0 / 3.0) + std::sqrt(1.0 / 3.0));
    }

    // A cell's arrow, from the box's corner: anywhere in its cell (its centre moved by up to half the spacing along each
    // axis), the same every run. A regular lattice of arrows seen in perspective lines up into stripes; a jittered one
    // does not, and seen from above leaves no lattice of holes.
    glm::dvec3 getCellPoint(const glm::dvec3 &corner, const glm::dvec3 &step, uint32_t x, uint32_t y, uint32_t z)
    {
        const glm::dvec3 cell = glm::round(corner / step) + glm::dvec3(x, y, z);
        uint32_t hash = static_cast<uint32_t>(cell.x) * 73856093u ^ static_cast<uint32_t>(cell.y) * 19349663u ^ static_cast<uint32_t>(cell.z) * 83492791u;
        glm::dvec3 jitter(0.0);
        for (int axis = 0; axis < 3; ++axis)
        {
            hash = hash * 1664525u + 1013904223u;
            jitter[axis] = static_cast<double>(hash >> 8) / 16777216.0 - 0.5;
        }
        return (cell + 0.5 + jitter) * step;
    }

    // The same spacing along every axis, as near as whole blocks allow: (BOX_SIZE / s)^2 (BOX_HEIGHT / s) = glyphs.
    FieldGrid makeGrid(uint64_t glyphs)
    {
        FieldGrid grid;
        const double spacing = std::cbrt(BOX_SIZE * BOX_SIZE * BOX_HEIGHT / static_cast<double>(glyphs));
        grid.blocksX = std::max(1u, static_cast<uint32_t>(std::lround(BOX_SIZE / spacing / BLOCK)));
        grid.blocksY = std::max(1u, static_cast<uint32_t>(std::lround(BOX_HEIGHT / spacing / BLOCK_HEIGHT)));
        grid.blocksZ = grid.blocksX;
        grid.nx = grid.blocksX * BLOCK;
        grid.ny = grid.blocksY * BLOCK_HEIGHT;
        grid.nz = grid.nx;
        grid.spacing = BOX_SIZE / grid.nx;
        return grid;
    }

    // An arrow at the centre of each cell of the block, along the flow, its size and its scalar the speed.
    Block makeBlock(const FieldGrid &grid, uint32_t bx, uint32_t by, uint32_t bz)
    {
        Block block;
        block.corner = glm::dvec3(bx * BLOCK, by * BLOCK_HEIGHT, bz * BLOCK) * glm::dvec3(grid.spacing, BOX_HEIGHT / grid.ny, grid.spacing);
        const glm::dvec3 step(grid.spacing, BOX_HEIGHT / grid.ny, grid.spacing);
        const float top = maxSpeed();
        for (uint32_t y = 0; y < BLOCK_HEIGHT; ++y)
        {
            for (uint32_t z = 0; z < BLOCK; ++z)
            {
                for (uint32_t x = 0; x < BLOCK; ++x)
                {
                    const glm::dvec3 local = getCellPoint(block.corner, step, x, y, z) - block.corner;
                    const glm::dvec3 v = flow(block.corner + local);
                    const auto speed = static_cast<float>(glm::length(v));
                    const float size = static_cast<float>(grid.spacing) * (0.25f + 0.85f * speed / top);
                    block.positions.push_back(glm::vec3(local));
                    block.orientations.push_back(rasm::getGlyphOrientation(glm::vec3(v)));
                    block.scales.push_back(glm::vec3(size));
                    block.speeds.push_back(speed);
                }
            }
        }
        return block;
    }

    // A streamline from a seed: RK4 steps of LINE_STEP metres along the flow, until it leaves the box. Its scalars are
    // the speed at each point.
    void traceLine(const glm::dvec3 &seed, const glm::dvec3 &origin, std::vector<glm::dvec3> &points, std::vector<float> &speeds, std::vector<uint32_t> &sizes)
    {
        glm::dvec3 p = seed;
        uint32_t count = 0;
        auto direction = [](const glm::dvec3 &q)
        {
            const glm::dvec3 v = flow(q);
            return v / std::max(glm::length(v), 1e-6);
        };
        for (uint32_t i = 0; i < LINE_POINTS; ++i)
        {
            if (p.x < 0.0 || p.x > BOX_SIZE || p.y < 0.0 || p.y > BOX_HEIGHT || p.z < 0.0 || p.z > BOX_SIZE)
                break;
            points.push_back(origin + p);
            speeds.push_back(static_cast<float>(glm::length(flow(p))));
            ++count;
            const glm::dvec3 k1 = direction(p);
            const glm::dvec3 k2 = direction(p + 0.5 * LINE_STEP * k1);
            const glm::dvec3 k3 = direction(p + 0.5 * LINE_STEP * k2);
            const glm::dvec3 k4 = direction(p + LINE_STEP * k3);
            p += LINE_STEP / 6.0 * (k1 + 2.0 * k2 + 2.0 * k3 + k4);
        }
        sizes.push_back(count);
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
            if (value && std::strcmp(argv[i], "--glyphs") == 0)
                options.glyphs = std::max<uint64_t>(1, std::strtoull(argv[++i], nullptr, 10));
            else if (value && std::strcmp(argv[i], "--lines") == 0)
                options.lines = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            else if (value && std::strcmp(argv[i], "--shape") == 0)
            {
                const char *shape = argv[++i];
                options.shape = std::strcmp(shape, "sphere") == 0 ? rasm::GlyphShape::SPHERE : (std::strcmp(shape, "cube") == 0 ? rasm::GlyphShape::CUBE : rasm::GlyphShape::ARROW);
            }
            options.still = options.still || std::strcmp(argv[i], "--still") == 0;
            options.swapAa = options.swapAa || std::strcmp(argv[i], "--swap-aa") == 0;
            options.atOrigin = options.atOrigin || std::strcmp(argv[i], "--at-origin") == 0;
            options.wall = options.wall || std::strcmp(argv[i], "--wall") == 0;
        }
        return options;
    }

    // The pick's check: its nearest hit is a glyph of a chunk, and its point lies on that glyph's arrow, within its reach
    // of the arrow's place in the field.
    bool checkPick(const rasm::PickHit *hits, uint32_t count, const std::vector<rasm::EntityHandle> &chunks, const std::vector<glm::dvec3> &corners, const FieldGrid &grid, const glm::dvec3 &origin,
                   bool arrows)
    {
        const auto found = count > 0 ? std::find(chunks.begin(), chunks.end(), hits[0].entity) : chunks.end();
        bool ok = found != chunks.end() && (hits[0].flags & rasm::PICK_HIT_GLYPH) != 0 && hits[0].feature < BLOCK * BLOCK * BLOCK_HEIGHT;
        double distance = 0.0;
        double reach = 0.0;
        if (ok)
        {
            const auto chunk = static_cast<size_t>(found - chunks.begin());
            const uint32_t x = hits[0].feature % BLOCK;
            const uint32_t z = hits[0].feature / BLOCK % BLOCK;
            const uint32_t y = hits[0].feature / (BLOCK * BLOCK);
            const glm::dvec3 step(grid.spacing, BOX_HEIGHT / grid.ny, grid.spacing);
            const glm::dvec3 local = getCellPoint(corners[chunk], step, x, y, z);
            const glm::dvec3 v = flow(local);
            // An arrow runs from its place along the flow, as long as its scale.
            reach = grid.spacing * (0.25 + 0.85 * glm::length(v) / maxSpeed());
            const glm::dvec3 start = origin + local;
            const glm::dvec3 axis = glm::normalize(v);
            const double along = std::clamp(glm::dot(hits[0].position - start, axis), 0.0, reach);
            distance = glm::length(hits[0].position - (start + along * axis));
            // An arrow's head is 0.13 of its length across; other shapes are checked by their glyph alone.
            ok = !arrows || distance < 0.15 * reach + 1e-3;
        }
        std::printf("glyphs check %-14s %s: %s, glyph %u, %.4f m from its arrow's axis (arrow %.3f m long)\n", "pick", ok ? "ok  " : "FAIL",
                    count == 0 ? "no hit" : (found == chunks.end() ? "not a chunk" : "a chunk"), count > 0 ? hits[0].feature : 0u, distance, reach);
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

    const FieldGrid grid = makeGrid(options.glyphs);
    const uint32_t blockCount = grid.blocksX * grid.blocksY * grid.blocksZ;
    const uint32_t blockGlyphs = BLOCK * BLOCK * BLOCK_HEIGHT;
    rasm::EngineConfig config = {
        .appName = "RASM: glyphs",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR};
    // A chunk takes 4 units of header and 4 units of 8 B a glyph: position and value, orientation, scale.
    config.geometryPoolPoints = blockCount * (4 + 4 * blockGlyphs);
    applyExampleArgs(args, config);
    ExampleBench bench{.on = hasBenchFlag(argc, argv), .warmup = getBenchWarmup(argc, argv)};
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

    const glm::dvec3 origin = options.atOrigin ? glm::dvec3(0.0) : FAR_ORIGIN;
    const rasm::SceneHandle scene = engine.createScene();
    const rasm::MeshHandle shape = engine.createMesh(rasm::buildGlyphShape(options.shape));
    const rasm::MaterialHandle glyphMaterial = engine.createMaterial(rasm::MaterialType::GLYPHS);
    rasm::Material glyphStyle = engine.getMaterial(glyphMaterial);
    glyphStyle.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = 0.45f;
    glyphStyle.params[static_cast<uint32_t>(rasm::PbrParam::METALLIC)] = 0.0f;
    glyphStyle.glyphs = {.colorMode = rasm::PointColorMode::SCALAR, .colormap = engine.createColormap(rasm::ColormapPreset::VIRIDIS), .range = {.min = 0.0f, .max = maxSpeed()}};
    engine.updateMaterial(glyphMaterial, glyphStyle);

    std::vector<rasm::EntityHandle> chunks;
    std::vector<glm::dvec3> corners;
    for (uint32_t by = 0; by < grid.blocksY; ++by)
    {
        for (uint32_t bz = 0; bz < grid.blocksZ; ++bz)
        {
            for (uint32_t bx = 0; bx < grid.blocksX; ++bx)
            {
                const Block block = makeBlock(grid, bx, by, bz);
                rasm::MeshBuild build = rasm::buildGlyphChunk({.shape = shape,
                                                               .positions = block.positions.data(),
                                                               .orientations = block.orientations.data(),
                                                               .scales = block.scales.data(),
                                                               .scalars = block.speeds.data(),
                                                               .count = static_cast<uint32_t>(block.positions.size())});
                const rasm::EntityHandle chunk = engine.createEntity("glyphs", engine.createMesh(std::move(build)), glyphMaterial, rasm::Transform{origin + block.corner});
                engine.addEntityToScene(scene, chunk);
                chunks.push_back(chunk);
                corners.push_back(block.corner);
            }
        }
    }

    // Streamlines from seeds on a jittered grid, a mesh of LINES_PER_MESH each, colored by their speed.
    const rasm::MaterialHandle lineMaterial = engine.createMaterial(rasm::MaterialType::VECTOR);
    rasm::Material lineStyle = engine.getMaterial(lineMaterial);
    lineStyle.vector.width = 2.0f;
    lineStyle.vector.colorMode = rasm::PointColorMode::SCALAR;
    lineStyle.vector.colormap = engine.createColormap(rasm::ColormapPreset::PLASMA);
    lineStyle.vector.range = {.min = 0.0f, .max = maxSpeed()};
    engine.updateMaterial(lineMaterial, lineStyle);
    uint32_t linePoints = 0;
    const auto seedsPerSide = static_cast<uint32_t>(std::ceil(std::sqrt(options.lines / 3.0)));
    for (uint32_t first = 0; first < options.lines; first += LINES_PER_MESH)
    {
        std::vector<glm::dvec3> points;
        std::vector<float> speeds;
        std::vector<uint32_t> sizes;
        for (uint32_t i = first; i < std::min(options.lines, first + LINES_PER_MESH); ++i)
        {
            const uint32_t layer = i / (seedsPerSide * seedsPerSide);
            const uint32_t sx = i % seedsPerSide;
            const uint32_t sz = i / seedsPerSide % seedsPerSide;
            const double jitter = 0.3 * std::sin(i * 12.9898);
            const glm::dvec3 seed((sx + 0.5 + jitter) * BOX_SIZE / seedsPerSide, (layer + 0.5) * BOX_HEIGHT / 3.0, (sz + 0.5 - jitter) * BOX_SIZE / seedsPerSide);
            traceLine(seed, origin, points, speeds, sizes);
        }
        rasm::VectorGeometry geometry;
        rasm::addVectorLines(geometry, origin, points.data(), sizes.data(), static_cast<uint32_t>(sizes.size()), {.join = rasm::LineJoin::MITER}, {}, nullptr, nullptr, speeds.data());
        linePoints += static_cast<uint32_t>(points.size());
        const rasm::EntityHandle lines = engine.createEntity("streamlines", engine.createMesh(rasm::buildVectorMesh(std::move(geometry))), lineMaterial, rasm::Transform{origin});
        engine.addEntityToScene(scene, lines);
    }

    const rasm::EnvironmentHandle sky = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    engine.setSceneEnvironment(scene, {.environment = sky, .intensity = 0.6f});
    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.95f, 0.9f), glm::dvec3(0.0), glm::normalize(glm::vec3(-0.4f, -0.8f, -0.5f)), 3.0f, 0.0f);
    engine.addLightToScene(scene, sun);
    if (options.wall)
    {
        const rasm::MaterialHandle stone = engine.createMaterial(rasm::MaterialType::PBR);
        const rasm::EntityHandle wall = engine.createEntity("wall", engine.createMesh(rasm::buildGlyphShape(rasm::GlyphShape::CUBE)), stone,
                                                            rasm::Transform{origin + glm::dvec3(0.5 * BOX_SIZE, 0.6 * BOX_HEIGHT, BOX_SIZE + 10.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                                                            glm::vec3(static_cast<float>(BOX_SIZE), static_cast<float>(1.2 * BOX_HEIGHT), 2.0f)});
        engine.addEntityToScene(scene, wall);
    }

    std::printf("glyphs: %u chunks of %u arrows (%.2f M, %u x %u x %u, %.2f m apart), 32 B a glyph; %u streamlines of %u points in all\n", blockCount, blockGlyphs,
                blockCount * static_cast<double>(blockGlyphs) / 1e6, grid.nx, grid.ny, grid.nz, grid.spacing, options.lines, linePoints);

    const float aspect = 0.5f * static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight);
    const glm::dvec3 center = origin + glm::dvec3(0.5 * BOX_SIZE, 0.5 * BOX_HEIGHT, 0.5 * BOX_SIZE);
    ViewCamera views[VIEW_COUNT] = {
        createViewCamera(engine, scene, {.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = 50.0f, .aspect = aspect, .nearZ = 0.5f}}, center,
                         glm::normalize(glm::dvec3(-0.55, -0.42, -0.72)), 1.05 * BOX_SIZE, glm::vec3(0.0f, 1.0f, 0.0f), 1.0f),
        createViewCamera(engine, scene, orthographic(static_cast<float>(0.55 * BOX_SIZE), aspect), center + glm::dvec3(0.0, 200.0, 0.0), glm::dvec3(0.0, -1.0, 0.0), 400.0,
                         glm::vec3(0.0f, 0.0f, -1.0f), 0.0f),
    };
    views[TOP].input.buttons.left = rasm::CameraDrag::PAN;
    rasm::Grid floor;
    floor.enabled = true;
    floor.origin = origin;
    floor.cellSize = 10.0;
    floor.extent = glm::vec2(static_cast<float>(BOX_SIZE));
    floor.fadeDistance = 1200.0f;
    for (const ViewCamera &view : views)
        engine.setCameraGrid(view.camera, floor);
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
    double glyphsTested = 0.0;
    double glyphsVisible = 0.0;
    while (exampleRunning(engine, args))
    {
        ++frame;
        // A headless run orbits the left view (a drag that goes on for the whole run) and zooms into the right one.
        if ((check || bench.on) && !options.still && frame > 2)
        {
            if (frame == 3)
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_BUTTON, .down = true, .x = quarterWidth, .y = halfHeight});
            engine.addInputEvent({.type = rasm::InputEventType::MOUSE_MOVE, .x = quarterWidth + 2.0f * static_cast<float>(frame - 2), .y = halfHeight});
        }
        engine.pollEvents();
        for (ViewCamera &view : views)
            updateViewCamera(engine, view, RECTS[&view - views]);
        if ((check || bench.on) && !options.still && frame > 2)
        {
            // The right view's box shrinks about its centre, a hundredth a frame down to a fifth.
            rasm::CameraProjection::Orthographic &box = views[TOP].projection.orthographic;
            const float scale = (box.right - box.left) > 0.2f * static_cast<float>(1.1 * BOX_SIZE) ? 0.99f : 1.0f;
            box.left *= scale;
            box.right *= scale;
            box.bottom *= scale;
            box.top *= scale;
            engine.setCameraProjection(views[TOP].camera, views[TOP].projection);
        }
        engine.setMouseGrab(views[PERSPECTIVE].input.dragActive || views[TOP].input.dragActive);
        if (check && frame == PICK_FRAME)
        {
            const rasm::ViewPixels pixels = rasm::getViewPixels(RECTS[TOP], window.pixelWidth, window.pixelHeight);
            pick = engine.pick(list[TOP], {.rect = {.x = pixels.width / 2 - 4, .y = pixels.height / 2 - 4, .width = 9, .height = 9}, .skip = rasm::PICK_SKIP_VECTORS});
        }
        engine.render(list, VIEW_COUNT);
        const rasm::FrameStats &stats = engine.getFrameStats();
        addBenchFrame(bench, stats);
        if (isBenchMeasured(bench))
        {
            glyphsTested += stats.glyphs;
            glyphsVisible += stats.glyphsVisible;
        }

        rasm::PickHit hits[MAX_HITS];
        uint32_t hitCount = 0;
        if (pick.isValid() && engine.isPickReady(pick))
        {
            failures += engine.takePick(pick, hits, MAX_HITS, hitCount) && checkPick(hits, hitCount, chunks, corners, grid, origin, options.shape == rasm::GlyphShape::ARROW) ? 0 : 1;
            pick = {};
            --checksLeft;
        }
        if (check && frame == CHECK_DEADLINE && checksLeft > 0)
        {
            std::printf("glyphs check: %u checks got no answer by frame %u\n", checksLeft, CHECK_DEADLINE);
            failures += checksLeft;
            checksLeft = 0;
        }
    }
    if (bench.on && bench.measured > 0)
    {
        const double n = bench.measured;
        std::printf("  %-22s %8.3f M tested, %.3f M drawn (%.1f%% culled) a frame\n", "glyphs", glyphsTested / n / 1e6, glyphsVisible / n / 1e6,
                    glyphsTested > 0.0 ? 100.0 * (1.0 - glyphsVisible / glyphsTested) : 0.0);
    }
    printBench(bench, config, engine.getRenderContext().hasMeshShaders());
    if (check && checksLeft > 0)
    {
        std::printf("glyphs check: %u checks got no answer before the run ended\n", checksLeft);
        failures += checksLeft;
    }
    if (check)
        std::printf("glyphs checks: %s (%u failed)\n", failures == 0 ? "all passed" : "FAILED", failures);
    return failures > 0 ? 1 : exampleExit(args);
}
