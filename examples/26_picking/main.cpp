#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"
#include "exampleBench.h"

#if defined(RASM_IMGUI)
#include "imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Picking: what is under a pixel of a view (Renderer::pick, rasm/picking.h). A hilly terrain tile 6 000 km from the
// world's origin, with buildings, a skinned figure (CesiumMan), a morphing cube, a model with cut-out and see-through
// parts (AlphaBlendModeTest), a road and a trail draped on the ground, a see-through lake, markers with their names, and
// an overlay route. Two views: the main camera on the left with TAA, and a map from above on the right with 4x MSAA.
// - Hover: a 7 x 7 pick under the mouse every frame; the window shows its nearest hit: the entity, its sub-mesh, a
//   vector feature's id, the world position in doubles and the normal. Click to keep a hit; hold Shift to look through
//   the labels and the vectors (PickDesc::skip), hold Alt to see only the ground.
// - B: a box pick of 128 x 128 under the mouse, listing what it covers.
// A headless run (or --check) also runs scripted picks after the loads, each at the pixel a known world point projects
// to, and checks the answers: the entity, the feature, the kind, and the position within a tolerance (to a millimetre
// where the surface is a known plane, far from the origin too). The road is draped across its whole width
// (LineShape::drapeWidth), so picks at both of its edges from above hit it on the terrain's surface, and the markers'
// names are drawn whole (the Label Depth pass), so picks at their left half hit them. Then it picks a building, destroys it and makes another
// in the next frame, which takes its instance row: the answer must still name the destroyed one. A failed check makes the
// run exit with 1.
//   --bench    the GPU time of a 1 x 1 pick every frame (exampleBench.h), at 1920 x 1080
//   --near     the scene at the world's origin instead
//   --fly      the main camera circles the tile, a degree a frame (after the checks, when they run)
//   --no-aa    both views without anti-aliasing (the vectors' own smooth edges only)

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr double TILE_SIZE = 96.0;
    constexpr uint32_t CELLS = 48;
    constexpr float ROAD_WIDTH = 4.0f; // metres: the material's width and the width the road is draped at
    constexpr double HEIGHT_STEP = 1.0 / 64.0;
    constexpr float MAP_HALF_SIZE = 52.0f;
    constexpr uint32_t CHECK_FRAME = 12;
    constexpr uint32_t REUSE_FRAME = 30;
    constexpr uint32_t CHECK_DEADLINE = 50;
    constexpr uint32_t HOVER_SIZE = 7;
    constexpr uint32_t BOX_SIZE = 128;
    constexpr uint32_t MAX_HITS = 64;
    constexpr double FLY_RADIUS = 83.6;
    constexpr double FLY_HEIGHT = 42.0;
    const glm::dvec3 FAR_ORIGIN = glm::dvec3(4.1e6, 320.0, -4.7e6);
    const rasm::ViewRect MAIN_RECT = {.x = 0.0f, .y = 0.0f, .width = 0.62f, .height = 1.0f};
    const rasm::ViewRect MAP_RECT = {.x = 0.62f, .y = 0.0f, .width = 0.38f, .height = 1.0f};

    struct Terrain
    {
        rasm::TerrainTileDesc desc;
        std::vector<uint16_t> samples;
        rasm::EntityHandle entity;

        rasm::TerrainHeights heights() const
        {
            return {.unorm16 = samples.data(), .width = CELLS + 3};
        }

        // The drawn ground at a point of the tile, in fractions of it.
        glm::dvec3 at(double u, double v) const
        {
            return rasm::getTerrainPosition(desc, heights(), u, v);
        }
    };

    // How a check knows the right position: a plane through the probe (the pixel's ray meets it), the drawn terrain
    // (its rule at the hit's place), or near the probe, within some pixels.
    enum class Surface
    {
        PLANE,
        TERRAIN,
        NEAR,
    };

    struct Probe
    {
        const char *name = "";
        bool map = false;                   // in the map view, else the main one
        glm::dvec3 point = glm::dvec3(0.0); // world space
        rasm::EntityHandle entity;          // the hit nearest the pixel names it; invalid: any
        uint32_t feature = 0;
        uint32_t flags = 0; // PICK_HIT_* the hit has
        Surface surface = Surface::NEAR;
        glm::dvec3 normal = glm::dvec3(0.0, 1.0, 0.0); // PLANE
        double pixels = 3.0;                           // NEAR: the tolerance in pixels at the probe's distance
        uint32_t skip = 0;
        float minAlpha = 0.5f;
        uint32_t size = 1;                // the rect, size x size around the pixel; above 1 the entity or feature may be any hit
        glm::ivec2 shift = glm::ivec2(0); // the rect's centre from the point's pixel
        rasm::PickHandle pick;
        uint32_t pixelX = 0;
        uint32_t pixelY = 0;
        bool done = false;
    };

    // An answer takes a couple of frames, so a pick every frame keeps up to three in flight; the newest answer shows.
    struct Hover
    {
        rasm::PickHandle picks[3];
        rasm::PickHit hits[MAX_HITS];
        uint32_t count = 0;
    };

    struct Scene
    {
        rasm::SceneHandle scene;
        Terrain terrain;
        rasm::MeshHandle cube;
        rasm::EntityHandle buildings[5];
        glm::dvec3 buildingTops[5];
        rasm::MaterialHandle buildingMaterial;
        rasm::EntityHandle man;
        rasm::EntityHandle morph;
        rasm::EntityHandle model;
        rasm::EntityHandle road;
        rasm::EntityHandle lake;
        rasm::EntityHandle markers;
        rasm::EntityHandle labels;
        rasm::EntityHandle route;
        glm::dvec3 roadPoint = glm::dvec3(0.0);
        glm::dvec3 roadEdges[12];
        glm::dvec3 lakePoint = glm::dvec3(0.0);
        glm::dvec3 markerPoints[4];
        glm::dvec3 labelPoint = glm::dvec3(0.0);
        glm::dvec3 routePoint = glm::dvec3(0.0);
        glm::dvec3 groundPoint = glm::dvec3(0.0);
    };
}

namespace
{
    rasm::MeshHandle createCube(rasm::Engine &engine)
    {
        const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const glm::vec3 &n : normals)
        {
            const glm::vec3 u = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 v = glm::cross(n, u);
            const glm::vec3 corners[4] = {n - u - v, n + u - v, n + u + v, n - u + v};
            const auto first = static_cast<uint32_t>(vertices.size());
            for (const glm::vec3 &corner : corners)
                vertices.push_back({.pos = corner * 0.5f, .normal = n, .tangent = glm::vec4(0.0f)});
            for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
                indices.push_back(first + index);
        }
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

    rasm::MaterialHandle createColorMaterial(rasm::Engine &engine, const glm::vec3 &color, float roughness)
    {
        const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::PBR);
        rasm::Material values = engine.getMaterial(material);
        for (uint32_t i = 0; i < 3; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[i];
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        engine.updateMaterial(material, values);
        return material;
    }

    rasm::MaterialHandle createVectorMaterial(rasm::Engine &engine, const rasm::VectorMaterial &style)
    {
        const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::VECTOR);
        rasm::Material values = engine.getMaterial(material);
        values.vector = style;
        engine.updateMaterial(material, values);
        return material;
    }

    rasm::EntityHandle addVectors(rasm::Engine &engine, rasm::SceneHandle scene, const glm::dvec3 &origin, rasm::VectorGeometry geometry, rasm::MaterialHandle material)
    {
        const rasm::EntityHandle entity = engine.createEntity("vectors", engine.createMesh(rasm::buildVectorMesh(std::move(geometry))), material, rasm::Transform{origin});
        engine.addEntityToScene(scene, entity);
        return entity;
    }

    // Rolling hills in steps of HEIGHT_STEP, so the R16_UNORM samples hold them exactly.
    Terrain createTerrain(rasm::Engine &engine, rasm::SceneHandle scene, const glm::dvec3 &center)
    {
        Terrain terrain;
        const uint32_t width = CELLS + 3;
        const double cell = TILE_SIZE / CELLS;
        std::vector<int64_t> steps(static_cast<size_t>(width) * width);
        int64_t lowest = INT64_MAX;
        int64_t highest = INT64_MIN;
        for (uint32_t j = 0; j < width; ++j)
        {
            for (uint32_t i = 0; i < width; ++i)
            {
                const double x = (static_cast<double>(i) - 1.0) * cell - 0.5 * TILE_SIZE;
                const double z = (static_cast<double>(j) - 1.0) * cell - 0.5 * TILE_SIZE;
                const double height = 3.0 * std::sin(x * 0.07) * std::cos(z * 0.05) + 1.2 * std::sin(x * 0.17 + z * 0.11);
                steps[static_cast<size_t>(j) * width + i] = std::llround(height / HEIGHT_STEP);
                lowest = std::min(lowest, steps[static_cast<size_t>(j) * width + i]);
                highest = std::max(highest, steps[static_cast<size_t>(j) * width + i]);
            }
        }
        terrain.samples.resize(steps.size());
        for (size_t k = 0; k < steps.size(); ++k)
            terrain.samples[k] = static_cast<uint16_t>(steps[k] - lowest);
        const rasm::TextureHandle heights = engine.createTexture({.name = "terrain heights", .width = width, .height = width, .format = rasm::Format::R16_UNORM, .usage = rasm::TextureUsage::SAMPLED});
        engine.getRenderContext().fillTexture(heights, terrain.samples.data());
        terrain.desc = {.center = center,
                        .size = TILE_SIZE,
                        .terrain = {.heights = heights,
                                    .heightScale = static_cast<float>(HEIGHT_STEP),
                                    .heightOffset = static_cast<float>(static_cast<double>(lowest) * HEIGHT_STEP),
                                    .minHeight = static_cast<float>(static_cast<double>(lowest) * HEIGHT_STEP),
                                    .maxHeight = static_cast<float>(static_cast<double>(highest) * HEIGHT_STEP),
                                    .skirt = 2.0f}};
        const rasm::TerrainTile tile = engine.createTerrainTile(terrain.desc);
        engine.addEntityToScene(scene, tile.entity);
        terrain.entity = tile.entity;
        return terrain;
    }

    rasm::EntityHandle addBuilding(rasm::Engine &engine, Scene &content, uint32_t index, double u, double v, double height)
    {
        const glm::dvec3 ground = content.terrain.at(u, v);
        // Sunk 1 m, so it stands on the slope.
        const glm::dvec3 center = ground + glm::dvec3(0.0, 0.5 * height - 1.0, 0.0);
        const rasm::EntityHandle entity = engine.createEntity("building", content.cube, content.buildingMaterial, rasm::Transform{center, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(8.0f, static_cast<float>(height), 8.0f)});
        engine.addEntityToScene(content.scene, entity);
        content.buildingTops[index] = center + glm::dvec3(0.0, 0.5 * height, 0.0);
        return entity;
    }

    bool buildScene(rasm::Engine &engine, Scene &content, const glm::dvec3 &origin)
    {
        content.scene = engine.createScene();
        content.cube = createCube(engine);
        content.terrain = createTerrain(engine, content.scene, origin);
        content.buildingMaterial = createColorMaterial(engine, glm::vec3(0.75f, 0.72f, 0.68f), 0.7f);
        const double places[5][3] = {{0.3, 0.3, 14.0}, {0.62, 0.28, 10.0}, {0.5, 0.5, 18.0}, {0.75, 0.62, 12.0}, {0.28, 0.68, 9.0}};
        for (uint32_t i = 0; i < 5; ++i)
            content.buildings[i] = addBuilding(engine, content, i, places[i][0], places[i][1], places[i][2]);

        const rasm::MeshHandle man = engine.loadGLTF("models/gltf/CesiumMan/CesiumMan.glb");
        const rasm::MeshHandle morph = engine.loadGLTF("models/gltf/AnimatedMorphCube/AnimatedMorphCube.glb");
        const rasm::MeshHandle model = engine.loadGLTF("models/gltf/AlphaBlendModeTest/AlphaBlendModeTest.glb");
        if (!man.isValid() || !morph.isValid() || !model.isValid() || !content.cube.isValid())
            return false;
        content.man = engine.createEntity("CesiumMan", man, {}, rasm::Transform{content.terrain.at(0.42, 0.66), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(4.0f)});
        content.morph = engine.createEntity("AnimatedMorphCube", morph, {}, rasm::Transform{content.terrain.at(0.7, 0.42) + glm::dvec3(0.0, 2.0, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(3.0f)});
        content.model = engine.createEntity("AlphaBlendModeTest", model, {}, rasm::Transform{content.terrain.at(0.5, 0.78) + glm::dvec3(0.0, 1.0, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(2.5f)});
        for (const rasm::EntityHandle entity : {content.man, content.morph, content.model})
            engine.addEntityToScene(content.scene, entity);
        engine.playAnimation(content.man, rasm::AnimationPlay{.clip = 0});
        engine.playAnimation(content.morph, rasm::AnimationPlay{.clip = rasm::ALL_ANIMATION_CLIPS});

        // Vector features, draped on the tile: the road (id 101) and the trail (102), the lake (201), the markers (301 to
        // 304) and their names (401 to 404), and the route over everything (501).
        const rasm::VectorPlacement drape = {.drapeTile = &content.terrain.desc, .drapeHeights = content.terrain.heights()};
        std::vector<glm::dvec3> road;
        for (int32_t k = 0; k <= 40; ++k)
            road.push_back(origin + glm::dvec3(-44.0 + 2.2 * k, 0.0, 30.0 + 6.0 * std::sin(0.15 * k)));
        const auto roadSize = static_cast<uint32_t>(road.size());
        const uint32_t roadId = 101;
        rasm::VectorGeometry roads;
        rasm::addVectorLines(roads, origin, road.data(), &roadSize, 1, {.drapeWidth = ROAD_WIDTH}, drape, &roadId);
        content.road = addVectors(engine, content.scene, origin, std::move(roads),
                                  createVectorMaterial(engine, {.color = glm::vec4(0.25f, 0.25f, 0.27f, 1.0f), .width = ROAD_WIDTH, .widthUnit = rasm::VectorUnit::METRES, .facing = rasm::VectorFacing::GROUND, .outlineColor = glm::vec4(0.95f, 0.9f, 0.6f, 1.0f), .outlineWidth = 0.4f}));
        content.roadPoint = content.terrain.at((road[20].x - origin.x) / TILE_SIZE + 0.5, (road[20].z - origin.z) / TILE_SIZE + 0.5);
        // Points 1.5 m off the middle on both sides, away from the model and the names that cover the road from above.
        const int32_t edgePoints[6] = {10, 12, 13, 27, 29, 31};
        for (uint32_t i = 0; i < 6; ++i)
        {
            const int32_t k = edgePoints[i];
            const glm::dvec3 along = glm::normalize(road[k + 1] - road[k - 1]);
            const glm::dvec3 across(-along.z, 0.0, along.x);
            for (uint32_t s = 0; s < 2; ++s)
            {
                const glm::dvec3 edge = road[k] + across * (s == 0 ? -1.5 : 1.5) - origin;
                content.roadEdges[i * 2 + s] = content.terrain.at(edge.x / TILE_SIZE + 0.5, edge.z / TILE_SIZE + 0.5);
            }
        }

        std::vector<glm::dvec3> lake;
        for (int32_t k = 0; k < 32; ++k)
        {
            const double a = 2.0 * 3.14159265358979 * k / 32.0;
            lake.push_back(origin + glm::dvec3(-26.0 + 10.0 * std::cos(a), 0.0, -8.0 + 7.0 * std::sin(a)));
        }
        const uint32_t lakeSize = 32;
        const uint32_t lakeRings = 1;
        const uint32_t lakeId = 201;
        rasm::VectorGeometry lakes;
        rasm::addVectorPolygons(lakes, origin, lake.data(), &lakeSize, &lakeRings, 1, drape, &lakeId);
        content.lake = addVectors(engine, content.scene, origin, std::move(lakes), createVectorMaterial(engine, {.color = glm::vec4(0.15f, 0.35f, 0.8f, 0.4f), .drawOrder = -5}));
        content.lakePoint = content.terrain.at(-26.0 / TILE_SIZE + 0.5, -8.0 / TILE_SIZE + 0.5);

        rasm::FontAtlas atlas;
        if (!engine.loadFont("fonts/Roboto-Medium.ttf", atlas))
            return false;
        const rasm::FontHandle font = engine.createFont(atlas);
        const double markerPlaces[4][2] = {{0.12, 0.15}, {0.88, 0.12}, {0.86, 0.86}, {0.14, 0.88}};
        const char *names[4] = {"North Gate", "Mill", "Harbour", "Old Well"};
        uint32_t markerIds[4] = {};
        rasm::LabelDesc labels[4];
        for (uint32_t i = 0; i < 4; ++i)
        {
            content.markerPoints[i] = content.terrain.at(markerPlaces[i][0], markerPlaces[i][1]);
            markerIds[i] = 301 + i;
            labels[i] = {.position = content.markerPoints[i], .text = names[i], .priority = 1.0f, .anchor = rasm::LabelAnchor::BOTTOM, .offset = glm::vec2(0.0f, 0.4f), .featureId = 401 + i};
        }
        rasm::VectorGeometry markers;
        rasm::addVectorPoints(markers, origin, content.markerPoints, 4, {}, markerIds);
        content.markers = addVectors(engine, content.scene, origin, std::move(markers),
                                     createVectorMaterial(engine, {.color = glm::vec4(0.9f, 0.15f, 0.2f, 1.0f), .width = 14.0f, .outlineColor = glm::vec4(1.0f), .outlineWidth = 3.0f, .drawOrder = 5}));
        // A name of its own on open ground, which the checks pick.
        content.labelPoint = content.terrain.at(0.18, 0.45);
        const rasm::LabelDesc open = {.position = content.labelPoint, .text = "MEADOW", .priority = 9.0f, .featureId = 405};
        rasm::VectorGeometry text;
        rasm::addVectorLabels(text, origin, atlas, labels, 4);
        rasm::addVectorLabels(text, origin, atlas, &open, 1);
        content.labels = addVectors(engine, content.scene, origin, std::move(text),
                                    createVectorMaterial(engine, {.color = glm::vec4(0.1f, 0.08f, 0.05f, 1.0f), .width = 18.0f, .outlineColor = glm::vec4(1.0f), .outlineWidth = 3.0f, .drawOrder = 40, .font = font}));

        std::vector<glm::dvec3> route;
        for (int32_t k = 0; k <= 30; ++k)
            route.push_back(origin + glm::dvec3(-40.0 + 2.6 * k, 0.0, -36.0 + 1.6 * k));
        const auto routeSize = static_cast<uint32_t>(route.size());
        const uint32_t routeId = 501;
        rasm::VectorGeometry routes;
        rasm::addVectorLines(routes, origin, route.data(), &routeSize, 1, {}, drape, &routeId);
        content.route = addVectors(engine, content.scene, origin, std::move(routes),
                                   createVectorMaterial(engine, {.color = glm::vec4(0.9f, 0.2f, 0.85f, 1.0f), .width = 6.0f, .drawOrder = 20, .depth = rasm::VectorDepth::OVERLAY}));
        content.routePoint = content.terrain.at((route[4].x - origin.x) / TILE_SIZE + 0.5, (route[4].z - origin.z) / TILE_SIZE + 0.5);
        content.groundPoint = content.terrain.at(0.12, 0.42);
        return font.isValid() && content.terrain.entity.isValid();
    }

    rasm::Transform lookAt(const glm::dvec3 &eye, const glm::dvec3 &center)
    {
        return rasm::Transform{eye, glm::quatLookAt(glm::normalize(glm::vec3(center - eye)), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)};
    }

    // The view-projection a view draws with (without TAA's jitter), in doubles.
    glm::dmat4 getViewProjection(rasm::Engine &engine, rasm::CameraHandle camera, const rasm::ViewPixels &view)
    {
        return glm::dmat4(engine.getProjectionMatrix(camera, static_cast<float>(view.width) / static_cast<float>(view.height))) * engine.getViewMatrix(camera);
    }

    bool projectToPixel(const glm::dmat4 &viewProjection, const rasm::ViewPixels &view, const glm::dvec3 &world, uint32_t &x, uint32_t &y)
    {
        const glm::dvec4 clip = viewProjection * glm::dvec4(world, 1.0);
        if (clip.w <= 0.0)
            return false;
        const double px = (clip.x / clip.w * 0.5 + 0.5) * view.width;
        const double py = (0.5 - clip.y / clip.w * 0.5) * view.height;
        if (px < 0.0 || py < 0.0 || px >= view.width || py >= view.height)
            return false;
        x = static_cast<uint32_t>(px);
        y = static_cast<uint32_t>(py);
        return true;
    }

    // Where the ray through a pixel's centre meets a plane, in doubles.
    glm::dvec3 intersectPixel(const glm::dmat4 &viewProjection, const rasm::ViewPixels &view, uint32_t x, uint32_t y, const glm::dvec3 &point, const glm::dvec3 &normal)
    {
        const glm::dmat4 inverse = glm::inverse(viewProjection);
        const glm::dvec2 ndc((x + 0.5) / view.width * 2.0 - 1.0, 1.0 - (y + 0.5) / view.height * 2.0);
        const glm::dvec4 a = inverse * glm::dvec4(ndc, 1.0, 1.0);
        const glm::dvec4 b = inverse * glm::dvec4(ndc, 0.5, 1.0);
        const glm::dvec3 from = glm::dvec3(a) / a.w;
        const glm::dvec3 direction = glm::dvec3(b) / b.w - from;
        const double t = glm::dot(point - from, normal) / glm::dot(direction, normal);
        return from + t * direction;
    }

    // A pixel's size at the distance of a point, in metres.
    double getPixelSize(rasm::Engine &engine, rasm::CameraHandle camera, const rasm::ViewPixels &view, const glm::dvec3 &point)
    {
        const glm::mat4 projection = engine.getProjectionMatrix(camera, static_cast<float>(view.width) / static_cast<float>(view.height));
        const double distance = projection[3][3] == 0.0f ? glm::length(point - engine.getCameraTransform(camera).position) : 1.0;
        return 2.0 * distance / (std::abs(static_cast<double>(projection[1][1])) * view.height);
    }

    std::string describe(const rasm::PickHit &hit)
    {
        char text[256];
        std::snprintf(text, sizeof(text), "entity %llu:%llu sub-mesh %u feature %u flags %#x at (%.4f, %.4f, %.4f)", static_cast<unsigned long long>(hit.entity.index),
                      static_cast<unsigned long long>(hit.entity.generation), hit.subMesh, hit.feature, hit.flags, hit.position.x, hit.position.y, hit.position.z);
        return text;
    }

    // Whether the answer of a probe's pick is right. Prints one line either way.
    bool checkProbe(rasm::Engine &engine, const Scene &content, const Probe &probe, rasm::CameraHandle camera, const rasm::ViewPixels &view, const rasm::PickHit *hits, uint32_t count)
    {
        const rasm::PickHit *hit = nullptr;
        for (uint32_t i = 0; i < count && i < MAX_HITS && hit == nullptr; ++i)
        {
            const bool entity = !probe.entity.isValid() || hits[i].entity == probe.entity;
            const bool feature = probe.feature == 0 || hits[i].feature == probe.feature;
            if ((probe.size == 1 || (entity && feature)) && (hits[i].flags & probe.flags) == probe.flags)
                hit = &hits[i];
            if (probe.size == 1)
                break;
        }
        if (hit == nullptr)
        {
            std::printf("pick check %-18s FAIL: %s\n", probe.name, count == 0 ? "no hit" : ("nearest is " + describe(hits[0])).c_str());
            return false;
        }
        bool ok = (!probe.entity.isValid() || hit->entity == probe.entity) && (probe.feature == 0 || hit->feature == probe.feature);
        double error = 0.0;
        double tolerance = 0.0;
        if (probe.surface == Surface::PLANE)
        {
            const glm::dvec3 expected = intersectPixel(getViewProjection(engine, camera, view), view, hit->x, hit->y, probe.point, probe.normal);
            error = glm::length(hit->position - expected);
            tolerance = 1e-3;
        }
        else if (probe.surface == Surface::TERRAIN)
        {
            const glm::dvec3 local = hit->position - content.terrain.desc.center;
            const glm::dvec3 expected = content.terrain.at(local.x / TILE_SIZE + 0.5, local.z / TILE_SIZE + 0.5);
            error = std::abs(hit->position.y - expected.y);
            tolerance = 1e-3;
        }
        else
        {
            error = glm::length(hit->position - probe.point);
            tolerance = probe.pixels * getPixelSize(engine, camera, view, probe.point);
        }
        ok = ok && error <= tolerance;
        std::printf("pick check %-18s %s: %s, %u pixels, off by %.5f m (at most %.5f)\n", probe.name, ok ? "ok  " : "FAIL", describe(*hit).c_str(), hit->pixels, error, tolerance);
        return ok;
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    bool near = false;
    bool check = args.headless;
    bool fly = false;
    bool noAntiAliasing = false;
    for (int i = 1; i < argc; ++i)
    {
        near = near || std::strcmp(argv[i], "--near") == 0;
        fly = fly || std::strcmp(argv[i], "--fly") == 0;
        noAntiAliasing = noAntiAliasing || std::strcmp(argv[i], "--no-aa") == 0;
        check = check || std::strcmp(argv[i], "--check") == 0;
    }
    rasm::EngineConfig config = {
        .appName = "RASM: picking",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .ui = true};
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

    const glm::dvec3 origin = near ? glm::dvec3(0.0) : FAR_ORIGIN;
    Scene content;
    if (!buildScene(engine, content, origin))
        return 1;
    const rasm::EnvironmentHandle sunset = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    engine.setSceneEnvironment(content.scene, {.environment = sunset, .intensity = 0.8f});
    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.9f, 0.8f), glm::dvec3(0.0), glm::normalize(glm::vec3(0.4f, -0.8f, -0.3f)), 3.0f, 0.0f);
    engine.addLightToScene(content.scene, sun);

    const rasm::CameraHandle mainCamera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                                     .perspective{.fovY = 50.0f, .aspect = 1.0f, .nearZ = 0.5f}},
                                                              lookAt(origin + glm::dvec3(-30.0, 42.0, 78.0), origin + glm::dvec3(0.0, 0.0, 4.0)));
    const rasm::CameraHandle mapCamera = engine.createCamera(
        rasm::CameraProjection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC,
                               .orthographic{.left = -MAP_HALF_SIZE, .right = MAP_HALF_SIZE, .bottom = -MAP_HALF_SIZE, .top = MAP_HALF_SIZE, .nearZ = 1.0f, .farZ = 200.0f}},
        rasm::Transform{origin + glm::dvec3(0.0, 100.0, 0.0), glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(1.0f)});
    engine.setCameraController(mapCamera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
    if (bench.on || check || fly)
        engine.setCameraController(mainCamera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
    engine.addCameraToScene(content.scene, mainCamera);
    engine.addCameraToScene(content.scene, mapCamera);
    const rasm::View views[2] = {{.scene = content.scene, .camera = mainCamera, .output = {.rect = MAIN_RECT}, .antiAliasing = noAntiAliasing ? rasm::AntiAliasing::NONE : rasm::AntiAliasing::TAA},
                                 {.scene = content.scene, .camera = mapCamera, .output = {.rect = MAP_RECT}, .antiAliasing = noAntiAliasing ? rasm::AntiAliasing::NONE : rasm::AntiAliasing::MSAA_4X}};

    // The probes: a known point each, and what a pick at its pixel must answer.
    const uint32_t labelOnly = rasm::PICK_SKIP_ALL & ~rasm::PICK_SKIP_LABELS;
    std::vector<Probe> probes = {
        {.name = "building top", .point = content.buildingTops[2], .entity = content.buildings[2], .surface = Surface::PLANE},
        {.name = "building side", .point = content.buildingTops[0] - glm::dvec3(0.0, 4.0, -4.0), .entity = content.buildings[0], .surface = Surface::PLANE, .normal = glm::dvec3(0.0, 0.0, 1.0)},
        {.name = "terrain", .point = content.groundPoint, .entity = content.terrain.entity, .flags = rasm::PICK_HIT_TERRAIN, .surface = Surface::TERRAIN},
        {.name = "skinned figure", .point = engine.getTransform(content.man).position + glm::dvec3(0.0, 4.0, 0.0), .entity = content.man, .pixels = 40.0},
        {.name = "morph cube", .point = engine.getTransform(content.morph).position, .entity = content.morph, .pixels = 30.0},
        {.name = "road", .point = content.roadPoint, .entity = content.road, .feature = 101, .flags = rasm::PICK_HIT_FEATURE},
        {.name = "marker", .point = content.markerPoints[3], .entity = content.markers, .feature = 304, .flags = rasm::PICK_HIT_FEATURE, .pixels = 8.0},
        {.name = "label", .point = content.labelPoint, .entity = content.labels, .feature = 405, .flags = rasm::PICK_HIT_LABEL, .pixels = 40.0, .size = 9},
        {.name = "label only", .point = content.labelPoint, .entity = content.labels, .feature = 405, .flags = rasm::PICK_HIT_LABEL, .pixels = 40.0, .skip = labelOnly, .size = 9},
        {.name = "overlay route", .point = content.routePoint, .entity = content.route, .feature = 501, .flags = rasm::PICK_HIT_OVERLAY, .pixels = 6.0},
        {.name = "lake at 0.3", .point = content.lakePoint, .entity = content.lake, .feature = 201, .flags = rasm::PICK_HIT_FEATURE, .minAlpha = 0.3f},
        {.name = "lake at 0.5", .point = content.lakePoint, .entity = content.terrain.entity, .flags = rasm::PICK_HIT_TERRAIN, .surface = Surface::TERRAIN},
        {.name = "ground only", .point = content.buildingTops[4], .entity = content.terrain.entity, .flags = rasm::PICK_HIT_TERRAIN, .surface = Surface::TERRAIN, .skip = rasm::PICK_SKIP_ALL & ~rasm::PICK_SKIP_TERRAIN},
        {.name = "map: building top", .map = true, .point = content.buildingTops[3], .entity = content.buildings[3], .surface = Surface::PLANE},
        {.name = "map: box", .map = true, .point = content.buildingTops[2], .entity = content.buildings[2], .surface = Surface::PLANE, .size = 64},
        {.name = "map: cut-out model", .map = true, .point = engine.getTransform(content.model).position, .entity = content.model, .pixels = 60.0, .size = 32},
    };
    // The road's edges, 1.5 m off its middle on both sides, seen from above: all of the road lies on the drawn ground
    // (LineShape::drapeWidth), so the ground never covers it and each hit is on the terrain's surface.
    for (uint32_t i = 0; i < 6; ++i)
    {
        for (const double side : {-1.0, 1.0})
            probes.push_back({.name = side < 0.0 ? "map: road edge -" : "map: road edge +", .map = true, .point = content.roadEdges[i * 2 + (side < 0.0 ? 0 : 1)], .entity = content.road, .feature = 101, .flags = rasm::PICK_HIT_FEATURE, .surface = Surface::TERRAIN});
    }
    // The left half of the markers' names, seen from above: a label is drawn whole when its anchor is in view (the
    // Label Depth pass), never cut where the hills beside it rise above its anchor.
    const char *nameProbes[3] = {"map: North Gate", "map: Harbour", "map: Old Well"};
    const uint32_t nameMarkers[3] = {0, 2, 3};
    for (uint32_t i = 0; i < 3; ++i)
        probes.push_back({.name = nameProbes[i], .map = true, .point = content.markerPoints[nameMarkers[i]], .entity = content.labels, .feature = 401 + nameMarkers[i], .flags = rasm::PICK_HIT_LABEL, .pixels = 40.0, .size = 10, .shift = glm::ivec2(-20, -13)});
    uint32_t failures = 0;
    uint32_t checksLeft = check ? static_cast<uint32_t>(probes.size()) + 1 : 0;
    // The row reuse check: the destroyed building and the one made after it.
    rasm::EntityHandle victim;
    rasm::EntityHandle replacement;
    rasm::PickHandle victimPick;

    Hover hover;
    rasm::PickHit kept;
    bool keptValid = false;
    rasm::PickHandle box;
    rasm::PickHit boxHits[MAX_HITS];
    uint32_t boxCount = 0;

#if defined(RASM_IMGUI)
    engine.setUiCallback(
        [&]()
        {
            ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Picking");
            ImGui::TextUnformatted("Hover: the nearest hit under the mouse. Click: keep it. Shift: through labels and vectors. Alt: the ground only. B: a box.");
            auto show = [&](const char *title, const rasm::PickHit &hit)
            {
                ImGui::Text("%s: entity %llu, sub-mesh %u%s", title, static_cast<unsigned long long>(hit.entity.index), hit.subMesh, engine.isAlive(hit.entity) ? "" : " (destroyed)");
                if ((hit.flags & rasm::PICK_HIT_FEATURE) != 0)
                    ImGui::Text("  feature %u%s%s", hit.feature, (hit.flags & rasm::PICK_HIT_LABEL) != 0 ? ", a label" : "", (hit.flags & rasm::PICK_HIT_OVERLAY) != 0 ? ", overlay" : "");
                ImGui::Text("  world (%.3f, %.3f, %.3f)", hit.position.x, hit.position.y, hit.position.z);
                ImGui::Text("  normal (%.2f, %.2f, %.2f), %.1f m away", hit.normal.x, hit.normal.y, hit.normal.z, hit.distance);
            };
            if (hover.count > 0)
                show("Hover", hover.hits[0]);
            else
                ImGui::TextUnformatted("Hover: nothing");
            if (keptValid)
                show("Kept", kept);
            if (boxCount > 0)
            {
                ImGui::Text("Box: %u things", boxCount);
                for (uint32_t i = 0; i < std::min(boxCount, MAX_HITS); ++i)
                    ImGui::Text("  entity %llu sub-mesh %u feature %u, %u pixels", static_cast<unsigned long long>(boxHits[i].entity.index), boxHits[i].subMesh, boxHits[i].feature, boxHits[i].pixels);
            }
            ImGui::End();
        });
#endif

    if (args.waitForLoads || check)
        engine.waitForLoads();

    uint32_t frame = 0;
    bool clicked = false;
    bool boxAsked = false;
    float mouseX = -1.0f;
    float mouseY = -1.0f;
    bool shift = false;
    bool alt = false;
    while (exampleRunning(engine, args))
    {
        ++frame;
        engine.pollEvents();
        const rasm::Window &window = engine.getMainWindow();
        const rasm::ViewPixels mainPixels = rasm::getViewPixels(MAIN_RECT, window.pixelWidth, window.pixelHeight);
        const rasm::ViewPixels mapPixels = rasm::getViewPixels(MAP_RECT, window.pixelWidth, window.pixelHeight);
        uint32_t count = 0;
        const rasm::InputEvent *events = engine.getInputEvents(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const rasm::InputEvent &event = events[i];
            if (event.type == rasm::InputEventType::MOUSE_MOVE || event.type == rasm::InputEventType::MOUSE_BUTTON)
            {
                mouseX = event.x;
                mouseY = event.y;
            }
            clicked = clicked || (event.type == rasm::InputEventType::MOUSE_BUTTON && event.down && event.button == rasm::MouseButton::LEFT && !engine.getUiInputCapture().mouse);
            boxAsked = boxAsked || (event.type == rasm::InputEventType::KEY && event.down && !event.repeat && event.key == rasm::Key::B && !engine.getUiInputCapture().keyboard);
            if (event.type == rasm::InputEventType::MOUSE_LEAVE)
                mouseX = -1.0f;
            if (event.type == rasm::InputEventType::KEY)
            {
                shift = (event.modifiers & rasm::KEY_MOD_SHIFT) != 0;
                alt = (event.modifiers & rasm::KEY_MOD_ALT) != 0;
            }
        }

        // The hover picks: the answers that are in, then this frame's pick under the mouse (the main view's centre in
        // a headless or bench run).
        for (rasm::PickHandle &pick : hover.picks)
        {
            if (!pick.isValid() || !engine.isPickReady(pick))
                continue;
            engine.takePick(pick, hover.hits, MAX_HITS, hover.count);
            pick = {};
            if (clicked && hover.count > 0)
            {
                kept = hover.hits[0];
                keptValid = true;
                clicked = false;
            }
        }
        if (box.isValid() && engine.isPickReady(box))
        {
            engine.takePick(box, boxHits, MAX_HITS, boxCount);
            box = {};
        }
        uint32_t px = mainPixels.width / 2;
        uint32_t py = mainPixels.height / 2;
        bool onMap = false;
        if (mouseX >= 0.0f && !rasm::getViewPixel(window, MAIN_RECT, mouseX, mouseY, px, py))
            onMap = rasm::getViewPixel(window, MAP_RECT, mouseX, mouseY, px, py);
        for (rasm::PickHandle &pick : hover.picks)
        {
            if (pick.isValid())
                continue;
            const uint32_t size = bench.on ? 1 : HOVER_SIZE;
            const uint32_t skip = alt ? rasm::PICK_SKIP_ALL & ~rasm::PICK_SKIP_TERRAIN : (shift ? rasm::PICK_SKIP_LABELS | rasm::PICK_SKIP_VECTORS : 0);
            const rasm::PickDesc desc = {.rect = {.x = px - std::min(px, size / 2), .y = py - std::min(py, size / 2), .width = size, .height = size}, .skip = skip};
            pick = engine.pick(views[onMap ? 1 : 0], desc);
            break;
        }
        if (boxAsked && !box.isValid())
        {
            box = engine.pick(views[onMap ? 1 : 0], {.rect = {.x = px - std::min(px, BOX_SIZE / 2), .y = py - std::min(py, BOX_SIZE / 2), .width = BOX_SIZE, .height = BOX_SIZE}});
            boxAsked = false;
        }

        const uint32_t flyStart = check ? CHECK_DEADLINE : 0;
        if (fly && frame > flyStart)
        {
            const double angle = glm::radians(static_cast<double>(frame - flyStart)) + std::atan2(-30.0, 78.0);
            engine.setCameraTransform(mainCamera, lookAt(origin + glm::dvec3(FLY_RADIUS * std::sin(angle), FLY_HEIGHT, FLY_RADIUS * std::cos(angle)), origin + glm::dvec3(0.0, 0.0, 4.0)));
        }

        // The scripted checks: each probe's pick at the pixel its point projects to.
        if (check && frame == CHECK_FRAME)
        {
            for (Probe &probe : probes)
            {
                const rasm::ViewPixels &view = probe.map ? mapPixels : mainPixels;
                const glm::dmat4 viewProjection = getViewProjection(engine, probe.map ? mapCamera : mainCamera, view);
                if (!projectToPixel(viewProjection, view, probe.point, probe.pixelX, probe.pixelY))
                {
                    std::printf("pick check %-18s FAIL: its point is outside the view\n", probe.name);
                    ++failures;
                    probe.done = true;
                    --checksLeft;
                    continue;
                }
                probe.pixelX = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(probe.pixelX) + probe.shift.x, 0, static_cast<int32_t>(view.width) - 1));
                probe.pixelY = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(probe.pixelY) + probe.shift.y, 0, static_cast<int32_t>(view.height) - 1));
                const uint32_t half = probe.size / 2;
                const rasm::PickDesc desc = {.rect = {.x = probe.pixelX - std::min(probe.pixelX, half), .y = probe.pixelY - std::min(probe.pixelY, half), .width = probe.size, .height = probe.size},
                                             .minAlpha = probe.minAlpha,
                                             .skip = probe.skip};
                probe.pick = engine.pick(views[probe.map ? 1 : 0], desc);
            }
        }
        if (check && frame == REUSE_FRAME)
        {
            uint32_t x = 0;
            uint32_t y = 0;
            victim = content.buildings[4];
            if (projectToPixel(getViewProjection(engine, mainCamera, mainPixels), mainPixels, content.buildingTops[4], x, y))
                victimPick = engine.pick(views[0], {.rect = {.x = x, .y = y, .width = 1, .height = 1}});
        }
        engine.render(views, 2);
        // After the frame that draws the pick: the building goes, and the next frame's new one takes its instance row.
        if (check && frame == REUSE_FRAME)
        {
            const rasm::Transform transform = engine.getTransform(victim);
            engine.destroyEntity(victim);
            replacement = engine.createEntity("building", content.cube, content.buildingMaterial, transform);
            engine.addEntityToScene(content.scene, replacement);
        }
        addBenchFrame(bench, engine.getFrameStats());

        for (Probe &probe : probes)
        {
            if (probe.done || !probe.pick.isValid() || !engine.isPickReady(probe.pick))
                continue;
            rasm::PickHit hits[MAX_HITS];
            uint32_t hitCount = 0;
            const bool taken = engine.takePick(probe.pick, hits, MAX_HITS, hitCount);
            failures += taken && checkProbe(engine, content, probe, probe.map ? mapCamera : mainCamera, probe.map ? mapPixels : mainPixels, hits, hitCount) ? 0 : 1;
            probe.done = true;
            --checksLeft;
        }
        if (victimPick.isValid() && engine.isPickReady(victimPick))
        {
            rasm::PickHit hit;
            uint32_t hitCount = 0;
            const bool ok = engine.takePick(victimPick, &hit, 1, hitCount) && hitCount > 0 && hit.entity == victim && !engine.isAlive(victim) && hit.entity != replacement;
            std::printf("pick check %-18s %s: %s (the destroyed building is %llu:%llu, alive %d; the new one %llu:%llu)\n", "row reused", ok ? "ok  " : "FAIL",
                        hitCount > 0 ? describe(hit).c_str() : "no hit", static_cast<unsigned long long>(victim.index), static_cast<unsigned long long>(victim.generation),
                        engine.isAlive(victim) ? 1 : 0, static_cast<unsigned long long>(replacement.index), static_cast<unsigned long long>(replacement.generation));
            failures += ok ? 0 : 1;
            victimPick = {};
            --checksLeft;
        }
        if (check && frame == CHECK_DEADLINE && checksLeft > 0)
        {
            std::printf("pick check: %u checks got no answer by frame %u\n", checksLeft, CHECK_DEADLINE);
            failures += checksLeft;
            checksLeft = 0;
        }
    }
    printBench(bench, config, engine.getRenderContext().hasMeshShaders());
    if (check && checksLeft > 0)
    {
        std::printf("pick check: %u checks got no answer before the run ended\n", checksLeft);
        failures += checksLeft;
    }
    if (check)
        std::printf("pick checks: %s (%u failed)\n", failures == 0 ? "all passed" : "FAILED", failures);
    return failures > 0 ? 1 : exampleExit(args);
}
