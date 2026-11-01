#include "rasm/scene.h"
#include "rasm/camera.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <cstddef>
#include <cstdlib>
#include <variant>

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
      auto texCoordOf = [](uint32_t texCoord) -> uint32_t { return texCoord < MAX_VERTEX_UV_SETS ? texCoord : 0u; };

      gmat.pbr.baseColorTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::ALBEDO)]);
      gmat.pbr.baseColorTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::ALBEDO)]);
      gmat.pbr.metallicRoughnessTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::METALLIC)]);
      gmat.pbr.metallicRoughtnessTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::METALLIC)]);

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

    void addSceneDraw(std::vector<SceneDraw> &draws, const Entity &entity, const ObjRaw &geometry, const glm::mat4 &transform, MaterialHandle material)
    {
      if (geometry.vertices.empty() || geometry.indices.empty())
      {
        spdlog::warn("Skipping a mesh of entity {}: it has {} vertices and {} indices, and drawing needs both.",
                     entity.handle.index, geometry.vertices.size(), geometry.indices.size());
        return;
      }

      draws.push_back({&entity, &geometry, transform, material});
    }

    void addMeshDraws(std::vector<SceneDraw> &draws, const Entity &entity, const Mesh &mesh)
    {
      if (mesh.type == Mesh::MeshType::OBJ)
      {
        addSceneDraw(draws, entity, std::get<ObjRaw>(mesh.data), glm::mat4(1.0f), MaterialHandle{});
        return;
      }

      for (const auto &subGLTF : std::get<GLTFRaw>(mesh.data).subGltfRaws)
        addSceneDraw(draws, entity, subGLTF.objRaw, subGLTF.transform, subGLTF.material);
    }

    // A failed preparation is not cached, so anything it created so far would leak on every retry.
    const PreparedScene &discardSceneBuffers(RenderContext &ctx, const PreparedScene &preparedScene)
    {
      std::vector<BufferHandle> buffers = {preparedScene.megaVertexBuffer, preparedScene.megaIndexBuffer, preparedScene.drawInfoBuffer};
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

  SceneHandle Engine::createScene()
  {
    const SceneHandle id = handleManager.getNextSceneHandle();

    if (id.index >= MAX_SCENES)
    {
      spdlog::warn("Exceeded maximum number of scenes.");
      return {};
    }

    auto scene = Scene{.handle = id,
                       .entities = {},
                       .entityCount = 0,
                       .cameras = {},
                       .cameraCount = 0,
                       .lights = {},
                       .lightCount = 0};

    registery.scenes.resize(id.index + 1);
    registery.scenes[id.index] = scene;

    currentScene = id;

    return id;
  }

  void Engine::addEntityToScene(SceneHandle scene, EntityHandle entity)
  {
    if (!scene.isValid() || scene.index >= MAX_SCENES)
    {
      spdlog::error("Invalid scene handle for adding entity.");
      return;
    }

    if (!entity.isValid())
    {
      spdlog::error("Invalid entity handle for adding to scene.");
      return;
    }

    auto &sceneRef = registery.scenes[scene.index];

    if (sceneRef.entityCount + 1 > MAX_ENTITIES_PER_SCENE)
    {
      spdlog::warn("Exceeded maximum entities per scene.");
      return;
    }

    sceneRef.entities[sceneRef.entityCount++] = entity;
  }

  void Engine::addCameraToScene(SceneHandle scene, CameraHandle camera)
  {
    if (!scene.isValid() || scene.index >= MAX_SCENES)
    {
      spdlog::error("Invalid scene handle for adding camera.");
      return;
    }

    if (!camera.isValid())
    {
      spdlog::error("Invalid camera handle for adding to scene.");
      return;
    }

    auto &sceneRef = registery.scenes[scene.index];

    if (sceneRef.cameraCount + 1 > MAX_CAMERAS_PER_SCENE)
    {
      spdlog::warn("Exceeded maximum cameras per scene.");
      return;
    }

    sceneRef.cameras[sceneRef.cameraCount++] = camera;
  }

  void Engine::addLightToScene(SceneHandle scene, LightHandle light)
  {
    if (!scene.isValid() || scene.index >= MAX_SCENES)
    {
      spdlog::error("Invalid scene handle for adding light.");
      return;
    }

    if (!light.isValid())
    {
      spdlog::error("Invalid light handle for adding to scene.");
      return;
    }

    auto &sceneRef = registery.scenes[scene.index];

    if (sceneRef.lightCount + 1 > MAX_LIGHTS_PER_SCENE)
    {
      spdlog::warn("Exceeded maximum lights per scene.");
      return;
    }

    sceneRef.lights[sceneRef.lightCount++] = light;
  }

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

    auto &sceneRef = registery.scenes[scene.index];

    // find all meshes associated with the entities in the scene and compute the total byte size required for them.
    uint64_t totalVertexBufferSize = 0;
    uint64_t totalIndexBufferSize = 0;

    std::vector<SceneDraw> sceneDraws;

    for (auto entityHandle : sceneRef.entities)
    {
      if (!entityHandle.isValid())
        continue;

      const auto &entity = registery.entities[entityHandle.index];

      auto mesh = entity.mesh;
      if (!mesh.isValid())
        continue;

      addMeshDraws(sceneDraws, entity, registery.meshes[mesh.index]);
    }

    // Nothing to draw: zero-sized buffers are invalid, and staying uncached lets entities added later show up.
    if (sceneDraws.empty())
      return EMPTY_PREPARED_SCENE;

    for (const auto &draw : sceneDraws)
    {
      totalVertexBufferSize += draw.geometry->vertices.size() * sizeof(Vertex);
      totalIndexBufferSize += draw.geometry->indices.size() * sizeof(uint32_t);
    }

    // allocate one mega vertex-buffer and index-buffer to hold all the mesh data for the scene.
    queryMemoryStats();
    if (profiler.gpu_memory_stats.vram_usage + totalVertexBufferSize + totalIndexBufferSize > profiler.gpu_memory_stats.vram_budget)
    {
      spdlog::warn("Not enough VRAM to allocate buffers for the scene.");
      return EMPTY_PREPARED_SCENE;
    }

    // create a pipeline for the UberMaterial. A failed preparation retries every frame, so this comes
    // before the buffers: a shader error must not allocate and fill them each time.
    preparedScene.uberMaterialPipeline = createPipeline("rasm", readFile("./shaders/passes/rasm.slang"));

    if (!preparedScene.uberMaterialPipeline.isValid())
    {
      spdlog::error("Failed to create the uber material pipeline for the scene.");
      return EMPTY_PREPARED_SCENE;
    }

    // Allocate the vertex and index buffers for the scene.
    auto bufferDesc = ResourceDesc{};
    bufferDesc.name = "Mega Vertex Buffer";
    bufferDesc.type = ResourceType::BUFFER;
    bufferDesc.buffer.usage = BufferUsage::STORAGE;
    bufferDesc.buffer.size = totalVertexBufferSize;

    preparedScene.megaVertexBuffer = ctx.createBuffer(bufferDesc);

    bufferDesc.name = "Mega Index Buffer";
    bufferDesc.buffer.usage = BufferUsage::INDEX;
    bufferDesc.buffer.size = totalIndexBufferSize;

    preparedScene.megaIndexBuffer = ctx.createBuffer(bufferDesc);

    if (!preparedScene.megaVertexBuffer.isValid() || !preparedScene.megaIndexBuffer.isValid())
    {
      spdlog::error("Failed to create vertex or index buffer for the scene.");
      return discardSceneBuffers(ctx, preparedScene);
    }

    // allocate mega-model buffer and draw info buffer for the scene.
    bufferDesc.name = "Mega Model data Buffer";
    bufferDesc.buffer.usage = BufferUsage::STORAGE;
    bufferDesc.buffer.size = sceneDraws.size() * sizeof(gfx::Modeldata);
    // Mutable uniforms belong to a frame slot; other slots may still be executing on the GPU.
    for (auto &buffer : preparedScene.megaModelDataBuffer)
    {
      buffer = ctx.createBuffer(bufferDesc);
      if (!buffer.isValid()) return discardSceneBuffers(ctx, preparedScene);
    }

    bufferDesc.name = "Draw Info Buffer";
    bufferDesc.buffer.usage = BufferUsage::INDIRECT;
    bufferDesc.buffer.size = sceneDraws.size() * sizeof(gfx::DrawInfo);

    preparedScene.drawInfoBuffer = ctx.createBuffer(bufferDesc);

    if (!preparedScene.drawInfoBuffer.isValid())
    {
      spdlog::error("Failed to create draw info buffer for the scene.");
      return discardSceneBuffers(ctx, preparedScene);
    }

    // create sceneData buffer for the scene.
    bufferDesc.name = "Scene Data Buffer";
    bufferDesc.buffer.usage = BufferUsage::STORAGE;
    bufferDesc.buffer.size = MAX_SCENE_DRAWS_PER_FRAME * sizeof(gfx::SceneData);

    for (auto &buffer : preparedScene.sceneDataBuffer)
    {
      buffer = ctx.createBuffer(bufferDesc);
      if (!buffer.isValid()) return discardSceneBuffers(ctx, preparedScene);
    }

    uint64_t vertexBufferOffset = 0;
    uint64_t indexBufferOffset = 0;
    uint64_t modelBufferOffset = 0;
    uint64_t drawInfoBufferOffset = 0;

    uint32_t IndexOffset = 0;
    uint32_t VertexOffset = 0;

    ZoneNamedN(uploadZone, "uploadScene", true);

    for (const auto &draw : sceneDraws)
    {
      auto nVertices = draw.geometry->vertices.size();
      auto nIndices = draw.geometry->indices.size();
      auto vSize = nVertices * sizeof(gfx::VertexFull);
      auto iSize = nIndices * sizeof(uint32_t);

      ctx.fillBuffer(preparedScene.megaVertexBuffer, draw.geometry->vertices.data(), vSize, vertexBufferOffset);
      ctx.fillBuffer(preparedScene.megaIndexBuffer, draw.geometry->indices.data(), iSize, indexBufferOffset);

      vertexBufferOffset += vSize;
      indexBufferOffset += iSize;

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
      modelData.vertFormat = gfx::VertexFormat::Full;
      // Primitives without a material use the entity's; getMaterial resolves an invalid one to the default material.
      auto material = draw.material.isValid() ? draw.material : draw.entity->material;
      const auto &materialRef = getMaterial(material);
      modelData.material = convertToGfxMaterial(*this, materialRef);
      modelData.materialId = getGfxMaterialId(materialRef.type);

      for (auto buffer : preparedScene.megaModelDataBuffer)
        ctx.fillBuffer(buffer, &modelData, sizeof(modelData), modelBufferOffset);
      modelBufferOffset += sizeof(modelData);

      auto drawInfo = gfx::DrawInfo{};
      drawInfo.indexCount = nIndices;
      drawInfo.instanceCount = 1; // TODO: support multiple instances
      drawInfo.firstIndex = IndexOffset;
      drawInfo.vertexOffset = VertexOffset;
      drawInfo.firstInstance = 0; // TODO: support multiple instances

      ctx.fillBuffer(preparedScene.drawInfoBuffer, &drawInfo, sizeof(drawInfo), drawInfoBufferOffset);
      drawInfoBufferOffset += sizeof(drawInfo);

      IndexOffset += nIndices;
      VertexOffset += nVertices;
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
      auto entity = registery.entities[EntityHandle.index];
      auto modelTransform = entity.transform;

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

} // namespace rasm
