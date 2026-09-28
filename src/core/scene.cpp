#include "rasm/core/scene.h"
#include "rasm/core/camera.h"
#include "rasm/core/engine.h"
#include "rasm/core/utils.h"

#include <variant>

namespace rasm
{
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

  PreparedScene Engine::prepareScene(SceneHandle scene)
  {
    auto preparedScene = PreparedScene{};

    if (!scene.isValid() || scene.index >= MAX_SCENES)
    {
      spdlog::error("Invalid scene handle for preparation.");
      return PreparedScene{};
    }

    if (preparedScenes.find(scene) != preparedScenes.end())
    {
      return preparedScenes[scene];
    }

    auto &sceneRef = registery.scenes[scene.index];

    // find all meshes associated with the entities in the scene and compute the total byte size required for them.
    uint64_t totalVertexBufferSize = 0;
    uint64_t totalIndexBufferSize = 0;
    uint64_t nMeshes = 0;

    for (auto entityHandle : sceneRef.entities)
    {
      if (!entityHandle.isValid())
        continue;

      auto &entity = registery.entities[entityHandle.index];

      auto mesh = entity.mesh;
      if (!mesh.isValid())
        continue;

      auto &meshData = registery.meshes[mesh.index];

      auto meshSize = getMeshByteSize(mesh);

      totalVertexBufferSize += meshSize.verticesByteSize;
      totalIndexBufferSize += meshSize.indicesByteSize;
      nMeshes++;
    }

    // allocate one mega vertex-buffer and index-buffer to hold all the mesh data for the scene.
    queryMemoryStats();
    if (profiler.gpu_memory_stats.vram_usage + totalVertexBufferSize + totalIndexBufferSize > profiler.gpu_memory_stats.vram_budget)
    {
      spdlog::warn("Not enough VRAM to allocate buffers for the scene.");
      return PreparedScene{};
    }

    // Allocate the vertex and index buffers for the scene.
    auto bufferDesc = ResourceDesc{};
    bufferDesc.name = "Mega Vertex Buffer";
    bufferDesc.type = ResourceType::BUFFER;
    bufferDesc.buffer.usage = BufferUsage::STORAGE;
    bufferDesc.buffer.size = totalVertexBufferSize;
    bufferDesc.buffer.storageBuffer.offset = 0;

    auto vertexBufferHandle = ctx.createBuffer(bufferDesc);

    bufferDesc.name = "Mega Index Buffer";
    bufferDesc.buffer.usage = BufferUsage::INDEX;
    bufferDesc.buffer.size = totalIndexBufferSize;
    bufferDesc.buffer.storageBuffer.offset = 0;

    auto indexBufferHandle = ctx.createBuffer(bufferDesc);

    if (!vertexBufferHandle.isValid() || !indexBufferHandle.isValid())
    {
      spdlog::error("Failed to create vertex or index buffer for the scene.");
      return PreparedScene{};
    }

    preparedScene.megaVertexBuffer = vertexBufferHandle;
    preparedScene.megaIndexBuffer = indexBufferHandle;

    // allocate mega-model buffer and draw info buffer for the scene.
    bufferDesc.name = "Mega Model Buffer";
    bufferDesc.buffer.usage = BufferUsage::STORAGE;
    bufferDesc.buffer.size = nMeshes * sizeof(glm::mat4);
    auto megaModelBufferHandle = ctx.createBuffer(bufferDesc);

    if (!megaModelBufferHandle.isValid())
    {
      spdlog::error("Failed to create mega model buffer for the scene.");
      return PreparedScene{};
    }

    preparedScene.megaModelMatsBuffer = megaModelBufferHandle;

    bufferDesc.name = "Draw Info Buffer";
    bufferDesc.buffer.usage = BufferUsage::INDIRECT;
    bufferDesc.buffer.size = nMeshes * sizeof(DrawInfo);

    auto drawInfoBufferHandle = ctx.createBuffer(bufferDesc);

    if (!drawInfoBufferHandle.isValid())
    {
      spdlog::error("Failed to create draw info buffer for the scene.");
      return PreparedScene{};
    }

    preparedScene.drawInfoBuffer = drawInfoBufferHandle;

    // create sceneData buffer for the scene.
    bufferDesc.name = "Scene Data Buffer";
    bufferDesc.buffer.usage = BufferUsage::STORAGE;
    bufferDesc.buffer.size = sizeof(SceneData);

    auto sceneDataBufferHandle = ctx.createBuffer(bufferDesc);

    if (!sceneDataBufferHandle.isValid())
    {
      spdlog::error("Failed to create scene data buffer for the scene.");
      return PreparedScene{};
    }

    preparedScene.sceneDataBuffer = sceneDataBufferHandle;

    uint64_t vertexBufferOffset = 0;
    uint64_t indexBufferOffset = 0;
    uint64_t modelBufferOffset = 0;
    uint64_t drawInfoBufferOffset = 0;

    uint32_t IndexOffset = 0;
    uint32_t VertexOffset = 0;

    for (auto entityHandle : sceneRef.entities)
    {
      if (!entityHandle.isValid())
        continue;

      auto &entity = registery.entities[entityHandle.index];

      auto mesh = entity.mesh;
      if (!mesh.isValid())
        continue;

      auto &meshData = registery.meshes[mesh.index];

      auto meshSize = getMeshByteSize(mesh);

      uint32_t nVertices = 0;
      uint32_t nIndices = 0;

      if (meshData.type == Mesh::MeshType::OBJ)
      {
        auto &objRaw = std::get<ObjRaw>(meshData.data);

        ctx.fillBuffer(preparedScene.megaVertexBuffer, objRaw.vertices.data(), meshSize.verticesByteSize, vertexBufferOffset);
        ctx.fillBuffer(preparedScene.megaIndexBuffer, objRaw.indices.data(), meshSize.indicesByteSize, indexBufferOffset);

        vertexBufferOffset += meshSize.verticesByteSize;
        indexBufferOffset += meshSize.indicesByteSize;

        nVertices = static_cast<uint32_t>(objRaw.vertices.size());
        nIndices = static_cast<uint32_t>(objRaw.indices.size());
      }
      else
      {
        spdlog::warn("Uploading Mesh data not Implemented for mesh type other than OBJ. Skipping.");
      }

      // fill the model matrix buffer with the current entity's model matrix.
      auto modelTransform = entity.transform;

      auto model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                   glm::mat4_cast(modelTransform.rotation) *
                   glm::scale(glm::mat4(1.0f), modelTransform.scale);

      ctx.fillBuffer(preparedScene.megaModelMatsBuffer, &model, sizeof(model), modelBufferOffset);
      modelBufferOffset += sizeof(model);

      // TODO: fill the draw info buffer with the current entity's draw info.

      auto drawInfo = DrawInfo{};
      drawInfo.indexCount = nIndices;
      drawInfo.instanceCount = 1; // TODO: support multiple instances
      drawInfo.firstIndex = IndexOffset;
      drawInfo.vertexOffset = VertexOffset;
      drawInfo.firstInstance = 0; // TODO: support multiple instances
      drawInfo.vertFormat = VertexFormat::Full; // TODO: determine the correct vertex format based on the mesh

      ctx.fillBuffer(preparedScene.drawInfoBuffer, &drawInfo, sizeof(drawInfo), drawInfoBufferOffset);
      drawInfoBufferOffset += sizeof(drawInfo);

      IndexOffset += nIndices;
      VertexOffset += nVertices;
    }

    // create a pipeline for the UberMaterial.
    auto swapchainFormat = ctx.getSwapchainImageFormat();
    auto depthFormat = Format::D24_UNORM_S8_UINT;

    auto pipelineDesc = ResourceDesc{};
    pipelineDesc.type = ResourceType::GRAPHICS_PIPELINE;
    pipelineDesc.name = "Pipeline for Uber-Material";

    auto shaderSource = readFile("./shaders/common/rasm.slang");
    auto vertShader = shaderCompiler.compileFromString(shaderSource, "rasm.vert.slang", "vertMain");
    auto fragShader = shaderCompiler.compileFromString(shaderSource, "rasm.frag.slang", "fragMain");

    auto shaderDesc = ResourceDesc{};
    shaderDesc.type = ResourceType::SHADER;
    shaderDesc.shader.shaderType = ShaderType::VERTEX;
    shaderDesc.shader.sourceSize = vertShader.code.size();
    shaderDesc.shader.source = reinterpret_cast<const char *>(vertShader.code.data());

    pipelineDesc.pipeline.vertexShader = ctx.createShader(shaderDesc);

    shaderDesc.shader.shaderType = ShaderType::FRAGMENT;
    shaderDesc.shader.sourceSize = fragShader.code.size();
    shaderDesc.shader.source = reinterpret_cast<const char *>(fragShader.code.data());

    pipelineDesc.pipeline.fragmentShader = ctx.createShader(shaderDesc);

    pipelineDesc.pipeline.topology = PrimitiveTopology::TRIANGLE_LIST;

    pipelineDesc.pipeline.colorAttachmentFormat = swapchainFormat;
    pipelineDesc.pipeline.depthStencilAttachmentFormat = depthFormat;

    pipelineDesc.pipeline.vertexInputLayout = {.vertexPulling = true};

    pipelineDesc.pipeline.descriptorSetLayout = bindlessDescriptorSetLayout;

    preparedScene.uberMaterialPipeline = ctx.createPipeline(pipelineDesc);
    preparedScene.drawInfoCount = nMeshes;

    preparedScenes[scene] = preparedScene;
    return preparedScene;
  }

  CompiledScene Engine::compileScene(SceneHandle scene)
  {
    if (!scene.isValid() || scene.index >= MAX_SCENES)
    {
      spdlog::error("Invalid scene handle for compilation.");
      return CompiledScene{};
    }

    if (compiledScenes.find(scene) != compiledScenes.end())
    {
      return compiledScenes[scene];
    }

    CompiledScene compiledScene;

    auto _scene = registery.scenes[scene.index];

    for (auto entityHandle : _scene.entities)
    {
      if (!entityHandle.isValid())
        continue;

      auto entity = registery.entities[entityHandle.index];

      auto mat = entity.material;
      auto mesh = entity.mesh;

      auto bufferHandle = uploadMesh(mesh);
      if (!bufferHandle.isValid())
      {
        spdlog::error("Failed to upload mesh for Entity {}. Skipping.", entity.handle.index);
        continue;
      }

      compiledScene.meshData[entity.handle] = bufferHandle;
      compiledScene.materialToMeshes[mat].insert(entity.handle);
    }

    auto swapchainFormat = ctx.getSwapchainImageFormat();
    auto depthFormat = Format::D24_UNORM_S8_UINT;

    for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
    {
      if (!materialHandle.isValid())
      {
        spdlog::error("Material is invalid: {}.", materialHandle.index);
        continue;
      }

      // create a pipeline for each material.
      auto pipelineDesc = ResourceDesc{};
      pipelineDesc.type = ResourceType::GRAPHICS_PIPELINE;
      pipelineDesc.name = "Pipeline for Material " + std::to_string(materialHandle.index);

      auto material = registery.materials[materialHandle.index];

      auto shader = material.shader;
      auto shaderSource = registery.shaders[shader.index];

      auto shaderName = material.name;
      auto vertShader = shaderCompiler.compileFromString(shaderSource, shaderName + std::string(".vert.slang"), "vertMain");
      auto fragShader = shaderCompiler.compileFromString(shaderSource, shaderName + std::string(".frag.slang"), "fragMain");

      auto shaderDesc = ResourceDesc{};
      shaderDesc.type = ResourceType::SHADER;
      shaderDesc.shader.shaderType = ShaderType::VERTEX;
      shaderDesc.shader.sourceSize = vertShader.code.size();
      shaderDesc.shader.source = reinterpret_cast<const char *>(vertShader.code.data());

      pipelineDesc.pipeline.vertexShader = ctx.createShader(shaderDesc);

      shaderDesc.shader.shaderType = ShaderType::FRAGMENT;
      shaderDesc.shader.sourceSize = fragShader.code.size();
      shaderDesc.shader.source = reinterpret_cast<const char *>(fragShader.code.data());

      pipelineDesc.pipeline.fragmentShader = ctx.createShader(shaderDesc);

      pipelineDesc.pipeline.topology = PrimitiveTopology::TRIANGLE_LIST;

      pipelineDesc.pipeline.colorAttachmentFormat = swapchainFormat;
      pipelineDesc.pipeline.depthStencilAttachmentFormat = depthFormat;

      rasm::VertexAttributeDescription attrDesc[4] = {};
      auto size = std::min(vertShader.vertexInputLayout.size(), static_cast<size_t>(4));
      for (size_t i = 0; i < size; ++i)
      {
        attrDesc[i] = vertShader.vertexInputLayout[i];
      }

      pipelineDesc.pipeline.vertexInputLayout = {.attributes = {attrDesc[0], attrDesc[1], attrDesc[2], attrDesc[3]},
                                                 .binding = 0,
                                                 .stride = sizeof(Vertex),
                                                 .perInstance = false};

      pipelineDesc.pipeline.descriptorSetLayout = bindlessDescriptorSetLayout;

      auto pipeline = ctx.createPipeline(pipelineDesc);
      compiledScene.materialToPipeline[materialHandle] = pipeline;
    }

    compiledScenes[scene] = compiledScene;

    return compiledScene;
  }
} // namespace rasm
 