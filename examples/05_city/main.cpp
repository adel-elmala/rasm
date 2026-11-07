#include "glm/glm.hpp"
#include "glm/gtc/constants.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace
{
    // The city, in meters. An odd block count puts a block, the plaza, at the centre.
    constexpr int CITY_BLOCKS = 49;
    constexpr int CENTER_BLOCK = CITY_BLOCKS / 2;
    constexpr float BLOCK_SIZE = 60.0f;
    constexpr float STREET_WIDTH = 16.0f;
    constexpr float BLOCK_PITCH = BLOCK_SIZE + STREET_WIDTH;
    constexpr float CITY_HALF_SIZE = CITY_BLOCKS * BLOCK_PITCH * 0.5f;
    constexpr float CURB_HEIGHT = 0.2f;
    constexpr float LOT_OFFSET = 13.0f; // 2 x 2 lots of 26 m inside the block's 4 m sidewalk

    // Facades: one bay is one window wide. The textures hold TEXTURE_CELLS x TEXTURE_CELLS windows and repeat.
    constexpr float BAY_WIDTH = 3.0f;
    constexpr float FLOOR_HEIGHT = 3.5f;
    constexpr int TEXTURE_CELLS = 8;
    constexpr int CELL_PIXELS = 32;
    constexpr int TEXTURE_SIZE = TEXTURE_CELLS * CELL_PIXELS;
    constexpr int FACADE_STYLE_COUNT = 3;
    constexpr int WINDOW_PATTERNS = 4;
    constexpr int GLASS = 0;
    constexpr int OFFICE = 1;
    constexpr int BRICK = 2;
    // A wall texel in every style. A roof's UVs are all this one, so it has no UV derivatives and reads mip 0.
    const glm::vec2 ROOF_UV((0.5f) / TEXTURE_SIZE, (1.5f) / TEXTURE_SIZE);

    constexpr float TRUNK_HEIGHT = 4.0f;
    constexpr int LEAF_COLORS = 3;

    // The sky is a box around everything. Its walls are 15 km away, so the horizon at y = 0 sits at SKY_HORIZON_V.
    constexpr float SKY_SIZE = 30000.0f;
    constexpr float SKY_BOTTOM = -3000.0f;
    constexpr float SKY_TOP = 12000.0f;
    constexpr float SKY_HORIZON_V = SKY_TOP / (SKY_TOP - SKY_BOTTOM);
    constexpr int SKY_TEXTURE_WIDTH = 4;
    constexpr int SKY_TEXTURE_HEIGHT = 256;

    // The outward direction of each side of a block.
    const glm::vec2 BLOCK_SIDES[4] = {{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}};

    constexpr float PLINTH_HEIGHT = 3.0f;
    constexpr float MONUMENT_SCALE = 14.0f;
    // DamagedHelmet spans y = -0.9 .. 0.9 after its node rotation.
    const glm::vec3 MONUMENT_POSITION(0.0f, CURB_HEIGHT + PLINTH_HEIGHT + 0.9f * MONUMENT_SCALE, 0.0f);

    // Traffic drives on every 4th street, 4 lanes each, and wraps around at the city's edge.
    constexpr int AVENUE_EVERY = 4;
    constexpr int CARS_PER_LANE = 48;
    constexpr float LANE_LENGTH = 2.0f * CITY_HALF_SIZE;

    // Animation runs on frames, not seconds, so a given frame renders the same on every run (verify-change).
    constexpr float FRAMES_PER_SECOND = 60.0f;
    constexpr float TOUR_SECONDS = 124.0f;

    enum class Segment
    {
        TOWER,
        BLOCK,
        SLAB,

        COUNT,
    };

    // Building pieces stack: a tower is a podium, tower segments and a roof box.
    const glm::vec3 SEGMENT_SIZES[static_cast<int>(Segment::COUNT)] = {
        {6 * BAY_WIDTH, 8 * FLOOR_HEIGHT, 6 * BAY_WIDTH},
        {8 * BAY_WIDTH, 4 * FLOOR_HEIGHT, 8 * BAY_WIDTH},
        {8 * BAY_WIDTH, 3 * FLOOR_HEIGHT, 4 * BAY_WIDTH},
    };

    // A small PCG hash, so the city is the same on every platform (std distributions are not).
    struct Rng
    {
        uint32_t state = 1;

        uint32_t next()
        {
            state = state * 747796405u + 2891336453u;
            const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
            return (word >> 22u) ^ word;
        }
        float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
        float range(float low, float high) { return low + (high - low) * uniform(); }
        int below(int count) { return static_cast<int>(next() % static_cast<uint32_t>(count)); }
        bool chance(float probability) { return uniform() < probability; }
    };

    struct FacadeStyle
    {
        glm::vec3 wall; // linear albedo
        glm::vec3 glass;
        float wallRoughness;
        float wallMetallic;
        int windowLeft; // the window's pixels inside a CELL_PIXELS cell, top row first
        int windowRight;
        int windowTop;
        int windowBottom;
        float litChance;
    };

    const FacadeStyle FACADE_STYLES[FACADE_STYLE_COUNT] = {
        {{0.05f, 0.06f, 0.075f}, {0.02f, 0.03f, 0.05f}, 0.35f, 1.0f, 2, 30, 4, 30, 0.30f},   // glass curtain wall
        {{0.25f, 0.24f, 0.22f}, {0.02f, 0.025f, 0.035f}, 0.85f, 0.0f, 0, 32, 10, 26, 0.35f}, // office, ribbon windows
        {{0.20f, 0.10f, 0.07f}, {0.02f, 0.02f, 0.03f}, 0.9f, 0.0f, 9, 23, 6, 26, 0.38f},     // brick apartments
    };

    const glm::vec3 WINDOW_LIGHTS[3] = {{1.0f, 0.62f, 0.32f}, {0.65f, 0.78f, 1.0f}, {0.6f, 0.35f, 0.15f}};

    struct Car
    {
        rasm::EntityHandle entity;
        glm::vec3 laneStart; // where the lane enters the city
        glm::vec3 direction;
        glm::vec3 scale;
        float offset; // meters along the lane at frame 0
        float speed;  // meters per second
    };

    struct TourKey
    {
        float seconds;
        glm::vec3 position;
        glm::vec3 target;
    };

    // Each key also sets the speed: the path is a Catmull-Rom spline in time. It enters downtown along the street
    // at x = -38, circles the monument, cranes up over the plaza and orbits the city high up. Away from the
    // street and the plaza it stays above the tallest tower (about 300 m).
    const TourKey TOUR[] = {
        {0.0f, {-1321.0f, 220.0f, 1070.0f}, {0.0f, 140.0f, 0.0f}},
        {12.0f, {-38.0f, 250.0f, 1500.0f}, {-38.0f, 40.0f, 600.0f}},
        {21.0f, {-38.0f, 60.0f, 920.0f}, {-38.0f, 30.0f, 300.0f}},
        {34.0f, {-38.0f, 30.0f, 520.0f}, {-38.0f, 25.0f, 0.0f}},
        {46.0f, {-38.0f, 26.0f, 170.0f}, {0.0f, 18.0f, 0.0f}},
        {52.0f, {-31.5f, 22.0f, 45.0f}, {0.0f, 16.0f, 0.0f}},
        {58.0f, {45.0f, 20.0f, 31.5f}, {0.0f, 16.0f, 0.0f}},
        {64.0f, {31.5f, 24.0f, -45.0f}, {0.0f, 16.0f, 0.0f}},
        {70.0f, {-45.0f, 30.0f, -31.5f}, {0.0f, 16.0f, 0.0f}},
        {82.0f, {-30.0f, 380.0f, -20.0f}, {0.0f, 0.0f, 0.0f}},
        {96.0f, {-513.0f, 650.0f, -1410.0f}, {0.0f, 40.0f, 0.0f}},
        {110.0f, {-1642.0f, 620.0f, -440.0f}, {0.0f, 40.0f, 0.0f}},
    };
    constexpr int TOUR_KEY_COUNT = static_cast<int>(sizeof(TOUR) / sizeof(TOUR[0]));

    struct CityAssets
    {
        rasm::MeshHandle cube;
        rasm::MeshHandle skyBox;
        rasm::MeshHandle canopy;
        rasm::MeshHandle segments[static_cast<int>(Segment::COUNT)];
        rasm::MeshHandle monument;

        rasm::MaterialHandle skyMaterial;
        rasm::MaterialHandle asphalt;
        rasm::MaterialHandle streets;
        rasm::MaterialHandle sidewalk;
        rasm::MaterialHandle plaza;
        rasm::MaterialHandle plinth;
        rasm::MaterialHandle lawn;
        rasm::MaterialHandle bark;
        rasm::MaterialHandle leaves[LEAF_COLORS];
        rasm::MaterialHandle roof;
        rasm::MaterialHandle pole;
        rasm::MaterialHandle lamp;
        rasm::MaterialHandle aviation;
        rasm::MaterialHandle headlights;
        rasm::MaterialHandle taillights;
        rasm::MaterialHandle facades[FACADE_STYLE_COUNT][WINDOW_PATTERNS];
    };

    struct City
    {
        rasm::Engine *engine = nullptr;
        rasm::SceneHandle scene;
        CityAssets assets;
        std::vector<Car> cars;
        rasm::EntityHandle monument;
        uint32_t entityCount = 0;
    };
}

namespace
{
    uint8_t toSrgb8(float linear)
    {
        linear = std::clamp(linear, 0.0f, 1.0f);
        const float srgb = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        return static_cast<uint8_t>(std::lround(srgb * 255.0f));
    }

    uint8_t toUnorm8(float value)
    {
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    void setColor(rasm::Material &values, rasm::PbrParam param, const glm::vec3 &color)
    {
        for (uint32_t i = 0; i < 3; ++i)
            values.params[static_cast<uint32_t>(param) + i] = color[i];
    }

    rasm::MaterialHandle createSolidMaterial(rasm::Engine &engine, const glm::vec3 &albedo, float roughness, float metallic, const glm::vec3 &emissive = glm::vec3(0.0f))
    {
        const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::PBR);
        rasm::Material values = engine.getMaterial(material);
        setColor(values, rasm::PbrParam::ALBEDO, albedo);
        setColor(values, rasm::PbrParam::EMISSIVE_FACTOR, emissive);
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        values.params[static_cast<uint32_t>(rasm::PbrParam::METALLIC)] = metallic;
        engine.updateMaterial(material, values);
        return material;
    }

    // fillTexture uploads the pixels and builds the mip chain.
    rasm::TextureHandle createFilledTexture(rasm::Engine &engine, const char *name, rasm::Format format, uint32_t width, uint32_t height, const std::vector<uint8_t> &pixels)
    {
        const rasm::TextureHandle texture = engine.createTexture({.name = name, .width = width, .height = height, .format = format});
        if (!texture.isValid() || !engine.getRenderContext().fillTexture(texture, pixels.data()))
            return {};
        return texture;
    }

    bool isWindow(const FacadeStyle &style, int x, int y)
    {
        return x >= style.windowLeft && x < style.windowRight && y >= style.windowTop && y < style.windowBottom;
    }

    // Each style shares its albedo and metallic-roughness textures. Each window pattern adds an emissive texture
    // that lights a random set of windows.
    bool createFacadeMaterials(rasm::Engine &engine, CityAssets &assets)
    {
        const size_t byteCount = static_cast<size_t>(TEXTURE_SIZE) * TEXTURE_SIZE * 4;
        for (int s = 0; s < FACADE_STYLE_COUNT; ++s)
        {
            const FacadeStyle &style = FACADE_STYLES[s];
            std::vector<uint8_t> albedo(byteCount);
            std::vector<uint8_t> metallicRoughness(byteCount);
            for (int y = 0; y < TEXTURE_SIZE; ++y)
            {
                for (int x = 0; x < TEXTURE_SIZE; ++x)
                {
                    const bool window = isWindow(style, x % CELL_PIXELS, y % CELL_PIXELS);
                    const glm::vec3 color = window ? style.glass : style.wall;
                    uint8_t *albedoPixel = &albedo[(static_cast<size_t>(y) * TEXTURE_SIZE + x) * 4];
                    albedoPixel[0] = toSrgb8(color.r);
                    albedoPixel[1] = toSrgb8(color.g);
                    albedoPixel[2] = toSrgb8(color.b);
                    albedoPixel[3] = 255;
                    // glTF packing: roughness in G, metallic in B.
                    uint8_t *mrPixel = &metallicRoughness[(static_cast<size_t>(y) * TEXTURE_SIZE + x) * 4];
                    mrPixel[0] = 0;
                    mrPixel[1] = toUnorm8(window ? 0.08f : style.wallRoughness);
                    mrPixel[2] = toUnorm8(window ? 0.0f : style.wallMetallic);
                    mrPixel[3] = 255;
                }
            }
            const rasm::TextureHandle albedoTexture = createFilledTexture(engine, "facade albedo", rasm::Format::R8G8B8A8_SRGB, TEXTURE_SIZE, TEXTURE_SIZE, albedo);
            const rasm::TextureHandle mrTexture = createFilledTexture(engine, "facade metallic roughness", rasm::Format::R8G8B8A8_UNORM, TEXTURE_SIZE, TEXTURE_SIZE, metallicRoughness);
            if (!albedoTexture.isValid() || !mrTexture.isValid())
                return false;

            for (int p = 0; p < WINDOW_PATTERNS; ++p)
            {
                Rng rng{static_cast<uint32_t>(s * 16 + p + 1)};
                std::vector<uint8_t> emissive(byteCount, 0);
                for (int cy = 0; cy < TEXTURE_CELLS; ++cy)
                {
                    for (int cx = 0; cx < TEXTURE_CELLS; ++cx)
                    {
                        if (!rng.chance(style.litChance))
                            continue;
                        const float roll = rng.uniform();
                        const glm::vec3 light = WINDOW_LIGHTS[roll < 0.7f ? 0 : (roll < 0.82f ? 1 : 2)] * rng.range(0.6f, 1.0f);
                        for (int y = style.windowTop; y < style.windowBottom; ++y)
                        {
                            // Rooms are brighter near the floor, where the lamps are.
                            const float shade = 0.75f + 0.25f * static_cast<float>(y - style.windowTop) / static_cast<float>(style.windowBottom - style.windowTop);
                            for (int x = style.windowLeft; x < style.windowRight; ++x)
                            {
                                uint8_t *pixel = &emissive[(static_cast<size_t>(cy * CELL_PIXELS + y) * TEXTURE_SIZE + cx * CELL_PIXELS + x) * 4];
                                pixel[0] = toSrgb8(light.r * shade);
                                pixel[1] = toSrgb8(light.g * shade);
                                pixel[2] = toSrgb8(light.b * shade);
                            }
                        }
                    }
                }
                for (size_t i = 3; i < byteCount; i += 4)
                    emissive[i] = 255;

                const rasm::TextureHandle emissiveTexture = createFilledTexture(engine, "facade windows", rasm::Format::R8G8B8A8_SRGB, TEXTURE_SIZE, TEXTURE_SIZE, emissive);
                if (!emissiveTexture.isValid())
                    return false;

                // The texture order follows rasm::PbrSlot. Metallic and roughness share one texture, as in glTF.
                const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::PBR, {albedoTexture, mrTexture, mrTexture, {}, {}, emissiveTexture});
                rasm::Material values = engine.getMaterial(material);
                values.params[static_cast<uint32_t>(rasm::PbrParam::METALLIC)] = 1.0f;
                setColor(values, rasm::PbrParam::EMISSIVE_FACTOR, glm::vec3(1.8f));
                engine.updateMaterial(material, values);
                assets.facades[s][p] = material;
            }
        }
        return true;
    }

    // The corners go counter-clockwise, seen from outside.
    void addQuad(std::vector<rasm::Vertex> &vertices, std::vector<uint32_t> &indices, const glm::vec3 corners[4], const glm::vec2 uvs[4])
    {
        const glm::vec3 normal = glm::normalize(glm::cross(corners[1] - corners[0], corners[3] - corners[0]));
        const auto first = static_cast<uint32_t>(vertices.size());
        for (int i = 0; i < 4; ++i)
            vertices.push_back({.pos = corners[i], .normal = normal, .uv = {uvs[i], uvs[i], uvs[i]}, .tangent = glm::vec4(0.0f)});
        for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
            indices.push_back(first + index);
    }

    // A box standing on y = 0 and centred on x and z, without a bottom face. The side UVs count windows, so a
    // facade texture repeats every TEXTURE_CELLS bays and floors, with the texture's top at the box's top.
    rasm::MeshHandle createBoxMesh(rasm::Engine &engine, const glm::vec3 &size)
    {
        const float x = size.x * 0.5f;
        const float y = size.y;
        const float z = size.z * 0.5f;
        const glm::vec3 sides[4][4] = {
            {{-x, 0.0f, z}, {x, 0.0f, z}, {x, y, z}, {-x, y, z}},
            {{x, 0.0f, z}, {x, 0.0f, -z}, {x, y, -z}, {x, y, z}},
            {{x, 0.0f, -z}, {-x, 0.0f, -z}, {-x, y, -z}, {x, y, -z}},
            {{-x, 0.0f, -z}, {-x, 0.0f, z}, {-x, y, z}, {-x, y, -z}},
        };
        const glm::vec3 top[4] = {{-x, y, z}, {x, y, z}, {x, y, -z}, {-x, y, -z}};

        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        const float floors = y / FLOOR_HEIGHT / TEXTURE_CELLS;
        for (const auto &side : sides)
        {
            const float bays = glm::length(side[1] - side[0]) / BAY_WIDTH / TEXTURE_CELLS;
            const glm::vec2 uvs[4] = {{0.0f, floors}, {bays, floors}, {bays, 0.0f}, {0.0f, 0.0f}};
            addQuad(vertices, indices, side, uvs);
        }
        const glm::vec2 roofUvs[4] = {ROOF_UV, ROOF_UV, ROOF_UV, ROOF_UV};
        addQuad(vertices, indices, top, roofUvs);
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

    // The inside of a unit box standing on y = 0: the sky. The swapchain clears to a fixed grey, and this covers it.
    // v runs from 0 at the top to 1 at the bottom, so the gradient texture is one column.
    rasm::MeshHandle createSkyMesh(rasm::Engine &engine)
    {
        const glm::vec3 sides[4][4] = {
            {{-0.5f, 0.0f, 0.5f}, {0.5f, 0.0f, 0.5f}, {0.5f, 1.0f, 0.5f}, {-0.5f, 1.0f, 0.5f}},
            {{0.5f, 0.0f, 0.5f}, {0.5f, 0.0f, -0.5f}, {0.5f, 1.0f, -0.5f}, {0.5f, 1.0f, 0.5f}},
            {{0.5f, 0.0f, -0.5f}, {-0.5f, 0.0f, -0.5f}, {-0.5f, 1.0f, -0.5f}, {0.5f, 1.0f, -0.5f}},
            {{-0.5f, 0.0f, -0.5f}, {-0.5f, 0.0f, 0.5f}, {-0.5f, 1.0f, 0.5f}, {-0.5f, 1.0f, -0.5f}},
        };
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const auto &side : sides)
        {
            // The outside's corners in reverse, so the faces point inward.
            const glm::vec3 corners[4] = {side[3], side[2], side[1], side[0]};
            // Half a texel in from the ends, so the repeating sampler does not blend the top row with the bottom one.
            const float first = 0.5f / SKY_TEXTURE_HEIGHT;
            const float last = 1.0f - first;
            const glm::vec2 uvs[4] = {{0.0f, first}, {1.0f, first}, {1.0f, last}, {0.0f, last}};
            addQuad(vertices, indices, corners, uvs);
        }
        const glm::vec3 top[4] = {{-0.5f, 1.0f, -0.5f}, {0.5f, 1.0f, -0.5f}, {0.5f, 1.0f, 0.5f}, {-0.5f, 1.0f, 0.5f}};
        const glm::vec2 zenith(0.5f, 0.5f / SKY_TEXTURE_HEIGHT);
        const glm::vec2 topUvs[4] = {zenith, zenith, zenith, zenith};
        addQuad(vertices, indices, top, topUvs);
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

    // Dark blue overhead, warmed near the horizon by the city's lights.
    rasm::MaterialHandle createSkyMaterial(rasm::Engine &engine)
    {
        const glm::vec3 zenith(0.0015f, 0.002f, 0.008f);
        const glm::vec3 high(0.006f, 0.007f, 0.02f);
        const glm::vec3 glow(0.05f, 0.03f, 0.035f);
        std::vector<uint8_t> pixels(static_cast<size_t>(SKY_TEXTURE_WIDTH) * SKY_TEXTURE_HEIGHT * 4);
        for (int y = 0; y < SKY_TEXTURE_HEIGHT; ++y)
        {
            const float v = (static_cast<float>(y) + 0.5f) / SKY_TEXTURE_HEIGHT;
            const glm::vec3 color = glm::mix(glm::mix(zenith, high, glm::smoothstep(0.3f, 0.65f, v)), glow, glm::smoothstep(0.62f, SKY_HORIZON_V, v));
            for (int x = 0; x < SKY_TEXTURE_WIDTH; ++x)
            {
                uint8_t *pixel = &pixels[(static_cast<size_t>(y) * SKY_TEXTURE_WIDTH + x) * 4];
                pixel[0] = toSrgb8(color.r);
                pixel[1] = toSrgb8(color.g);
                pixel[2] = toSrgb8(color.b);
                pixel[3] = 255;
            }
        }
        const rasm::TextureHandle texture = createFilledTexture(engine, "sky", rasm::Format::R8G8B8A8_SRGB, SKY_TEXTURE_WIDTH, SKY_TEXTURE_HEIGHT, pixels);
        if (!texture.isValid())
            return {};
        // Unlit: the base color reaches the screen as it is, with no lights and no tone mapping.
        return engine.createMaterial(rasm::MaterialType::UNLIT, {texture});
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

    // A lumpy subdivided icosahedron: 5120 triangles with shared vertices, so meshoptimizer can build its LODs.
    rasm::MeshHandle createCanopyMesh(rasm::Engine &engine)
    {
        const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
        std::vector<glm::vec3> points = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
        for (auto &point : points)
            point = glm::normalize(point);
        std::vector<uint32_t> triangles = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                                           3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};

        std::unordered_map<uint64_t, uint32_t> cache;
        for (int level = 0; level < 4; ++level)
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
            const float lumps = 0.12f * std::sin(5.1f * p.x + 1.7f) * std::sin(4.3f * p.y + 0.4f) * std::sin(4.7f * p.z + 2.9f) +
                                0.05f * std::sin(11.0f * p.x + 0.3f) * std::sin(12.0f * p.z + 1.1f);
            vertices[i] = {.pos = p * (1.0f + lumps) * glm::vec3(2.6f, 2.2f, 2.6f), .normal = glm::vec3(0.0f), .uv = {}, .tangent = glm::vec4(0.0f)};
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

    rasm::EntityHandle place(City &city, rasm::MeshHandle mesh, rasm::MaterialHandle material, const glm::vec3 &position, const glm::vec3 &scale, float yaw = 0.0f)
    {
        const rasm::Transform transform{position, glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)), scale};
        const rasm::EntityHandle entity = city.engine->createEntity("city", mesh, material, transform);
        if (entity.isValid())
        {
            city.engine->addEntityToScene(city.scene, entity);
            ++city.entityCount;
        }
        return entity;
    }

    // Towers rise around the centre and around a second, smaller downtown to the east.
    float cityDensity(const glm::vec2 &position)
    {
        const float centre = glm::length(position) / CITY_HALF_SIZE / 0.28f;
        const float east = glm::length(position - glm::vec2(760.0f, -680.0f)) / CITY_HALF_SIZE / 0.11f;
        return std::exp(-centre * centre) + 0.7f * std::exp(-east * east);
    }

    void addTree(City &city, Rng &rng, const glm::vec2 &position, float base, float size)
    {
        const CityAssets &assets = city.assets;
        place(city, assets.cube, assets.bark, {position.x, base, position.y}, glm::vec3(0.35f, TRUNK_HEIGHT, 0.35f) * size);
        const glm::vec3 canopyScale(size, size * rng.range(0.85f, 1.25f), size);
        place(city, assets.canopy, assets.leaves[rng.below(LEAF_COLORS)], {position.x, base + (TRUNK_HEIGHT + 0.6f) * size, position.y}, canopyScale, rng.range(0.0f, glm::two_pi<float>()));
    }

    void addStreetlights(City &city, const glm::vec2 &block)
    {
        const CityAssets &assets = city.assets;
        for (const glm::vec2 &out : BLOCK_SIDES)
        {
            const glm::vec2 along(std::abs(out.y), std::abs(out.x));
            const glm::vec3 lampScale = out.x != 0.0f ? glm::vec3(1.4f, 0.18f, 0.45f) : glm::vec3(0.45f, 0.18f, 1.4f);
            for (float offset : {-15.0f, 15.0f})
            {
                const glm::vec2 foot = block + out * (BLOCK_SIZE * 0.5f - 0.7f) + along * offset;
                const glm::vec2 head = foot + out * 0.6f;
                place(city, assets.cube, assets.pole, {foot.x, CURB_HEIGHT, foot.y}, {0.18f, 7.0f, 0.18f});
                place(city, assets.cube, assets.lamp, {head.x, CURB_HEIGHT + 7.0f, head.y}, lampScale);
            }
        }
    }

    void addStreetTrees(City &city, Rng &rng, const glm::vec2 &block)
    {
        for (const glm::vec2 &out : BLOCK_SIDES)
        {
            const glm::vec2 along(std::abs(out.y), std::abs(out.x));
            for (float offset : {-22.5f, -7.5f, 7.5f, 22.5f})
                addTree(city, rng, block + out * (BLOCK_SIZE * 0.5f - 1.8f) + along * offset, CURB_HEIGHT, rng.range(0.7f, 1.0f));
        }
    }

    void addPark(City &city, Rng &rng, const glm::vec2 &centre, float size, float spacing, float skipChance)
    {
        place(city, city.assets.cube, city.assets.lawn, {centre.x, CURB_HEIGHT, centre.y}, {size, 0.1f, size});
        const int count = static_cast<int>(size / spacing);
        for (int z = 0; z < count; ++z)
        {
            for (int x = 0; x < count; ++x)
            {
                if (rng.chance(skipChance))
                    continue;
                const glm::vec2 cell = centre + (glm::vec2(x, z) - (count - 1) * 0.5f) * spacing;
                const glm::vec2 jitter(rng.range(-0.3f, 0.3f) * spacing, rng.range(-0.3f, 0.3f) * spacing);
                addTree(city, rng, cell + jitter, CURB_HEIGHT + 0.1f, rng.range(0.9f, 1.4f));
            }
        }
    }

    void addPlaza(City &city)
    {
        place(city, city.assets.cube, city.assets.plinth, {0.0f, CURB_HEIGHT, 0.0f}, {16.0f, PLINTH_HEIGHT, 16.0f});
        city.monument = place(city, city.assets.monument, {}, MONUMENT_POSITION, glm::vec3(MONUMENT_SCALE));
    }

    // Stacks a building piece on top of base, turned by a random quarter, and returns the new top.
    float addSegment(City &city, Rng &rng, Segment segment, int style, const glm::vec2 &lot, float base)
    {
        const int index = static_cast<int>(segment);
        const float yaw = glm::half_pi<float>() * static_cast<float>(rng.below(4));
        place(city, city.assets.segments[index], city.assets.facades[style][rng.below(WINDOW_PATTERNS)], {lot.x, base, lot.y}, glm::vec3(1.0f), yaw);
        return base + SEGMENT_SIZES[index].y;
    }

    void addRoofBox(City &city, const glm::vec2 &lot, float base, const glm::vec3 &size)
    {
        place(city, city.assets.cube, city.assets.roof, {lot.x, base, lot.y}, size);
    }

    void addTower(City &city, Rng &rng, const glm::vec2 &lot, float density)
    {
        const int style = rng.chance(0.7f) ? GLASS : OFFICE;
        float top = CURB_HEIGHT;
        if (rng.chance(0.5f))
            top = addSegment(city, rng, Segment::BLOCK, OFFICE, lot, top);
        const int segments = std::min(1 + static_cast<int>(density * 7.0f * rng.range(0.5f, 1.15f)), 9);
        for (int i = 0; i < segments; ++i)
            top = addSegment(city, rng, Segment::TOWER, style, lot, top);
        addRoofBox(city, lot, top, {7.0f, 3.0f, 7.0f});
        top += 3.0f;
        if (top < 90.0f)
            return;
        if (rng.chance(0.5f))
        {
            place(city, city.assets.cube, city.assets.pole, {lot.x, top, lot.y}, {0.35f, 18.0f, 0.35f});
            top += 18.0f;
        }
        place(city, city.assets.cube, city.assets.aviation, {lot.x, top, lot.y}, glm::vec3(0.9f));
    }

    void addMidrise(City &city, Rng &rng, const glm::vec2 &lot, float density)
    {
        const int style = rng.chance(0.6f) ? OFFICE : BRICK;
        const int segments = std::clamp(1 + static_cast<int>(density * 6.0f * rng.range(0.4f, 1.1f)), 1, 3);
        float top = CURB_HEIGHT;
        for (int i = 0; i < segments; ++i)
            top = addSegment(city, rng, Segment::BLOCK, style, lot, top);
        if (rng.chance(0.5f))
            addRoofBox(city, lot, top, {6.0f, 2.5f, 6.0f});
    }

    // Two long, low slabs side by side.
    void addRowhouses(City &city, Rng &rng, const glm::vec2 &lot)
    {
        const bool turned = rng.chance(0.5f);
        const int style = rng.chance(0.7f) ? BRICK : OFFICE;
        const int stories = rng.chance(0.3f) ? 2 : 1;
        const int slab = static_cast<int>(Segment::SLAB);
        for (float side : {-6.5f, 6.5f})
        {
            const glm::vec2 position = lot + (turned ? glm::vec2(side, 0.0f) : glm::vec2(0.0f, side));
            for (int i = 0; i < stories; ++i)
            {
                const float yaw = (turned ? glm::half_pi<float>() : 0.0f) + (rng.chance(0.5f) ? glm::pi<float>() : 0.0f);
                const glm::vec3 base(position.x, CURB_HEIGHT + i * SEGMENT_SIZES[slab].y, position.y);
                place(city, city.assets.segments[slab], city.assets.facades[style][rng.below(WINDOW_PATTERNS)], base, glm::vec3(1.0f), yaw);
            }
        }
    }

    void addLot(City &city, Rng &rng, const glm::vec2 &lot, float density)
    {
        if (rng.chance(0.05f))
            addPark(city, rng, lot, 24.0f, 12.0f, 0.0f);
        else if (density > 0.35f && rng.chance(0.85f))
            addTower(city, rng, lot, density);
        else if (density > 0.12f || rng.chance(0.4f))
            addMidrise(city, rng, lot, density);
        else
            addRowhouses(city, rng, lot);
    }

    glm::vec3 carPosition(const Car &car, float seconds)
    {
        return car.laneStart + car.direction * std::fmod(car.offset + car.speed * seconds, LANE_LENGTH);
    }

    // Cars are glowing boxes: white heading one way, red the other, like a long exposure of the street.
    void addTraffic(City &city, Rng &rng)
    {
        for (int axis = 0; axis < 2; ++axis)
        {
            for (int street = 0; street <= CITY_BLOCKS; street += AVENUE_EVERY)
            {
                const float centre = (static_cast<float>(street) - CITY_BLOCKS * 0.5f) * BLOCK_PITCH;
                for (float lane : {-6.0f, -2.0f, 2.0f, 6.0f})
                {
                    const float sign = lane > 0.0f ? 1.0f : -1.0f;
                    const glm::vec3 direction = axis == 0 ? glm::vec3(sign, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, sign);
                    const glm::vec3 across = axis == 0 ? glm::vec3(0.0f, 0.0f, centre + lane) : glm::vec3(centre + lane, 0.0f, 0.0f);
                    const glm::vec3 scale = axis == 0 ? glm::vec3(4.4f, 1.4f, 1.9f) : glm::vec3(1.9f, 1.4f, 4.4f);
                    const rasm::MaterialHandle material = sign > 0.0f ? city.assets.headlights : city.assets.taillights;
                    const float speed = rng.range(10.0f, 16.0f); // one speed per lane, so cars never catch up
                    for (int i = 0; i < CARS_PER_LANE; ++i)
                    {
                        Car car{.laneStart = across - direction * CITY_HALF_SIZE,
                                .direction = direction,
                                .scale = scale,
                                .offset = (static_cast<float>(i) + rng.range(0.0f, 0.6f)) * LANE_LENGTH / CARS_PER_LANE,
                                .speed = speed};
                        car.entity = place(city, city.assets.cube, material, carPosition(car, 0.0f), scale);
                        city.cars.push_back(car);
                    }
                }
            }
        }
    }

    void buildCity(City &city)
    {
        Rng rng{2026};
        place(city, city.assets.skyBox, city.assets.skyMaterial, {0.0f, SKY_BOTTOM, 0.0f}, {SKY_SIZE, SKY_TOP - SKY_BOTTOM, SKY_SIZE});
        // The ground reaches the sky's walls, just short of them. Inside the city the streets glow orange, as if lit
        // by the streetlights: the scene has too few lights for the real thing.
        place(city, city.assets.cube, city.assets.asphalt, {0.0f, -1.05f, 0.0f}, {SKY_SIZE - 20.0f, 1.0f, SKY_SIZE - 20.0f});
        place(city, city.assets.cube, city.assets.streets, {0.0f, -0.05f, 0.0f}, {LANE_LENGTH + STREET_WIDTH, 0.05f, LANE_LENGTH + STREET_WIDTH});

        for (int bz = 0; bz < CITY_BLOCKS; ++bz)
        {
            for (int bx = 0; bx < CITY_BLOCKS; ++bx)
            {
                const glm::vec2 block = glm::vec2(bx - CENTER_BLOCK, bz - CENTER_BLOCK) * BLOCK_PITCH;
                const int ring = std::max(std::abs(bx - CENTER_BLOCK), std::abs(bz - CENTER_BLOCK));
                const float density = cityDensity(block);

                place(city, city.assets.cube, ring == 0 ? city.assets.plaza : city.assets.sidewalk, {block.x, 0.0f, block.y}, {BLOCK_SIZE, CURB_HEIGHT, BLOCK_SIZE});
                addStreetlights(city, block);
                if (ring == 0)
                {
                    addPlaza(city);
                    continue;
                }
                if (ring == 1 || rng.chance(0.06f))
                {
                    addPark(city, rng, block, BLOCK_SIZE - 8.0f, 7.0f, 0.3f);
                    continue;
                }
                for (float lz : {-LOT_OFFSET, LOT_OFFSET})
                {
                    for (float lx : {-LOT_OFFSET, LOT_OFFSET})
                        addLot(city, rng, block + glm::vec2(lx, lz), density);
                }
                if (density < 0.25f)
                    addStreetTrees(city, rng, block);
            }
        }
        addTraffic(city, rng);
    }

    bool createAssets(rasm::Engine &engine, CityAssets &assets)
    {
        assets.cube = createBoxMesh(engine, glm::vec3(1.0f));
        assets.skyBox = createSkyMesh(engine);
        assets.canopy = createCanopyMesh(engine);
        for (int i = 0; i < static_cast<int>(Segment::COUNT); ++i)
            assets.segments[i] = createBoxMesh(engine, SEGMENT_SIZES[i]);
        assets.monument = engine.loadGLTF("models/gltf/DamagedHelmet/DamagedHelmet.gltf");
        if (!assets.cube.isValid() || !assets.skyBox.isValid() || !assets.canopy.isValid() || !assets.monument.isValid())
            return false;
        for (const auto &segment : assets.segments)
        {
            if (!segment.isValid())
                return false;
        }

        assets.skyMaterial = createSkyMaterial(engine);
        if (!assets.skyMaterial.isValid())
            return false;
        assets.asphalt = createSolidMaterial(engine, glm::vec3(0.025f, 0.025f, 0.03f), 0.65f, 0.0f);
        assets.streets = createSolidMaterial(engine, glm::vec3(0.025f, 0.025f, 0.03f), 0.55f, 0.0f, glm::vec3(0.035f, 0.02f, 0.008f));
        assets.sidewalk = createSolidMaterial(engine, glm::vec3(0.16f, 0.16f, 0.17f), 0.9f, 0.0f);
        assets.plaza = createSolidMaterial(engine, glm::vec3(0.32f, 0.30f, 0.27f), 0.5f, 0.0f);
        assets.plinth = createSolidMaterial(engine, glm::vec3(0.20f, 0.20f, 0.22f), 0.25f, 0.0f);
        assets.lawn = createSolidMaterial(engine, glm::vec3(0.04f, 0.10f, 0.03f), 0.95f, 0.0f);
        assets.bark = createSolidMaterial(engine, glm::vec3(0.10f, 0.07f, 0.04f), 0.9f, 0.0f);
        assets.leaves[0] = createSolidMaterial(engine, glm::vec3(0.05f, 0.15f, 0.04f), 0.85f, 0.0f);
        assets.leaves[1] = createSolidMaterial(engine, glm::vec3(0.08f, 0.18f, 0.05f), 0.85f, 0.0f);
        assets.leaves[2] = createSolidMaterial(engine, glm::vec3(0.04f, 0.11f, 0.06f), 0.85f, 0.0f);
        assets.roof = createSolidMaterial(engine, glm::vec3(0.10f, 0.10f, 0.11f), 0.8f, 0.0f);
        assets.pole = createSolidMaterial(engine, glm::vec3(0.06f, 0.06f, 0.07f), 0.4f, 0.8f);
        assets.lamp = createSolidMaterial(engine, glm::vec3(1.0f, 0.9f, 0.7f), 0.5f, 0.0f, glm::vec3(4.0f, 3.0f, 1.6f));
        assets.aviation = createSolidMaterial(engine, glm::vec3(0.5f, 0.0f, 0.0f), 0.5f, 0.0f);
        assets.headlights = createSolidMaterial(engine, glm::vec3(0.02f), 0.5f, 0.0f, glm::vec3(3.2f, 3.0f, 2.6f));
        assets.taillights = createSolidMaterial(engine, glm::vec3(0.02f), 0.5f, 0.0f, glm::vec3(3.0f, 0.18f, 0.08f));
        return createFacadeMaterials(engine, assets);
    }

    glm::vec3 hermite(const glm::vec3 &p0, const glm::vec3 &p1, const glm::vec3 &m0, const glm::vec3 &m1, float u)
    {
        const float u2 = u * u;
        const float u3 = u2 * u;
        return (2.0f * u3 - 3.0f * u2 + 1.0f) * p0 + (u3 - 2.0f * u2 + u) * m0 + (-2.0f * u3 + 3.0f * u2) * p1 + (u3 - u2) * m1;
    }

    // Key i of the looping tour, with its time moved by whole loops for indices outside 0 .. TOUR_KEY_COUNT - 1.
    TourKey tourKey(int i)
    {
        const int loops = i >= 0 ? i / TOUR_KEY_COUNT : (i - TOUR_KEY_COUNT + 1) / TOUR_KEY_COUNT;
        TourKey key = TOUR[i - loops * TOUR_KEY_COUNT];
        key.seconds += static_cast<float>(loops) * TOUR_SECONDS;
        return key;
    }

    // Tangents are scaled to the segment's duration, so the speed is continuous across keys.
    glm::vec3 tourSpline(const glm::vec3 &p0, const glm::vec3 &p1, const glm::vec3 &p2, const glm::vec3 &p3, const TourKey &k0, const TourKey &k1, const TourKey &k2, const TourKey &k3, float u)
    {
        const float duration = k2.seconds - k1.seconds;
        const glm::vec3 m1 = (p2 - p0) * (duration / (k2.seconds - k0.seconds));
        const glm::vec3 m2 = (p3 - p1) * (duration / (k3.seconds - k1.seconds));
        return hermite(p1, p2, m1, m2, u);
    }

    // The camera's pose on the tour, built from yaw and pitch the way the free-fly controller builds it, so the
    // controller takes over without a jump.
    rasm::Transform tourTransform(float seconds)
    {
        const float t = std::fmod(seconds, TOUR_SECONDS);
        int i = 0;
        while (i + 1 < TOUR_KEY_COUNT && TOUR[i + 1].seconds <= t)
            ++i;
        const TourKey k0 = tourKey(i - 1);
        const TourKey k1 = tourKey(i);
        const TourKey k2 = tourKey(i + 1);
        const TourKey k3 = tourKey(i + 2);
        const float u = (t - k1.seconds) / (k2.seconds - k1.seconds);
        const glm::vec3 position = tourSpline(k0.position, k1.position, k2.position, k3.position, k0, k1, k2, k3, u);
        const glm::vec3 target = tourSpline(k0.target, k1.target, k2.target, k3.target, k0, k1, k2, k3, u);

        const glm::vec3 forward = glm::normalize(target - position);
        const float yaw = std::atan2(-forward.x, -forward.z);
        const float pitch = std::asin(glm::clamp(forward.y, -1.0f, 1.0f));
        const glm::quat rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(pitch, glm::vec3(1.0f, 0.0f, 0.0f));
        return rasm::Transform{position, glm::normalize(rotation), glm::vec3(1.0f)};
    }

    bool cameraMovedByUser(const rasm::Transform &rendered, const rasm::Transform &tour)
    {
        return glm::distance(rendered.position, tour.position) > 1e-3f || std::abs(glm::dot(rendered.rotation, tour.rotation)) < 0.99999f;
    }

    void updateTraffic(rasm::Engine &engine, const std::vector<Car> &cars, float seconds)
    {
        for (const Car &car : cars)
            engine.updateTransform(car.entity, rasm::Transform{carPosition(car, seconds), glm::identity<glm::quat>(), car.scale});
    }
}

int main()
{
    rasm::EngineConfig config = {
        .appName = "rasm city",
        .windowWidth = 1280,
        .windowHeight = 720,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .maxInstances = 1u << 18,
        .showStats = true};

    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    const auto buildStart = std::chrono::steady_clock::now();
    City city{.engine = &engine, .scene = engine.createScene()};
    if (!createAssets(engine, city.assets))
        return 1;
    buildCity(city);
    // The monument loads on a worker thread. Waiting keeps frame N the same on every run.
    engine.waitForLoads();
    const float buildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - buildStart).count();

    std::printf("rasm city: %u entities (%zu moving cars), built in %.0f ms.\n", city.entityCount, city.cars.size(), buildMs);
    std::printf("The camera flies a tour. Drag the mouse or press WASD to take over. F1: stats, F2: cycle LODs.\n");
    std::fflush(stdout);

    const rasm::Transform start = tourTransform(0.0f);
    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 50.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.5f}},
                                                    start);
    const rasm::CameraController freeFly{.type = rasm::CameraController::Type::FREE_FLY,
                                         .freeFly = {.speed = 80.0f, .lookSensitivity = 1.5f},
                                         .easingSpeed = 6.0f};
    engine.setCameraController(camera, freeFly);
    engine.addCameraToScene(city.scene, camera);

    // A cool moon, a warm glow from the city's own lights, and floodlights around the monument: 7 lights, all the
    // engine has (MAX_LIGHTS, whose slot 0 is never used).
    const glm::vec3 floodColor(1.0f, 0.82f, 0.6f);
    const rasm::LightHandle lights[] = {
        engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(0.55f, 0.65f, 1.0f), glm::vec3(0.0f), glm::normalize(glm::vec3(-0.35f, -1.0f, -0.45f)), 0.3f, 0.0f),
        engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.6f, 0.35f), glm::vec3(0.0f), glm::normalize(glm::vec3(0.5f, -0.25f, 0.8f)), 0.12f, 0.0f),
        engine.createLight(rasm::LightType::POINT, floodColor, glm::vec3(20.0f, 14.0f, 20.0f), glm::vec3(0.0f), 900.0f, 90.0f),
        engine.createLight(rasm::LightType::POINT, floodColor, glm::vec3(-20.0f, 14.0f, 20.0f), glm::vec3(0.0f), 900.0f, 90.0f),
        engine.createLight(rasm::LightType::POINT, floodColor, glm::vec3(20.0f, 14.0f, -20.0f), glm::vec3(0.0f), 900.0f, 90.0f),
        engine.createLight(rasm::LightType::POINT, floodColor, glm::vec3(-20.0f, 14.0f, -20.0f), glm::vec3(0.0f), 900.0f, 90.0f),
        engine.createLight(rasm::LightType::POINT, glm::vec3(0.2f, 0.7f, 1.0f), glm::vec3(0.0f, 5.0f, 16.0f), glm::vec3(0.0f), 300.0f, 60.0f),
    };
    for (const rasm::LightHandle &light : lights)
        engine.addLightToScene(city.scene, light);

    uint64_t frame = 0;
    bool touring = true;
    bool aviationOn = false;
    while (engine.running())
    {
        const float seconds = static_cast<float>(frame) / FRAMES_PER_SECOND;

        // The tour sets the pose and re-seeds the free-fly controller from it. Mouse or WASD input then moves the
        // camera away from the pose, and from that frame on the user flies.
        rasm::Transform tour{};
        if (touring)
        {
            tour = tourTransform(seconds);
            engine.setCameraTransform(camera, tour);
            engine.setCameraController(camera, freeFly);
        }

        // Moving cars rewrite only their own instance rows.
        updateTraffic(engine, city.cars, seconds);
        engine.updateTransform(city.monument, rasm::Transform{MONUMENT_POSITION, glm::angleAxis(seconds * 0.25f, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(MONUMENT_SCALE)});

        // Every aviation light blinks through one material row.
        const bool blinkOn = frame % 90 < 12;
        if (blinkOn != aviationOn)
        {
            aviationOn = blinkOn;
            rasm::Material values = engine.getMaterial(city.assets.aviation);
            setColor(values, rasm::PbrParam::EMISSIVE_FACTOR, aviationOn ? glm::vec3(4.0f, 0.15f, 0.08f) : glm::vec3(0.2f, 0.0f, 0.0f));
            engine.updateMaterial(city.assets.aviation, values);
        }

        engine.render(city.scene, camera);

        if (touring && cameraMovedByUser(engine.getCameraTransform(camera), tour))
            touring = false;
        ++frame;
    }

    return 0;
}
