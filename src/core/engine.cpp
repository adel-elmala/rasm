#include "rasm/core/engine.h"
#include "rasm/core/mesh.h"
#include "rasm/core/material.h"
#include "rasm/core/light.h"
#include "rasm/core/math.h"
#include "rasm/core/entity.h"
#include "rasm/core/camera.h"
#include "rasm/core/renderGraph.h"
#include "rasm/gfx/context.h"

#include "spdlog/spdlog.h"

#define TINYGLTF_IMPLEMENTATION
#include "tiny_gltf.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/quaternion.hpp"

namespace rasm
{

    Engine::Engine(const EngineConfig &config) : config(config)
    {
        // In a real implementation, this is where we'd initialize the window, graphics context, etc.
        spdlog::info("Engine initialized with config: appName={}, windowWidth={}, windowHeight={}, enableValidation={}",
                     config.appName, config.windowWidth, config.windowHeight, config.enableValidation);

        this->window = Window(this, config.windowWidth, config.windowHeight);
        this->mainWindow = this->window.createWindow();

        ctx = RenderContext(this);
        if (!ctx.initialize(config.preferredBackend))
        {
            spdlog::error("Failed to initialize render context.");
            ctx.cleanup();
            isRunning = false;
        }

        // Initialize frame resources for double buffering
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            frameResources[i].commandPool = ctx.createCommandPool(vkb::QueueType::graphics);
            frameResources[i].commandBuffer = ctx.createCommandBuffer(frameResources[i].commandPool);
            frameResources[i].readyToDrawSemaphore = ctx.createSemaphore();
            frameResources[i].inFlightFence = ctx.createFence(true);

            // Assuming a fixed size for shader data buffer for simplicity
            ResourceDesc shaderDataBufferDesc{};
            shaderDataBufferDesc.type = ResourceDesc::Type::BUFFER;
            shaderDataBufferDesc.name = "ShaderDataBuffer for Frame " + std::to_string(i);
            shaderDataBufferDesc.buffer.size = 1024 * 1024; // 1 MB
            shaderDataBufferDesc.buffer.usage = BufferUsage::DEVICE_ADDRESS;

            frameResources[i].shaderDataBuffer = ctx.createBuffer(shaderDataBufferDesc);

            // Create a depth texture.
            auto depthTextureDesc = ResourceDesc{};
            depthTextureDesc.type = ResourceDesc::Type::TEXTURE;
            depthTextureDesc.name = "DepthTexture for Frame " + std::to_string(i);
            depthTextureDesc.texture.width = static_cast<uint32_t>(config.windowWidth);
            depthTextureDesc.texture.height = static_cast<uint32_t>(config.windowHeight);
            depthTextureDesc.texture.format = Format::D24_UNORM_S8_UINT; // TODO: check this format
            depthTextureDesc.texture.usage = TextureUsage::DEPTH_STENCIL_ATTACHMENT;

            frameResources[i].depthTexture = ctx.createTexture(depthTextureDesc);
        }

        // Create swapchain and associated resources
        swapchain.imageHandles = ctx.getSwapchainImages();
        for (size_t i = 0; i < swapchain.imageHandles.size(); ++i)
        {
            swapchain.readyToPresentSemaphores.push_back(ctx.createSemaphore());
        }
        swapchain.imageFormat = ctx.getSwapchainImageFormat();
    }

    Engine::~Engine()
    {
        // Clean up resources, free memory, etc.
        for (auto &[handle, tex] : textureData)
        {
            stbi_image_free(tex.data);
        }

        for (auto &[handle, mesh] : meshData)
        {
            if (mesh.type == MeshRaw::MeshType::GLTF)
            {
                // tinygltf::Model doesn't require explicit cleanup.
            }

            if (mesh.type == MeshRaw::MeshType::OBJ)
            {
                // If we had implemented OBJ loading, we would clean up any allocated resources here.
            }
        }

        ctx.cleanup();

        this->window.destroyWindow(this->mainWindow);
        spdlog::info("Engine shutdown, cleaned up resources.");
    }

    Scene Engine::createScene()
    {
        const SceneHandle id{nextHandle.scene++, 1};
        return Scene(this, id);
    }

    MeshHandle Engine::loadMesh(const std::string &path)
    {
        // find if the mesh is already loaded, if so return existing handle
        if (loadedMeshes.find(path) != loadedMeshes.end())
        {
            return loadedMeshes[path];
        }
        // find if the model extension is supported, if not return invalid handle
        auto extension = path.substr(path.find_last_of(".") + 1);
        if (supportedMeshExtensions.find(extension) == supportedMeshExtensions.end())
        {
            spdlog::error("Unsupported mesh format: {}", extension);
            return MeshHandle{};
        }

        MeshRaw mesh{};
        if (extension == "gltf" || extension == "glb")
        {
            tinygltf::TinyGLTF loader;
            tinygltf::Model model;

            std::string err;
            std::string warn;
            bool ret = false;
            if (extension == "gltf")
            {
                ret = loader.LoadASCIIFromFile(&model, &err, &warn, path);
            }
            else
            {
                ret = loader.LoadBinaryFromFile(&model, &err, &warn, path);
            }

            if (!warn.empty())
            {
                spdlog::warn("GLTF loader warning: {}", warn);
            }
            if (!err.empty())
            {
                spdlog::error("GLTF loader error: {}", err);
            }
            if (!ret)
            {
                spdlog::error("Failed to load GLTF model: {}", path);
                return MeshHandle{};
            }

            mesh.type = MeshRaw::MeshType::GLTF;
            mesh.data = std::move(model);
        }
        else
        {
            tinyobj::attrib_t attrib;
            std::vector<tinyobj::shape_t> shapes;
            std::vector<tinyobj::material_t> materials;

            if (!tinyobj::LoadObj(&attrib, &shapes, &materials, nullptr, nullptr, path.c_str()))
            {
                spdlog::error("Failed to load OBJ model: {}", path);
                return MeshHandle{};
            }

            // Load vertex and index data
            std::vector<Vertex> out_vertices;
            std::vector<uint32_t> out_indices;

            for (auto &index : shapes[0].mesh.indices)
            {
                Vertex v{
                    .pos = {attrib.vertices[index.vertex_index * 3], -attrib.vertices[index.vertex_index * 3 + 1], attrib.vertices[index.vertex_index * 3 + 2]},
                    .normal = {attrib.normals[index.normal_index * 3], -attrib.normals[index.normal_index * 3 + 1], attrib.normals[index.normal_index * 3 + 2]},
                    .uv = {attrib.texcoords[index.texcoord_index * 2], 1.0 - attrib.texcoords[index.texcoord_index * 2 + 1]}};

                out_vertices.push_back(v);
                out_indices.push_back(static_cast<uint32_t>(out_indices.size()));
            }

            mesh.type = MeshRaw::MeshType::OBJ;

            ObjRaw objRaw{};
            objRaw.vertices = std::move(out_vertices);
            objRaw.indices = std::move(out_indices);
            mesh.data = std::move(objRaw);
        }

        auto handle = getNextMeshHandle();
        loadedMeshes[path] = handle;
        meshData.insert({handle, std::move(mesh)});

        return handle;
    }

    TextureHandle Engine::loadTexture(const std::string &path)
    {
        // find if the texture is already loaded, if so return existing handle
        if (loadedTextures.find(path) != loadedTextures.end())
        {
            return loadedTextures[path];
        }

        // find if the texture extension is supported, if not return invalid handle
        auto extension = path.substr(path.find_last_of(".") + 1);
        if (supportedTextureExtensions.find(extension) == supportedTextureExtensions.end())
        {
            spdlog::error("Unsupported texture format: {}", extension);
            return TextureHandle{};
        }

        int width, height, nChannels;
        unsigned char *data = stbi_load(path.c_str(), &width, &height, &nChannels, 0);

        if (!data)
        {
            spdlog::error("Failed to load texture: {}", path);
            return TextureHandle{};
        }

        TextureRaw tex{};
        tex.width = width;
        tex.height = height;
        tex.channels = nChannels;
        tex.data = data;

        auto handle = getNextTextureHandle();
        loadedTextures[path] = handle;
        textureData.insert({handle, std::move(tex)});

        return handle;
    }

    BufferHandle Engine::uploadMesh(const MeshHandle &handle)
    {
        // TODO: remove mesh from meshData after uploading to GPU.
        auto it = meshData.find(handle);
        if (it == meshData.end())
        {
            spdlog::error("Mesh handle not found for upload.");
            return BufferHandle{};
        }

        auto &mesh = it->second;

        if (mesh.type == MeshRaw::MeshType::GLTF)
        {
            // In a real implementation, this is where we'd upload the GLTF mesh data to the GPU.
            spdlog::info("Uploading GLTF mesh with handle: {}", handle.index);
        }
        else if (mesh.type == MeshRaw::MeshType::OBJ)
        {
            auto &objRaw = std::get<ObjRaw>(mesh.data);

            auto bufferDesc = ResourceDesc{};
            bufferDesc.name = "OBJ Vertex + index Buffer";
            bufferDesc.type = ResourceDesc::Type::BUFFER;
            bufferDesc.buffer.usage = BufferUsage::VERTEXINDEX;
            bufferDesc.buffer.size = objRaw.vertices.size() * sizeof(Vertex) + objRaw.indices.size() * sizeof(uint32_t);
            bufferDesc.buffer.vertexIndexBuffer.indexCount = objRaw.indices.size();
            bufferDesc.buffer.vertexIndexBuffer.vertexCount = objRaw.vertices.size();
            bufferDesc.buffer.vertexIndexBuffer.offset = objRaw.vertices.size() * sizeof(Vertex);

            auto bufferHandle = ctx.createBuffer(bufferDesc);

            ctx.fillBuffer(bufferHandle, objRaw.vertices.data(), objRaw.vertices.size() * sizeof(Vertex), 0);
            ctx.fillBuffer(bufferHandle, objRaw.indices.data(), objRaw.indices.size() * sizeof(uint32_t), objRaw.vertices.size() * sizeof(Vertex));
            return bufferHandle;
        }
        else
        {
            spdlog::error("Unknown mesh type for upload.");
            return BufferHandle{};
        }

        return BufferHandle{};
    }

    Material Engine::createMaterial(MaterialTemplate type)
    {
        const MaterialHandle id{nextHandle.material++, 1};
        return Material(id, type);
    }

    RenderGraph Engine::createRenderGraph()
    {
        RenderGraph graph(this);
        return graph;
    }

    void Engine::beginFrame()
    {
        // wait for the gpu to finish rendering the previous frame
        auto &currentFrame = frameResources[frameCount];
        ctx.waitForFence(currentFrame.inFlightFence);
        ctx.resetFence(currentFrame.inFlightFence);

        this->window.pollEvents();

        if (this->resized)
        {
            recreateSwapchain();
            this->resized = false;
        }

        // acquire the next image from the swapchain
        ctx.acquireNextImage(currentFrame.readyToDrawSemaphore, UINT64_MAX, imageIdx);

        // update shader data buffer with per-frame data (e.g., camera matrices, time, etc.)
        // For this example, we'll just fill it with 0x00.
        std::vector<char> shaderData(1024 * 1024, 0x00);
        ctx.fillBuffer(currentFrame.shaderDataBuffer, shaderData.data(), shaderData.size());

        // record commands for the current frame
        ctx.beginCommandBuffer(currentFrame.commandBuffer);

        // transition swapchain image to be ready for rendering
        ctx.transitionImageLayout(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], TextureUsage::UNKNOWN, TextureUsage::COLOR_ATTACHMENT);
        ctx.transitionImageLayout(currentFrame.commandBuffer, currentFrame.depthTexture, TextureUsage::UNKNOWN, TextureUsage::DEPTH_STENCIL_ATTACHMENT);

        // begin dynamic rendering
        ctx.beginRendering(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], currentFrame.depthTexture);
        ctx.setViewport(currentFrame.commandBuffer, 0.0f, 0.0f, static_cast<float>(config.windowWidth), static_cast<float>(config.windowHeight));
        ctx.setScissor(currentFrame.commandBuffer, 0, 0, config.windowWidth, config.windowHeight);
    }

    void Engine::endFrame()
    {
        auto &currentFrame = frameResources[frameCount];

        ctx.endRendering(currentFrame.commandBuffer);

        ctx.transitionImageLayout(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], TextureUsage::COLOR_ATTACHMENT, TextureUsage::PRESENT_SRC);

        ctx.endCommandBuffer(frameResources[frameCount].commandBuffer);

        ctx.submit(frameResources[frameCount].commandBuffer,
                   {frameResources[frameCount].readyToDrawSemaphore},
                   {swapchain.readyToPresentSemaphores[imageIdx]},
                   frameResources[frameCount].inFlightFence);

        ctx.present(imageIdx, swapchain.readyToPresentSemaphores[imageIdx]);

        frameCount = (frameCount + 1) % 2; // Toggle between 0 and 1 for double buffering
    }

    void Engine::recreateSwapchain()
    {
        ctx.recreateSwapchain();
        swapchain.imageHandles = ctx.getSwapchainImages();
        swapchain.imageFormat = ctx.getSwapchainImageFormat();

        // Destroy old semaphores
        for (auto &semaphore : swapchain.readyToPresentSemaphores)
        {
            ctx.destroySemaphore(semaphore);
        }
        swapchain.readyToPresentSemaphores.clear();

        for (size_t i = 0; i < swapchain.imageHandles.size(); ++i)
        {
            swapchain.readyToPresentSemaphores.push_back(ctx.createSemaphore());
        }

        // Update depth textures for each frame resource
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            auto depthTextureDesc = ResourceDesc{};
            depthTextureDesc.type = ResourceDesc::Type::TEXTURE;
            depthTextureDesc.name = "DepthTexture for Frame " + std::to_string(i);
            depthTextureDesc.texture.width = static_cast<uint32_t>(config.windowWidth);
            depthTextureDesc.texture.height = static_cast<uint32_t>(config.windowHeight);
            depthTextureDesc.texture.format = Format::D24_UNORM_S8_UINT; // TODO: check this format
            depthTextureDesc.texture.usage = TextureUsage::DEPTH_STENCIL_ATTACHMENT;

            frameResources[i].depthTexture = ctx.createTexture(depthTextureDesc);
        }
    }

    CompiledScene Engine::compileScene(Scene &scene)
    {
        if (!scene.isValid())
        {
            spdlog::error("Invalid scene handle for compilation.");
            return CompiledScene{};
        }

        if (compiledScenes.find(scene.id()) != compiledScenes.end())
        {
            return compiledScenes[scene.id()];
        }

        CompiledScene compiledScene;

        for (Entity &entity : scene.entities)
        {
            if (entity.hasComponent<Mesh>() && entity.hasComponent<Material>())
            {
                auto &matComp = entity.getComponent<Material>();
                auto &meshComp = entity.getComponent<Mesh>();

                auto bufferHandle = uploadMesh(meshComp.id());
                if (!bufferHandle.isValid())
                {
                    spdlog::error("Failed to upload mesh for Entity {}. Skipping.", entity.id().index);
                    continue;
                }

                compiledScene.meshData[entity.id()] = bufferHandle;
                compiledScene.materialToMeshes[matComp.id()].insert(entity.id());
            }
        }

        auto swapchainFormat = ctx.getSwapchainImageFormat();
        auto depthFormat = Format::D24_UNORM_S8_UINT;

        for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
        {
            // create a pipeline for each material.
            auto pipelineDesc = ResourceDesc{};
            pipelineDesc.type = ResourceDesc::Type::GRAPHICS_PIPELINE;
            pipelineDesc.name = "Pipeline for Material " + std::to_string(materialHandle.index);

            auto [vertSrc, fragSrc] = Material::getShaderSources(materialHandle, MaterialTemplate::BASIC); // TODO: pick the right material

            auto shaderDesc = ResourceDesc{};
            shaderDesc.type = ResourceDesc::Type::SHADER;
            shaderDesc.shader.shaderType = ShaderType::VERTEX;
            shaderDesc.shader.sourceSize = vertSrc.size();
            shaderDesc.shader.source = vertSrc.data();

            pipelineDesc.pipeline.vertexShader = ctx.createShader(shaderDesc);

            shaderDesc.shader.shaderType = ShaderType::FRAGMENT;
            shaderDesc.shader.sourceSize = fragSrc.size();
            shaderDesc.shader.source = fragSrc.data();

            pipelineDesc.pipeline.fragmentShader = ctx.createShader(shaderDesc);

            pipelineDesc.pipeline.topology = PrimitiveTopology::TRIANGLE_LIST;

            pipelineDesc.pipeline.colorAttachmentFormat = swapchainFormat;
            pipelineDesc.pipeline.depthStencilAttachmentFormat = depthFormat;
            pipelineDesc.pipeline.vertexInputLayout = {
                .attributes = {
                    {.binding = 0, .location = 0, .format = Format::R32G32B32_SFLOAT, .size = sizeof(float) * 3, .offset = 0, .used = true},  // position
                    {.binding = 0, .location = 1, .format = Format::R32G32B32_SFLOAT, .size = sizeof(float) * 3, .offset = 12, .used = true}, // normal
                    {.binding = 0, .location = 2, .format = Format::R32G32_SFLOAT, .size = sizeof(float) * 2, .offset = 24, .used = true},    // uv
                    {.used = false}},
                .binding = 0,
                .stride = sizeof(Vertex),
                .perInstance = false};

            auto pipeline = ctx.createPipeline(pipelineDesc);
            compiledScene.materialToPipeline[materialHandle] = pipeline;
        }

        compiledScenes[scene.id()] = compiledScene;

        return compiledScene;
    }

    void Engine::render(Scene &scene, Entity &camera)
    {
        (void)camera;

        auto compiledScene = compileScene(scene);
        auto &currentFrame = frameResources[frameCount];
        auto commandBuffer = currentFrame.commandBuffer;
        auto shaderDataBuffer = currentFrame.shaderDataBuffer;

        auto windowAspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight);
        auto camPos = glm::vec3(0.0f, 0.0f, -5.0f); // Example camera position

        auto projection = glm::perspective(glm::radians(45.0f), windowAspect, 0.1f, 32.0f);
        auto view = glm::translate(glm::mat4(1.0f), camPos);
        auto model = glm::scale(glm::mat4(1.0f), glm::vec3(0.01f));

        ctx.fillBuffer(shaderDataBuffer, &projection, sizeof(projection), 0);
        ctx.fillBuffer(shaderDataBuffer, &view, sizeof(view), sizeof(projection));
        ctx.fillBuffer(shaderDataBuffer, &model, sizeof(model), sizeof(projection) + sizeof(view));

        auto shaderDataBufferAddress = ctx.getBufferDeviceAddress(shaderDataBuffer);

        for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
        {
            auto pipeline = compiledScene.materialToPipeline[materialHandle];
            ctx.bindPipeline(commandBuffer, pipeline);
            for (const auto &entityHandle : entitySet)
            {
                auto bufferHandle = compiledScene.meshData[entityHandle];
                auto desc = ctx.getResourceDesc(bufferHandle);
                ctx.bindVertexBuffer(commandBuffer, bufferHandle, 0);
                ctx.bindIndexBuffer(commandBuffer, bufferHandle, desc.buffer.vertexIndexBuffer.offset, Format::U32_UINT);
                ctx.pushConstants(commandBuffer, pipeline, ShaderType::VERTEX, &shaderDataBufferAddress, sizeof(shaderDataBufferAddress), 0);
                ctx.drawIndexed(commandBuffer, static_cast<uint32_t>(desc.buffer.vertexIndexBuffer.indexCount), 1, 0, 0, 0);
            }
        }

#if 0
        for (Entity &entity : scene.entities)
        {
            if (entity.hasComponent<Mesh>() && entity.hasComponent<Material>() && entity.hasComponent<Transform>())
            {
                // In a real implementation, this is where we'd issue draw calls to the GPU.
                // For this example, we'll just log the entity ID and its mesh/material.
                auto &meshComp = entity.getComponent<Mesh>();
                auto &matComp = entity.getComponent<Material>();
                spdlog::info("Rendering Entity {} with Mesh {} and Material {}",
                             entity.id().index, meshComp.id().index, matComp.id().index);
            }
            else if (entity.hasComponent<Light>())
            {
                auto &lightComp = entity.getComponent<Light>();
                spdlog::info("Light Entity {}, Color: ({}, {}, {}), Intensity: {}",
                             entity.id().index, lightComp.color.r, lightComp.color.g, lightComp.color.b, lightComp.intensity);
            }
        }

        auto &cameraComp = camera.getComponent<Camera>();
        spdlog::info("Camera Entity {}, Type: {}",
                     camera.id().index,
                     cameraComp.type == CameraType::PERSPECTIVE ? "Perspective" : "Orthographic");
#endif
    }

    bool Engine::running() const
    {
        return isRunning;
    }

    Entity Engine::createEntity(const Scene &scene, const std::string &name)
    {
        if (!scene.isValid())
        {
            return Entity();
        }
        const EntityHandle id{nextHandle.entity++, 1};
        return Entity(id, name);
    }

    EngineConfig Engine::getConfig() const
    {
        return config;
    }

    Window &Engine::getWindow()
    {
        return window;
    }

    WindowHandle Engine::getMainWindow() const
    {
        return mainWindow;
    }

    RenderContext &Engine::getRenderContext()
    {
        return ctx;
    }

    MeshHandle Engine::getNextMeshHandle()
    {
        return MeshHandle{nextHandle.mesh++, 1};
    }

    BufferHandle Engine::getNextBufferHandle()
    {
        return BufferHandle{nextHandle.buffer++, 1};
    }

    TextureHandle Engine::getNextTextureHandle()
    {
        return TextureHandle{nextHandle.texture++, 1};
    }

    ShaderHandle Engine::getNextShaderHandle()
    {
        return ShaderHandle{nextHandle.shader++, 1};
    }

    PipelineHandle Engine::getNextPipelineHandle()
    {
        return PipelineHandle{nextHandle.pipeline++, 1};
    }

    CommandPoolHandle Engine::getNextCommandPoolHandle()
    {
        return CommandPoolHandle{nextHandle.commandPool++, 1};
    }

    CommandBufferHandle Engine::getNextCommandBufferHandle()
    {
        return CommandBufferHandle{nextHandle.commandBuffer++, 1};
    }

    SemaphoreHandle Engine::getNextSemaphoreHandle()
    {
        return SemaphoreHandle{nextHandle.semaphore++, 1};
    }

    FenceHandle Engine::getNextFenceHandle()
    {
        return FenceHandle{nextHandle.fence++, 1};
    }

    SwapchainHandle Engine::getNextSwapchainHandle()
    {
        return SwapchainHandle{nextHandle.swapchain++, 1};
    }
}
