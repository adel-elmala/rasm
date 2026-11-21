#include "renderer/gpuScene.h"
#include "rasm/scene.h"
#include "rasm/camera.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/blendSort.h"
#include "renderer/environmentBake.h"
#include "renderer/geometryPool.h"
#include "renderer/localShadows.h"
#include "renderer/shadowCascades.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cmath>
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

        // The blend sort's keys (a uint2 each), then the draws it copies aside (shaders/passes/sort.slang).
        uint64_t getBlendSortBufferSize(uint32_t capacity)
        {
            return static_cast<uint64_t>(capacity) * (2 * sizeof(uint32_t) + sizeof(gfx::DrawInfo));
        }

        bool hasRun(const SceneDrawList &drawList, AlphaMode alphaMode)
        {
            for (const DrawRun &run : drawList.runs)
            {
                if (run.alphaMode == alphaMode)
                    return true;
            }
            return false;
        }

        void destroyBuffer(RenderContext &ctx, BufferHandle &buffer)
        {
            if (buffer.isValid())
                ctx.destroyBuffer(buffer);
            buffer = {};
        }

        // An orthographic camera can see from a near plane at or behind itself.
        float getNearZ(const Camera &camera)
        {
            return camera.projection.type == CameraProjection::Type::PERSPECTIVE ? camera.projection.perspective.nearZ : camera.projection.orthographic.nearZ;
        }

        // The view and projection of each of the shadow's maps this frame. The view that reads them and the maps' own
        // views both call it, so the maps are drawn and read with the same matrices. A directional light's cascades are
        // fitted to the camera's view, and also give their splits; a spot light's map follows its cone; a point
        // light's cube has one projection for its faces, and the view of face. False, and no shadow, when the camera's near
        // plane has moved to a directional shadow's distance or beyond since addShadowPasses.
        bool getShadowMaps(const Light &light, const Camera &camera, const glm::mat4 &projection, const glm::mat4 &view, const SceneShadow &shadow, uint32_t mapSize, uint32_t face, ShadowCascade *maps, float *splits)
        {
            if (light.type == LightType::SPOT)
            {
                maps[0] = {.view = getSpotShadowView(light.position, light.direction), .projection = getSpotShadowProjection(light.outerConeAngle, shadow.distance)};
                return true;
            }
            if (light.type == LightType::POINT)
            {
                maps[0] = {.view = getCubeFaceView(light.position, face), .projection = getCubeFaceProjection(shadow.distance)};
                return true;
            }
            const float nearZ = getNearZ(camera);
            if (!(shadow.distance > nearZ))
                return false;
            getShadowSplits(nearZ, shadow.distance, shadow.splitLambda, shadow.mapCount, splits);
            const glm::mat4 world = glm::inverse(view);
            for (uint32_t i = 0; i < shadow.mapCount; ++i)
                maps[i] = fitShadowCascade(projection, world, light.direction, i == 0 ? nearZ : splits[i - 1], splits[i], mapSize, shadow.distance);
            return true;
        }

        bool holdsShadow(const SceneView &view, const SceneShadow &shadow)
        {
            return std::find(view.shadows, view.shadows + view.shadowCount, shadow) != view.shadows + view.shadowCount;
        }

        // The view shades with the cone's falloff of a spot light, and nothing of it for another light.
        void setSpotCone(const Light &light, gfx::Light &gpuLight)
        {
            gpuLight.spotScale = 0.0f;
            gpuLight.spotOffset = 1.0f;
            if (light.type != LightType::SPOT)
                return;
            const float cosOuter = std::cos(light.outerConeAngle);
            gpuLight.spotScale = 1.0f / std::max(std::cos(light.innerConeAngle) - cosOuter, 1e-3f);
            gpuLight.spotOffset = -cosOuter * gpuLight.spotScale;
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
        // det(T * R * S * M) = sx * sy * sz * det(M). The blend run is one run, so that it stays in one sorted order:
        // fragBlend drops the back faces of a single-sided material instead.
        CullMode getCullMode(const Material &material, const Transform &transform, const glm::mat4 &meshTransform)
        {
            if (material.doubleSided || material.alphaMode == AlphaMode::BLEND)
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

        bool isIdentity(const TextureTransform &transform)
        {
            return transform.offset == glm::vec2(0.0f) && transform.rotation == 0.0f && transform.scale == glm::vec2(1.0f);
        }

        // KHR_texture_transform's translation * rotation * scale, as the rows of a 2x2 and an offset.
        gfx::TextureTransform toGfxTextureTransform(const TextureTransform &transform)
        {
            const float c = std::cos(transform.rotation);
            const float s = std::sin(transform.rotation);
            return {.matrix = glm::vec4(c * transform.scale.x, s * transform.scale.y, -s * transform.scale.x, c * transform.scale.y), .offset = transform.offset};
        }

        // The draw runs come in this order: OPAQUE then MASK, each by cull mode, then the one BLEND run.
        uint32_t getRunKey(AlphaMode alphaMode, CullMode cullMode)
        {
            return static_cast<uint32_t>(alphaMode) * gfx::MAX_DRAW_RUNS + static_cast<uint32_t>(cullMode);
        }

        gfx::Material convertToGfxMaterial(Engine &engine, const Material &mat)
        {
            gfx::Material gmat{};

            // base color (albedo) and its alpha
            for (uint32_t i = 0; i < 4; ++i)
                gmat.pbr.baseColorFactor[i] = mat.params[static_cast<uint32_t>(PbrParam::ALBEDO) + i];

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
            // KHR_materials_emissive_strength scales the factor, which may then pass 1.
            const float emissiveStrength = mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_STRENGTH)];
            for (uint32_t i = 0; i < 3; ++i)
                gmat.emissive.emissiveFactor[i] = mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR) + i] * emissiveStrength;

            return gmat;
        }

        // Most materials have no texture transform, so their shading skips the transforms and does not load them.
        gfx::MaterialData toMaterialData(Engine &engine, const Material &mat)
        {
            gfx::MaterialData row{.material = convertToGfxMaterial(engine, mat),
                                  .materialId = getGfxMaterialId(mat.type),
                                  .alphaMode = mat.alphaMode,
                                  .alphaCutoff = mat.alphaCutoff,
                                  .flags = mat.doubleSided ? gfx::MATERIAL_DOUBLE_SIDED : 0u};
            const PbrSlot transformSlots[gfx::TEXTURE_TRANSFORMS] = {PbrSlot::ALBEDO, PbrSlot::METALLIC, PbrSlot::NORMAL, PbrSlot::OCCLUSION, PbrSlot::EMISSIVE};
            for (uint32_t i = 0; i < gfx::TEXTURE_TRANSFORMS; ++i)
            {
                const TextureTransform &transform = mat.textureTransforms[static_cast<uint32_t>(transformSlots[i])];
                row.textureTransforms[i] = toGfxTextureTransform(transform);
                if (!isIdentity(transform))
                    row.flags |= gfx::MATERIAL_TEXTURE_TRANSFORM;
            }
            return row;
        }
    }

    static_assert(MAX_SCENES <= 16, "EntityDrawState::scenes has one bit per scene");
    static_assert(2 * (static_cast<uint32_t>(CullMode::BACK) + 1) + 1 == gfx::MAX_DRAW_RUNS, "a draw list has a run per cull mode for OPAQUE and MASK, and one for BLEND");
    static_assert(static_cast<uint32_t>(gfx::TextureTransformSlot::EMISSIVE) + 1 == gfx::TEXTURE_TRANSFORMS, "every texture transform slot has a row");
    static_assert(static_cast<uint32_t>(BlendSortStep::WRITE) + 1 == BLEND_SORT_STEPS, "every sort step has a pipeline");

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
                    gpuScene->instanceAlphaModes.resize(gpuScene->instanceCount, AlphaMode::NONE);
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
        updateDrawModes(entity);
    }

    // A changed cull mode or alpha mode moves the entity's draws to another run, so its scenes rebuild their draw lists.
    void Engine::updateDrawModes(EntityHandle entity)
    {
        const EntityDrawState &state = gpuScene->entities[entity.index];
        const Entity &entityRef = registry.entities[entity.index];
        const Mesh &mesh = registry.meshes[entityRef.mesh.index];

        bool changed = false;
        for (uint32_t i = 0; i < state.instanceCount; ++i)
        {
            const SubMesh &subMesh = mesh.subMeshes[i];
            const Material &material = getSubMeshMaterial(*this, entityRef, subMesh);
            const CullMode cullMode = getCullMode(material, entityRef.transform, subMesh.transform);
            CullMode &storedCullMode = gpuScene->instanceCullModes[state.firstInstance + i];
            AlphaMode &storedAlphaMode = gpuScene->instanceAlphaModes[state.firstInstance + i];
            changed = changed || storedCullMode != cullMode || storedAlphaMode != material.alphaMode;
            storedCullMode = cullMode;
            storedAlphaMode = material.alphaMode;
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
            updateDrawModes(entityRef.handle);
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
        const gfx::MaterialData row = toMaterialData(*this, materialRef);
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

        // drawSceneView issues one indirect draw per run of draws with the same alpha mode and cull mode, and
        // drawTransparent one for the blend run, which comes last.
        const auto &cullModes = gpuScene->instanceCullModes;
        const auto &alphaModes = gpuScene->instanceAlphaModes;
        std::stable_sort(drawList.draws.begin(), drawList.draws.end(), [&](const gfx::DrawInfo &a, const gfx::DrawInfo &b)
                         { return getRunKey(alphaModes[a.firstInstance], cullModes[a.firstInstance]) < getRunKey(alphaModes[b.firstInstance], cullModes[b.firstInstance]); });
        for (uint32_t i = 0; i < drawList.draws.size(); ++i)
        {
            const AlphaMode alphaMode = alphaModes[drawList.draws[i].firstInstance];
            const CullMode cullMode = cullModes[drawList.draws[i].firstInstance];
            if (drawList.runs.empty() || drawList.runs.back().alphaMode != alphaMode || drawList.runs.back().cullMode != cullMode)
                drawList.runs.push_back({alphaMode, cullMode, i, 0});
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
    // and the late draws. They draw into renderTarget, so later passes on that target load what they drew. A
    // depth-only target gets only depth (passes named "Shadow ...", or "Cascade i ...", "Spot ..." and "Face i ..." for
    // a shadow's maps). Each view (scene, camera, target, face, shadows) culls into buffers of its own, kept from frame
    // to frame while a graph holds the view, which its passes declare: per-slot buffers by both slots, so their handles
    // hold for every frame. The cull stats are not declared: the views only add to them atomically, and only the CPU
    // reads them, after the slot's fence, so views need no order between them. The draws read the shadows' maps, so
    // they run after the passes that drew them.
    bool Engine::addScenePasses(RenderGraph &graph, SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget, const SceneShadow *shadows, uint32_t shadowCount)
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
        auto isDepthOnly = [this](const RenderTargetHandle &target)
        {
            const auto &targetData = ctx.getRenderTarget(target);
            return targetData.colorCount == 0 && targetData.depthFormat == DEPTH_FORMAT;
        };
        // The scene's radiance is unbounded, so a color target is HDR, and a tone-map pass reads it.
        if (!renderTarget.isValid())
        {
            spdlog::error("addScenePasses needs a valid render target.");
            return false;
        }
        const auto &renderTargetData = ctx.getRenderTarget(renderTarget);
        const bool depthOnly = isDepthOnly(renderTarget);
        const bool color = renderTargetData.colorCount == 1 && renderTargetData.colorFormats[0] == SCENE_COLOR_FORMAT && renderTargetData.depthFormat == DEPTH_FORMAT;
        if ((!depthOnly && !color) || ctx.getRenderTargetDesc(renderTarget).cube)
        {
            spdlog::error("addScenePasses needs a render target with one SCENE_COLOR_FORMAT color attachment and D32_SFLOAT depth, or D32_SFLOAT depth only, not a cube.");
            return false;
        }
        if (shadowCount > 0 && depthOnly)
        {
            spdlog::error("addScenePasses draws a depth-only target without shading, so it takes no shadow.");
            return false;
        }
        if (shadowCount > MAX_VIEW_SHADOWS || (shadowCount > 0 && shadows == nullptr))
        {
            spdlog::error("addScenePasses takes at most {} shadows.", MAX_VIEW_SHADOWS);
            return false;
        }
        // The view reads the maps as fitted to its camera, so they must be drawn from the same camera.
        for (uint32_t s = 0; s < shadowCount; ++s)
        {
            const SceneShadow &shadow = shadows[s];
            bool drawn = shadow.mapCount > 0 && shadow.mapCount <= MAX_SHADOW_CASCADES;
            for (uint32_t i = 0; drawn && i < shadow.mapCount; ++i)
            {
                drawn = false;
                for (uint32_t heldView : graph.sceneViews)
                {
                    const SceneView &held = gpuScene->views[heldView];
                    drawn = drawn || (held.scene == scene && held.camera == camera && held.renderTarget == shadow.maps[i] && held.depthOnly && held.shadows[0] == shadow);
                }
            }
            if (!drawn)
            {
                spdlog::error("addScenePasses needs shadows that addShadowPasses made for the same scene and camera in this graph.");
                return false;
            }
            for (uint32_t other = 0; other < s; ++other)
            {
                if (shadows[other].light == shadow.light)
                {
                    spdlog::error("addScenePasses: two shadows of one light.");
                    return false;
                }
            }
            // A directional light's cascades are fitted to the aspect ratio of the one view that reads them.
            for (uint32_t heldView : graph.sceneViews)
            {
                const SceneView &held = gpuScene->views[heldView];
                if (!held.depthOnly && holdsShadow(held, shadow))
                {
                    spdlog::error("addScenePasses: a shadow from addShadowPasses is read by one view only.");
                    return false;
                }
            }
        }

        return addSceneViewPasses(graph, scene, camera, renderTarget, 0, shadows, shadowCount, depthOnly);
    }

    // A light's shadow: depth-only maps, each drawn by its own scene view, with LODs from the camera. A directional
    // light's cascades cover slices of the camera's view; a spot light's map looks along its cone; a point light's
    // cube is drawn by a view per face. The view that reads the shadow gets it from addScenePasses.
    SceneShadow Engine::addShadowPasses(RenderGraph &graph, SceneHandle scene, CameraHandle camera, const ShadowDesc &desc)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("addShadowPasses needs a valid camera handle.");
            return {};
        }
        if (!scene.isValid() || scene.index >= MAX_SCENES || scene.index >= registry.scenes.size())
        {
            spdlog::error("addShadowPasses needs a valid scene handle.");
            return {};
        }
        if (!desc.light.isValid() || desc.light.index >= registry.lights.size())
        {
            spdlog::error("addShadowPasses needs a valid light.");
            return {};
        }
        const LightType type = registry.lights[desc.light.index].type;
        const bool directional = type == LightType::DIRECTIONAL;
        // The checks are written so that NaN fails them.
        if (directional && (desc.cascadeCount == 0 || desc.cascadeCount > MAX_SHADOW_CASCADES || desc.size == 0 || !(desc.distance > getNearZ(registry.cameras[camera.index])) ||
                            !(desc.splitLambda >= 0.0f && desc.splitLambda <= 1.0f)))
        {
            spdlog::error("addShadowPasses needs 1 to {} cascades, a size above 0, a distance beyond the camera's near plane and a splitLambda from 0 to 1.", MAX_SHADOW_CASCADES);
            return {};
        }
        if (!directional && (desc.size == 0 || !(desc.distance > 0.0f) || std::isinf(desc.distance)))
        {
            spdlog::error("addShadowPasses needs a size above 0 and a finite distance above 0.");
            return {};
        }

        // Each map, and each face of a cube, is a new view, so check for room before adding any, and leave no map
        // without a reader.
        const uint32_t viewCount = directional ? desc.cascadeCount : (type == LightType::POINT ? CUBE_FACES : 1);
        uint32_t freeViews = 0;
        for (const SceneView &view : gpuScene->views)
            freeViews += view.graphs == 0 ? 1 : 0;
        if (freeViews < viewCount)
        {
            spdlog::error("addShadowPasses needs {} free scene views, and {} of {} are free.", viewCount, freeViews, MAX_SCENE_VIEWS);
            return {};
        }

        SceneShadow shadow = {.light = desc.light, .mapCount = directional ? desc.cascadeCount : 1, .distance = desc.distance, .splitLambda = directional ? desc.splitLambda : 0.0f};
        for (uint32_t i = 0; i < shadow.mapCount; ++i)
        {
            std::string name = "shadow_cascade_" + std::to_string(i);
            if (!directional)
                name = (type == LightType::POINT ? "shadow_cube_" : "shadow_spot_") + std::to_string(desc.light.index);
            shadow.maps[i] = graph.createRenderTarget({.name = name, .width = desc.size, .height = desc.size, .depthFormat = DEPTH_FORMAT, .cube = type == LightType::POINT});
            if (!shadow.maps[i].isValid())
            {
                spdlog::error("addShadowPasses could not create the shadow's render targets.");
                return {};
            }
        }
        for (uint32_t i = 0; i < viewCount; ++i)
        {
            const bool cube = type == LightType::POINT;
            if (!addSceneViewPasses(graph, scene, camera, shadow.maps[cube ? 0 : i], cube ? i : 0, &shadow, 1, true))
                return {};
        }
        return shadow;
    }

    bool Engine::addSceneViewPasses(RenderGraph &graph, SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget, uint32_t face, const SceneShadow *shadows, uint32_t shadowCount, bool depthOnly)
    {
        // A view's passes run once per frame: a second set would read its own visibility as last frame's.
        for (uint32_t heldView : graph.sceneViews)
        {
            const SceneView &held = gpuScene->views[heldView];
            if (held.scene == scene && held.camera == camera && held.renderTarget == renderTarget && held.face == face)
            {
                spdlog::error("addScenePasses was called twice for the same scene, camera and target in one graph.");
                return false;
            }
        }

        const uint32_t view = createSceneView(scene, camera, renderTarget, face, shadows, shadowCount, depthOnly);
        if (view == UINT32_MAX)
            return false;
        graph.sceneViews.push_back(view);
        const SceneView &sceneView = gpuScene->views[view];
        const bool occlusion = sceneView.visibility.isValid();
        std::string drawName = depthOnly ? "Shadow" : "Scene";
        if (depthOnly && shadowCount > 0)
        {
            // Local lights' passes name the light, like their targets, so two of them can be told apart.
            const LightType type = registry.lights[shadows[0].light.index].type;
            const std::string light = std::to_string(shadows[0].light.index);
            if (type == LightType::SPOT)
                drawName = "Spot " + light;
            if (type == LightType::POINT)
                drawName = "Cube " + light + " Face " + std::to_string(face);
            for (uint32_t i = 0; type == LightType::DIRECTIONAL && i < shadows[0].mapCount; ++i)
            {
                if (shadows[0].maps[i] == renderTarget)
                    drawName = "Cascade " + std::to_string(i);
            }
        }
        const std::string prefix = depthOnly ? drawName + " " : "";

        // Each frame uses only its slot's draw list and Hi-Z, so culling the next frame does not wait for
        // this frame's draws on the other queue. A shaded view's culling also sorts the blend run, in the late pass,
        // or in the early one in a frame without a Hi-Z.
        auto writeCulling = [&sceneView](PassBuilder &builder)
        {
            builder.write(sceneView.visibleDraws);
            if (!sceneView.depthOnly)
                builder.write(sceneView.sortBuffers);
        };
        auto drawScenePass = [&sceneView, renderTarget, face](PassBuilder &builder)
        {
            builder.read(sceneView.visibleDraws);
            for (uint32_t s = 0; !sceneView.depthOnly && s < sceneView.shadowCount; ++s)
            {
                for (uint32_t i = 0; i < sceneView.shadows[s].mapCount; ++i)
                    builder.readDepth(sceneView.shadows[s].maps[i]);
            }
            builder.write(renderTarget, face);
        };

        graph.addAsyncComputePass(
            prefix + (occlusion ? "Cull 1" : "Cull"),
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
        // The sky goes behind what the view's last draw pass of the frame drew: the late pass, unless this frame has
        // none (no Hi-Z).
        graph.addPass(
            occlusion ? drawName + " 1" : drawName,
            drawScenePass,
            [this, view, occlusion](RenderContext &)
            {
                drawSceneView(view);
                if (!occlusion || gpuScene->views[view].hizLevelCount == 0)
                    drawSky(view);
            });
        // See-through draws go over everything the draw passes and the sky left, so a shaded view ends with them. The
        // pass is there even while the scene has no blend run, since the graph is fixed and the scene is not.
        auto addTransparentPass = [&]()
        {
            if (!depthOnly)
                graph.addPass("Transparent", drawScenePass, [this, view](RenderContext &)
                              { drawTransparent(view); });
        };
        if (!occlusion)
        {
            addTransparentPass();
            return true;
        }

        graph.addAsyncComputePass(
            prefix + "Hi-Z",
            [&](PassBuilder &builder)
            {
                builder.readDepth(renderTarget);
                builder.write(sceneView.hiz);
            },
            [this, view](RenderContext &)
            { recordHiz(view); });
        graph.addAsyncComputePass(
            prefix + "Cull 2",
            [&](PassBuilder &builder)
            {
                builder.read(sceneView.hiz);
                writeCulling(builder);
                builder.write(sceneView.visibility);
            },
            [this, view](RenderContext &)
            {
                if (gpuScene->views[view].hizLevelCount > 0)
                    recordSceneCulling(view, CULL_LATE);
            });
        graph.addPass(
            drawName + " 2",
            drawScenePass,
            [this, view](RenderContext &)
            {
                if (gpuScene->views[view].hizLevelCount == 0)
                    return;
                drawSceneView(view);
                drawSky(view);
            });
        addTransparentPass();
        return true;
    }

    // A view lives while a graph holds it, so graphs built for the same scene, camera, target, face and shadows share
    // it. Its buffers start small and grow in prepareSceneView under the same handles.
    uint32_t Engine::createSceneView(SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget, uint32_t face, const SceneShadow *shadows, uint32_t shadowCount, bool depthOnly)
    {
        SceneShadow viewShadows[MAX_VIEW_SHADOWS] = {};
        std::copy(shadows, shadows + shadowCount, viewShadows);
        uint32_t freeSlot = UINT32_MAX;
        for (uint32_t i = 0; i < MAX_SCENE_VIEWS; ++i)
        {
            SceneView &view = gpuScene->views[i];
            if (view.graphs == 0)
            {
                freeSlot = std::min(freeSlot, i);
                continue;
            }
            if (view.scene == scene && view.camera == camera && view.renderTarget == renderTarget && view.face == face && view.shadowCount == shadowCount &&
                std::equal(view.shadows, view.shadows + MAX_VIEW_SHADOWS, viewShadows))
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

        SceneView view = {.scene = scene, .camera = camera, .renderTarget = renderTarget, .face = face, .shadowCount = shadowCount, .depthOnly = depthOnly, .graphs = 1};
        std::copy(viewShadows, viewShadows + MAX_VIEW_SHADOWS, view.shadows);
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

        // A shaded view sorts its blend run; the buffers start at one block and grow under the same handles.
        for (uint32_t i = 0; !depthOnly && i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            view.sortCapacity[i] = BLEND_SORT_BLOCK;
            view.sortBuffers[i] = ctx.createBuffer({.name = "Blend Sort Buffer", .usage = BufferUsage::STORAGE, .size = getBlendSortBufferSize(BLEND_SORT_BLOCK)});
            if (!view.sortBuffers[i].isValid())
            {
                spdlog::error("Failed to create the blend sort buffers of a scene view.");
                for (uint32_t j = 0; j < MAX_FRAMES_IN_FLIGHT; ++j)
                {
                    destroyBuffer(ctx, view.visibleDraws[j]);
                    destroyBuffer(ctx, view.sortBuffers[j]);
                }
                return UINT32_MAX;
            }
        }

        // The view's visibility, one flag per instance row, is kept across frames and shared by both frame slots: the
        // GPU runs the frames in order. Rows start unseen, so a new instance is drawn late first. Without these
        // buffers the view is only frustum culled, like a cube's faces: the Hi-Z build reads a 2D depth.
        if (config.occlusionCulling && !ctx.getRenderTargetDesc(renderTarget).cube)
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
        if (view.graphs == 0)
            return;
        --view.graphs;
        if (view.graphs > 0)
            return;
        ctx.waitForFence(frameFence, frameFenceValue);
        if (asyncCompute)
            ctx.waitForFence(computeFence, computeFenceValue);
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            destroyBuffer(ctx, view.visibleDraws[i]);
            destroyBuffer(ctx, view.hiz[i]);
            destroyBuffer(ctx, view.sortBuffers[i]);
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
        view.sky = false;
        view.drawMask = false;
        view.drawBlend = false;
        view.sortBlend = false;
        view.blendRun = gfx::MAX_DRAW_RUNS;
        view.groupCount = 0;
        view.hizLevelCount = 0;

        const auto &drawList = gpuScene->drawLists[view.scene.index];
        const auto frame = static_cast<uint32_t>(frameCount);
        if (drawList.draws.empty() || (drawList.staleSlots & (1u << frame)) != 0)
        {
            // Nothing to draw, but the sky still shows, so a scene whose meshes still load is not left black. The sky
            // reads only the camera and the environment.
            if (view.depthOnly)
                return;
            const auto &target = ctx.getRenderTarget(view.renderTarget);
            gfx::SceneData sceneData{};
            sceneData.projection = getProjectionMatrix(view.camera, static_cast<float>(target.width) / static_cast<float>(target.height));
            sceneData.view = getViewMatrix(view.camera);
            sceneData.cameraWorldPosition = registry.cameras[view.camera.index].transform.position;
            view.sky = writeSceneEnvironment(*environments, registry.scenes[view.scene.index].environment, sceneData) &&
                       ctx.fillBuffer(gpuScene->sceneData[frame], &sceneData, sizeof(sceneData), viewIndex * sizeof(gfx::SceneData));
            return;
        }

        // A shader error retries every frame, so a shader fixed on disk is picked up while the application runs.
        if (!view.depthOnly && !gpuScene->pipeline.isValid())
        {
            gpuScene->pipeline = createScenePipeline("rasm", "passes/rasm.slang", "", view.renderTarget, "vertMain", "fragMain", BlendMode::NONE, true);
            if (!gpuScene->pipeline.isValid())
            {
                spdlog::error("Failed to create the uber material pipeline for the scene.");
                return;
            }
        }
        if (view.depthOnly && !gpuScene->depthPipeline.isValid())
        {
            gpuScene->depthPipeline = createScenePipeline("shadow", "passes/shadow.slang", "", view.renderTarget, "vertMain", "fragMain", BlendMode::NONE, true);
            if (!gpuScene->depthPipeline.isValid())
            {
                spdlog::error("Failed to create the depth-only pipeline for the scene.");
                return;
            }
        }
        // The pipelines of the other alpha modes are made once a scene has their draws, so a scene without them pays
        // for no compile. A failed one leaves out only its runs. MASK and BLEND are rasm.slang's fragMain compiled with
        // a define: an entry point that called the opaque one would be a function call that reads every varying up
        // front, which costs the opaque draws, and Slang's API does not compile an entry point that calls another.
        const bool hasMask = hasRun(drawList, AlphaMode::MASK);
        const bool hasBlend = hasRun(drawList, AlphaMode::BLEND);
        if (!view.depthOnly && hasMask && !gpuScene->maskPipeline.isValid())
        {
            gpuScene->maskPipeline = createScenePipeline("rasmMask", "passes/rasm.slang", "#define RASM_ALPHA_MASK 1\n", view.renderTarget, "vertMain", "fragMain", BlendMode::NONE, true);
            if (!gpuScene->maskPipeline.isValid())
                spdlog::error("Failed to create the MASK pipeline; the scene is drawn without its cut-out materials.");
        }
        if (!view.depthOnly && hasBlend && !gpuScene->blendPipeline.isValid())
        {
            gpuScene->blendPipeline = createScenePipeline("rasmBlend", "passes/rasm.slang", "#define RASM_ALPHA_BLEND 1\n", view.renderTarget, "vertMain", "fragMain", BlendMode::PREMULTIPLIED_ALPHA, false);
            if (!gpuScene->blendPipeline.isValid())
                spdlog::error("Failed to create the BLEND pipeline; the scene is drawn without its see-through materials.");
        }
        if (view.depthOnly && (hasMask || hasBlend) && !gpuScene->depthMaskPipeline.isValid())
        {
            gpuScene->depthMaskPipeline = createScenePipeline("shadowMask", "passes/shadow.slang", "", view.renderTarget, "vertMask", "fragMask", BlendMode::NONE, true);
            if (!gpuScene->depthMaskPipeline.isValid())
                spdlog::error("Failed to create the depth-only MASK pipeline; cut-out and see-through materials cast no shadow.");
        }
        view.drawMask = view.depthOnly ? gpuScene->depthMaskPipeline.isValid() : gpuScene->maskPipeline.isValid();
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

        // A shaded view draws its blend run last, back to front: the run is sorted on the GPU once culling has packed it.
        for (uint32_t i = 0; !view.depthOnly && i < drawList.runs.size(); ++i)
        {
            if (drawList.runs[i].alphaMode == AlphaMode::BLEND)
                view.blendRun = i;
        }
        view.drawBlend = view.blendRun < gfx::MAX_DRAW_RUNS && gpuScene->blendPipeline.isValid();
        bool sortPipelines = true;
        for (const PipelineHandle &pipeline : gpuScene->sortPipelines)
            sortPipelines = sortPipelines && pipeline.isValid();
        if (view.drawBlend && culled && !sortPipelines)
        {
            const std::string sortSource = readFile(pathFromUtf8(config.shaderDir) / "passes/sort.slang");
            const char *entryPoints[BLEND_SORT_STEPS] = {"sortKeysMain", "sortMergeMain", "sortLocalMergeMain", "sortWriteMain"};
            sortPipelines = true;
            for (uint32_t i = 0; i < BLEND_SORT_STEPS; ++i)
            {
                if (!gpuScene->sortPipelines[i].isValid())
                    gpuScene->sortPipelines[i] = createComputePipeline("blendSort", sortSource, entryPoints[i]);
                sortPipelines = sortPipelines && gpuScene->sortPipelines[i].isValid();
            }
            if (!sortPipelines)
                spdlog::error("Failed to create the blend sort pipelines; see-through materials are drawn unsorted.");
        }
        if (view.drawBlend && culled && sortPipelines)
        {
            const uint32_t capacity = getBlendSortCapacity(drawList.runs[view.blendRun].count);
            uint32_t &sortCapacity = view.sortCapacity[frame];
            if (sortCapacity < capacity)
            {
                if (ctx.resizeBuffer(view.sortBuffers[frame], getBlendSortBufferSize(capacity)))
                    sortCapacity = capacity;
                else
                    spdlog::error("Failed to grow a blend sort buffer to {} draws; see-through materials are drawn unsorted.", capacity);
            }
            view.sortBlend = sortCapacity >= capacity;
        }

        // Without the Hi-Z pipeline it is still frustum culled.
        if (culled && config.occlusionCulling && !gpuScene->hizPipeline.isValid())
        {
            gpuScene->hizPipeline = createComputePipeline("hiz", readFile(pathFromUtf8(config.shaderDir) / "passes/hiz.slang"), "hizMain");
            if (!gpuScene->hizPipeline.isValid())
                spdlog::error("Failed to create the Hi-Z pipeline; the scene is drawn without occlusion culling.");
        }

        const auto &renderTargetData = ctx.getRenderTarget(view.renderTarget);
        const uint32_t width = renderTargetData.width;
        const uint32_t height = renderTargetData.height;
        const TextureHandle depthTexture = renderTargetData.attachments[frame].depth;

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
        // (RenderContext::createAttachments). Its Hi-Z covers the whole attachment.
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

        // A shadow's map is fitted to the view that reads it, and takes its LODs from that view's camera and size, so a
        // surface is shadowed by the same triangles it is drawn with. Any other view uses its own camera and size.
        const bool shadowView = view.depthOnly && view.shadowCount > 0;
        uint32_t cameraWidth = width;
        uint32_t cameraHeight = height;
        for (uint32_t i = 0; shadowView && i < MAX_SCENE_VIEWS; ++i)
        {
            const SceneView &reader = gpuScene->views[i];
            if (reader.graphs == 0 || reader.depthOnly || reader.scene != view.scene || reader.camera != view.camera || !holdsShadow(reader, view.shadows[0]))
                continue;
            cameraWidth = ctx.getRenderTarget(reader.renderTarget).width;
            cameraHeight = ctx.getRenderTarget(reader.renderTarget).height;
            break;
        }
        const glm::mat4 cameraProjection = getProjectionMatrix(view.camera, static_cast<float>(cameraWidth) / static_cast<float>(cameraHeight));
        const glm::mat4 cameraView = getViewMatrix(view.camera);

        // Each shadow's light, as an index in the scene's lights, and its maps. A light taken out of the scene casts no
        // shadow, and its maps draw nothing.
        int32_t shadowLights[MAX_VIEW_SHADOWS] = {};
        ShadowCascade shadowMaps[MAX_VIEW_SHADOWS][MAX_SHADOW_CASCADES] = {};
        float splits[MAX_VIEW_SHADOWS][MAX_SHADOW_CASCADES] = {};
        for (uint32_t s = 0; s < view.shadowCount; ++s)
        {
            const SceneShadow &shadow = view.shadows[s];
            shadowLights[s] = -1;
            for (uint32_t i = 0; i < sceneRef.lights.size(); ++i)
            {
                if (sceneRef.lights[i] == shadow.light)
                    shadowLights[s] = static_cast<int32_t>(i);
            }
            if (shadowLights[s] >= 0 && !getShadowMaps(registry.lights[shadow.light.index], cameraRef, cameraProjection, cameraView, shadow, ctx.getRenderTarget(shadow.maps[0]).width, view.face, shadowMaps[s], splits[s]))
                shadowLights[s] = -1;
        }
        if (shadowView && shadowLights[0] < 0)
            return;

        gfx::SceneData sceneData{};
        sceneData.projection = cameraProjection;
        sceneData.view = cameraView;
        for (uint32_t i = 0; shadowView && i < view.shadows[0].mapCount; ++i)
        {
            if (view.shadows[0].maps[i] != view.renderTarget)
                continue;
            sceneData.projection = shadowMaps[0][i].projection;
            sceneData.view = shadowMaps[0][i].view;
        }
        view.mirrored = glm::determinant(glm::mat3(sceneData.view)) < 0.0f;
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
            setSpotCone(sceneLight, light);
            light.shadowMap = -1;

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
        sceneData.colorsPtr = ctx.getBufferDeviceAddress(geometryPool->colorBuffer());
        if (view.sortBlend)
        {
            const uint32_t sortCapacity = getBlendSortCapacity(drawList.runs[view.blendRun].count);
            sceneData.sortKeysPtr = ctx.getBufferDeviceAddress(view.sortBuffers[frame]);
            sceneData.sortDrawsPtr = sceneData.sortKeysPtr + static_cast<uint64_t>(sortCapacity) * 2 * sizeof(uint32_t);
            sceneData.sortCapacity = sortCapacity;
        }
        sceneData.drawCount = drawCount;
        sceneData.runCount = static_cast<uint32_t>(drawList.runs.size());
        for (uint32_t i = 0; i < sceneData.runCount; ++i)
            sceneData.runFirst[i] = drawList.runs[i].first;
        // Culling keeps the blend run for the late phase only, and the Transparent pass draws it. A depth-only view
        // draws it like any other run, so it has no blend run.
        sceneData.blendRun = view.blendRun;
        sceneData.hizLevelCount = view.hizLevelCount;
        sceneData.depthTexture = depthSlot;
        sceneData.viewportWidth = width;
        sceneData.viewportHeight = height;
        sceneData.lodErrorPixels = config.lodErrorPixels;
        sceneData.forcedLod = config.forcedLod;
        // The shaded view's shadow maps, in one table: each shadowed light names its first map and their count. A
        // point light's cube has its faces' projection; the shader makes the view from the light's position.
        uint32_t mapCount = 0;
        for (uint32_t s = 0; !view.depthOnly && s < view.shadowCount; ++s)
        {
            if (shadowLights[s] < 0)
                continue;
            const SceneShadow &shadow = view.shadows[s];
            const bool cube = registry.lights[shadow.light.index].type == LightType::POINT;
            gfx::Light &light = sceneData.lights[shadowLights[s]];
            light.shadowMap = static_cast<int32_t>(mapCount);
            light.shadowMapCount = shadow.mapCount;
            for (uint32_t i = 0; i < shadow.mapCount; ++i)
            {
                gfx::ShadowMap &map = sceneData.shadowMaps[mapCount++];
                map.matrix = cube ? shadowMaps[s][i].projection : shadowMaps[s][i].projection * shadowMaps[s][i].view;
                map.texture = getBindlessTextureIndex(ctx.getRenderTarget(shadow.maps[i]).attachments[frame].depth);
                map.split = splits[s][i];
            }
        }

        // A depth-only view shades nothing, so it has no environment.
        const bool sky = writeSceneEnvironment(*environments, view.depthOnly ? SceneEnvironment{} : sceneRef.environment, sceneData);

        sceneData.lodPixelsPerUnit = std::abs(cameraProjection[1][1]) * 0.5f * static_cast<float>(cameraHeight);
        if (cameraProjection[3][3] == 0.0f)
        {
            const glm::mat4 lodViewProjection = cameraProjection * cameraView;
            sceneData.lodDepthPlane = glm::vec4(lodViewProjection[0][3], lodViewProjection[1][3], lodViewProjection[2][3], lodViewProjection[3][3]);
        }
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
        view.sky = sky;
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

        // The phase that keeps the blend run's draws: the late one, or the only one in a frame without a Hi-Z.
        if (view.sortBlend && (phase == CULL_LATE || view.hizLevelCount == 0))
        {
            ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
            recordBlendSort(viewIndex);
        }
    }

    // A bitonic sort of the blend run's visible draws, back to front, in place (shaders/passes/sort.slang). Its size
    // is the run's, not its visible count, which only the GPU knows: the draws past the count sort to the end.
    void Engine::recordBlendSort(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        const auto frame = static_cast<uint32_t>(frameCount);
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const uint32_t capacity = getBlendSortCapacity(gpuScene->drawLists[view.scene.index].runs[view.blendRun].count);
        BlendSortDispatch dispatches[MAX_BLEND_SORT_DISPATCHES];
        const uint32_t dispatchCount = std::min(getBlendSortDispatches(capacity, dispatches, MAX_BLEND_SORT_DISPATCHES), MAX_BLEND_SORT_DISPATCHES);

        const uint32_t timer = beginGpuTimer(commandBuffer, "Blend Sort");
        gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, frame, viewIndex)};
        for (uint32_t i = 0; i < dispatchCount; ++i)
        {
            if (i > 0)
                ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
            const PipelineHandle pipeline = gpuScene->sortPipelines[static_cast<uint32_t>(dispatches[i].step)];
            pushConstants.argument = dispatches[i].argument;
            ctx.bindPipeline(commandBuffer, pipeline);
            ctx.pushConstants(commandBuffer, pipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
            ctx.dispatch(commandBuffer, dispatches[i].groups);
        }
        endGpuTimer(commandBuffer, timer);
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
        ctx.bindIndexBuffer(commandBuffer, geometryPool->indexBuffer(), 0, Format::U32_UINT);

        // Opaque draws use the pipeline that never discards; a depth-only view draws MASK and BLEND with its cut-out one,
        // and a shaded view leaves BLEND to the Transparent pass.
        PipelineHandle bound = {};
        gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex)};
        for (uint32_t i = 0; i < drawList.runs.size(); ++i)
        {
            const DrawRun &run = drawList.runs[i];
            if (i == view.blendRun || (run.alphaMode != AlphaMode::NONE && !view.drawMask))
                continue;
            PipelineHandle pipeline = view.depthOnly ? gpuScene->depthPipeline : gpuScene->pipeline;
            if (run.alphaMode != AlphaMode::NONE)
                pipeline = view.depthOnly ? gpuScene->depthMaskPipeline : gpuScene->maskPipeline;
            if (pipeline != bound)
            {
                ctx.bindPipeline(commandBuffer, pipeline);
                ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
                bound = pipeline;
            }
            pushConstants.firstDraw = run.first;
            // Through a mirrored view the front faces turn clockwise, so the side to cull swaps.
            CullMode cullMode = run.cullMode;
            if (view.mirrored && cullMode != CullMode::NONE)
                cullMode = cullMode == CullMode::BACK ? CullMode::FRONT : CullMode::BACK;
            ctx.setCullMode(commandBuffer, cullMode);
            ctx.pushConstants(commandBuffer, pipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
            if (view.culled)
                ctx.drawIndexedIndirectCount(commandBuffer, view.visibleDraws[frame], run.first * sizeof(gfx::DrawInfo), view.visibleDraws[frame],
                                             drawCountsOffset + i * sizeof(uint32_t), run.count, sizeof(gfx::DrawInfo));
            else
                ctx.drawIndexedIndirect(commandBuffer, drawList.buffers[frame], run.first * sizeof(gfx::DrawInfo), run.count, sizeof(gfx::DrawInfo));
        }
        // beginPass sets NONE for every other pipeline.
        ctx.setCullMode(commandBuffer, CullMode::NONE);
    }

    // The blend run, after the view's draws and its sky: depth tested against them, but not written, so each see-through
    // surface blends over what is behind it, in the order the sort left (back to front; unsorted without culling).
    void Engine::drawTransparent(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.ready || !view.drawBlend)
            return;

        const auto frame = static_cast<uint32_t>(frameCount);
        const DrawRun &run = gpuScene->drawLists[view.scene.index].runs[view.blendRun];
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, frame, viewIndex), .firstDraw = run.first};
        ctx.bindPipeline(commandBuffer, gpuScene->blendPipeline);
        ctx.bindDescriptorSet(commandBuffer, gpuScene->blendPipeline, bindlessDescriptorSet, 0);
        ctx.bindIndexBuffer(commandBuffer, geometryPool->indexBuffer(), 0, Format::U32_UINT);
        ctx.setCullMode(commandBuffer, CullMode::NONE);
        ctx.pushConstants(commandBuffer, gpuScene->blendPipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
        if (view.culled)
            ctx.drawIndexedIndirectCount(commandBuffer, view.visibleDraws[frame], run.first * sizeof(gfx::DrawInfo), view.visibleDraws[frame],
                                         getDrawCountsOffset(view.visibleCapacity[frame]) + view.blendRun * sizeof(uint32_t), run.count, sizeof(gfx::DrawInfo));
        else
            ctx.drawIndexedIndirect(commandBuffer, gpuScene->drawLists[view.scene.index].buffers[frame], run.first * sizeof(gfx::DrawInfo), run.count, sizeof(gfx::DrawInfo));
    }

    // One triangle at the far plane, after the view's draws: the depth test passes only where they left the clear
    // depth (0, reversed-Z), so the sky costs only the pixels it shows.
    void Engine::drawSky(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.sky)
            return;
        if (!gpuScene->skyPipeline.isValid())
        {
            gpuScene->skyPipeline = createScenePipeline("sky", "passes/sky.slang", "", view.renderTarget, "vertMain", "fragMain", BlendMode::NONE, true);
            if (!gpuScene->skyPipeline.isValid())
            {
                spdlog::error("Failed to create the sky pipeline; the scene has no sky.");
                return;
            }
        }
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex)};
        ctx.bindPipeline(commandBuffer, gpuScene->skyPipeline);
        ctx.bindDescriptorSet(commandBuffer, gpuScene->skyPipeline, bindlessDescriptorSet, 0);
        ctx.pushConstants(commandBuffer, gpuScene->skyPipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
        ctx.setCullMode(commandBuffer, CullMode::NONE);
        ctx.draw(commandBuffer, 3);
    }
}
