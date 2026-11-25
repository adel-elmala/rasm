#include "scene/terrain.h"

#include "assets/meshData.h"
#include "rasm/renderer.h"
#include "rasm/terrain.h"
#include "renderer/geometryPool.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>

namespace rasm
{
    namespace
    {
        // Below this the box is too thin for its normal matrix: a flat tile still gets a box of 1 mm.
        constexpr double MIN_BOX_HEIGHT = 1e-3;

        // A grid point's height, as the shader computes it: the product, then the sum, in floats. Two statements, so the
        // compiler does not fuse them.
        float getCornerHeight(const TerrainHeights &heights, const TerrainMaterial &terrain, uint32_t x, uint32_t y)
        {
            const size_t at = static_cast<size_t>(y) * heights.width + x;
            const float value = heights.unorm16 != nullptr ? static_cast<float>(heights.unorm16[at]) : heights.floats[at];
            const float scaled = terrain.heightScale * value;
            return terrain.heightOffset + scaled;
        }

        // How far the tile's frame lies below a sphere of that radius at d metres from the centre: d^2 / 2R.
        double getCurvatureDrop(double x, double z, double sphereRadius)
        {
            return sphereRadius > 0.0 ? (x * x + z * z) / (2.0 * sphereRadius) : 0.0;
        }

        bool isHeightFormat(Format format)
        {
            return format == Format::R16_UNORM || format == Format::R32_SFLOAT;
        }
    }

    void buildTerrainGrid(uint32_t cells, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices)
    {
        const uint32_t side = cells + 1;
        const float step = 1.0f / static_cast<float>(cells);
        vertices.clear();
        indices.clear();
        vertices.reserve(static_cast<size_t>(side) * side + 4 * side);
        indices.reserve(static_cast<size_t>(cells) * cells * 6 + 4 * static_cast<size_t>(cells) * 6);
        for (uint32_t j = 0; j < side; ++j)
        {
            for (uint32_t i = 0; i < side; ++i)
            {
                const glm::vec2 uv(static_cast<float>(i) * step, static_cast<float>(j) * step);
                vertices.push_back({.pos = glm::vec3(uv.x - 0.5f, 1.0f, uv.y - 0.5f),
                                    .normal = glm::vec3(0.0f, 1.0f, 0.0f),
                                    .uv = {uv, glm::vec2(static_cast<float>(i), static_cast<float>(j)), glm::vec2(static_cast<float>(cells), 0.0f)},
                                    .tangent = glm::vec4(0.0f)});
            }
        }
        for (uint32_t j = 0; j < cells; ++j)
        {
            for (uint32_t i = 0; i < cells; ++i)
            {
                const uint32_t a = j * side + i;
                indices.insert(indices.end(), {a, a + side, a + 1, a + 1, a + side, a + side + 1});
            }
        }
        // Each edge again at y = 0, joined to the edge by a strip that faces out of the tile.
        const uint32_t starts[4] = {0, cells, cells * side, 0};
        const uint32_t strides[4] = {1, side, 1, side};
        const glm::vec3 outward[4] = {{0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}};
        for (uint32_t e = 0; e < 4; ++e)
        {
            const auto first = static_cast<uint32_t>(vertices.size());
            for (uint32_t k = 0; k < side; ++k)
            {
                Vertex vertex = vertices[starts[e] + k * strides[e]];
                vertex.pos.y = 0.0f;
                vertices.push_back(vertex);
            }
            for (uint32_t k = 0; k < cells; ++k)
            {
                const uint32_t top = starts[e] + k * strides[e];
                const uint32_t nextTop = starts[e] + (k + 1) * strides[e];
                const uint32_t bottom = first + k;
                const glm::vec3 normal = glm::cross(vertices[bottom].pos - vertices[top].pos, vertices[nextTop].pos - vertices[top].pos);
                if (glm::dot(normal, outward[e]) > 0.0f)
                    indices.insert(indices.end(), {top, bottom, nextTop, nextTop, bottom, bottom + 1});
                else
                    indices.insert(indices.end(), {top, nextTop, bottom, nextTop, bottom + 1, bottom});
            }
        }
    }

    Transform getTerrainTransform(const TerrainTileDesc &tile)
    {
        const TerrainMaterial &terrain = tile.terrain;
        const double corner = 0.5 * tile.size;
        const double low = static_cast<double>(terrain.minHeight) - getCurvatureDrop(corner, corner, terrain.sphereRadius);
        const double height = std::max(static_cast<double>(terrain.maxHeight) - low, MIN_BOX_HEIGHT);
        const glm::dquat orientation(tile.orientation);
        return {.position = tile.center + orientation * glm::dvec3(0.0, low, 0.0),
                .rotation = tile.orientation,
                .scale = glm::vec3(static_cast<float>(tile.size), static_cast<float>(height), static_cast<float>(tile.size))};
    }

    double sampleTerrainHeight(const TerrainHeights &heights, const TerrainMaterial &terrain, double u, double v)
    {
        if (heights.width < 4 || (heights.unorm16 == nullptr && heights.floats == nullptr))
            return 0.0;
        const uint32_t cells = heights.width - 3;
        const double x = std::clamp(u, 0.0, 1.0) * cells;
        const double z = std::clamp(v, 0.0, 1.0) * cells;
        const uint32_t i = std::min(static_cast<uint32_t>(x), cells - 1);
        const uint32_t j = std::min(static_cast<uint32_t>(z), cells - 1);
        const double fx = x - i;
        const double fz = z - j;
        // The grid's point (i, j) is sample (i + 1, j + 1): the border comes first.
        const double a = getCornerHeight(heights, terrain, i + 1, j + 1);
        const double b = getCornerHeight(heights, terrain, i + 1, j + 2);
        const double c = getCornerHeight(heights, terrain, i + 2, j + 1);
        const double d = getCornerHeight(heights, terrain, i + 2, j + 2);
        if (fx + fz <= 1.0)
            return a + fx * (c - a) + fz * (b - a);
        return d + (1.0 - fx) * (b - d) + (1.0 - fz) * (c - d);
    }

    glm::dvec3 getTerrainPosition(const TerrainTileDesc &tile, const TerrainHeights &heights, double u, double v)
    {
        const double x = (u - 0.5) * tile.size;
        const double z = (v - 0.5) * tile.size;
        const double y = sampleTerrainHeight(heights, tile.terrain, u, v) - getCurvatureDrop(x, z, tile.terrain.sphereRadius);
        return tile.center + glm::dquat(tile.orientation) * glm::dvec3(x, y, z);
    }

    // The grid has no LODs (each tile is a level of the application's tree) and no meshlets: the shader moves every
    // vertex, so bounds made from the flat grid would not hold.
    MeshHandle Renderer::getTerrainMesh(uint32_t cells)
    {
        if (cells == 0 || cells > MAX_TERRAIN_CELLS)
        {
            spdlog::error("getTerrainMesh: {} cells; a terrain grid has 1 to {}.", cells, MAX_TERRAIN_CELLS);
            return {};
        }
        if (terrainMeshes.size() <= cells)
            terrainMeshes.resize(MAX_TERRAIN_CELLS + 1);
        if (isLive(terrainMeshes[cells]))
            return terrainMeshes[cells];

        ObjRaw raw;
        buildTerrainGrid(cells, raw.vertices, raw.indices);
        Mesh mesh{.state = MeshState::READY, .subMeshes = {SubMesh{.transform = glm::mat4(1.0f)}}};
        if (!geometryPool->check(raw))
            return {};
        if (!geometryPool->allocate(raw, mesh.subMeshes[0].geometry))
        {
            geometryPool->logFull(1);
            return {};
        }
        if (!geometryPool->write(ctx, raw, mesh.subMeshes[0].geometry))
        {
            geometryPool->free(mesh.subMeshes[0].geometry);
            return {};
        }
        const HandleSlot slot = registry.meshHandles.allocate();
        const MeshHandle handle{slot.index, slot.generation};
        registry.meshes.resize(registry.meshHandles.size());
        registry.meshes[handle.index] = std::move(mesh);
        terrainMeshes[cells] = handle;
        return handle;
    }

    TerrainTile Renderer::createTerrainTile(const TerrainTileDesc &desc)
    {
        const TextureHandle heights = desc.terrain.heights;
        if (!ctx.isAlive(heights))
        {
            spdlog::error("createTerrainTile: the heights texture {{{}, {}}} is invalid or was destroyed.", heights.index, heights.generation);
            return {};
        }
        const TextureDesc &heightsDesc = ctx.getTextureDesc(heights);
        if (!isHeightFormat(heightsDesc.format) || heightsDesc.width != heightsDesc.height || heightsDesc.width < 4 || heightsDesc.width > MAX_TERRAIN_CELLS + 3)
        {
            spdlog::error("createTerrainTile: the heights '{}' are {} x {}; they must be R16_UNORM or R32_SFLOAT, square, with cells + 3 samples per side (4 to {}).", heightsDesc.name,
                          heightsDesc.width, heightsDesc.height, MAX_TERRAIN_CELLS + 3);
            return {};
        }
        if (!(desc.size > 0.0) || !std::isfinite(desc.size) || !(desc.terrain.maxHeight >= desc.terrain.minHeight) || !(desc.terrain.sphereRadius >= 0.0))
        {
            spdlog::error("createTerrainTile: the size ({}) must be above 0, maxHeight ({}) at least minHeight ({}), and sphereRadius ({}) 0 or more.", desc.size, desc.terrain.maxHeight,
                          desc.terrain.minHeight, desc.terrain.sphereRadius);
            return {};
        }
        const MeshHandle mesh = getTerrainMesh(heightsDesc.width - 3);
        if (!mesh.isValid())
            return {};
        // The imagery clamps at its edges, so a tile does not sample its neighbour's side of the image.
        std::vector<TextureHandle> textures;
        if (desc.imagery.isValid())
        {
            if (getBindlessTextureIndex(desc.imagery) == UINT32_MAX)
                addBindlessTexture(bindlessDescriptorSet, desc.imagery, SamplerType::LINEAR_CLAMP);
            textures.push_back(desc.imagery);
        }
        const MaterialHandle material = createMaterial(MaterialType::TERRAIN, textures);
        if (!material.isValid())
            return {};
        Material values = getMaterial(material);
        values.params[static_cast<uint32_t>(PbrParam::METALLIC)] = 0.0f;
        values.params[static_cast<uint32_t>(PbrParam::ROUGHNESS)] = desc.roughness;
        values.textureTransforms[static_cast<uint32_t>(PbrSlot::ALBEDO)] = desc.imageryRect;
        values.terrain = desc.terrain;
        updateMaterial(material, values);
        const EntityHandle entity = createEntity("terrain", mesh, material, getTerrainTransform(desc));
        if (!entity.isValid())
        {
            destroyMaterial(material);
            return {};
        }
        return {.entity = entity, .material = material};
    }
}
