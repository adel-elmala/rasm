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

        // The CPU rewrites SceneData in every drawScene call, so it stays host-visible.
        for (auto &buffer : gpuScene->sceneData)
        {
            buffer = ctx.createBuffer({.name = "Scene Data Buffer", .usage = BufferUsage::STORAGE, .size = MAX_SCENE_DRAWS_PER_FRAME * sizeof(gfx::SceneData), .hostVisible = true});
            if (!buffer.isValid())
            {
                spdlog::error("Failed to create the scene data buffer.");
                return false;
            }
        }

        // Culling writes a visible draw count per run of each drawScene call; the draws read them as indirect counts.
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            gpuScene->cullCommandBuffers[i] = ctx.createCommandBuffer("Cull Command Buffer " + std::to_string(i));
            gpuScene->drawCounts[i] = ctx.createBuffer({.name = "Draw Count Buffer " + std::to_string(i),
                                                        .usage = BufferUsage::INDIRECT,
                                                        .size = static_cast<uint64_t>(MAX_SCENE_DRAWS_PER_FRAME) * gfx::MAX_DRAW_RUNS * sizeof(uint32_t)});
            // The frame's visible draws and triangles (low, high), which updateFrameStats reads back once the slot's fence passed.
            gpuScene->cullStats[i] = ctx.createBuffer({.name = "Cull Stats Buffer " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = CULL_STATS_WORDS * sizeof(uint32_t), .hostVisible = true, .hostReadable = true});
            if (!gpuScene->cullCommandBuffers[i].isValid() || !gpuScene->drawCounts[i].isValid() || !gpuScene->cullStats[i].isValid())
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

        // drawScene issues one indirect draw per run of draws with the same cull mode.
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

    void Engine::drawScene(SceneHandle scene, CameraHandle camera)
    {
        ZoneScoped;

        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("drawScene needs a valid camera handle.");
            return;
        }
        if (!scene.isValid() || scene.index >= MAX_SCENES || scene.index >= registry.scenes.size())
        {
            spdlog::error("drawScene needs a valid scene handle.");
            return;
        }

        // An empty scene, or one whose entities still load, draws nothing; the pass keeps its cleared attachments.
        const auto &drawList = gpuScene->drawLists[scene.index];
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
        TextureHandle colorTexture = swapchain.imageHandles[imageIdx];
        TextureHandle depthTexture = frameResources[frame].depthTexture;

        if (currentRenderTarget.isValid())
        {
            const auto &renderTargetData = ctx.getRenderTarget(currentRenderTarget);
            width = renderTargetData.width;
            height = renderTargetData.height;
            colorTexture = renderTargetData.attachments[frame].color;
            depthTexture = renderTargetData.attachments[frame].depth;

            // The pipeline's formats must match the attachments' for the draw to be defined.
            if (renderTargetData.colorFormat != swapchain.imageFormat || renderTargetData.depthFormat != DEPTH_FORMAT)
            {
                spdlog::error("drawScene needs a render target with the swapchain's color format and D32_SFLOAT depth.");
                return;
            }
        }

        // The GPU reads SceneData after the whole frame is recorded, so every call needs its own slot.
        if (sceneDrawCount >= MAX_SCENE_DRAWS_PER_FRAME)
        {
            spdlog::error("Exceeded maximum number of drawScene calls per frame.");
            return;
        }
        const uint32_t call = sceneDrawCount++;
        const uint64_t sceneDataOffset = call * sizeof(gfx::SceneData);
        const uint64_t drawCountsOffset = static_cast<uint64_t>(call) * gfx::MAX_DRAW_RUNS * sizeof(uint32_t);
        const auto drawCount = static_cast<uint32_t>(drawList.draws.size());
        gpuScene->cullGroupCounts[call] = 0;

        // Each call culls into its own list, so passes with different cameras can draw one scene in a frame.
        // The buffer holds the packed draws, then culling's scratch: a slot per draw and a count per group, at
        // most one group per CULL_GROUP_SIZE draws plus one per run. Only this slot's last frame used the old
        // buffer, and the fence wait covered it.
        BufferHandle &visibleDraws = gpuScene->visibleDraws[frame][call];
        uint32_t &visibleCapacity = gpuScene->visibleCapacity[frame][call];
        if (culled && visibleCapacity < drawCount)
        {
            if (visibleDraws.isValid())
                ctx.destroyBuffer(visibleDraws);
            visibleCapacity = std::max(drawCount, visibleCapacity * 2);
            const uint64_t size = static_cast<uint64_t>(visibleCapacity) * (sizeof(gfx::DrawInfo) + sizeof(uint32_t)) +
                                  (visibleCapacity / CULL_GROUP_SIZE + gfx::MAX_DRAW_RUNS) * sizeof(uint32_t);
            visibleDraws = ctx.createBuffer({.name = "Visible Draw Buffer", .usage = BufferUsage::INDIRECT, .size = size});
            if (!visibleDraws.isValid())
            {
                spdlog::error("Failed to create a visible draw buffer of {} draws.", visibleCapacity);
                visibleCapacity = 0;
                return;
            }
        }

        // Occlusion culling samples the pass's depth, which is in the bindless set from its creation
        // (createSwapchainResources, createRenderTarget). Its Hi-Z covers the whole attachment and its test maps
        // NDC onto it, so another viewport or depth range only gets frustum culling.
        uint32_t hizLevelCount = 0;
        const uint32_t depthSlot = getBindlessTextureIndex(depthTexture);
        const Viewport &viewport = ctx.getViewport();
        const bool fullViewport = viewport.x == 0.0f && viewport.y == 0.0f && viewport.width == static_cast<float>(width) &&
                                  viewport.height == static_cast<float>(height) && viewport.minDepth == 0.0f && viewport.maxDepth == 1.0f;
        if (culled && config.occlusionCulling && gpuScene->hizPipeline.isValid() && depthSlot != UINT32_MAX && fullViewport)
        {
            // The call's visibility, one flag per instance row, is kept across frames and shared by both frame
            // slots: the GPU runs the frames in order. Rows start unseen, so a new instance is drawn late first.
            BufferHandle &visibility = gpuScene->visibility[call];
            if (!visibility.isValid())
            {
                const std::vector<uint32_t> unseen(config.maxInstances, 0);
                visibility = ctx.createBuffer({.name = "Visibility Buffer", .usage = BufferUsage::STORAGE, .size = unseen.size() * sizeof(uint32_t)});
                if (visibility.isValid() && !ctx.fillBuffer(visibility, unseen.data(), unseen.size() * sizeof(uint32_t)))
                {
                    ctx.destroyBuffer(visibility);
                    visibility = {};
                }
                if (!visibility.isValid())
                    spdlog::error("Failed to create a visibility buffer; the scene is drawn without occlusion culling.");
            }

            // The call's Hi-Z, grown like its visible draws.
            uint32_t texelCount = 0;
            const uint32_t levelCount = getHizLevelCount(width, height, texelCount);
            BufferHandle &hiz = gpuScene->hiz[frame][call];
            uint32_t &hizCapacity = gpuScene->hizCapacity[frame][call];
            if (hizCapacity < texelCount)
            {
                if (hiz.isValid())
                    ctx.destroyBuffer(hiz);
                hizCapacity = texelCount;
                hiz = ctx.createBuffer({.name = "Hi-Z Buffer", .usage = BufferUsage::STORAGE, .size = static_cast<uint64_t>(texelCount) * sizeof(float)});
                if (!hiz.isValid())
                {
                    spdlog::error("Failed to create a Hi-Z buffer of {} texels; the scene is drawn without occlusion culling.", texelCount);
                    hizCapacity = 0;
                }
            }
            if (visibility.isValid() && hiz.isValid())
                hizLevelCount = levelCount;
        }

        auto &sceneRef = registry.scenes[scene.index];
        auto commandBuffer = frameResources[frameCount].commandBuffer;
        auto &cameraRef = registry.cameras[camera.index];

        gfx::SceneData sceneData{};
        sceneData.projection = getProjectionMatrix(camera, static_cast<float>(width) / static_cast<float>(height));
        sceneData.view = getViewMatrix(camera);
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
        sceneData.drawInfoPtr = ctx.getBufferDeviceAddress(culled ? visibleDraws : drawList.buffers[frame]);
        sceneData.instancesPtr = ctx.getBufferDeviceAddress(gpuScene->instances.buffer(frame));
        sceneData.materialsPtr = ctx.getBufferDeviceAddress(gpuScene->materials.buffer(frame));
        sceneData.geometryPtr = ctx.getBufferDeviceAddress(geometryPool->tableBuffer());
        sceneData.allDrawsPtr = ctx.getBufferDeviceAddress(drawList.buffers[frame]);
        sceneData.drawCountsPtr = ctx.getBufferDeviceAddress(gpuScene->drawCounts[frame]) + drawCountsOffset;
        if (culled)
        {
            sceneData.cullSlotsPtr = sceneData.drawInfoPtr + static_cast<uint64_t>(visibleCapacity) * sizeof(gfx::DrawInfo);
            sceneData.groupCountsPtr = sceneData.cullSlotsPtr + static_cast<uint64_t>(visibleCapacity) * sizeof(uint32_t);
            sceneData.cullStatsPtr = ctx.getBufferDeviceAddress(gpuScene->cullStats[frame]);
        }
        if (hizLevelCount > 0)
        {
            sceneData.visibilityPtr = ctx.getBufferDeviceAddress(gpuScene->visibility[call]);
            sceneData.hizPtr = ctx.getBufferDeviceAddress(gpuScene->hiz[frame][call]);
        }
        sceneData.drawCount = drawCount;
        sceneData.runCount = static_cast<uint32_t>(drawList.runs.size());
        for (uint32_t i = 0; i < sceneData.runCount; ++i)
            sceneData.runFirst[i] = drawList.runs[i].first;
        sceneData.hizLevelCount = hizLevelCount;
        sceneData.depthTexture = depthSlot;
        sceneData.viewportWidth = width;
        sceneData.viewportHeight = height;
        sceneData.lodErrorPixels = config.lodErrorPixels;
        sceneData.forcedLod = config.forcedLod;
        ctx.fillBuffer(gpuScene->sceneData[frame], &sceneData, sizeof(sceneData), sceneDataOffset);

        // cull.slang's groups each lie inside one run.
        if (culled)
        {
            for (const DrawRun &run : drawList.runs)
                gpuScene->cullGroupCounts[call] += (run.count + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE;
        }

        // Culling counts its visible draws on the GPU; an unculled call draws all of them.
        DrawTotals &totals = gpuScene->drawTotals[frame];
        totals.draws += drawCount;
        totals.triangles += drawList.triangleCount;
        if (!culled)
        {
            totals.unculledDraws += drawCount;
            totals.unculledTriangles += drawList.triangleCount;
        }

        const uint64_t sceneDataAddress = ctx.getBufferDeviceAddress(gpuScene->sceneData[frame]) + sceneDataOffset;
        auto drawRuns = [&]()
        {
            ctx.bindPipeline(commandBuffer, gpuScene->pipeline);
            ctx.bindDescriptorSet(commandBuffer, gpuScene->pipeline, bindlessDescriptorSet, 0);
            ctx.bindIndexBuffer(commandBuffer, geometryPool->indexBuffer(), 0, Format::U32_UINT);

            gfx::PushConstants pushConstants{.scene = sceneDataAddress};
            for (uint32_t i = 0; i < drawList.runs.size(); ++i)
            {
                // Culling packs a run's visible draws at the start of its range and counts them (recordCulling).
                const DrawRun &run = drawList.runs[i];
                pushConstants.firstDraw = run.first;
                ctx.setCullMode(commandBuffer, run.cullMode);
                ctx.pushConstants(commandBuffer, gpuScene->pipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
                if (culled)
                    ctx.drawIndexedIndirectCount(commandBuffer, visibleDraws, run.first * sizeof(gfx::DrawInfo), gpuScene->drawCounts[frame],
                                                 drawCountsOffset + i * sizeof(uint32_t), run.count, sizeof(gfx::DrawInfo));
                else
                    ctx.drawIndexedIndirect(commandBuffer, drawList.buffers[frame], run.first * sizeof(gfx::DrawInfo), run.count, sizeof(gfx::DrawInfo));
            }
            ctx.setCullMode(commandBuffer, CullMode::NONE);
        };

        // The early draws, then the late ones into the same lists, which the late culling rewrote.
        drawRuns();
        if (hizLevelCount > 0)
        {
            recordOcclusionCulling(sceneDataAddress, call, colorTexture, depthTexture, width, height, hizLevelCount);
            drawRuns();
        }
    }

    // The second half of occlusion culling, in the middle of the pass: the early draws left their depth, so the
    // pass is paused for the Hi-Z build and the late culling, then resumed with its attachments loaded.
    void Engine::recordOcclusionCulling(uint64_t sceneDataAddress, uint32_t call, TextureHandle colorTexture, TextureHandle depthTexture, uint32_t width, uint32_t height, uint32_t levelCount)
    {
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const uint32_t timer = beginGpuTimer(commandBuffer, "Hi-Z");
        ctx.endRendering(commandBuffer);
        ctx.transitionImageLayout(commandBuffer, depthTexture, TextureUsage::SAMPLED);

        // The early draws are done with the lists the late culling rewrites, and the early culling's slots are visible.
        ctx.memoryBarrier(commandBuffer, PipelineStage::GRAPHICS | PipelineStage::COMPUTE, PipelineStage::COMPUTE);

        gfx::PushConstants pushConstants{.scene = sceneDataAddress};
        ctx.bindPipeline(commandBuffer, gpuScene->hizPipeline);
        ctx.bindDescriptorSet(commandBuffer, gpuScene->hizPipeline, bindlessDescriptorSet, 0);
        uint32_t levelWidth = width;
        uint32_t levelHeight = height;
        for (uint32_t level = 0; level < levelCount; ++level)
        {
            levelWidth = (levelWidth + 1) / 2;
            levelHeight = (levelHeight + 1) / 2;
            pushConstants.argument = level;
            ctx.pushConstants(commandBuffer, gpuScene->hizPipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
            ctx.dispatch(commandBuffer, (levelWidth + HIZ_GROUP_SIZE - 1) / HIZ_GROUP_SIZE, (levelHeight + HIZ_GROUP_SIZE - 1) / HIZ_GROUP_SIZE);
            ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        }

        pushConstants.argument = CULL_LATE;
        ctx.bindPipeline(commandBuffer, gpuScene->cullCountPipeline);
        ctx.pushConstants(commandBuffer, gpuScene->cullCountPipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, gpuScene->cullGroupCounts[call]);
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        ctx.bindPipeline(commandBuffer, gpuScene->cullWritePipeline);
        ctx.pushConstants(commandBuffer, gpuScene->cullWritePipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, gpuScene->cullGroupCounts[call]);

        // The host side is for the cull stats, as in recordCulling.
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::GRAPHICS | PipelineStage::HOST);
        ctx.transitionImageLayout(commandBuffer, depthTexture, TextureUsage::DEPTH_STENCIL_ATTACHMENT);
        // The same layout: only orders the early draws' color writes before the load.
        ctx.transitionImageLayout(commandBuffer, colorTexture, TextureUsage::COLOR_ATTACHMENT);
        // Depth is stored again, for a later drawScene call in the same pass.
        ctx.beginRendering(commandBuffer, colorTexture, depthTexture, glm::vec4(0.0f), {.loadColor = true, .loadDepth = true, .storeDepth = true});
        endGpuTimer(commandBuffer, timer);
    }

    // drawScene records inside a render pass, where dispatches are not allowed, so the frame's culling is recorded
    // here, after the frame's command buffer, and endFrame submits it first. Each call gets countMain, a barrier,
    // then writeMain; the last barrier makes the visible lists and counts visible to the draws. recorded tells
    // whether there was culling to submit. Returns false when culling was needed but could not be recorded: the
    // frame's draws would then read stale counts.
    bool Engine::recordCulling(bool &recorded)
    {
        ZoneScoped;

        recorded = false;
        uint32_t cullTimer = UINT32_MAX;
        const auto frame = static_cast<uint32_t>(frameCount);
        const CommandBufferHandle commandBuffer = gpuScene->cullCommandBuffers[frame];
        const uint64_t sceneDataAddress = ctx.getBufferDeviceAddress(gpuScene->sceneData[frame]);
        for (uint32_t i = 0; i < sceneDrawCount; ++i)
        {
            const uint32_t groupCount = gpuScene->cullGroupCounts[i];
            if (groupCount == 0)
                continue;
            if (!recorded)
            {
                if (!ctx.beginCommandBuffer(commandBuffer))
                    return false;
                recorded = true;
                cullTimer = beginCullingTimer(commandBuffer);
                // The previous frame's late culling wrote the visibility the early culling reads.
                ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
            }

            const gfx::PushConstants pushConstants{.scene = sceneDataAddress + i * sizeof(gfx::SceneData), .argument = CULL_EARLY};
            ctx.bindPipeline(commandBuffer, gpuScene->cullCountPipeline);
            ctx.pushConstants(commandBuffer, gpuScene->cullCountPipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
            ctx.dispatch(commandBuffer, groupCount);
            ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
            ctx.bindPipeline(commandBuffer, gpuScene->cullWritePipeline);
            ctx.pushConstants(commandBuffer, gpuScene->cullWritePipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
            ctx.dispatch(commandBuffer, groupCount);
        }
        if (!recorded)
            return true;

        // The host side is for updateFrameStats, which reads the cull stats after the frame fence.
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::GRAPHICS | PipelineStage::HOST);
        endGpuTimer(commandBuffer, cullTimer);
        return ctx.endCommandBuffer(commandBuffer);
    }
}
