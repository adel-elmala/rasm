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
#include <iterator>
#include <vector>

namespace rasm
{
    namespace
    {
        // What prepareScene returns when there is nothing to draw. Its pipeline is invalid.
        const PreparedScene EMPTY_PREPARED_SCENE{};

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

        void addMeshDraws(std::vector<SceneDraw> &draws, const Entity &entity, const Mesh &mesh)
        {
            for (const auto &subMesh : mesh.subMeshes)
            {
                if (subMesh.geometry.vertexCount == 0 || subMesh.geometry.indexCount == 0)
                {
                    spdlog::warn("Skipping a mesh of entity {}: it has {} vertices and {} indices, and drawing needs both.",
                                 entity.handle.index, subMesh.geometry.vertexCount, subMesh.geometry.indexCount);
                    continue;
                }
                draws.push_back({&entity, subMesh.geometry, subMesh.transform, subMesh.material});
            }
        }

        // glTF: a transform with a negative determinant mirrors the mesh, so its front faces turn clockwise (D8).
        // det(T * R * S * M) = sx * sy * sz * det(M). The sign is fixed at preparation, like the draw order.
        CullMode getCullMode(const Material &material, const Transform &transform, const glm::mat4 &meshTransform)
        {
            if (material.doubleSided)
                return CullMode::NONE;
            const float determinant = transform.scale.x * transform.scale.y * transform.scale.z * glm::determinant(glm::mat3(meshTransform));
            return determinant < 0.0f ? CullMode::FRONT : CullMode::BACK;
        }

        // A failed preparation is not cached, so anything it created so far would leak on every retry.
        const PreparedScene &discardSceneBuffers(RenderContext &ctx, const PreparedScene &preparedScene)
        {
            std::vector<BufferHandle> buffers = {preparedScene.drawInfoBuffer};
            buffers.insert(buffers.end(), std::begin(preparedScene.megaModelDataBuffer), std::end(preparedScene.megaModelDataBuffer));
            buffers.insert(buffers.end(), std::begin(preparedScene.sceneDataBuffer), std::end(preparedScene.sceneDataBuffer));

            for (auto buffer : buffers)
            {
                if (buffer.isValid())
                    ctx.destroyBuffer(buffer);
            }

            return EMPTY_PREPARED_SCENE;
        }
    }

    // The mega vertex buffer is written with Vertex and read in the shader as gfx::VertexFull
    // (scalar layout); the two must not drift apart.
    static_assert(sizeof(Vertex) == sizeof(gfx::VertexFull), "Vertex must match the shader's VertexFull");
    static_assert(offsetof(Vertex, uv) == offsetof(gfx::VertexFull, uv), "Vertex must match the shader's VertexFull");
    static_assert(offsetof(Vertex, tangent) == offsetof(gfx::VertexFull, tangent), "Vertex must match the shader's VertexFull");

    const PreparedScene &Engine::prepareScene(SceneHandle scene)
    {
        auto preparedScene = PreparedScene{};

        if (!scene.isValid() || scene.index >= MAX_SCENES)
        {
            spdlog::error("Invalid scene handle for preparation.");
            return EMPTY_PREPARED_SCENE;
        }

        auto cachedScene = preparedScenes.find(scene);
        if (cachedScene != preparedScenes.end())
        {
            return cachedScene->second;
        }

        // Below the cache lookup, which runs every frame: this zone marks real preparations.
        ZoneScoped;

        auto &sceneRef = registry.scenes[scene.index];

        std::vector<SceneDraw> sceneDraws;

        for (auto entityHandle : sceneRef.entities)
        {
            if (!entityHandle.isValid())
                continue;

            const auto &entity = registry.entities[entityHandle.index];

            // A scene waits for all of its assets, so it is prepared once and nothing in use is rebuilt (D4).
            for (auto texture : getMaterial(entity.material).textures)
            {
                if (texture.isValid() && !isLoaded(texture))
                    return EMPTY_PREPARED_SCENE;
            }

            auto mesh = entity.mesh;
            if (!mesh.isValid())
                continue;

            const auto &meshRef = registry.meshes[mesh.index];
            if (meshRef.state == MeshState::LOADING)
                return EMPTY_PREPARED_SCENE;

            addMeshDraws(sceneDraws, entity, meshRef);
        }

        // Nothing to draw: zero-sized buffers are invalid, and staying uncached lets entities added later show up.
        if (sceneDraws.empty())
            return EMPTY_PREPARED_SCENE;

        // Each cull mode is one pipeline and one indirect draw, so the draws are grouped by it.
        for (auto &draw : sceneDraws)
        {
            const auto &material = getMaterial(draw.material.isValid() ? draw.material : draw.entity->material);
            draw.cullMode = getCullMode(material, draw.entity->transform, draw.transform);
        }
        std::stable_sort(sceneDraws.begin(), sceneDraws.end(), [](const SceneDraw &a, const SceneDraw &b)
                         { return a.cullMode < b.cullMode; });

        // create the UberMaterial pipelines. A failed preparation retries every frame, so this comes
        // before the buffers: a shader error must not allocate and fill them each time.
        // The shaders are compiled once; the other cull modes copy the first pipeline's desc.
        auto pipeline = createPipeline("rasm", readFile(pathFromUtf8(config.shaderDir) / "passes/rasm.slang"));
        for (uint32_t i = 0; i < CULL_MODE_COUNT && pipeline.isValid(); ++i)
        {
            auto pipelineDesc = ctx.getPipelineDesc(pipeline);
            pipelineDesc.cullMode = static_cast<CullMode>(i);
            preparedScene.uberMaterialPipelines[i] = ctx.createPipeline(pipelineDesc);
            if (!preparedScene.uberMaterialPipelines[i].isValid())
                pipeline = {};
        }

        if (!pipeline.isValid())
        {
            spdlog::error("Failed to create the uber material pipelines for the scene.");
            return EMPTY_PREPARED_SCENE;
        }

        // allocate mega-model buffer and draw info buffer for the scene.
        BufferDesc bufferDesc;
        bufferDesc.name = "Mega Model data Buffer";
        bufferDesc.usage = BufferUsage::STORAGE;
        bufferDesc.size = sceneDraws.size() * sizeof(gfx::Modeldata);
        bufferDesc.hostVisible = true;
        // Mutable uniforms belong to a frame slot; other slots may still be executing on the GPU.
        for (auto &buffer : preparedScene.megaModelDataBuffer)
        {
            buffer = ctx.createBuffer(bufferDesc);
            if (!buffer.isValid())
                return discardSceneBuffers(ctx, preparedScene);
        }

        bufferDesc.name = "Draw Info Buffer";
        bufferDesc.usage = BufferUsage::INDIRECT;
        bufferDesc.size = sceneDraws.size() * sizeof(gfx::DrawInfo);
        bufferDesc.hostVisible = false;

        preparedScene.drawInfoBuffer = ctx.createBuffer(bufferDesc);

        if (!preparedScene.drawInfoBuffer.isValid())
        {
            spdlog::error("Failed to create draw info buffer for the scene.");
            return discardSceneBuffers(ctx, preparedScene);
        }

        // create sceneData buffer for the scene.
        bufferDesc.name = "Scene Data Buffer";
        bufferDesc.usage = BufferUsage::STORAGE;
        bufferDesc.size = MAX_SCENE_DRAWS_PER_FRAME * sizeof(gfx::SceneData);
        bufferDesc.hostVisible = true;

        for (auto &buffer : preparedScene.sceneDataBuffer)
        {
            buffer = ctx.createBuffer(bufferDesc);
            if (!buffer.isValid())
                return discardSceneBuffers(ctx, preparedScene);
        }

        uint64_t modelBufferOffset = 0;
        uint64_t drawInfoBufferOffset = 0;

        ZoneNamedN(uploadZone, "uploadScene", true);

        for (const auto &draw : sceneDraws)
        {
            // fill the model matrix buffer with the current entity's model matrix.
            auto modelTransform = draw.entity->transform;

            auto model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                         glm::mat4_cast(modelTransform.rotation) *
                         glm::scale(glm::mat4(1.0f), modelTransform.scale);

            model = model * draw.transform;

            preparedScene.modelUniforms.push_back({draw.entity->handle, draw.transform});

            gfx::Modeldata modelData{};
            modelData.worldMatrix = model;
            modelData.normalMatrix = glm::transpose(glm::inverse(glm::mat3(model)));
            modelData.vertFormat = gfx::VertexFormat::FULL;
            // Primitives without a material use the entity's; getMaterial resolves an invalid one to the default material.
            auto material = draw.material.isValid() ? draw.material : draw.entity->material;
            const auto &materialRef = getMaterial(material);
            modelData.material = convertToGfxMaterial(*this, materialRef);
            modelData.materialId = getGfxMaterialId(materialRef.type);

            for (auto buffer : preparedScene.megaModelDataBuffer)
                ctx.fillBuffer(buffer, &modelData, sizeof(modelData), modelBufferOffset);
            modelBufferOffset += sizeof(modelData);

            auto drawInfo = gfx::DrawInfo{};
            // Indices are local to their sub-mesh; vertexOffset moves them to its place in the geometry pool.
            drawInfo.indexCount = draw.geometry.indexCount;
            drawInfo.instanceCount = 1; // TODO: support multiple instances
            drawInfo.firstIndex = draw.geometry.firstIndex;
            drawInfo.vertexOffset = static_cast<int32_t>(draw.geometry.firstVertex);
            drawInfo.firstInstance = 0; // TODO: support multiple instances

            ctx.fillBuffer(preparedScene.drawInfoBuffer, &drawInfo, sizeof(drawInfo), drawInfoBufferOffset);
            drawInfoBufferOffset += sizeof(drawInfo);
            ++preparedScene.drawInfoCounts[static_cast<uint32_t>(draw.cullMode)];
        }

        preparedScene.drawInfoCount = sceneDraws.size();

        return preparedScenes.emplace(scene, std::move(preparedScene)).first->second;
    }

    void Engine::updateUniforms(const PreparedScene &preparedScene)
    {
        ZoneScoped;

        size_t modelBufferOffset = 0;

        for (const auto &[EntityHandle, gltfTransform] : preparedScene.modelUniforms)
        {
            const auto &modelTransform = registry.entities[EntityHandle.index].transform;

            auto model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                         glm::mat4_cast(modelTransform.rotation) *
                         glm::scale(glm::mat4(1.0f), modelTransform.scale);

            model = model * gltfTransform;

            gfx::Modeldata modelData{};
            modelData.worldMatrix = model;
            modelData.normalMatrix = glm::transpose(glm::inverse(glm::mat3(model)));

            ctx.fillBuffer(preparedScene.megaModelDataBuffer[frameCount], &modelData, offsetof(gfx::Modeldata, material), modelBufferOffset);
            modelBufferOffset += sizeof(modelData);
        }
    }

    void Engine::drawScene(SceneHandle scene, CameraHandle camera)
    {
        ZoneScoped;

        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("drawScene needs a valid camera handle.");
            return;
        }

        const auto &preparedScene = prepareScene(scene);

        // An invalid or empty scene, or a failed preparation (which prepareScene logged and retries next
        // frame), has no pipeline to bind; the pass keeps its cleared attachments.
        if (!preparedScene.uberMaterialPipelines[0].isValid())
            return;

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
                spdlog::error("drawScene needs a render target with the swapchain's color format and D32_SFLOAT_S8_UINT depth.");
                return;
            }
        }

        // The GPU reads SceneData after the whole frame is recorded, so every call needs its own slot.
        if (sceneDrawCount >= MAX_SCENE_DRAWS_PER_FRAME)
        {
            spdlog::error("Exceeded maximum number of drawScene calls per frame.");
            return;
        }
        // Modeldata has one slot per frame, so a scene drawn by several passes needs its uniforms written once.
        bool uniformsWritten = false;
        for (uint32_t i = 0; i < sceneDrawCount; ++i)
            uniformsWritten = uniformsWritten || drawnScenes[i] == scene;

        drawnScenes[sceneDrawCount] = scene;
        const uint64_t sceneDataOffset = sceneDrawCount++ * sizeof(gfx::SceneData);

        auto &sceneRef = registry.scenes[scene.index];
        auto &currentFrame = frameResources[frameCount];
        auto commandBuffer = currentFrame.commandBuffer;

        auto &cameraRef = registry.cameras[camera.index];
        auto projection = getProjectionMatrix(camera, static_cast<float>(width) / static_cast<float>(height));
        auto view = getViewMatrix(camera);

        {
            gfx::SceneData sceneData{};
            sceneData.projection = projection;
            sceneData.view = view;
            sceneData.cameraWorldPosition = cameraRef.transform.position;
            sceneData.numLights = sceneRef.lightCount;
            for (uint32_t i = 0; i < sceneRef.lightCount; ++i)
            {
                LightHandle sceneLight = sceneRef.lights[i];
                Light sceneLightData = registry.lights[sceneLight.index];

                gfx::Light light{};
                light.position = sceneLightData.position;
                light.direction = sceneLightData.direction;
                light.color = sceneLightData.color;
                light.intensity = sceneLightData.intensity;
                light.range = sceneLightData.range;
                light.type = static_cast<gfx::LightType>(sceneLightData.type);

                sceneData.lights[i] = light;
            }
            sceneData.vertsPtr = ctx.getBufferDeviceAddress(geometryVertexBuffer);
            sceneData.drawInfoPtr = ctx.getBufferDeviceAddress(preparedScene.drawInfoBuffer);
            sceneData.modeldataPtr = ctx.getBufferDeviceAddress(preparedScene.megaModelDataBuffer[frameCount]);

            ctx.fillBuffer(preparedScene.sceneDataBuffer[frameCount], &sceneData, sizeof(sceneData), sceneDataOffset);

            if (!uniformsWritten)
                updateUniforms(preparedScene);

            ctx.bindIndexBuffer(commandBuffer, geometryIndexBuffer, 0, Format::U32_UINT);

            // One indirect draw per cull mode; the draws are grouped by cull mode in enum order (D8).
            gfx::PushConstants pushConstants{};
            pushConstants.scene = ctx.getBufferDeviceAddress(preparedScene.sceneDataBuffer[frameCount]) + sceneDataOffset;
            for (uint32_t i = 0; i < CULL_MODE_COUNT; ++i)
            {
                const uint32_t drawCount = preparedScene.drawInfoCounts[i];
                if (drawCount == 0)
                    continue;

                const auto pipeline = preparedScene.uberMaterialPipelines[i];
                ctx.bindPipeline(commandBuffer, pipeline);
                ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
                ctx.pushConstants(commandBuffer, pipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
                ctx.drawIndexedIndirect(commandBuffer, preparedScene.drawInfoBuffer, pushConstants.firstDraw * sizeof(gfx::DrawInfo), drawCount, sizeof(gfx::DrawInfo));
                pushConstants.firstDraw += drawCount;
            }
        }
    }
}
