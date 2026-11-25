#include "rasm/tileStreamer.h"

#include "rasm/renderer.h"
#include "renderer/gpuScene.h"
#include "renderer/uberMaterialTypes.h"
#include "scene/tileResidency.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rasm
{
    namespace
    {
        // The device budget is read every this many updates: often enough to follow other programs, and the frees of
        // the last frames (the release queue) do not make it jump.
        constexpr uint32_t BUDGET_REFRESH = 30;
        // Without a device-local heap budget: the automatic budget.
        constexpr uint64_t FALLBACK_BUDGET = 512ull << 20;

        struct TileContentLists
        {
            std::vector<EntityHandle> entities;
            std::vector<MeshHandle> meshes;
            std::vector<MaterialHandle> materials;
            std::vector<TextureHandle> textures;
        };

        enum class ContentState
        {
            WAITING,
            READY,
            FAILED,
        };
    }

    namespace
    {
        uint64_t &unit(TileCost &cost, TileUnit which)
        {
            return cost.units[static_cast<uint32_t>(which)];
        }

        // Left, right, bottom and top of a view-projection matrix (Gribb and Hartmann), in world space.
        void getSidePlanes(const glm::dmat4 &viewProjection, glm::dvec4 planes[4])
        {
            const glm::dvec4 row0(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0], viewProjection[3][0]);
            const glm::dvec4 row1(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1], viewProjection[3][1]);
            const glm::dvec4 row3(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3], viewProjection[3][3]);
            planes[0] = row3 + row0;
            planes[1] = row3 - row0;
            planes[2] = row3 + row1;
            planes[3] = row3 - row1;
            for (uint32_t i = 0; i < 4; ++i)
            {
                const double length = glm::length(glm::dvec3(planes[i]));
                planes[i] = length > 0.0 ? planes[i] / length : glm::dvec4(0.0, 0.0, 0.0, 1.0);
            }
        }
    }

    struct TileStreamer::Impl
    {
        Impl(Renderer &renderer, SceneHandle scene, const TileStreamerConfig &config)
            : renderer(renderer), scene(scene), config(config), residency(config)
        {
        }

        Renderer &renderer;
        SceneHandle scene;
        TileStreamerConfig config;
        TileResidency residency;
        std::vector<TileContentLists> contents; // by tile index
        TileView views[MAX_SCENE_VIEWS] = {};
        uint32_t viewCount = 0;
        std::vector<TileHandle> toLoad;
        std::vector<uint32_t> polled;
        uint64_t budget = 0;
        uint32_t budgetAge = BUDGET_REFRESH;
        VRAMStats memory = {};
        TileStreamerStats stats;

        bool sceneLive() const
        {
            return renderer.isLive(scene);
        }

        TileContentLists &contentOf(uint64_t index)
        {
            if (contents.size() <= index)
                contents.resize(index + 1);
            return contents[index];
        }

        // Bytes: the textures' allocations and the meshes' share of the geometry pools. Rows: of each fixed pool.
        TileCost measure(const TileContentLists &content) const
        {
            TileCost cost;
            RenderContext &ctx = renderer.ctx;
            for (const TextureHandle &texture : content.textures)
            {
                unit(cost, TileUnit::BYTES) += ctx.getTextureMemoryBytes(texture);
                ++unit(cost, TileUnit::TEXTURES);
            }
            for (const MeshHandle &handle : content.meshes)
            {
                if (!renderer.isLive(handle))
                    continue;
                const Mesh &mesh = renderer.registry.meshes[handle.index];
                for (const SubMesh &subMesh : mesh.subMeshes)
                {
                    const MeshGeometry &geometry = subMesh.geometry;
                    const uint64_t indices = static_cast<uint64_t>(geometry.indexCount) + geometry.lodIndexCount;
                    unit(cost, TileUnit::BYTES) += geometry.vertexCount * sizeof(Vertex) + geometry.colorCount * sizeof(uint32_t) + geometry.skinCount * sizeof(SkinVertex) +
                                                   geometry.morphCount * sizeof(MorphDelta) + indices * sizeof(uint32_t) + geometry.meshletCount * sizeof(gfx::GeometryMeshlet) +
                                                   geometry.meshletDataCount * sizeof(uint32_t);
                    unit(cost, TileUnit::VERTICES) += geometry.vertexCount;
                    unit(cost, TileUnit::INDICES) += indices;
                    unit(cost, TileUnit::MESHLETS) += geometry.meshletCount;
                    ++unit(cost, TileUnit::SUB_MESHES);
                }
                unit(cost, TileUnit::MATERIALS) += mesh.ownedMaterials.size();
                for (const TextureHandle &texture : mesh.ownedTextures)
                {
                    unit(cost, TileUnit::BYTES) += ctx.getTextureMemoryBytes(texture);
                    ++unit(cost, TileUnit::TEXTURES);
                }
            }
            unit(cost, TileUnit::MATERIALS) += content.materials.size();
            for (const EntityHandle &entity : content.entities)
            {
                const MeshHandle mesh = renderer.isLive(entity) ? renderer.registry.entities[entity.index].mesh : MeshHandle{};
                if (renderer.isLive(mesh))
                    unit(cost, TileUnit::INSTANCES) += renderer.registry.meshes[mesh.index].subMeshes.size();
            }
            return cost;
        }

        // The renderer's own state, not the loader's: a mesh READY with its import's textures filled, a texture out of
        // the loading set, and each entity with its instance rows and its textures, so it draws once it is in the scene.
        ContentState poll(const TileContentLists &content) const
        {
            for (const MeshHandle &mesh : content.meshes)
            {
                if (!renderer.isLive(mesh) || renderer.registry.meshes[mesh.index].state == MeshState::FAILED)
                    return ContentState::FAILED;
                if (!renderer.isLoaded(mesh))
                    return ContentState::WAITING;
            }
            for (const TextureHandle &texture : content.textures)
            {
                if (!renderer.ctx.isAlive(texture))
                    return ContentState::FAILED;
                if (!renderer.isLoaded(texture))
                    return ContentState::WAITING;
            }
            for (const EntityHandle &entity : content.entities)
            {
                if (!renderer.isLive(entity))
                    return ContentState::FAILED;
                const MeshHandle mesh = renderer.registry.entities[entity.index].mesh;
                if (!mesh.isValid())
                    continue;
                if (!renderer.isLive(mesh) || renderer.registry.meshes[mesh.index].state == MeshState::FAILED)
                    return ContentState::FAILED;
                const std::vector<EntityDrawState> &states = renderer.gpuScene->entities;
                if (renderer.registry.meshes[mesh.index].state != MeshState::READY || entity.index >= states.size() || states[entity.index].instanceCount == 0 ||
                    states[entity.index].waitingForTextures)
                    return ContentState::WAITING;
            }
            return ContentState::READY;
        }

        bool isLive(const TileContent &content) const
        {
            for (uint32_t i = 0; i < content.entityCount; ++i)
            {
                if (!renderer.isLive(content.entities[i]))
                    return false;
            }
            for (uint32_t i = 0; i < content.meshCount; ++i)
            {
                if (!renderer.isLive(content.meshes[i]))
                    return false;
            }
            for (uint32_t i = 0; i < content.materialCount; ++i)
            {
                if (!renderer.isLive(content.materials[i]))
                    return false;
            }
            for (uint32_t i = 0; i < content.textureCount; ++i)
            {
                if (!renderer.ctx.isAlive(content.textures[i]))
                    return false;
            }
            return true;
        }

        // The entities first, so nothing draws a mesh or a material that is gone. Handles the application destroyed
        // already are skipped.
        void destroy(const EntityHandle *entities, uint32_t entityCount, const MeshHandle *meshes, uint32_t meshCount, const MaterialHandle *materials, uint32_t materialCount,
                     const TextureHandle *textures, uint32_t textureCount)
        {
            for (uint32_t i = 0; i < entityCount; ++i)
            {
                if (renderer.isLive(entities[i]))
                    renderer.destroyEntity(entities[i]);
            }
            for (uint32_t i = 0; i < meshCount; ++i)
            {
                if (renderer.isLive(meshes[i]))
                    renderer.destroyMesh(meshes[i]);
            }
            for (uint32_t i = 0; i < materialCount; ++i)
            {
                if (renderer.isLive(materials[i]))
                    renderer.destroyMaterial(materials[i]);
            }
            for (uint32_t i = 0; i < textureCount; ++i)
            {
                if (renderer.ctx.isAlive(textures[i]))
                    renderer.destroyTexture(textures[i]);
            }
        }

        void destroyContent(uint64_t index)
        {
            if (index >= contents.size())
                return;
            TileContentLists &content = contents[index];
            destroy(content.entities.data(), static_cast<uint32_t>(content.entities.size()), content.meshes.data(), static_cast<uint32_t>(content.meshes.size()),
                    content.materials.data(), static_cast<uint32_t>(content.materials.size()), content.textures.data(), static_cast<uint32_t>(content.textures.size()));
            content.entities.clear();
            content.meshes.clear();
            content.materials.clear();
            content.textures.clear();
        }

        void show(uint64_t index, bool shown)
        {
            if (index >= contents.size() || !renderer.isLive(scene))
                return;
            for (const EntityHandle &entity : contents[index].entities)
            {
                if (!renderer.isLive(entity))
                    continue;
                if (shown)
                    renderer.addEntityToScene(scene, entity);
                else
                    renderer.removeEntityFromScene(scene, entity);
            }
        }

        bool makeView(const View &view, TileView &tileView)
        {
            if (!(view.scene == scene) || !renderer.isLive(view.camera))
                return false;
            uint32_t width = 0;
            uint32_t height = 0;
            if (view.output.target.isValid())
            {
                if (!renderer.ctx.isAlive(view.output.target))
                    return false;
                const RenderTarget &target = renderer.ctx.getRenderTarget(view.output.target);
                width = target.width;
                height = target.height;
            }
            else
            {
                const Window &window = renderer.getWindow(view.output.window);
                width = window.pixelWidth;
                height = window.pixelHeight;
            }
            const ViewPixels pixels = getViewPixels(view.output.rect, width, height);
            if (pixels.width == 0 || pixels.height == 0)
                return false;

            const Camera &camera = renderer.registry.cameras[view.camera.index];
            const glm::mat4 projection = renderer.getProjectionMatrix(view.camera, static_cast<float>(pixels.width) / static_cast<float>(pixels.height));
            const glm::dmat4 viewMatrix = renderer.getViewMatrix(view.camera);
            getSidePlanes(glm::dmat4(projection) * viewMatrix, tileView.planes);
            tileView.origin = camera.rig.transform.position;
            tileView.forward = glm::normalize(-glm::dvec3(viewMatrix[0][2], viewMatrix[1][2], viewMatrix[2][2]));
            tileView.pixelsPerUnit = std::abs(static_cast<double>(projection[1][1])) * 0.5 * static_cast<double>(pixels.height);
            tileView.orthographic = camera.projection.type == CameraProjection::Type::ORTHOGRAPHIC;
            return true;
        }

        // What the tiles may hold: the byte budget, and of each pool what nothing else holds.
        TileCost getLimits()
        {
            const TileCost &used = residency.getUsed();
            if (++budgetAge >= BUDGET_REFRESH)
            {
                budgetAge = 0;
                memory = renderer.queryMemoryStats();
                const uint64_t tiles = used.units[static_cast<uint32_t>(TileUnit::BYTES)];
                const uint64_t others = memory.vramUsage - std::min(memory.vramUsage, tiles);
                const uint64_t left = memory.vramBudget - std::min(memory.vramBudget, others);
                const uint64_t automatic = memory.vramBudget > 0 ? static_cast<uint64_t>(static_cast<double>(left) * static_cast<double>(config.deviceShare)) : FALLBACK_BUDGET;
                budget = config.memoryBudget > 0 ? config.memoryBudget : automatic;
            }

            const PoolStats pools = renderer.getPoolStats();
            const RendererConfig &rendererConfig = renderer.config;
            const uint64_t capacities[TILE_UNITS] = {budget,
                                                     rendererConfig.geometryPoolVertices,
                                                     rendererConfig.geometryPoolIndices,
                                                     rendererConfig.geometryPoolSubMeshes,
                                                     rendererConfig.geometryPoolMeshlets,
                                                     rendererConfig.maxInstances,
                                                     rendererConfig.maxMaterials,
                                                     renderer.ctx.getBindlessTextureCapacity()};
            const uint64_t inUse[TILE_UNITS] = {0, pools.vertices, pools.indices, pools.subMeshes, pools.meshlets, pools.instances, pools.materials, pools.bindlessTextures};
            TileCost limits;
            limits.units[0] = budget;
            for (uint32_t k = 1; k < TILE_UNITS; ++k)
            {
                const uint64_t others = inUse[k] - std::min(inUse[k], used.units[k]);
                limits.units[k] = capacities[k] - std::min(capacities[k], others);
            }
            return limits;
        }
    };

    TileStreamer::TileStreamer(Renderer &renderer, SceneHandle scene, const TileStreamerConfig &config)
        : impl(std::make_unique<Impl>(renderer, scene, config))
    {
        if (!impl->sceneLive())
            spdlog::error("TileStreamer: the scene handle is invalid, so no tile is drawn.");
    }

    TileStreamer::~TileStreamer()
    {
        for (uint64_t index = 0; index < impl->contents.size(); ++index)
            impl->destroyContent(index);
    }

    TileHandle TileStreamer::createTile(const TileDesc &desc)
    {
        return impl->residency.create(desc);
    }

    void TileStreamer::destroyTile(TileHandle tile)
    {
        const TileRecord *record = impl->residency.find(tile);
        if (record == nullptr)
        {
            spdlog::error("destroyTile: invalid or destroyed tile handle.");
            return;
        }
        if (record->drawn)
            impl->show(tile.index, false);
        impl->destroyContent(tile.index);
        impl->residency.destroy(tile);
    }

    void TileStreamer::setViews(const View *views, uint32_t viewCount)
    {
        impl->viewCount = 0;
        for (uint32_t i = 0; views != nullptr && i < viewCount && impl->viewCount < MAX_SCENE_VIEWS; ++i)
        {
            if (impl->makeView(views[i], impl->views[impl->viewCount]))
                ++impl->viewCount;
        }
    }

    double TileStreamer::getScreenError(const glm::dvec3 &center, double radius, double geometricError) const
    {
        return getTileScreenError(impl->views, impl->viewCount, center, radius, geometricError);
    }

    double TileStreamer::getScreenError(TileHandle tile) const
    {
        const TileRecord *record = impl->residency.find(tile);
        return record != nullptr ? getScreenError(record->center, record->radius, record->geometricError) : 0.0;
    }

    void TileStreamer::update(const TileHandle *wanted, uint32_t wantedCount)
    {
        ZoneScoped;
        // A renderer that did not start has no pools nor a device to measure: nothing loads.
        if (!impl->renderer.running())
            return;
        TileResidency &residency = impl->residency;

        // Loads that came in, or failed.
        impl->polled.assign(residency.getLoading().begin(), residency.getLoading().end());
        for (uint32_t index : impl->polled)
        {
            const ContentState state = impl->poll(impl->contentOf(index));
            if (state == ContentState::READY)
                residency.setReady(residency.handleOf(index), impl->measure(impl->contents[index]));
            else if (state == ContentState::WAITING)
                residency.updateLoading(residency.handleOf(index), impl->measure(impl->contents[index]));
            else if (state == ContentState::FAILED)
            {
                spdlog::warn("Tile {} failed to load: a mesh failed, or the application destroyed a handle of its content. It is tried again in {} frames.", index,
                             impl->config.retryFailedAfter);
                impl->destroyContent(index);
                residency.setFailed(residency.handleOf(index));
            }
        }

        residency.update(impl->views, impl->viewCount, wanted, wantedCount, impl->getLimits());

        // Hidden first, so a tile and its stand-in never draw together.
        for (uint32_t index : residency.getToHide())
            impl->show(index, false);
        for (uint32_t index : residency.getToDrop())
            impl->destroyContent(index);
        for (uint32_t index : residency.getToShow())
            impl->show(index, true);
        impl->toLoad.clear();
        for (uint32_t index : residency.getToLoad())
            impl->toLoad.push_back(residency.handleOf(index));

        impl->stats = residency.getStats();
        impl->stats.deviceBudget = impl->memory.vramBudget;
        impl->stats.deviceUsage = impl->memory.vramUsage;
    }

    const TileHandle *TileStreamer::getTilesToLoad(uint32_t &count) const
    {
        count = static_cast<uint32_t>(impl->toLoad.size());
        return impl->toLoad.data();
    }

    bool TileStreamer::setTileContent(TileHandle tile, const TileContent &content)
    {
        const TileRecord *record = impl->residency.find(tile);
        const bool accepted = record != nullptr && record->state != TileState::LOADING && record->state != TileState::READY;
        if (!accepted || !impl->isLive(content))
        {
            if (record == nullptr)
                spdlog::error("setTileContent: invalid or destroyed tile handle; its content is destroyed.");
            else if (!accepted)
                spdlog::error("setTileContent: tile {} has content already; the new content is destroyed.", tile.index);
            else
                spdlog::error("setTileContent: tile {}'s content has an invalid or destroyed handle; its content is destroyed and the tile failed.", tile.index);
            impl->destroy(content.entities, content.entityCount, content.meshes, content.meshCount, content.materials, content.materialCount, content.textures, content.textureCount);
            if (accepted)
                impl->residency.setFailed(tile);
            return false;
        }

        TileContentLists &lists = impl->contentOf(tile.index);
        lists.entities.assign(content.entities, content.entities + content.entityCount);
        lists.meshes.assign(content.meshes, content.meshes + content.meshCount);
        lists.materials.assign(content.materials, content.materials + content.materialCount);
        lists.textures.assign(content.textures, content.textures + content.textureCount);
        impl->residency.setLoading(tile, impl->measure(lists));
        return true;
    }

    TileState TileStreamer::getTileState(TileHandle tile) const
    {
        const TileRecord *record = impl->residency.find(tile);
        return record != nullptr ? record->state : TileState::EMPTY;
    }

    bool TileStreamer::isTileDrawn(TileHandle tile) const
    {
        const TileRecord *record = impl->residency.find(tile);
        return record != nullptr && record->drawn;
    }

    const TileStreamerStats &TileStreamer::getStats() const
    {
        return impl->stats;
    }

    TileStreamerConfig TileStreamer::getConfig() const
    {
        return impl->config;
    }
}
