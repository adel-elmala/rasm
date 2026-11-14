#include "renderer/gpuScene.h"
#include "rasm/scene.h"
#include "rasm/camera.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/geometryPool.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace rasm
{
    namespace
    {
        constexpr uint32_t CULL_GROUP_SIZE = 64; // numthreads of cull.slang
        constexpr uint32_t HIZ_GROUP_SIZE = 8;   // numthreads of hiz.slang, in x and y
        constexpr uint32_t CULL_EARLY = 0;       // the cull phases of cull.slang
        constexpr uint32_t CULL_LATE = 1;

        // A view's visible draw buffer holds the packed draws, then culling's scratch (a slot per draw and a count per
        // group, at most one group per CULL_GROUP_SIZE draws plus one per run), then the visible draws per run, which
        // the draws read as indirect counts.
        uint64_t getDrawCountsOffset(uint32_t capacity)
        {
            return static_cast<uint64_t>(capacity) * (sizeof(gfx::DrawInfo) + sizeof(uint32_t)) + (capacity / CULL_GROUP_SIZE + gfx::MAX_DRAW_RUNS) * sizeof(uint32_t);
        }

        uint64_t getVisibleDrawBufferSize(uint32_t capacity)
        {
            return getDrawCountsOffset(capacity) + gfx::MAX_DRAW_RUNS * sizeof(uint32_t);
        }

        void destroyBuffer(RenderContext &ctx, BufferHandle &buffer)
        {
            if (buffer.isValid())
                ctx.destroyBuffer(buffer);
            buffer = {};
        }

        // Level 0 is half the pass's size, rounded up, and each level halves the one before, down to 1x1 (occlusionCulling.slang).
        uint32_t getHizLevelCount(uint32_t width, uint32_t height, uint32_t &texelCount)
        {
            uint32_t levelWidth = (width + 1) / 2;
            uint32_t levelHeight = (height + 1) / 2;
            uint32_t levelCount = 1;
            texelCount = levelWidth * levelHeight;
            while (levelWidth > 1 || levelHeight > 1)
            {
                levelWidth = (levelWidth + 1) / 2;
                levelHeight = (levelHeight + 1) / 2;
                texelCount += levelWidth * levelHeight;
                ++levelCount;
            }
            return levelCount;
        }

        // The planes of clip space (-w <= x, y <= w and 0 <= z <= w) are sums of rows of projection * view, in world space.
        // The infinite far plane of reversed-Z comes out as (0, 0, 0, nearZ), which every sphere passes.
        void getFrustumPlanes(const glm::mat4 &viewProjection, glm::vec4 planes[6])
        {
            const glm::mat4 rows = glm::transpose(viewProjection);
            planes[0] = rows[3] + rows[0];
            planes[1] = rows[3] - rows[0];
            planes[2] = rows[3] + rows[1];
            planes[3] = rows[3] - rows[1];
            planes[4] = rows[2];
            planes[5] = rows[3] - rows[2];
            for (uint32_t i = 0; i < 6; ++i)
            {
                const float length = glm::length(glm::vec3(planes[i]));
                if (length > 0.0f)
                    planes[i] /= length;
            }
        }

        uint64_t getSceneDataAddress(RenderContext &ctx, const GpuScene &gpuScene, uint32_t frame, uint32_t view)
        {
            return ctx.getBufferDeviceAddress(gpuScene.sceneData[frame]) + view * sizeof(gfx::SceneData);
        }

        // glTF: a transform with a negative determinant mirrors the mesh, so its front faces turn clockwise (D8).
        // det(T * R * S * M) = sx * sy * sz * det(M).
        CullMode getCullMode(bool doubleSided, const Transform &transform, const glm::mat4 &meshTransform)
        {
            if (doubleSided)
                return CullMode::NONE;
            const float determinant = transform.scale.x * transform.scale.y * transform.scale.z * glm::determinant(glm::mat3(meshTransform));
            return determinant < 0.0f ? CullMode::FRONT : CullMode::BACK;
        }

        // Primitives without a material use the entity's; getMaterial resolves an invalid one to the default material.
        const Material &getSubMeshMaterial(Engine &engine, const Entity &entity, const SubMesh &subMesh)
        {
            return engine.getMaterial(subMesh.material.isValid() ? subMesh.material : entity.material);
        }

        bool texturesLoaded(Engine &engine, const Entity &entity, const Mesh &mesh)
        {
            for (const auto &subMesh : mesh.subMeshes)
            {
                for (auto texture : getSubMeshMaterial(engine, entity, subMesh).textures)
                {
                    if (texture.isValid() && !engine.isLoaded(texture))
                        return false;
                }
            }
            return true;
        }

        gfx::MaterialId getGfxMaterialId(MaterialType type)
        {
            switch (type)
            {
            case MaterialType::UNLIT:
                return gfx::MaterialId::UNLIT;
            default:
                return gfx::MaterialId::PBR;
            }
        }

        gfx::Material convertToGfxMaterial(Engine &engine, const Material &mat)
        {
            gfx::Material gmat{};

            // base color (albedo)
            gmat.pbr.baseColorFactor[0] = mat.params[static_cast<uint32_t>(PbrParam::ALBEDO)];
            gmat.pbr.baseColorFactor[1] = mat.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 1];
            gmat.pbr.baseColorFactor[2] = mat.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 2];

            // glTF texCoord set per texture; the shader selects the matching interpolated UV set.
            auto texCoordOf = [](uint32_t texCoord) -> uint32_t
            { return texCoord < MAX_VERTEX_UV_SETS ? texCoord : 0u; };

            gmat.pbr.baseColorTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::ALBEDO)]);
            gmat.pbr.baseColorTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::ALBEDO)]);
            gmat.pbr.metallicRoughnessTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::METALLIC)]);
            gmat.pbr.metallicRoughnessTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::METALLIC)]);

            gmat.pbr.metallicFactor = mat.params[static_cast<uint32_t>(PbrParam::METALLIC)];
            gmat.pbr.roughnessFactor = mat.params[static_cast<uint32_t>(PbrParam::ROUGHNESS)];

            // normal map
            gmat.normal.normalTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::NORMAL)]);
            gmat.normal.normalTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::NORMAL)]);
            gmat.normal.scale = mat.params[static_cast<uint32_t>(PbrParam::NORMAL_SCALE)];

            // occlusion map
            gmat.occlusion.occlusionTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::OCCLUSION)]);
            gmat.occlusion.occlusionTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::OCCLUSION)]);
            gmat.occlusion.strength = mat.params[static_cast<uint32_t>(PbrParam::OCCLUSION_STRENGTH)];

            // emissive map
            gmat.emissive.emissiveTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::EMISSIVE)]);
            gmat.emissive.emissiveTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::EMISSIVE)]);
            gmat.emissive.emissiveFactor[0] = mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR)];
            gmat.emissive.emissiveFactor[1] = mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR) + 1];
            gmat.emissive.emissiveFactor[2] = mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR) + 2];

            return gmat;
        }
    }

    static_assert(MAX_SCENES <= 16, "EntityDrawState::scenes has one bit per scene");
    static_assert(static_cast<uint32_t>(CullMode::BACK) + 1 == gfx::MAX_DRAW_RUNS, "a draw list has one run per cull mode");

    bool Engine::createGpuScene()
    {
        if (!gpuScene->instances.create(ctx, "Instance Table", sizeof(gfx::InstanceData), config.maxInstances) ||
            !gpuScene->materials.create(ctx, "Material Table", sizeof(gfx::MaterialData), MAX_MATERIALS))
        {
            spdlog::error("Failed to create the GPU scene tables.");
            return false;
        }

        // The CPU rewrites each view's SceneData every frame, so it stays host-visible.
        for (auto &buffer : gpuScene->sceneData)
        {
            buffer = ctx.createBuffer({.name = "Scene Data Buffer", .usage = BufferUsage::STORAGE, .size = MAX_SCENE_VIEWS * sizeof(gfx::SceneData), .hostVisible = true});
            if (!buffer.isValid())
            {
                spdlog::error("Failed to create the scene data buffer.");
                return false;
            }
        }

        // The frame's visible draws and triangles (low, high), which updateFrameStats reads back once the slot's fence passed.
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            gpuScene->cullStats[i] = ctx.createBuffer({.name = "Cull Stats Buffer " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = CULL_STATS_WORDS * sizeof(uint32_t), .hostVisible = true, .hostReadable = true});
            if (!gpuScene->cullStats[i].isValid())
            {
                spdlog::error("Failed to create the culling resources.");
                return false;
            }
        }
        return true;
    }

    // Runs once per frame, after the fence wait for this frame's slot, so the slot's buffers are free to rewrite.
    // Its cost follows what changed: only the scenes that hold a changed entity rebuild their draw lists (P7).
    void Engine::syncGpuScene()
    {
        ZoneScoped;

        auto &waitingForMesh = gpuScene->entitiesWaitingForMesh;
        for (size_t i = 0; i < waitingForMesh.size();)
        {
            const EntityHandle entity = waitingForMesh[i];
            const Entity &entityRef = registry.entities[entity.index];
            const bool meshKnown = entityRef.mesh.index < registry.meshes.size();
            if (meshKnown && registry.meshes[entityRef.mesh.index].state == MeshState::LOADING)
            {
                ++i;
                continue;
            }

            if (!isLoaded(entityRef.mesh))
            {
                spdlog::warn("Entity {} is not drawn: its mesh failed to load.", entity.index);
            }
            else
            {
                const Mesh &mesh = registry.meshes[entityRef.mesh.index];
                const auto rowCount = static_cast<uint32_t>(mesh.subMeshes.size());
                if (rowCount > gpuScene->instances.capacity() - gpuScene->instanceCount)
                {
                    spdlog::error("The instance table is full: entity {} needs {} rows ({}/{} in use). EngineConfig::maxInstances sets its size.",
                                  entity.index, rowCount, gpuScene->instanceCount, gpuScene->instances.capacity());
                }
                else
                {
                    for (const auto &subMesh : mesh.subMeshes)
                    {
                        if (subMesh.geometry.vertexCount == 0 || subMesh.geometry.indexCount == 0)
                            spdlog::warn("Skipping a mesh of entity {}: it has {} vertices and {} indices, and drawing needs both.",
                                         entity.index, subMesh.geometry.vertexCount, subMesh.geometry.indexCount);
                    }

                    // Rows are never freed: entities are not destroyed yet (D4).
                    if (gpuScene->entities.size() <= entity.index)
                        gpuScene->entities.resize(entity.index + 1);
                    EntityDrawState &state = gpuScene->entities[entity.index];
                    state.firstInstance = gpuScene->instanceCount;
                    state.instanceCount = rowCount;
                    state.waitingForTextures = !texturesLoaded(*this, entityRef, mesh);
                    gpuScene->instanceCount += rowCount;
                    gpuScene->instanceCullModes.resize(gpuScene->instanceCount, CullMode::NONE);
                    writeInstanceRows(entity);
                    if (state.waitingForTextures)
                        gpuScene->entitiesWaitingForTextures.push_back(entity);
                    else
                        markDrawListsDirty(entity);
                }
            }

            waitingForMesh[i] = waitingForMesh.back();
            waitingForMesh.pop_back();
        }

        // An entity is drawn only once the textures of all its sub-meshes are loaded, so nothing is drawn half-loaded.
        if (gpuScene->texturesFinished)
        {
            gpuScene->texturesFinished = false;
            auto &waitingForTextures = gpuScene->entitiesWaitingForTextures;
            for (size_t i = 0; i < waitingForTextures.size();)
            {
                const EntityHandle entity = waitingForTextures[i];
                const Entity &entityRef = registry.entities[entity.index];
                if (!texturesLoaded(*this, entityRef, registry.meshes[entityRef.mesh.index]))
                {
                    ++i;
                    continue;
                }

                gpuScene->entities[entity.index].waitingForTextures = false;
                markDrawListsDirty(entity);
                waitingForTextures[i] = waitingForTextures.back();
                waitingForTextures.pop_back();
            }
        }

        for (uint32_t i = 0; i < MAX_SCENES && i < registry.scenes.size(); ++i)
        {
            if (gpuScene->drawLists[i].rebuild)
                rebuildDrawList(registry.scenes[i].handle);
        }

        const auto frame = static_cast<uint32_t>(frameCount);
        gpuScene->instances.upload(ctx, frame);
        gpuScene->materials.upload(ctx, frame);
        for (auto &drawList : gpuScene->drawLists)
            uploadDrawList(drawList);
    }

    // Its rows are allocated once the mesh is ready (syncGpuScene).
    void Engine::queueInstanceRows(EntityHandle entity)
    {
        gpuScene->entitiesWaitingForMesh.push_back(entity);
    }

    void Engine::writeInstanceRows(EntityHandle entity)
    {
        if (entity.index >= gpuScene->entities.size())
            return;
        const EntityDrawState &state = gpuScene->entities[entity.index];
        if (state.instanceCount == 0)
            return;

        const Entity &entityRef = registry.entities[entity.index];
        const Mesh &mesh = registry.meshes[entityRef.mesh.index];
        const glm::mat4 entityMatrix = entityRef.transform.toMatrix();

        for (uint32_t i = 0; i < state.instanceCount; ++i)
        {
            const SubMesh &subMesh = mesh.subMeshes[i];
            const glm::mat4 model = entityMatrix * subMesh.transform;
            const gfx::InstanceData row{.worldMatrix = model,
                                        .normalMatrix = glm::transpose(glm::inverse(glm::mat3(model))),
                                        .geometryIndex = subMesh.geometry.geometryIndex,
                                        .materialIndex = static_cast<uint32_t>(getSubMeshMaterial(*this, entityRef, subMesh).handle.index)};
            gpuScene->instances.setRow(state.firstInstance + i, &row);
        }

        // A transform can start or stop mirroring the entity (D8).
        updateCullModes(entity);
    }

    // A changed cull mode moves the entity's draws to another cull-mode run, so its scenes rebuild their draw lists.
    void Engine::updateCullModes(EntityHandle entity)
    {
        const EntityDrawState &state = gpuScene->entities[entity.index];
        const Entity &entityRef = registry.entities[entity.index];
        const Mesh &mesh = registry.meshes[entityRef.mesh.index];

        bool changed = false;
        for (uint32_t i = 0; i < state.instanceCount; ++i)
        {
            const SubMesh &subMesh = mesh.subMeshes[i];
            const CullMode cullMode = getCullMode(getSubMeshMaterial(*this, entityRef, subMesh).doubleSided, entityRef.transform, subMesh.transform);
            CullMode &storedCullMode = gpuScene->instanceCullModes[state.firstInstance + i];
            changed = changed || storedCullMode != cullMode;
            storedCullMode = cullMode;
        }

        // An entity that waits for textures is in no draw list yet.
        if (changed && !state.waitingForTextures)
            markDrawListsDirty(entity);
    }

    // updateMaterial keeps the material's row index, so no instance row changes. Its instances may change cull mode,
    // and an entity that got a texture that still loads leaves the draw lists until the texture finishes.
    void Engine::refreshEntitiesAfterMaterialChange()
    {
        for (size_t i = 0; i < gpuScene->entities.size(); ++i)
        {
            EntityDrawState &state = gpuScene->entities[i];
            if (state.instanceCount == 0)
                continue;

            const Entity &entityRef = registry.entities[i];
            updateCullModes(entityRef.handle);
            if (!state.waitingForTextures && !texturesLoaded(*this, entityRef, registry.meshes[entityRef.mesh.index]))
            {
                state.waitingForTextures = true;
                gpuScene->entitiesWaitingForTextures.push_back(entityRef.handle);
                markDrawListsDirty(entityRef.handle);
            }
        }
    }

    void Engine::writeMaterialRow(MaterialHandle material)
    {
        const Material &materialRef = registry.materials[material.index];
        const gfx::MaterialData row{.material = convertToGfxMaterial(*this, materialRef), .materialId = getGfxMaterialId(materialRef.type)};
        gpuScene->materials.setRow(static_cast<uint32_t>(material.index), &row);
    }

    void Engine::addEntityToDrawList(SceneHandle scene, EntityHandle entity)
    {
        if (gpuScene->entities.size() <= entity.index)
            gpuScene->entities.resize(entity.index + 1);
        EntityDrawState &state = gpuScene->entities[entity.index];
        state.scenes |= static_cast<uint16_t>(1u << scene.index);

        // An entity whose mesh or textures still load joins the draw list when they finish (syncGpuScene).
        if (state.instanceCount > 0 && !state.waitingForTextures)
            gpuScene->drawLists[scene.index].rebuild = true;
    }

    void Engine::markDrawListsDirty(EntityHandle entity)
    {
        const uint16_t scenes = gpuScene->entities[entity.index].scenes;
        for (uint32_t i = 0; i < MAX_SCENES; ++i)
        {
            if ((scenes & (1u << i)) != 0)
                gpuScene->drawLists[i].rebuild = true;
        }
    }

    void Engine::rebuildDrawList(SceneHandle scene)
    {
        ZoneScoped;

        auto &drawList = gpuScene->drawLists[scene.index];
        drawList.rebuild = false;
        drawList.draws.clear();
        drawList.runs.clear();
        drawList.triangleCount = 0;

        for (auto entity : registry.scenes[scene.index].entities)
        {
            if (!entity.isValid() || entity.index >= gpuScene->entities.size())
                continue;
            const EntityDrawState &state = gpuScene->entities[entity.index];
            if (state.instanceCount == 0 || state.waitingForTextures)
                continue;

            const Mesh &mesh = registry.meshes[registry.entities[entity.index].mesh.index];
            for (uint32_t i = 0; i < state.instanceCount; ++i)
            {
                const MeshGeometry &geometry = mesh.subMeshes[i].geometry;
                if (geometry.vertexCount == 0 || geometry.indexCount == 0)
                    continue;
                // Indices are local to their sub-mesh; vertexOffset moves them to its place in the geometry pool.
                drawList.draws.push_back({.indexCount = geometry.indexCount,
                                          .instanceCount = 1,
                                          .firstIndex = geometry.firstIndex,
                                          .vertexOffset = static_cast<int32_t>(geometry.firstVertex),
                                          .firstInstance = state.firstInstance + i});
                drawList.triangleCount += geometry.indexCount / 3;
            }
        }

        // drawSceneView issues one indirect draw per run of draws with the same cull mode.
        const auto &cullModes = gpuScene->instanceCullModes;
        std::stable_sort(drawList.draws.begin(), drawList.draws.end(), [&](const gfx::DrawInfo &a, const gfx::DrawInfo &b)
                         { return cullModes[a.firstInstance] < cullModes[b.firstInstance]; });
        for (uint32_t i = 0; i < drawList.draws.size(); ++i)
        {
            const CullMode cullMode = cullModes[drawList.draws[i].firstInstance];
            if (drawList.runs.empty() || drawList.runs.back().cullMode != cullMode)
                drawList.runs.push_back({cullMode, i, 0});
            ++drawList.runs.back().count;
        }

        drawList.staleSlots = static_cast<uint8_t>((1u << MAX_FRAMES_IN_FLIGHT) - 1);
    }

    void Engine::uploadDrawList(SceneDrawList &drawList)
    {
        const auto frame = static_cast<uint32_t>(frameCount);
        const auto bit = static_cast<uint8_t>(1u << frame);
        if ((drawList.staleSlots & bit) == 0 || drawList.draws.empty())
            return;

        BufferHandle &buffer = drawList.buffers[frame];
        uint32_t &capacity = drawList.bufferCapacity[frame];
        const auto drawCount = static_cast<uint32_t>(drawList.draws.size());
        if (capacity < drawCount)
        {
            // Only this slot's last frame used the old buffer, and the fence wait covered it.
            if (buffer.isValid())
                ctx.destroyBuffer(buffer);
            capacity = std::max(drawCount, capacity * 2);
            buffer = ctx.createBuffer({.name = "Draw Info Buffer", .usage = BufferUsage::INDIRECT, .size = static_cast<uint64_t>(capacity) * sizeof(gfx::DrawInfo)});
            if (!buffer.isValid())
            {
                spdlog::error("Failed to create a draw info buffer of {} draws.", capacity);
                capacity = 0;
                return;
            }
        }

        if (ctx.fillBuffer(buffer, drawList.draws.data(), drawList.draws.size() * sizeof(gfx::DrawInfo), 0))
            drawList.staleSlots &= static_cast<uint8_t>(~bit);
    }

    // The scene in passes of the graph: culling, the draws, and with occlusion culling the Hi-Z build, the late culling
    // and the late draws. They draw into renderTarget, or the swapchain when it is invalid, so later passes on that
    // target load what they drew. Each view (scene, camera, target) culls into buffers of its own, kept from frame to
    // frame while a graph holds the view, which its passes declare: per-slot buffers by both slots, so their handles
    // hold for every frame. The cull stats are not declared: the views only add to them atomically, and only the CPU
    // reads them, after the slot's fence, so views need no order between them.
    bool Engine::addScenePasses(RenderGraph &graph, SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("addScenePasses needs a valid camera handle.");
            return false;
        }
        if (!scene.isValid() || scene.index >= MAX_SCENES || scene.index >= registry.scenes.size())
        {
            spdlog::error("addScenePasses needs a valid scene handle.");
            return false;
        }
        // The pipeline's formats must match the attachments' for the draw to be defined.
        if (renderTarget.isValid())
        {
            const auto &renderTargetData = ctx.getRenderTarget(renderTarget);
            if (renderTargetData.colorCount != 1 || renderTargetData.colorFormats[0] != swapchain.imageFormat || renderTargetData.depthFormat != DEPTH_FORMAT)
            {
                spdlog::error("addScenePasses needs a render target with one color attachment in the swapchain's format and D32_SFLOAT depth.");
                return false;
            }
        }

        // A view's passes run once per frame: a second set would read its own visibility as last frame's.
        for (uint32_t heldView : graph.sceneViews)
        {
            const SceneView &held = gpuScene->views[heldView];
            if (held.scene == scene && held.camera == camera && held.renderTarget == renderTarget)
            {
                spdlog::error("addScenePasses was called twice for the same scene, camera and target in one graph.");
                return false;
            }
        }

        const uint32_t view = createSceneView(scene, camera, renderTarget);
        if (view == UINT32_MAX)
            return false;
        graph.sceneViews.push_back(view);
        const SceneView &sceneView = gpuScene->views[view];
        const bool occlusion = sceneView.visibility.isValid();

        auto writeCulling = [&sceneView](PassBuilder &builder)
        {
            for (auto buffer : sceneView.visibleDraws)
                builder.write(buffer);
        };
        auto drawScenePass = [&sceneView, renderTarget](PassBuilder &builder)
        {
            for (auto buffer : sceneView.visibleDraws)
                builder.read(buffer);
            if (renderTarget.isValid())
                builder.write(renderTarget);
            else
                builder.writeSwapchain();
        };

        graph.addComputePass(
            occlusion ? "Cull 1" : "Cull",
            [&](PassBuilder &builder)
            {
                writeCulling(builder);
                if (occlusion)
                    builder.readPrevious(sceneView.visibility);
            },
            [this, view](RenderContext &)
            {
                prepareSceneView(view);
                recordSceneCulling(view, CULL_EARLY);
            });
        graph.addPass(
            occlusion ? "Scene 1" : "Scene",
            drawScenePass,
            [this, view](RenderContext &)
            { drawSceneView(view); });
        if (!occlusion)
            return true;

        graph.addComputePass(
            "Hi-Z",
            [&](PassBuilder &builder)
            {
                if (renderTarget.isValid())
                    builder.readDepth(renderTarget);
                else
                    builder.readSwapchainDepth();
                for (auto hiz : sceneView.hiz)
                    builder.write(hiz);
            },
            [this, view](RenderContext &)
            { recordHiz(view); });
        graph.addComputePass(
            "Cull 2",
            [&](PassBuilder &builder)
            {
                for (auto hiz : sceneView.hiz)
                    builder.read(hiz);
                writeCulling(builder);
                builder.write(sceneView.visibility);
            },
            [this, view](RenderContext &)
            {
                if (gpuScene->views[view].hizLevelCount > 0)
                    recordSceneCulling(view, CULL_LATE);
            });
        graph.addPass(
            "Scene 2",
            drawScenePass,
            [this, view](RenderContext &)
            {
                if (gpuScene->views[view].hizLevelCount > 0)
                    drawSceneView(view);
            });
        return true;
    }

    // A view lives while a graph holds it, so graphs built for the same scene, camera and target share it. Its
    // buffers start small and grow in prepareSceneView under the same handles.
    uint32_t Engine::createSceneView(SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget)
    {
        uint32_t freeSlot = UINT32_MAX;
        for (uint32_t i = 0; i < MAX_SCENE_VIEWS; ++i)
        {
            SceneView &view = gpuScene->views[i];
            if (view.graphs == 0)
            {
                freeSlot = std::min(freeSlot, i);
                continue;
            }
            if (view.scene == scene && view.camera == camera && view.renderTarget == renderTarget)
            {
                ++view.graphs;
                return i;
            }
        }
        if (freeSlot == UINT32_MAX)
        {
            spdlog::error("At most {} views (scene, camera, target) can be held by render graphs at once.", MAX_SCENE_VIEWS);
            return UINT32_MAX;
        }

        SceneView view = {.scene = scene, .camera = camera, .renderTarget = renderTarget, .graphs = 1};
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            view.visibleCapacity[i] = CULL_GROUP_SIZE;
            view.visibleDraws[i] = ctx.createBuffer({.name = "Visible Draw Buffer", .usage = BufferUsage::INDIRECT, .size = getVisibleDrawBufferSize(CULL_GROUP_SIZE)});
            if (!view.visibleDraws[i].isValid())
            {
                spdlog::error("Failed to create the culling buffers of a scene view.");
                for (auto &buffer : view.visibleDraws)
                    destroyBuffer(ctx, buffer);
                return UINT32_MAX;
            }
        }

        // The view's visibility, one flag per instance row, is kept across frames and shared by both frame slots: the
        // GPU runs the frames in order. Rows start unseen, so a new instance is drawn late first. Without these
        // buffers the view is only frustum culled.
        if (config.occlusionCulling)
        {
            bool created = true;
            for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
            {
                view.hizCapacity[i] = 1;
                view.hiz[i] = ctx.createBuffer({.name = "Hi-Z Buffer", .usage = BufferUsage::STORAGE, .size = sizeof(float)});
                created = created && view.hiz[i].isValid();
            }
            const std::vector<uint32_t> unseen(config.maxInstances, 0);
            view.visibility = ctx.createBuffer({.name = "Visibility Buffer", .usage = BufferUsage::STORAGE, .size = unseen.size() * sizeof(uint32_t)});
            created = created && view.visibility.isValid() && ctx.fillBuffer(view.visibility, unseen.data(), unseen.size() * sizeof(uint32_t));
            if (!created)
            {
                spdlog::error("Failed to create the occlusion culling buffers of a scene view; it is drawn with frustum culling only.");
                for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
                {
                    destroyBuffer(ctx, view.hiz[i]);
                    view.hizCapacity[i] = 0;
                }
                destroyBuffer(ctx, view.visibility);
            }
        }
        gpuScene->views[freeSlot] = view;
        return freeSlot;
    }

    // A graph lets go of a view when it is destroyed or replaced. The last one frees the view's buffers once the
    // frames that used them are done.
    void Engine::releaseSceneView(uint32_t viewIndex)
    {
        SceneView &view = gpuScene->views[viewIndex];
        if (view.graphs == 0 || --view.graphs > 0)
            return;
        ctx.waitForFence(frameFence, frameFenceValue);
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            destroyBuffer(ctx, view.visibleDraws[i]);
            destroyBuffer(ctx, view.hiz[i]);
        }
        destroyBuffer(ctx, view.visibility);
        view = {};
    }

    // The view's first pass runs it, before anything of the view is recorded. It decides what the view's passes do
    // this frame: nothing for an empty scene or one whose entities still load (the passes keep their cleared
    // attachments), the draws without culling when the culling pipelines fail, and no late half without a Hi-Z.
    void Engine::prepareSceneView(uint32_t viewIndex)
    {
        ZoneScoped;

        SceneView &view = gpuScene->views[viewIndex];
        view.ready = false;
        view.culled = false;
        view.groupCount = 0;
        view.hizLevelCount = 0;

        const auto &drawList = gpuScene->drawLists[view.scene.index];
        const auto frame = static_cast<uint32_t>(frameCount);
        if (drawList.draws.empty() || (drawList.staleSlots & (1u << frame)) != 0)
            return;

        // A shader error retries every frame, so a shader fixed on disk is picked up while the application runs.
        if (!gpuScene->pipeline.isValid())
        {
            gpuScene->pipeline = createPipeline("rasm", readFile(pathFromUtf8(config.shaderDir) / "passes/rasm.slang"));
            if (!gpuScene->pipeline.isValid())
            {
                spdlog::error("Failed to create the uber material pipeline for the scene.");
                return;
            }
        }
        // Without the culling pipelines the scene is still drawn, only unculled, so a culling shader error does not blank it.
        if (!gpuScene->cullCountPipeline.isValid() || !gpuScene->cullWritePipeline.isValid())
        {
            const std::string cullSource = readFile(pathFromUtf8(config.shaderDir) / "passes/cull.slang");
            gpuScene->cullCountPipeline = createComputePipeline("cullCount", cullSource, "countMain");
            gpuScene->cullWritePipeline = createComputePipeline("cullWrite", cullSource, "writeMain");
            if (!gpuScene->cullCountPipeline.isValid() || !gpuScene->cullWritePipeline.isValid())
                spdlog::error("Failed to create the culling pipelines; the scene is drawn without culling.");
        }
        const bool culled = gpuScene->cullCountPipeline.isValid() && gpuScene->cullWritePipeline.isValid();

        // Without the Hi-Z pipeline it is still frustum culled.
        if (culled && config.occlusionCulling && !gpuScene->hizPipeline.isValid())
        {
            gpuScene->hizPipeline = createComputePipeline("hiz", readFile(pathFromUtf8(config.shaderDir) / "passes/hiz.slang"), "hizMain");
            if (!gpuScene->hizPipeline.isValid())
                spdlog::error("Failed to create the Hi-Z pipeline; the scene is drawn without occlusion culling.");
        }

        uint32_t width = swapchain.width;
        uint32_t height = swapchain.height;
        TextureHandle depthTexture = frameResources[frame].depthTexture;
        if (view.renderTarget.isValid())
        {
            const auto &renderTargetData = ctx.getRenderTarget(view.renderTarget);
            width = renderTargetData.width;
            height = renderTargetData.height;
            depthTexture = renderTargetData.attachments[frame].depth;
        }

        // The buffer holds the packed draws, then culling's scratch. Only this slot's last frame used the old
        // buffer, and the fence wait covered it.
        const auto drawCount = static_cast<uint32_t>(drawList.draws.size());
        uint32_t &visibleCapacity = view.visibleCapacity[frame];
        if (culled && visibleCapacity < drawCount)
        {
            const uint32_t capacity = std::max(drawCount, visibleCapacity * 2);
            if (!ctx.resizeBuffer(view.visibleDraws[frame], getVisibleDrawBufferSize(capacity)))
            {
                spdlog::error("Failed to grow a visible draw buffer to {} draws.", capacity);
                return;
            }
            visibleCapacity = capacity;
        }

        // Occlusion culling samples the pass's depth, which is in the bindless set from its creation
        // (createSwapchainResources, RenderContext::createAttachments). Its Hi-Z covers the whole attachment.
        const uint32_t depthSlot = getBindlessTextureIndex(depthTexture);
        if (culled && config.occlusionCulling && gpuScene->hizPipeline.isValid() && depthSlot != UINT32_MAX && view.visibility.isValid())
        {
            uint32_t texelCount = 0;
            const uint32_t levelCount = getHizLevelCount(width, height, texelCount);
            uint32_t &hizCapacity = view.hizCapacity[frame];
            if (hizCapacity < texelCount)
            {
                if (ctx.resizeBuffer(view.hiz[frame], static_cast<uint64_t>(texelCount) * sizeof(float)))
                    hizCapacity = texelCount;
                else
                    spdlog::error("Failed to grow a Hi-Z buffer to {} texels; the scene is drawn without occlusion culling.", texelCount);
            }
            if (hizCapacity >= texelCount)
                view.hizLevelCount = levelCount;
        }

        const auto &sceneRef = registry.scenes[view.scene.index];
        const auto &cameraRef = registry.cameras[view.camera.index];

        gfx::SceneData sceneData{};
        sceneData.projection = getProjectionMatrix(view.camera, static_cast<float>(width) / static_cast<float>(height));
        sceneData.view = getViewMatrix(view.camera);
        getFrustumPlanes(sceneData.projection * sceneData.view, sceneData.frustumPlanes);
        sceneData.cameraWorldPosition = cameraRef.transform.position;
        sceneData.numLights = static_cast<uint32_t>(sceneRef.lights.size());
        for (uint32_t i = 0; i < sceneData.numLights; ++i)
        {
            const Light &sceneLight = registry.lights[sceneRef.lights[i].index];

            gfx::Light light{};
            light.position = sceneLight.position;
            light.direction = sceneLight.direction;
            light.color = sceneLight.color;
            light.intensity = sceneLight.intensity;
            light.range = sceneLight.range;
            light.type = static_cast<gfx::LightType>(sceneLight.type);

            sceneData.lights[i] = light;
        }
        sceneData.vertsPtr = ctx.getBufferDeviceAddress(geometryPool->vertexBuffer());
        sceneData.drawInfoPtr = ctx.getBufferDeviceAddress(culled ? view.visibleDraws[frame] : drawList.buffers[frame]);
        sceneData.instancesPtr = ctx.getBufferDeviceAddress(gpuScene->instances.buffer(frame));
        sceneData.materialsPtr = ctx.getBufferDeviceAddress(gpuScene->materials.buffer(frame));
        sceneData.geometryPtr = ctx.getBufferDeviceAddress(geometryPool->tableBuffer());
        sceneData.allDrawsPtr = ctx.getBufferDeviceAddress(drawList.buffers[frame]);
        if (culled)
        {
            sceneData.cullSlotsPtr = sceneData.drawInfoPtr + static_cast<uint64_t>(visibleCapacity) * sizeof(gfx::DrawInfo);
            sceneData.groupCountsPtr = sceneData.cullSlotsPtr + static_cast<uint64_t>(visibleCapacity) * sizeof(uint32_t);
            sceneData.drawCountsPtr = sceneData.drawInfoPtr + getDrawCountsOffset(visibleCapacity);
            sceneData.cullStatsPtr = ctx.getBufferDeviceAddress(gpuScene->cullStats[frame]);
        }
        if (view.hizLevelCount > 0)
        {
            sceneData.visibilityPtr = ctx.getBufferDeviceAddress(view.visibility);
            sceneData.hizPtr = ctx.getBufferDeviceAddress(view.hiz[frame]);
        }
        sceneData.drawCount = drawCount;
        sceneData.runCount = static_cast<uint32_t>(drawList.runs.size());
        for (uint32_t i = 0; i < sceneData.runCount; ++i)
            sceneData.runFirst[i] = drawList.runs[i].first;
        sceneData.hizLevelCount = view.hizLevelCount;
        sceneData.depthTexture = depthSlot;
        sceneData.viewportWidth = width;
        sceneData.viewportHeight = height;
        sceneData.lodErrorPixels = config.lodErrorPixels;
        sceneData.forcedLod = config.forcedLod;
        ctx.fillBuffer(gpuScene->sceneData[frame], &sceneData, sizeof(sceneData), viewIndex * sizeof(gfx::SceneData));

        // cull.slang's groups each lie inside one run.
        if (culled)
        {
            for (const DrawRun &run : drawList.runs)
                view.groupCount += (run.count + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE;
        }

        // Culling counts its visible draws on the GPU; an unculled view draws all of them.
        DrawTotals &totals = gpuScene->drawTotals[frame];
        totals.draws += drawCount;
        totals.triangles += drawList.triangleCount;
        if (!culled)
        {
            totals.unculledDraws += drawCount;
            totals.unculledTriangles += drawList.triangleCount;
        }

        view.width = width;
        view.height = height;
        view.culled = culled;
        view.ready = true;
    }

    // countMain, then writeMain, which reads the counts and slots countMain wrote. The early phase draws what was
    // visible last frame; the late one, after the Hi-Z, what the early one left out.
    void Engine::recordSceneCulling(uint32_t viewIndex, uint32_t phase)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.ready || !view.culled)
            return;

        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex), .argument = phase};
        ctx.bindPipeline(commandBuffer, gpuScene->cullCountPipeline);
        ctx.pushConstants(commandBuffer, gpuScene->cullCountPipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, view.groupCount);
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        ctx.bindPipeline(commandBuffer, gpuScene->cullWritePipeline);
        ctx.pushConstants(commandBuffer, gpuScene->cullWritePipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, view.groupCount);
    }

    // Each level reads the one before, level 0 the depth the early draws left.
    void Engine::recordHiz(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (view.hizLevelCount == 0)
            return;

        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex)};
        ctx.bindPipeline(commandBuffer, gpuScene->hizPipeline);
        ctx.bindDescriptorSet(commandBuffer, gpuScene->hizPipeline, bindlessDescriptorSet, 0);
        uint32_t levelWidth = view.width;
        uint32_t levelHeight = view.height;
        for (uint32_t level = 0; level < view.hizLevelCount; ++level)
        {
            if (level > 0)
                ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
            levelWidth = (levelWidth + 1) / 2;
            levelHeight = (levelHeight + 1) / 2;
            pushConstants.argument = level;
            ctx.pushConstants(commandBuffer, gpuScene->hizPipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
            ctx.dispatch(commandBuffer, (levelWidth + HIZ_GROUP_SIZE - 1) / HIZ_GROUP_SIZE, (levelHeight + HIZ_GROUP_SIZE - 1) / HIZ_GROUP_SIZE);
        }
    }

    // Culling packs a run's visible draws at the start of its range and counts them, so each run is one indirect
    // draw with a count; an unculled view draws its whole list.
    void Engine::drawSceneView(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.ready)
            return;

        const auto frame = static_cast<uint32_t>(frameCount);
        const auto &drawList = gpuScene->drawLists[view.scene.index];
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const uint64_t drawCountsOffset = getDrawCountsOffset(view.visibleCapacity[frame]);
        ctx.bindPipeline(commandBuffer, gpuScene->pipeline);
        ctx.bindDescriptorSet(commandBuffer, gpuScene->pipeline, bindlessDescriptorSet, 0);
        ctx.bindIndexBuffer(commandBuffer, geometryPool->indexBuffer(), 0, Format::U32_UINT);

        gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex)};
        for (uint32_t i = 0; i < drawList.runs.size(); ++i)
        {
            const DrawRun &run = drawList.runs[i];
            pushConstants.firstDraw = run.first;
            ctx.setCullMode(commandBuffer, run.cullMode);
            ctx.pushConstants(commandBuffer, gpuScene->pipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
            if (view.culled)
                ctx.drawIndexedIndirectCount(commandBuffer, view.visibleDraws[frame], run.first * sizeof(gfx::DrawInfo), view.visibleDraws[frame],
                                             drawCountsOffset + i * sizeof(uint32_t), run.count, sizeof(gfx::DrawInfo));
            else
                ctx.drawIndexedIndirect(commandBuffer, drawList.buffers[frame], run.first * sizeof(gfx::DrawInfo), run.count, sizeof(gfx::DrawInfo));
        }
        // beginPass sets NONE for every other pipeline.
        ctx.setCullMode(commandBuffer, CullMode::NONE);
    }
}
