#include "renderer/gpuScene.h"
#include "rasm/scene.h"
#include "rasm/camera.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
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
        // glTF: a transform with a negative determinant mirrors the mesh, so its front faces turn clockwise (D8).
        // det(T * R * S * M) = sx * sy * sz * det(M).
        CullMode getCullMode(bool doubleSided, const Transform &transform, const glm::mat4 &meshTransform)
        {
            if (doubleSided)
                return CullMode::NONE;
            const float determinant = transform.scale.x * transform.scale.y * transform.scale.z * glm::determinant(glm::mat3(meshTransform));
            return determinant < 0.0f ? CullMode::FRONT : CullMode::BACK;
        }

        glm::mat4 getModelMatrix(const Transform &transform)
        {
            return glm::translate(glm::mat4(1.0f), transform.position) *
                   glm::mat4_cast(transform.rotation) *
                   glm::scale(glm::mat4(1.0f), transform.scale);
        }

        bool texturesLoaded(Engine &engine, MaterialHandle material)
        {
            for (auto texture : engine.getMaterial(material).textures)
            {
                if (texture.isValid() && !engine.isLoaded(texture))
                    return false;
            }
            return true;
        }
    }

    // The mega vertex buffer is written with Vertex and read in the shader as gfx::VertexFull
    // (scalar layout); the two must not drift apart.
    static_assert(sizeof(Vertex) == sizeof(gfx::VertexFull), "Vertex must match the shader's VertexFull");
    static_assert(offsetof(Vertex, uv) == offsetof(gfx::VertexFull, uv), "Vertex must match the shader's VertexFull");
    static_assert(offsetof(Vertex, tangent) == offsetof(gfx::VertexFull, tangent), "Vertex must match the shader's VertexFull");

    bool Engine::createGpuScene()
    {
        if (config.geometryPoolSubMeshes == 0)
        {
            spdlog::error("The geometry pool needs room for at least one sub-mesh.");
            return false;
        }
        geometryTableBuffer = ctx.createBuffer({.name = "Geometry Table", .usage = BufferUsage::STORAGE, .size = static_cast<uint64_t>(config.geometryPoolSubMeshes) * sizeof(gfx::GeometryData)});

        if (!geometryTableBuffer.isValid() ||
            !gpuScene->instances.create(ctx, "Instance Table", sizeof(gfx::InstanceData), config.maxInstances) ||
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
        return true;
    }

    // Runs once per frame, after the fence wait for this frame's slot, so the slot's buffers are free to rewrite.
    // Its cost follows what changed, not how much the scenes hold (P7).
    void Engine::syncGpuScene()
    {
        ZoneScoped;

        auto &waiting = gpuScene->entitiesWaitingForMesh;
        for (size_t i = 0; i < waiting.size();)
        {
            const EntityHandle entity = waiting[i];
            const Entity &entityRef = registry.entities[entity.index];
            if (!isLoaded(entityRef.mesh))
            {
                ++i;
                continue;
            }

            const auto &subMeshes = registry.meshes[entityRef.mesh.index].subMeshes;
            const auto rowCount = static_cast<uint32_t>(subMeshes.size());
            if (rowCount > gpuScene->instances.capacity() - gpuScene->instanceCount)
            {
                spdlog::error("The instance table is full: entity {} needs {} rows ({}/{} in use). EngineConfig::maxInstances sets its size.",
                              entity.index, rowCount, gpuScene->instanceCount, gpuScene->instances.capacity());
            }
            else
            {
                for (const auto &subMesh : subMeshes)
                {
                    if (subMesh.geometry.vertexCount == 0 || subMesh.geometry.indexCount == 0)
                        spdlog::warn("Skipping a mesh of entity {}: it has {} vertices and {} indices, and drawing needs both.",
                                     entity.index, subMesh.geometry.vertexCount, subMesh.geometry.indexCount);
                }

                // Rows are never freed: entities are not destroyed yet (D4).
                if (gpuScene->entityInstances.size() <= entity.index)
                    gpuScene->entityInstances.resize(entity.index + 1);
                gpuScene->entityInstances[entity.index] = {gpuScene->instanceCount, rowCount};
                gpuScene->instanceCount += rowCount;
                gpuScene->instanceCullModes.resize(gpuScene->instanceCount, CullMode::NONE);
                writeInstanceRows(entity);
                markDrawListsDirty();
            }

            waiting[i] = waiting.back();
            waiting.pop_back();
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

    void Engine::writeInstanceRows(EntityHandle entity)
    {
        if (entity.index >= gpuScene->entityInstances.size())
            return;
        const InstanceRange range = gpuScene->entityInstances[entity.index];
        if (range.count == 0)
            return;

        const Entity &entityRef = registry.entities[entity.index];
        const Mesh &mesh = registry.meshes[entityRef.mesh.index];
        const glm::mat4 entityMatrix = getModelMatrix(entityRef.transform);

        bool cullModeChanged = false;
        for (uint32_t i = 0; i < range.count; ++i)
        {
            const SubMesh &subMesh = mesh.subMeshes[i];
            // Primitives without a material use the entity's; getMaterial resolves an invalid one to the default material.
            const Material &material = getMaterial(subMesh.material.isValid() ? subMesh.material : entityRef.material);
            const glm::mat4 model = entityMatrix * subMesh.transform;

            const gfx::InstanceData row{.worldMatrix = model,
                                        .normalMatrix = glm::transpose(glm::inverse(glm::mat3(model))),
                                        .geometryIndex = subMesh.geometry.geometryIndex,
                                        .materialIndex = static_cast<uint32_t>(material.handle.index)};
            gpuScene->instances.setRow(range.first + i, &row);

            // A transform can start or stop mirroring the entity, which moves its draws to another cull-mode run (D8).
            const CullMode cullMode = getCullMode(material.doubleSided, entityRef.transform, subMesh.transform);
            CullMode &storedCullMode = gpuScene->instanceCullModes[range.first + i];
            cullModeChanged = cullModeChanged || storedCullMode != cullMode;
            storedCullMode = cullMode;
        }

        if (cullModeChanged)
            markDrawListsDirty();
    }

    void Engine::markDrawListsDirty()
    {
        for (auto &drawList : gpuScene->drawLists)
            drawList.rebuild = true;
    }

    void Engine::rebuildDrawList(SceneHandle scene)
    {
        ZoneScoped;

        auto &drawList = gpuScene->drawLists[scene.index];
        drawList.rebuild = false;
        drawList.draws.clear();
        drawList.runs.clear();

        for (auto entity : registry.scenes[scene.index].entities)
        {
            if (!entity.isValid() || entity.index >= gpuScene->entityInstances.size())
                continue;
            const InstanceRange range = gpuScene->entityInstances[entity.index];
            const Entity &entityRef = registry.entities[entity.index];

            // An entity waits for all of its assets, so nothing is drawn half-loaded.
            if (range.count == 0 || !texturesLoaded(*this, entityRef.material))
                continue;

            const Mesh &mesh = registry.meshes[entityRef.mesh.index];
            for (uint32_t i = 0; i < range.count; ++i)
            {
                const MeshGeometry &geometry = mesh.subMeshes[i].geometry;
                if (geometry.vertexCount == 0 || geometry.indexCount == 0)
                    continue;
                // Indices are local to their sub-mesh; vertexOffset moves them to its place in the geometry pool.
                drawList.draws.push_back({.indexCount = geometry.indexCount,
                                          .instanceCount = 1,
                                          .firstIndex = geometry.firstIndex,
                                          .vertexOffset = static_cast<int32_t>(geometry.firstVertex),
                                          .firstInstance = range.first + i});
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

        uint32_t width = swapchain.width;
        uint32_t height = swapchain.height;

        if (currentRenderTarget.isValid())
        {
            const auto &renderTargetData = ctx.getRenderTarget(currentRenderTarget);
            width = renderTargetData.width;
            height = renderTargetData.height;

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
        const uint64_t sceneDataOffset = sceneDrawCount++ * sizeof(gfx::SceneData);

        auto &sceneRef = registry.scenes[scene.index];
        auto commandBuffer = frameResources[frameCount].commandBuffer;
        auto &cameraRef = registry.cameras[camera.index];

        gfx::SceneData sceneData{};
        sceneData.projection = getProjectionMatrix(camera, static_cast<float>(width) / static_cast<float>(height));
        sceneData.view = getViewMatrix(camera);
        sceneData.cameraWorldPosition = cameraRef.transform.position;
        sceneData.numLights = sceneRef.lightCount;
        for (uint32_t i = 0; i < sceneRef.lightCount; ++i)
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
        sceneData.vertsPtr = ctx.getBufferDeviceAddress(geometryVertexBuffer);
        sceneData.drawInfoPtr = ctx.getBufferDeviceAddress(drawList.buffers[frame]);
        sceneData.instancesPtr = ctx.getBufferDeviceAddress(gpuScene->instances.buffer(frame));
        sceneData.materialsPtr = ctx.getBufferDeviceAddress(gpuScene->materials.buffer(frame));
        sceneData.geometryPtr = ctx.getBufferDeviceAddress(geometryTableBuffer);
        ctx.fillBuffer(gpuScene->sceneData[frame], &sceneData, sizeof(sceneData), sceneDataOffset);

        ctx.bindPipeline(commandBuffer, gpuScene->pipeline);
        ctx.bindDescriptorSet(commandBuffer, gpuScene->pipeline, bindlessDescriptorSet, 0);
        ctx.bindIndexBuffer(commandBuffer, geometryIndexBuffer, 0, Format::U32_UINT);

        gfx::PushConstants pushConstants{};
        pushConstants.scene = ctx.getBufferDeviceAddress(gpuScene->sceneData[frame]) + sceneDataOffset;
        for (const auto &run : drawList.runs)
        {
            pushConstants.firstDraw = run.first;
            ctx.setCullMode(commandBuffer, run.cullMode);
            ctx.pushConstants(commandBuffer, gpuScene->pipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
            ctx.drawIndexedIndirect(commandBuffer, drawList.buffers[frame], run.first * sizeof(gfx::DrawInfo), run.count, sizeof(gfx::DrawInfo));
        }
        ctx.setCullMode(commandBuffer, CullMode::NONE);
    }
}
